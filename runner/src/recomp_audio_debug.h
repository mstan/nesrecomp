/* recomp_audio_debug.h - always-on digital "oscilloscope" for the *recomp audio path.
 *
 * The problem this solves: the assistant cannot hear. Round 1 "fixed" the output
 * clock-domain layer (recomp_audio_drc.h) on faith and only helped Genesis; NES/N64
 * still crackle because their crackle is born UPSTREAM and the bridge faithfully
 * reproduces it. To stop guessing, we make every audio boundary falsifiable: tap
 * the DIGITAL sample stream at each stage into always-on ring buffers and dump a
 * trailing window AFTER the user says "I heard it" (never arm-then-reproduce).
 *
 * Five canonical taps (register more as needed; taps are keyed by name):
 *   T0  per-channel raw generator   (e.g. nes pulse1/pulse2/tri/noise/dmc)
 *   T1  final emulator-rate PCM     (what the console produced, post-mix)
 *   T2  bridge input                (what recomp_audio_drc receives)
 *   T3  bridge output               (device-rate, what the bridge produces)
 *   T4  SDL callback bytes          (what the host is actually handed)
 * Cross-tap presence of an anomaly localizes the FIRST DIRTY TAP:
 *   in T1            -> synthesis/upstream
 *   T2 but not T1    -> per-system conversion/decimation
 *   T3 but not T2    -> shared bridge/resampler
 *   only T4          -> SDL callback fill/conversion/starvation
 *
 * Single-file. Define RECOMP_AUDIO_DEBUG_IMPL in exactly ONE translation unit.
 * C99 and C++ compatible. Depends only on the C stdlib (+ Win32 or pthreads for a
 * lock, already linked by every runner).
 *
 * Capture is ALWAYS-ON when enabled and bounded by a ring (eviction keeps memory
 * flat). It is gated by an env var so shipped builds pay nothing unless asked:
 *   RECOMP_AUDIO_DEBUG=<dir>     enable capture, dump into <dir>  (or =1 -> cwd)
 *   RECOMP_AUDIO_DEBUG_SECS=<n>  ring depth per tap in seconds (default 30)
 *   RECOMP_AUDIO_SYNTH=sine|square|impulse|silence  drive a synthetic test source
 * When the env is unset every entry point is a cheap early-return.
 */
#ifndef RECOMP_AUDIO_DEBUG_H
#define RECOMP_AUDIO_DEBUG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read env and allocate nothing yet (rings allocate lazily on first push). Safe to
 * call repeatedly; idempotent. Returns 1 if capture is enabled. */
int    recomp_audio_debug_init(void);
int    recomp_audio_debug_enabled(void);

/* Push interleaved samples into a named tap's ring (no-op while disabled). The tap
 * is created on first sight; rate/channels are remembered for the WAV header. */
void   recomp_audio_debug_push_i16(const char *tap, const int16_t *interleaved,
                                   int frames, double rate, int channels);
void   recomp_audio_debug_push_f32(const char *tap, const float *interleaved,
                                   int frames, double rate, int channels);

/* Record a structured event line (printf-style metadata) into the event ring.
 * Use for register writes, AI/RSP task boundaries, drc ratio/fill, fades, etc. */
void   recomp_audio_debug_eventf(const char *type, const char *fmt, ...);

/* Monotonic host time in milliseconds since init (for event timestamps). */
double recomp_audio_debug_now_ms(void);

/* Dump the trailing `seconds` of every tap to `dir` (NULL => env dir):
 *   <tap>.wav (int16), events.csv, summary.txt
 * Returns 0 on success, non-zero if disabled or on I/O failure. */
int    recomp_audio_debug_dump(double seconds, const char *dir);

/* Synthetic source modes. */
enum { RAD_SYNTH_OFF = -1, RAD_SYNTH_SILENCE = 0, RAD_SYNTH_SINE = 1,
       RAD_SYNTH_SQUARE = 2, RAD_SYNTH_IMPULSE = 3 };

/* Parse RECOMP_AUDIO_SYNTH into a mode (RAD_SYNTH_*), RAD_SYNTH_OFF if unset. */
int    recomp_audio_synth_mode(void);

/* Fill `out` with a clean test signal. *sample_pos must persist across calls
 * (monotonic frame counter; init to 0). 440 Hz tone / once-per-second impulse. */
void   recomp_audio_synth_fill(int mode, int16_t *out, int frames, int channels,
                               double rate, uint64_t *sample_pos);

#ifdef __cplusplus
}
#endif

/* ========================================================================== */
#ifdef RECOMP_AUDIO_DEBUG_IMPL

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#ifndef RAD_PI
#define RAD_PI 3.14159265358979323846
#endif

#ifndef RAD_MAX_TAPS
#define RAD_MAX_TAPS 32
#endif
#ifndef RAD_MAX_EVENTS
#define RAD_MAX_EVENTS (1u << 18)   /* 262144 event ring slots */
#endif
#ifndef RAD_EVENT_LEN
#define RAD_EVENT_LEN 160
#endif

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
  static CRITICAL_SECTION rad__cs;
  static int rad__cs_ready = 0;
  #define RAD_LOCK_INIT() do { if(!rad__cs_ready){ InitializeCriticalSection(&rad__cs); rad__cs_ready=1; } } while(0)
  #define RAD_LOCK()      EnterCriticalSection(&rad__cs)
  #define RAD_UNLOCK()    LeaveCriticalSection(&rad__cs)
#else
  #include <pthread.h>
  #include <time.h>
  static pthread_mutex_t rad__mx = PTHREAD_MUTEX_INITIALIZER;
  #define RAD_LOCK_INIT() ((void)0)
  #define RAD_LOCK()      pthread_mutex_lock(&rad__mx)
  #define RAD_UNLOCK()    pthread_mutex_unlock(&rad__mx)
#endif

typedef struct {
    char     name[48];
    float   *buf;          /* cap_frames * channels, interleaved float [-1,1]   */
    int64_t  cap_frames;
    int      channels;
    double   rate;
    uint64_t written;      /* total frames ever written (monotonic)             */
    /* coarse always-on stats */
    uint64_t clip_count;   /* |sample| >= ~1.0                                   */
    uint64_t zero_run_max; /* longest run of exact-zero frames                   */
    uint64_t zero_run_cur;
    uint64_t repeat_frames;/* frames whose ch0 == previous ch0 exactly          */
    float    last0;
    int      have_last;
} rad_tap;

typedef struct {
    double t_ms;
    char   line[RAD_EVENT_LEN];
} rad_event;

static int        rad__enabled = -1;        /* -1 = uninitialized                */
static char       rad__dir[512];
static double     rad__secs = 30.0;
static rad_tap    rad__taps[RAD_MAX_TAPS];
static int        rad__ntaps = 0;
static rad_event *rad__events = NULL;
static uint64_t   rad__ev_written = 0;
static double     rad__t0 = 0.0;            /* clock origin                      */

/* ----- monotonic clock ----- */
static double rad__clock_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

double recomp_audio_debug_now_ms(void) { return rad__clock_ms() - rad__t0; }

int recomp_audio_debug_init(void) {
    if (rad__enabled >= 0) return rad__enabled;
    RAD_LOCK_INIT();
    const char *e = getenv("RECOMP_AUDIO_DEBUG");
    if (!e || !*e || (e[0] == '0' && e[1] == '\0')) { rad__enabled = 0; return 0; }
    if (e[0] == '1' && e[1] == '\0') strcpy(rad__dir, ".");
    else { strncpy(rad__dir, e, sizeof(rad__dir) - 1); rad__dir[sizeof(rad__dir)-1] = 0; }
    const char *s = getenv("RECOMP_AUDIO_DEBUG_SECS");
    if (s && *s) { double v = atof(s); if (v >= 1.0 && v <= 600.0) rad__secs = v; }
    rad__events = (rad_event *)calloc(RAD_MAX_EVENTS, sizeof(rad_event));
    rad__t0 = rad__clock_ms();
    rad__enabled = rad__events ? 1 : 0;
    return rad__enabled;
}

int recomp_audio_debug_enabled(void) {
    if (rad__enabled < 0) recomp_audio_debug_init();
    return rad__enabled;
}

static rad_tap *rad__find_or_make(const char *name, double rate, int channels) {
    for (int i = 0; i < rad__ntaps; ++i)
        if (strcmp(rad__taps[i].name, name) == 0) return &rad__taps[i];
    if (rad__ntaps >= RAD_MAX_TAPS) return NULL;
    rad_tap *t = &rad__taps[rad__ntaps];
    memset(t, 0, sizeof(*t));
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->rate = rate > 0 ? rate : 48000.0;
    t->channels = (channels == 2) ? 2 : 1;
    t->cap_frames = (int64_t)(t->rate * rad__secs) + 4;
    t->buf = (float *)calloc((size_t)t->cap_frames * t->channels, sizeof(float));
    if (!t->buf) return NULL;
    rad__ntaps++;
    return t;
}

static void rad__write(rad_tap *t, const float *fr_ch0, const float *interleaved,
                       int frames, int src_channels) {
    int ch = t->channels;
    for (int f = 0; f < frames; ++f) {
        int64_t w = (int64_t)(t->written % (uint64_t)t->cap_frames);
        float *slot = t->buf + w * ch;
        float v0;
        if (interleaved) {
            for (int c = 0; c < ch; ++c) {
                float v = interleaved[f * src_channels + (c < src_channels ? c : 0)];
                slot[c] = v;
            }
            v0 = slot[0];
        } else {
            v0 = fr_ch0[f];
            for (int c = 0; c < ch; ++c) slot[c] = v0;
        }
        /* coarse stats */
        if (v0 >= 0.99997f || v0 <= -0.99997f) t->clip_count++;
        if (v0 == 0.0f) { t->zero_run_cur++; if (t->zero_run_cur > t->zero_run_max) t->zero_run_max = t->zero_run_cur; }
        else t->zero_run_cur = 0;
        if (t->have_last && v0 == t->last0) t->repeat_frames++;
        t->last0 = v0; t->have_last = 1;
        t->written++;
    }
}

void recomp_audio_debug_push_i16(const char *tap, const int16_t *in, int frames,
                                 double rate, int channels) {
    if (!recomp_audio_debug_enabled() || !in || frames <= 0) return;
    int sc = (channels == 2) ? 2 : 1;
    RAD_LOCK();
    rad_tap *t = rad__find_or_make(tap, rate, channels);
    if (t) {
        /* convert to a small stack scratch in chunks to reuse the float path */
        enum { CH = 1024 };
        float scratch[CH * 2];
        int done = 0;
        while (done < frames) {
            int n = frames - done; if (n > CH) n = CH;
            for (int f = 0; f < n; ++f)
                for (int c = 0; c < sc; ++c)
                    scratch[f * sc + c] = (float)in[(done + f) * sc + c] * (1.0f / 32768.0f);
            rad__write(t, NULL, scratch, n, sc);
            done += n;
        }
    }
    RAD_UNLOCK();
}

void recomp_audio_debug_push_f32(const char *tap, const float *in, int frames,
                                 double rate, int channels) {
    if (!recomp_audio_debug_enabled() || !in || frames <= 0) return;
    int sc = (channels == 2) ? 2 : 1;
    RAD_LOCK();
    rad_tap *t = rad__find_or_make(tap, rate, channels);
    if (t) rad__write(t, NULL, in, frames, sc);
    RAD_UNLOCK();
}

void recomp_audio_debug_eventf(const char *type, const char *fmt, ...) {
    if (!recomp_audio_debug_enabled()) return;
    RAD_LOCK();
    rad_event *ev = &rad__events[rad__ev_written % RAD_MAX_EVENTS];
    ev->t_ms = recomp_audio_debug_now_ms();
    char meta[RAD_EVENT_LEN];
    va_list ap; va_start(ap, fmt);
    vsnprintf(meta, sizeof(meta), fmt ? fmt : "", ap);
    va_end(ap);
    snprintf(ev->line, sizeof(ev->line), "%s,%s", type ? type : "", meta);
    rad__ev_written++;
    RAD_UNLOCK();
}

/* ----- WAV writer (16-bit PCM) ----- */
static void rad__w32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v>>8)&255, f); fputc((v>>16)&255, f); fputc((v>>24)&255, f); }
static void rad__w16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc((v>>8)&255, f); }

static int rad__dump_tap_wav(const rad_tap *t, double seconds, const char *dir) {
    int64_t want = (int64_t)(t->rate * seconds);
    int64_t have = (int64_t)(t->written < (uint64_t)t->cap_frames ? t->written : (uint64_t)t->cap_frames);
    if (want > have) want = have;
    if (want <= 0) return 0;
    int ch = t->channels;
    int64_t start = (int64_t)(t->written - (uint64_t)want); /* first frame to emit */

    char path[640];
    snprintf(path, sizeof(path), "%s/%s.wav", dir, t->name);
    FILE *f = fopen(path, "wb");
    if (!f) return 1;

    uint32_t data_bytes = (uint32_t)(want * ch * 2);
    uint32_t rate = (uint32_t)(t->rate + 0.5);
    fwrite("RIFF", 1, 4, f); rad__w32(f, 36 + data_bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); rad__w32(f, 16); rad__w16(f, 1); rad__w16(f, (uint16_t)ch);
    rad__w32(f, rate); rad__w32(f, rate * ch * 2); rad__w16(f, (uint16_t)(ch * 2)); rad__w16(f, 16);
    fwrite("data", 1, 4, f); rad__w32(f, data_bytes);

    for (int64_t i = 0; i < want; ++i) {
        int64_t fr = start + i;
        int64_t r  = (int64_t)((uint64_t)fr % (uint64_t)t->cap_frames);
        const float *slot = t->buf + r * ch;
        for (int c = 0; c < ch; ++c) {
            double v = (double)slot[c] * 32768.0;
            if (v >  32767.0) v =  32767.0;
            if (v < -32768.0) v = -32768.0;
            int16_t s = (int16_t)(v >= 0 ? v + 0.5 : v - 0.5);
            rad__w16(f, (uint16_t)s);
        }
    }
    fclose(f);
    return 0;
}

int recomp_audio_debug_dump(double seconds, const char *dir) {
    if (!recomp_audio_debug_enabled()) return 1;
    if (seconds <= 0) seconds = rad__secs;
    const char *d = (dir && *dir) ? dir : rad__dir;
    RAD_LOCK();

    /* WAV stems */
    for (int i = 0; i < rad__ntaps; ++i) rad__dump_tap_wav(&rad__taps[i], seconds, d);

    /* events.csv (trailing window) */
    char path[640];
    snprintf(path, sizeof(path), "%s/events.csv", d);
    FILE *fe = fopen(path, "wb");
    if (fe) {
        fprintf(fe, "t_ms,type,metadata\n");
        double now = recomp_audio_debug_now_ms();
        uint64_t total = rad__ev_written;
        uint64_t first = (total > RAD_MAX_EVENTS) ? (total - RAD_MAX_EVENTS) : 0;
        for (uint64_t k = first; k < total; ++k) {
            rad_event *ev = &rad__events[k % RAD_MAX_EVENTS];
            if (now - ev->t_ms <= seconds * 1000.0)
                fprintf(fe, "%.3f,%s\n", ev->t_ms, ev->line);
        }
        fclose(fe);
    }

    /* summary.txt */
    snprintf(path, sizeof(path), "%s/summary.txt", d);
    FILE *fs = fopen(path, "wb");
    if (fs) {
        fprintf(fs, "recomp_audio_debug dump  window=%.1fs\n", seconds);
        fprintf(fs, "%-22s %8s %3s %12s %10s %10s %12s\n",
                "tap", "rate", "ch", "written", "clip", "zrun_max", "repeat");
        for (int i = 0; i < rad__ntaps; ++i) {
            rad_tap *t = &rad__taps[i];
            fprintf(fs, "%-22s %8.0f %3d %12llu %10llu %10llu %12llu\n",
                    t->name, t->rate, t->channels,
                    (unsigned long long)t->written, (unsigned long long)t->clip_count,
                    (unsigned long long)t->zero_run_max, (unsigned long long)t->repeat_frames);
        }
        fprintf(fs, "events_total=%llu\n", (unsigned long long)rad__ev_written);
        fclose(fs);
    }
    RAD_UNLOCK();
    return 0;
}

/* ----- synthetic source ----- */
int recomp_audio_synth_mode(void) {
    const char *e = getenv("RECOMP_AUDIO_SYNTH");
    if (!e || !*e) return RAD_SYNTH_OFF;
    if (!strcmp(e, "silence")) return RAD_SYNTH_SILENCE;
    if (!strcmp(e, "sine"))    return RAD_SYNTH_SINE;
    if (!strcmp(e, "square"))  return RAD_SYNTH_SQUARE;
    if (!strcmp(e, "impulse")) return RAD_SYNTH_IMPULSE;
    return RAD_SYNTH_OFF;
}

void recomp_audio_synth_fill(int mode, int16_t *out, int frames, int channels,
                             double rate, uint64_t *sample_pos) {
    int ch = (channels == 2) ? 2 : 1;
    uint64_t pos = sample_pos ? *sample_pos : 0;
    const double freq = 440.0;
    const double amp  = 0.25 * 32767.0;   /* -12 dBFS, comfortable for ear tests */
    for (int f = 0; f < frames; ++f, ++pos) {
        double v = 0.0;
        switch (mode) {
            case RAD_SYNTH_SINE:   v = amp * sin(2.0 * RAD_PI * freq * (double)pos / rate); break;
            case RAD_SYNTH_SQUARE: v = (sin(2.0 * RAD_PI * freq * (double)pos / rate) >= 0.0 ? amp : -amp); break;
            case RAD_SYNTH_IMPULSE: v = ((pos % (uint64_t)(rate + 0.5)) == 0) ? (0.9 * 32767.0) : 0.0; break;
            case RAD_SYNTH_SILENCE:
            default: v = 0.0; break;
        }
        int16_t s = (int16_t)(v >= 0 ? v + 0.5 : v - 0.5);
        for (int c = 0; c < ch; ++c) out[f * ch + c] = s;
    }
    if (sample_pos) *sample_pos = pos;
}

#endif /* RECOMP_AUDIO_DEBUG_IMPL */
#endif /* RECOMP_AUDIO_DEBUG_H */
