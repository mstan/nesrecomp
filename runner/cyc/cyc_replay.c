/* cyc_replay.c - always-on diagnostic replay recording; see cyc_replay.h.
 *
 * Threading: the emulation thread formats a few hundred bytes per frame and
 * copies them (and, at a segment's start, one save state) into a fixed
 * single-producer / single-consumer byte queue. One writer thread owns every
 * file. The queue never grows and the producer never waits: a full queue or a
 * failed write sets a sticky error, recording stops with a logged reason, and
 * the game runs on. Files, retention and eviction run in the writer, in the
 * order the producer queued them.
 *
 * Without SDL (headless-only builds) there is no thread to write with, so the
 * module compiles to hooks that do nothing.
 */
#include "cyc_replay.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CYC_WITH_SDL

void cyc_replay_set_config_path(const char *path) { (void)path; }
bool cyc_replay_init(const char *title) { (void)title; return false; }
void cyc_replay_set_policy(uint32_t digest) { (void)digest; }
void cyc_replay_frame_begin(const uint8_t phys[2], const uint8_t logical[4], const uint8_t final_pads[2])
{ (void)phys; (void)logical; (void)final_pads; }
void cyc_replay_frame_end(void) {}
void cyc_replay_suspend(const char *why) { (void)why; }
void cyc_replay_shutdown(void) {}
bool cyc_replay_active(void) { return false; }
const char *cyc_replay_path(void) { return ""; }

#else

#include <SDL.h>
#include <errno.h>
#include <time.h>

#include "cyc_core.h"
#include "cyc_diagnostics.h"
#include "cyc_host.h"
#include "cyc_recomp.h"
#include "cyc_ring.h"
#include "cyc_run.h"
#include "cyc_session.h"
#include "cyc_settings.h"
#include "cyc_state.h"
#include "cyc_trace.h"

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#define replay_pid() ((long)_getpid())
/* Paths are UTF-8 (SDL's base path, the environment); the C runtime's narrow
 * calls would use the ANSI code page and fail on a user name with other letters. */
static wchar_t *u8w(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (w && !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) { free(w); w = NULL; }
    return w;
}
static FILE *u_fopen(const char *path, const char *mode)
{
    wchar_t *w = u8w(path), m[8] = { 0 };
    for (size_t i = 0; mode[i] && i < 7; ++i) m[i] = (wchar_t)mode[i];
    FILE *f = w ? _wfopen(w, m) : NULL;
    free(w);
    return f;
}
static int u_remove(const char *path)
{
    wchar_t *w = u8w(path);
    int r = w ? _wremove(w) : -1;
    free(w);
    return r;
}
static int u_mkdir(const char *path)
{
    wchar_t *w = u8w(path);
    int r = w ? _wmkdir(w) : -1;
    free(w);
    return r;
}
#else
#include <sys/stat.h>
#include <unistd.h>
#define replay_pid() ((long)getpid())
#define u_fopen fopen
#define u_remove remove
#define u_mkdir(p) mkdir(p, 0755)
#endif

/* ------------------------------------------------------------------------- */
/* The bounded writer                                                        */
/* ------------------------------------------------------------------------- */

enum { Q = CYC_REPLAY_QUEUE_BYTES, FILE_BUFFER = 65536 };
enum { OP_BEGIN, OP_STATE, OP_LINE, OP_REMOVE, OP_END };
enum { ERR_NONE, ERR_FULL, ERR_INVALID, ERR_IO };

typedef struct { uint32_t op, size; } Command;
typedef struct {
    char path[1280], config[1280], mods[1280];
    char header[6144];
} BeginPayload;

typedef struct Writer {
    SDL_Thread *thread;
    SDL_atomic_t stop, error, read_pos, write_pos;
    unsigned head;                       /* producer only */
    unsigned peak;                       /* producer only: most bytes ever queued */
    unsigned char bytes[Q];
} Writer;

static void w_fail(Writer *w, int e) { SDL_AtomicCAS(&w->error, ERR_NONE, e); }

static void w_put(Writer *w, unsigned at, const void *data, size_t n)
{
    size_t first = Q - at;
    if (first > n) first = n;
    if (first) memcpy(w->bytes + at, data, first);
    if (n > first) memcpy(w->bytes, (const char *)data + first, n - first);
}

static void w_get(Writer *w, unsigned at, void *data, size_t n)
{
    size_t first = Q - at;
    if (first > n) first = n;
    if (first) memcpy(data, w->bytes + at, first);
    if (n > first) memcpy((char *)data + first, w->bytes, n - first);
}

static bool w_enqueue(Writer *w, unsigned op, const void *data, size_t n)
{
    if (!w || SDL_AtomicGet(&w->stop) || SDL_AtomicGet(&w->error)) return false;
    if (sizeof(Command) + n >= (size_t)Q / 2) { w_fail(w, ERR_INVALID); return false; }
    unsigned tail = (unsigned)SDL_AtomicGet(&w->read_pos);
    if (sizeof(Command) + n > (size_t)((tail + Q - w->head - 1) % Q)) { w_fail(w, ERR_FULL); return false; }
    Command c = { op, (uint32_t)n };
    unsigned at = w->head;
    w_put(w, at, &c, sizeof(c));
    at = (unsigned)((at + sizeof(c)) % Q);
    if (n) w_put(w, at, data, n);
    at = (unsigned)((at + n) % Q);
    w->head = at;
    unsigned used = (at + Q - tail) % Q;
    if (used > w->peak) w->peak = used;
    /* SDL atomics order the payload writes before the consumer sees the head. */
    SDL_AtomicSet(&w->write_pos, (int)at);
    return true;
}

static bool w_write_ring(Writer *w, FILE *f, unsigned at, size_t n)
{
    size_t first = Q - at;
    if (first > n) first = n;
    return f && fwrite(w->bytes + at, 1, first, f) == first && (n == first || fwrite(w->bytes, 1, n - first, f) == n - first);
}

static bool copy_optional(const char *source, const char *dest)
{
    if (!*source) return true;
    FILE *in = u_fopen(source, "rb");
    if (!in) return errno == ENOENT;
    FILE *out = u_fopen(dest, "wb");
    bool ok = out != NULL;
    char bytes[8192];
    size_t n;
    while (ok && (n = fread(bytes, 1, sizeof(bytes), in))) ok = fwrite(bytes, 1, n, out) == n;
    if (ferror(in)) ok = false;
    fclose(in);
    if (out && fclose(out)) ok = false;
    return ok;
}

static int SDLCALL w_worker(void *opaque)
{
    Writer *w = (Writer *)opaque;
    FILE *text = NULL;
    static char text_buffer[FILE_BUFFER];
    unsigned tail = 0;
    uint32_t last_flush = SDL_GetTicks();
    bool dirty = false;
    {
        /* Test aid: a disk that has stopped answering (the overflow acceptance test). */
        const char *stall = getenv("NESRECOMP_REPLAY_TEST_STALL_MS");
        long ms = stall ? atol(stall) : 0;
        for (; ms > 0 && !SDL_AtomicGet(&w->stop); ms -= 10) SDL_Delay(10);
    }
    for (;;) {
        if ((unsigned)SDL_AtomicGet(&w->write_pos) != tail) {
            Command c;
            w_get(w, tail, &c, sizeof(c));
            unsigned at = (unsigned)((tail + sizeof(c)) % Q);
            bool ok = true;
            switch (c.op) {
            case OP_BEGIN: {
                BeginPayload *b = (BeginPayload *)malloc(sizeof(*b));
                char path[1400];
                if (!b || c.size != sizeof(*b)) { free(b); ok = false; break; }
                w_get(w, at, b, sizeof(*b));
                if (text) { ok = fclose(text) == 0; text = NULL; }
                text = ok ? u_fopen(b->path, "wb") : NULL;
                ok = ok && text && !setvbuf(text, text_buffer, _IOFBF, sizeof(text_buffer)) && fputs(b->header, text) != EOF;
                snprintf(path, sizeof(path), "%s.config.ini", b->path);
                ok = copy_optional(b->config, path) && ok;
                snprintf(path, sizeof(path), "%s.mods.toml", b->path);
                ok = copy_optional(b->mods, path) && ok;
                free(b);
                break;
            }
            case OP_STATE: {
                /* the checkpoint of the segment whose text was just opened; the
                 * path is that text file's name + ".state" (carried first) */
                char path[1400];
                char name[1280];
                w_get(w, at, name, sizeof(name));
                snprintf(path, sizeof(path), "%s.state", name);
                FILE *f = u_fopen(path, "wb");
                size_t bytes = c.size - sizeof(name);
                unsigned data_at = (unsigned)((at + sizeof(name)) % Q);
                ok = f && w_write_ring(w, f, data_at, bytes);
                if (f && fclose(f)) ok = false;
                break;
            }
            case OP_LINE: ok = w_write_ring(w, text, at, c.size); break;
            case OP_REMOVE: {
                char path[1400];
                if (c.size >= sizeof(path)) { ok = false; break; }
                w_get(w, at, path, c.size);
                path[c.size] = 0;
                ok = !u_remove(path) || errno == ENOENT;
                break;
            }
            case OP_END:
                if (text) ok = fclose(text) == 0;
                text = NULL;
                break;
            default: ok = false; break;
            }
            if (!ok) { w_fail(w, ERR_IO); break; }
            /* the payload stays owned until the disk work is done */
            tail = (unsigned)((tail + sizeof(c) + c.size) % Q);
            SDL_AtomicSet(&w->read_pos, (int)tail);
            dirty = text != NULL;
        } else {
            if (SDL_AtomicGet(&w->stop) || SDL_AtomicGet(&w->error)) {
                if ((unsigned)SDL_AtomicGet(&w->write_pos) == tail) break;
                continue;
            }
            SDL_Delay(2);
        }
        uint32_t now = SDL_GetTicks();
        if (dirty && now - last_flush >= 100) {
            /* A hung simulation still leaves its last queued input on disk. */
            if (text && fflush(text)) { w_fail(w, ERR_IO); break; }
            last_flush = now;
            dirty = false;
        }
    }
    if (text && fclose(text)) w_fail(w, ERR_IO);
    return SDL_AtomicGet(&w->error) != ERR_NONE;
}

static const char *w_error(Writer *w)
{
    switch (w ? SDL_AtomicGet(&w->error) : ERR_NONE) {
    case ERR_FULL: return "the replay disk writer fell behind (bounded queue full)";
    case ERR_INVALID: return "an invalid or oversized replay write";
    case ERR_IO: return "a replay file write failed";
    default: return NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* The recorder                                                              */
/* ------------------------------------------------------------------------- */

enum {
    WHY_FIRST = 1, WHY_CAP, WHY_SKIP, WHY_LOAD, WHY_SETTINGS, WHY_RESUME,
};
static const char *const WHY_NAME[] = { "", "start", "segment length", "frames skipped or machine replaced",
                                         "save state loaded", "settings changed", "recording resumed" };

#define EV_CAP (256u * 1024u)

static struct {
    bool enabled, failed, started, suspended;
    Writer *writer;
    char stem[1300], dir[1100], title[64], path[1400];
    unsigned segment, oldest, rows;
    unsigned segment_frames;
    size_t limit;
    size_t history_bytes, sizes[CYC_REPLAY_SEGMENTS];
    bool recording;
    uint32_t next_frame;
    uint64_t last_cycles;
    uint32_t seen_loads, seen_hle, seen_mods, seen_policy, policy;
    bool zapper_valid;
    int zapx, zapy, zaptrig;
    char *ev;
    size_t ev_len;
    bool ev_overflow;
    bool pending_hash;
    uint32_t pending_frame;
    bool announced;
    /* what recording costs the emulation thread (reported at exit) */
    uint64_t t_begin, t_end, t_open, t_open_worst;
    uint64_t frames, opens;
} S;

bool cyc_replay_active(void) { return S.recording && !S.failed && !S.suspended; }
const char *cyc_replay_path(void) { return S.recording ? S.path : ""; }
void cyc_replay_set_policy(uint32_t digest) { S.policy = digest; }

static void ring_event(unsigned code, uint32_t value, uint32_t frame)
{
    cyc_ring_push_len(CYC_EV_REPLAY, (uint16_t)code, value, frame);
}

static void stop_recording(const char *why)
{
    if (S.failed) return;
    S.failed = true;
    S.recording = false;
    fprintf(stderr, "[replay] recording stopped: %s\n", why ? why : "unknown error");
    fflush(stderr);
    cyc_diagnostics_error("[replay] recording stopped: %s\n", why ? why : "unknown error");
    ring_event(CYC_REPLAY_EV_FAILED, S.segment, cyc_ring_frame);
}

static bool check_writer(void)
{
    const char *e = w_error(S.writer);
    if (e) stop_recording(e);
    return !S.failed;
}

static bool line(const char *text, size_t n)
{
    return w_enqueue(S.writer, OP_LINE, text, n);
}

/* ---- notes from the machine's callers ---- */

static void ev_append(const char *fmt, ...)
{
    if (!S.ev || S.ev_overflow) return;
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return;
    if (S.ev_len + (size_t)n > EV_CAP) { S.ev_overflow = true; return; }
    memcpy(S.ev + S.ev_len, buf, (size_t)n);
    S.ev_len += (size_t)n;
}

static void on_cheat(uint16_t addr, uint8_t value)
{
    if (S.recording) ev_append("P %u %04X %02X\n", (unsigned)cyc_ring_frame, addr, value);
}

static void on_drive(char action, unsigned side)
{
    if (!S.recording) return;
    if (action == 'E') ev_append("D %u E\n", (unsigned)cyc_ring_frame);
    else ev_append("D %u I %u\n", (unsigned)cyc_ring_frame, side);
}

/* ---- where the files go ---- */

static bool open_directory(const char *folder)
{
    u_mkdir(folder);
    char probe[1300];
    snprintf(probe, sizeof(probe), "%s/.cyc-replay-%ld", folder, replay_pid());
    FILE *f = u_fopen(probe, "wb");
    if (!f) return false;
    fclose(f);
    u_remove(probe);
    return true;
}

/* An environment variable as UTF-8 (Windows keeps them in UTF-16). */
static const char *env_utf8(const char *name, char *buf, size_t n)
{
#ifdef _WIN32
    wchar_t *wname = u8w(name), wide[1300];
    DWORD got = wname ? GetEnvironmentVariableW(wname, wide, 1300) : 0;
    free(wname);
    if (!got || got >= 1300 || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, (int)n, NULL, NULL)) return NULL;
    return buf;
#else
    (void)buf; (void)n;
    return getenv(name);
#endif
}

static bool choose_directory(char *out, size_t n)
{
    char tmp[1300];
    const char *e = env_utf8("NESRECOMP_REPLAY_DIR", tmp, sizeof(tmp));
    if (e && *e) return snprintf(out, n, "%s", e) < (int)n && open_directory(out);
    e = env_utf8("NESRECOMP_DIAGNOSTICS_DIR", tmp, sizeof(tmp));
    if (e && *e && snprintf(out, n, "%s", e) < (int)n && open_directory(out)) return true;
    char *base = SDL_GetBasePath();
    if (base) {
        size_t len = strlen(base);
        while (len && (base[len - 1] == '/' || base[len - 1] == '\\')) base[--len] = 0;
        bool ok = snprintf(out, n, "%s/diagnostics", base) < (int)n && open_directory(out);
        SDL_free(base);
        if (ok) return true;
    }
    char *pref = SDL_GetPrefPath("NESRecomp", "diagnostics");
    if (pref) {
        size_t len = strlen(pref);
        while (len && (pref[len - 1] == '/' || pref[len - 1] == '\\')) pref[--len] = 0;
        bool ok = snprintf(out, n, "%s", pref) < (int)n && open_directory(out);
        SDL_free(pref);
        if (ok) return true;
    }
    return false;
}

static void abspath(char *p, size_t n)
{
    /* The writer thread must not depend on the working directory. */
    char full[1300];
#ifdef _WIN32
    wchar_t *w = u8w(p), wide[1300];
    DWORD got = w ? GetFullPathNameW(w, 1300, wide, NULL) : 0;
    free(w);
    if (got && got < 1300 && WideCharToMultiByte(CP_UTF8, 0, wide, -1, full, (int)sizeof(full), NULL, NULL)) snprintf(p, n, "%s", full);
#else
    if (p[0] != '/' && getcwd(full, sizeof(full))) {
        char joined[1400];
        snprintf(joined, sizeof(joined), "%s/%s", full, p);
        snprintf(p, n, "%s", joined);
    }
#endif
    for (char *c = p; *c; ++c)
        if (*c == '\\') *c = '/';
}

static char s_config_path[1100];
void cyc_replay_set_config_path(const char *path) { snprintf(s_config_path, sizeof(s_config_path), "%s", path ? path : ""); }

bool cyc_replay_init(const char *title)
{
    const char *off = getenv("NESRECOMP_REPLAY_RECORD");
    if (off && *off == '0') {
        cyc_diagnostics_note("[replay] recording off (NESRECOMP_REPLAY_RECORD=0)");
        return false;
    }
    memset(&S, 0, sizeof(S));
    S.segment_frames = CYC_REPLAY_SEGMENT_FRAMES;
    S.limit = CYC_REPLAY_HISTORY_BYTES;
    const char *v = getenv("NESRECOMP_REPLAY_HISTORY_MIB");
    if (v && *v) {
        char *end;
        unsigned long mib = strtoul(v, &end, 10);
        if (!*end && mib >= 2 && mib <= 256) S.limit = (size_t)mib * 1024 * 1024;
    }
    v = getenv("NESRECOMP_REPLAY_SEGMENT_FRAMES");
    if (v && *v) {
        char *end;
        unsigned long n = strtoul(v, &end, 10);
        if (!*end && n >= 2 && n <= CYC_REPLAY_SEGMENT_FRAMES) S.segment_frames = (unsigned)n;
    }
    size_t t = 0;
    for (const char *c = title ? title : "game"; *c && t + 1 < sizeof(S.title); ++c)
        S.title[t++] = ((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z')) ? *c : '-';
    S.title[t] = 0;
    if (!t) strcpy(S.title, "game");
    if (!choose_directory(S.dir, sizeof(S.dir))) {
        fprintf(stderr, "[replay] no writable diagnostics folder; replay recording is off\n");
        cyc_diagnostics_error("[replay] no writable diagnostics folder; replay recording is off\n");
        return false;
    }
    abspath(S.dir, sizeof(S.dir));
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char stamp[32];
    if (!tm || !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm)) strcpy(stamp, "session");
    snprintf(S.stem, sizeof(S.stem), "%s/%s-replay-%s-%ld", S.dir, S.title, stamp, replay_pid());
    S.ev = (char *)malloc(EV_CAP);
    Writer *w = (Writer *)calloc(1, sizeof(Writer));
    if (!S.ev || !w) { free(S.ev); free(w); S.ev = NULL; return false; }
    w->thread = SDL_CreateThread(w_worker, "cyc-replay-writer", w);
    if (!w->thread) { free(S.ev); free(w); S.ev = NULL; return false; }
    S.writer = w;
    S.enabled = true;
    S.oldest = 1;
    S.seen_loads = cyc_state_load_count;
    S.seen_hle = cyc_host_settings_revision;
    S.seen_mods = cyc_session_mods_revision;
    S.seen_policy = S.policy;
    cyc_cheat_change_hook = on_cheat;
    cyc_fds_change_hook = on_drive;
    S.started = true;
    char msg[1500];
    snprintf(msg, sizeof(msg), "[replay] recording to %s-NNNNNN.cycreplay (background writer; one hour or %u MiB of checkpoints; "
             "attach every segment and sidecar)", S.stem, (unsigned)(S.limit >> 20));
    printf("%s\n", msg);
    fflush(stdout);
    cyc_diagnostics_note("%s", msg);
    return true;
}

/* ---- segments ---- */

static void remove_segment_files(unsigned n)
{
    static const char *const SUFFIX[] = { "", ".state", ".config.ini", ".mods.toml" };
    for (unsigned i = 0; i < 4; ++i) {
        char p[1500];
        snprintf(p, sizeof(p), "%s-%06u.cycreplay%s", S.stem, n, SUFFIX[i]);
        w_enqueue(S.writer, OP_REMOVE, p, strlen(p));
    }
}

static bool evict_oldest(void)
{
    if (S.oldest >= S.segment) return false;
    remove_segment_files(S.oldest);
    unsigned slot = S.oldest % CYC_REPLAY_SEGMENTS;
    S.history_bytes -= S.sizes[slot];
    S.sizes[slot] = 0;
    ring_event(CYC_REPLAY_EV_EVICT, S.oldest, cyc_ring_frame);
    ++S.oldest;
    return check_writer();
}

static void end_segment(void)
{
    if (S.recording) w_enqueue(S.writer, OP_END, NULL, 0);
    S.recording = false;
}

static void zapper_line(char *out, size_t n, uint32_t frame, bool force)
{
    out[0] = 0;
    CycZapperState gun;
    cyc_zapper_state(&gun);
    if (!gun.port) return;
    int x = gun.x, y = gun.y, t = gun.trigger ? 1 : 0;
    if (!force && S.zapper_valid && x == S.zapx && y == S.zapy && t == S.zaptrig) return;
    S.zapper_valid = true;
    S.zapx = x; S.zapy = y; S.zaptrig = t;
    snprintf(out, n, "Z %u %d %d %d\n", frame, x, y, t);
}

static bool open_segment_impl(unsigned why, uint32_t frame);
static bool open_segment(unsigned why, uint32_t frame)
{
    uint64_t t = SDL_GetPerformanceCounter();
    bool ok = open_segment_impl(why, frame);
    t = SDL_GetPerformanceCounter() - t;
    S.t_open += t;
    ++S.opens;
    if (t > S.t_open_worst) S.t_open_worst = t;
    return ok;
}

static bool open_segment_impl(unsigned why, uint32_t frame)
{
    end_segment();
    if (S.segment) ring_event(why == WHY_CAP ? CYC_REPLAY_EV_CLOSE : CYC_REPLAY_EV_DISCONT, S.segment, frame);
    uint8_t *state = NULL;
    size_t len = 0;
    char err[200] = "";
    if (!cyc_state_save(&state, &len, err, sizeof(err))) {
        char msg[300];
        snprintf(msg, sizeof(msg), "cannot serialize the machine checkpoint: %s", err);
        stop_recording(msg);
        return false;
    }
    if (len + sizeof(Command) + 1280 + 64 >= (size_t)Q / 2) {
        free(state);
        stop_recording("the machine checkpoint is larger than the bounded writer queue allows");
        return false;
    }
    ++S.segment;
    /* The new segment's checkpoint must fit; evict the oldest until it does. */
    while ((S.segment - S.oldest >= CYC_REPLAY_SEGMENTS || S.history_bytes + len > S.limit) && S.oldest < S.segment)
        if (!evict_oldest()) { free(state); return false; }
    S.sizes[S.segment % CYC_REPLAY_SEGMENTS] = len;
    S.history_bytes += len;
    snprintf(S.path, sizeof(S.path), "%s-%06u.cycreplay", S.stem, S.segment);

    BeginPayload *b = (BeginPayload *)calloc(1, sizeof(*b));
    if (!b) { free(state); stop_recording("out of memory"); return false; }
    snprintf(b->path, sizeof(b->path), "%s", S.path);
    snprintf(b->config, sizeof(b->config), "%s", s_config_path[0] ? s_config_path : cyc_settings_default_path());
    b->mods[0] = 0;
    if (cyc_session_mods_enabled()) {
        char *base = SDL_GetBasePath();
        if (base) { snprintf(b->mods, sizeof(b->mods), "%smods/state.toml", base); SDL_free(base); }
    }
    char *h = b->header;
    size_t cap = sizeof(b->header), at = 0;
#define H(...) do { int w_ = snprintf(h + at, cap - at, __VA_ARGS__); if (w_ > 0 && (size_t)w_ < cap - at) at += (size_t)w_; } while (0)
    const NesFdsHlePlan *plan = cyc_is_fds() ? cyc_host_hle_plan() : NULL;
    H("CYCREPLAY 1\nSEGMENT %u\nSESSION %s\nFIRST_FRAME %u\nWHY %s\n", S.segment, S.stem, frame,
      WHY_NAME[why < sizeof(WHY_NAME) / sizeof(WHY_NAME[0]) ? why : 0]);
    H("PROGRAM %s prg=%08X cart=%08X\n", cyc_native_program_name ? cyc_native_program_name : "", cyc_native_prg_hash,
      cyc_native_cart_hash);
    H("REGION %s\nFDS %d\nEXTRA_SCANLINES %u\nNATIVE %d\n", cyc_region() == CYC_REGION_PAL ? "pal" : "ntsc",
      cyc_is_fds() ? 1 : 0, cyc_extra_scanlines(), cyc_run_native ? 1 : 0);
    if (plan) H("HLE auto_swap=%d fast_load=%d\n", plan->auto_swap ? 1 : 0, plan->fast_load ? 1 : 0);
    H("POLICY %08X\nMODS %d\n", S.policy, cyc_session_mods_enabled() ? 1 : 0);
    char opts[2048];
    cyc_session_taken_options(opts, sizeof(opts));
    H("%s", opts);
    H("BEGIN\n");
#undef H
    bool ok = w_enqueue(S.writer, OP_BEGIN, b, sizeof(*b));
    free(b);
    if (ok) {
        /* the checkpoint, tagged with the text file it belongs to */
        size_t total = 1280 + len;
        uint8_t *payload = (uint8_t *)malloc(total);
        if (!payload) { free(state); stop_recording("out of memory"); return false; }
        memset(payload, 0, 1280);
        snprintf((char *)payload, 1280, "%s", S.path);
        memcpy(payload + 1280, state, len);
        ok = w_enqueue(S.writer, OP_STATE, payload, total);
        free(payload);
    }
    free(state);
    if (!ok || !check_writer()) { if (!S.failed) stop_recording(w_error(S.writer) ? w_error(S.writer) : "queue refused a write"); return false; }
    S.recording = true;
    S.rows = 0;
    S.zapper_valid = false;
    S.seen_loads = cyc_state_load_count;
    S.seen_hle = cyc_host_settings_revision;
    S.seen_mods = cyc_session_mods_revision;
    S.seen_policy = S.policy;
    S.ev_len = 0;                        /* everything before the checkpoint is in it */
    S.ev_overflow = false;
    ring_event(CYC_REPLAY_EV_OPEN, S.segment, frame);
    if (S.segment == 1 || why == WHY_RESUME) {
        /* nothing more to say; the log line at start covers the first */
    }
    return true;
}

void cyc_replay_suspend(const char *why)
{
    if (!S.enabled || S.failed || S.suspended) return;
    end_segment();
    S.suspended = true;
    char msg[200];
    snprintf(msg, sizeof(msg), "[replay] recording paused: %s (it resumes with a new segment afterwards)", why ? why : "unsupported mode");
    fprintf(stderr, "%s\n", msg);
    cyc_diagnostics_note("%s", msg);
    ring_event(CYC_REPLAY_EV_SUSPEND, S.segment, cyc_ring_frame);
}

static void frame_begin_impl(const uint8_t phys[2], const uint8_t logical[4], const uint8_t final_pads[2])
{
    S.pending_hash = false;
    if (!S.enabled || S.failed) return;
    if (!check_writer()) return;
    unsigned why = 0;
    uint32_t frame = cyc_ring_frame;
    if (S.suspended) { S.suspended = false; why = WHY_RESUME; }
    else if (!S.recording) why = WHY_FIRST;
    else if (S.rows >= S.segment_frames) why = WHY_CAP;
    else if (S.seen_loads != cyc_state_load_count) why = WHY_LOAD;
    else if (S.seen_hle != cyc_host_settings_revision || S.seen_mods != cyc_session_mods_revision ||
             S.seen_policy != S.policy) why = WHY_SETTINGS;
    else if (frame != S.next_frame || cyc_cycle_count() != S.last_cycles) why = WHY_SKIP;
    if (why && !open_segment(why, frame)) return;
    char buf[512];
    /* the frame's pre-actions, in the order they happened */
    if (S.ev_overflow) {
        /* too many to log: the checkpoint is the honest way to carry them */
        if (!open_segment(WHY_SETTINGS, frame)) return;
    }
    if (S.ev_len && !line(S.ev, S.ev_len)) { check_writer(); return; }
    S.ev_len = 0;
    zapper_line(buf, sizeof(buf), frame, S.rows == 0);
    size_t n = strlen(buf);
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "I %u %02X %02X %02X %02X %02X %02X %02X %02X\n", frame, phys[0], phys[1],
                          logical[0], logical[1], logical[2], logical[3], final_pads[0], final_pads[1]);
    if (!line(buf, n)) { check_writer(); return; }
    S.pending_hash = true;
    S.pending_frame = frame;
}

static void frame_end_impl(void)
{
    if (!S.pending_hash || S.failed || !S.recording) return;
    S.pending_hash = false;
    CycCpuState s;
    cyc_cpu_state(&s);
    char buf[200];
    int n = snprintf(buf, sizeof(buf), "H %u %016llX %016llX %04X %02X %02X %02X %02X %02X %llu\n", S.pending_frame,
                     (unsigned long long)cyc_mem_state_hash(), (unsigned long long)cyc_hw_state_hash(), s.pc, s.a, s.x, s.y,
                     s.s, s.p, (unsigned long long)cyc_cycle_count());
    if (n <= 0 || !line(buf, (size_t)n)) { check_writer(); return; }
    ++S.rows;
    S.next_frame = S.pending_frame + 1;
    S.last_cycles = cyc_cycle_count();
}

void cyc_replay_frame_begin(const uint8_t phys[2], const uint8_t logical[4], const uint8_t final_pads[2])
{
    if (!S.enabled) return;
    uint64_t t = SDL_GetPerformanceCounter();
    frame_begin_impl(phys, logical, final_pads);
    S.t_begin += SDL_GetPerformanceCounter() - t;
}

void cyc_replay_frame_end(void)
{
    if (!S.enabled) return;
    uint64_t t = SDL_GetPerformanceCounter();
    frame_end_impl();
    S.t_end += SDL_GetPerformanceCounter() - t;
    ++S.frames;
}

void cyc_replay_shutdown(void)
{
    if (!S.started) return;
    S.started = false;
    cyc_cheat_change_hook = NULL;
    cyc_fds_change_hook = NULL;
    end_segment();
    Writer *w = S.writer;
    S.writer = NULL;
    if (w) {
        SDL_AtomicSet(&w->stop, 1);
        SDL_WaitThread(w->thread, NULL);
        double us = 1e6 / (double)SDL_GetPerformanceFrequency();
        char stats[512];
        snprintf(stats, sizeof(stats),
                 "[replay] %llu frames in %llu segments; recording cost per frame: %.1f us total (begin %.1f, hashes %.1f; "
                 "checkpoints are %.1f of begin); %llu checkpoints, mean %.2f ms, worst %.2f ms; writer queue peak %u KiB",
                 (unsigned long long)S.frames, (unsigned long long)S.opens,
                 S.frames ? (double)(S.t_begin + S.t_end) * us / (double)S.frames : 0.0,
                 S.frames ? (double)S.t_begin * us / (double)S.frames : 0.0,
                 S.frames ? (double)S.t_end * us / (double)S.frames : 0.0,
                 S.frames ? (double)S.t_open * us / (double)S.frames : 0.0,
                 (unsigned long long)S.opens, S.opens ? (double)S.t_open * us / 1000.0 / (double)S.opens : 0.0,
                 (double)S.t_open_worst * us / 1000.0, w->peak >> 10);
        printf("%s\n", stats);
        fflush(stdout);
        cyc_diagnostics_note("%s", stats);
        const char *e = w_error(w);
        if (e && !S.failed) {
            fprintf(stderr, "[replay] %s; the last frames may be missing\n", e);
            cyc_diagnostics_error("[replay] %s; the last frames may be missing\n", e);
        }
        free(w);
    }
    free(S.ev);
    S.ev = NULL;
    S.recording = false;
    S.enabled = false;
}

#endif /* CYC_WITH_SDL */
