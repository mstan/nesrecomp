/* cyc_fds_save.inc - Famicom Disk System disk saves for cyc_host.c.
 *
 * What a game writes to its disk goes to a save file beside nothing else: the
 * image is never written. The file (common/nes_fds_save.h) holds the whole
 * drive stream of each side that differs from the one the loader builds from
 * the image, and the image's identity; a save for another disk, or a damaged
 * one, is refused at start (the host exits rather than overwrite it).
 *
 * Where: --save-file FILE, as for cartridge NVRAM. Headless runs persist only
 * with it, so unattended comparisons stay reproducible. A windowed run without
 * it saves to <executable dir>/saves/<image stem>.fdssave (the player
 * runtime's saves/ convention, runner/src/save_ram.c); --no-save turns that off.
 *
 * When: writes land in the in-memory disk as the drive clocks them. The file
 * is rewritten (atomically: a flushed temporary file replaces it, cyc_save.inc)
 * once the disk has changed and the drive has then been idle (motor off) for
 * FDS_SAVE_IDLE_FRAMES, which is right after the BIOS finishes a save; at the
 * latest FDS_SAVE_MAX_FRAMES after the first unsaved change; when the disk is
 * ejected; and at exit. Emulated frames, not wall time, so a run's saves are
 * the same at any speed. Every save and load is a ring event (fds.save,
 * fds.load).
 *
 * Mesen: --fds-import-ips FILE applies a Mesen/nesref disk save (<stem>.ips,
 * an IPS patch of the image file) as the starting disk; --fds-export-ips FILE
 * writes one of the current disk with each save and at exit. */
#include "../../common/nes_fds_save.h"
#ifdef _WIN32
#include <direct.h>
#define fds_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define fds_mkdir(p) mkdir((p), 0755)
#endif

enum { FDS_SAVE_IDLE_FRAMES = 60, FDS_SAVE_MAX_FRAMES = 3600 };

static struct {
    const char *path;            /* the disk save file, or NULL */
    char        path_buf[1024];
    const char *export_ips;
    const uint8_t *image;        /* the image file as read (never written) */
    size_t      image_size;
    NesFdsImage img;
    NesFdsIdentity id;
    CycFdsOptions opt;
    uint64_t    saved_gen;       /* cyc_fds_disk_generation() at the last save or load */
    long        frame, dirty_since, idle;
    unsigned    load_source, load_sides;
    uint32_t    load_bytes;
    bool        loaded;
    unsigned    saves, failures;
    char        status[96];
} fds_save = { .dirty_since = -1 };

static uint8_t *fds_read_whole(const char *path, size_t *size, bool *missing)
{
    *missing = false;
    FILE *f = fopen(path, "rb");
    if (!f) { *missing = errno == ENOENT; return NULL; }
    uint8_t *buf = NULL;
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (n >= 0 && fseek(f, 0, SEEK_SET) == 0 && (buf = (uint8_t *)malloc(n ? (size_t)n : 1)) != NULL &&
        fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = n < 0 ? 0 : (size_t)n;
    return buf;
}

/* The host's save directory is beside the executable, independent of cwd. */
static bool default_save_path(const char *image_path, const char *extension, char *out, size_t cap)
{
    char dir[900] = ".";
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof(dir));
    if (n == 0 || n >= sizeof(dir)) strcpy(dir, ".");
#else
    ssize_t n = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
    if (n > 0) dir[n] = 0; else strcpy(dir, ".");
#endif
    char *cut = strrchr(dir, '/'), *bcut = strrchr(dir, '\\');
    if (bcut > cut) cut = bcut;
    if (cut) *cut = 0;
    const char *base = image_path, *s1 = strrchr(image_path, '/'), *s2 = strrchr(image_path, '\\');
    if (s1 && s1 + 1 > base) base = s1 + 1;
    if (s2 && s2 + 1 > base) base = s2 + 1;
    const char *dot = strrchr(base, '.');
    int stem = (int)(dot ? (size_t)(dot - base) : strlen(base));
    char saves[1000];
    snprintf(saves, sizeof(saves), "%s/saves", dir);
    if (fds_mkdir(saves) != 0 && errno != EEXIST) return false;
    int written = snprintf(out, cap, "%s/%.*s.%s", saves, stem, base, extension);
    return written >= 0 && (size_t)written < cap;
}

/* <executable dir>/saves/<image stem>.fdssave */
static const char *fds_default_save_path(const char *image_path)
{
    if (!default_save_path(image_path, "fdssave", fds_save.path_buf, sizeof(fds_save.path_buf))) return NULL;
    return fds_save.path_buf;
}

static void fds_status(const char *fmt, long frame)
{
    snprintf(fds_save.status, sizeof(fds_save.status), fmt, frame);
}

/* Mesen saves only images it can rebuild: not .qd (0.9.9 has no .qd), and
 * only a whole number of sides (CreatePatch asserts the rebuilt file is as
 * long as the image). */
static bool fds_ips_possible(void)
{
    size_t head = fds_save.img.format == NES_FDS_FWNES ? NES_FDS_HEADER_BYTES : 0;
    if (fds_save.img.format == NES_FDS_QD) {
        fprintf(stderr, "--fds-export-ips / --fds-import-ips: Mesen 0.9.9 has no .qd saves\n");
        return false;
    }
    if (head + (size_t)fds_save.img.sides * NES_FDS_SIDE_BYTES != fds_save.image_size) {
        fprintf(stderr, "--fds-export-ips / --fds-import-ips: the image is not a whole number of sides (%zu bytes)\n",
                fds_save.image_size);
        return false;
    }
    return true;
}

/* The disk as it is now, as Mesen would save it: an IPS patch of the image
 * file (nes_fds_save.h). */
static bool fds_write_ips(const char *path)
{
    size_t head = fds_save.img.format == NES_FDS_FWNES ? NES_FDS_HEADER_BYTES : 0;
    size_t n = head + (size_t)fds_save.img.sides * NES_FDS_SIDE_BYTES;
    uint8_t *rebuilt = (uint8_t *)calloc(1, n);
    if (!rebuilt) return false;
    if (head) { memcpy(rebuilt, "FDS\x1a", 4); rebuilt[4] = (uint8_t)fds_save.img.sides; }
    bool fits = true;
    for (unsigned s = 0; s < fds_save.img.sides; ++s) {
        uint32_t len;
        const uint8_t *stream = cyc_fds_side_stream(s, &len);
        fits &= nes_fds_rebuild_side(stream, len, rebuilt + head + (size_t)s * NES_FDS_SIDE_BYTES);
    }
    size_t size = nes_fds_ips_create(fds_save.image, rebuilt, n, NULL, 0);
    uint8_t *ips = (uint8_t *)malloc(size);
    bool ok = ips && nes_fds_ips_create(fds_save.image, rebuilt, n, ips, size) == size && save_atomic(path, ips, size);
    if (!fits) fprintf(stderr, "--fds-export-ips: a side's blocks exceed 65500 bytes; cut there\n");
    if (!ok) fprintf(stderr, "cannot write %s\n", path);
    free(ips);
    free(rebuilt);
    return ok;
}

/* Rewrite the save file if the disk changed since the last save or load. */
static bool fds_save_flush(unsigned reason)
{
    if (!cyc_is_fds()) return true;
    uint64_t gen = cyc_fds_disk_generation();
    if (gen == fds_save.saved_gen) return true;
    bool ok = true;
    NesFdsSave *s = (NesFdsSave *)calloc(1, sizeof(NesFdsSave));
    if (!s) return false;
    s->id = fds_save.id;
    for (unsigned i = 0; i < cyc_fds_side_count(); ++i) {
        uint32_t len, base_len;
        const uint8_t *now = cyc_fds_side_stream(i, &len), *base = cyc_fds_side_base(i, &base_len);
        if (len == base_len && !memcmp(now, base, len)) continue;
        s->sides[s->count].side = i;
        s->sides[s->count].bytes = now;
        s->sides[s->count].len = len;
        s->count++;
    }
    s->layout = fds_save.opt.profile == CYC_FDS_PROFILE_MESEN ? NES_FDS_PROFILE_MESEN099 : NES_FDS_PROFILE_MESEN2;
    s->crc_mode = fds_save.opt.stream_crc == CYC_FDS_CRC_MESEN ? NES_FDS_CRC_MESEN : NES_FDS_CRC_COMPUTED;
    s->write_at = fds_save.opt.write_at == CYC_FDS_WRITE_MESEN ? 2 : 0;
    size_t size = nes_fds_save_write(s, NULL, 0);
    if (fds_save.path) {
        uint8_t *buf = (uint8_t *)malloc(size);
        ok = buf && nes_fds_save_write(s, buf, size) == size && save_atomic(fds_save.path, buf, size);
        free(buf);
        cyc_ring_push_len(CYC_EV_FDS_SAVE, (uint16_t)(reason << 1 | (ok ? 1 : 0)), s->count, (uint32_t)size);
        if (ok) {
            fds_save.saves++;
            printf("[cyc disk] f=%ld saved %u changed side%s to %s\n", fds_save.frame, s->count,
                   s->count == 1 ? "" : "s", fds_save.path);
            fds_status("SAVED F%ld", fds_save.frame);
        } else {
            fds_save.failures++;
            fprintf(stderr, "cannot write disk save %s; previous file retained\n", fds_save.path);
            fds_status("SAVE FAILED F%ld", fds_save.frame);
        }
    }
    if (fds_save.export_ips && !fds_write_ips(fds_save.export_ips)) ok = false;
    free(s);
    if (ok) {
        fds_save.saved_gen = gen;
        fds_save.dirty_since = -1;
    } else {
        /* try again after another idle period, not every frame */
        fds_save.dirty_since = fds_save.frame;
        fds_save.idle = 0;
    }
    return ok;
}

/* Load the disk save (and a Mesen import) before power-on. False: refuse to
 * run, leaving every file as it is. */
static bool fds_save_open(const char *path, const char *import_ips, const char *export_ips, const uint8_t *image,
                          size_t image_size, const CycFdsOptions *opt)
{
    fds_save.path = path;
    fds_save.opt = *opt;
    fds_save.export_ips = export_ips;
    fds_save.image = image;
    fds_save.image_size = image_size;
    nes_fds_image(image, image_size, opt->qd ? NES_FDS_QD : NES_FDS_NONE, &fds_save.img);
    nes_fds_identity(&fds_save.img, &fds_save.id);
    if ((import_ips || export_ips) && !fds_ips_possible()) return false;
    snprintf(fds_save.status, sizeof(fds_save.status), path ? "NO SAVE YET" : "SAVING OFF");
    if (path) {
        size_t n = 0;
        bool missing;
        uint8_t *buf = fds_read_whole(path, &n, &missing);
        if (!buf && !missing) { fprintf(stderr, "cannot read disk save %s: %s\n", path, strerror(errno)); return false; }
        if (buf) {
            NesFdsSave *s = (NesFdsSave *)calloc(1, sizeof(NesFdsSave));
            NesFdsSaveStatus st = s ? nes_fds_save_parse(buf, n, &fds_save.id, s) : NES_FDS_SAVE_CORRUPT;
            if (st != NES_FDS_SAVE_OK) {
                fprintf(stderr, "disk save %s is %s; not applied, left as it is (move it away to start from the image)\n",
                        path, nes_fds_save_status_name(st));
                free(s); free(buf);
                return false;
            }
            for (unsigned i = 0; i < s->count; ++i)
                if (!cyc_fds_set_side_stream(s->sides[i].side, s->sides[i].bytes, s->sides[i].len)) {
                    fprintf(stderr, "disk save %s: side %u could not be put back\n", path, s->sides[i].side);
                    free(s); free(buf);
                    return false;
                }
            printf("fds: disk save %s: %u changed side%s put back\n", path, s->count, s->count == 1 ? "" : "s");
            fds_save.loaded = true;
            fds_save.load_source = 0;
            fds_save.load_sides = s->count;
            fds_save.load_bytes = (uint32_t)n;
            snprintf(fds_save.status, sizeof(fds_save.status), "SAVE LOADED");
            free(s); free(buf);
        } else {
            printf("fds: no disk save yet at %s; writes will be saved there\n", path);
        }
    }
    fds_save.saved_gen = cyc_fds_disk_generation();
    if (import_ips) {
        size_t n = 0, out_n = 0;
        bool missing;
        uint8_t *ips = fds_read_whole(import_ips, &n, &missing), *patched = NULL;
        if (!ips) { fprintf(stderr, "cannot read %s\n", import_ips); return false; }
        bool ok = nes_fds_ips_apply(ips, n, image, image_size, NULL, 0, &out_n) &&
                  (patched = (uint8_t *)malloc(out_n ? out_n : 1)) != NULL &&
                  nes_fds_ips_apply(ips, n, image, image_size, patched, out_n, &out_n);
        NesFdsImage pimg;
        ok = ok && nes_fds_image(patched, out_n, opt->qd ? NES_FDS_QD : NES_FDS_NONE, &pimg) &&
             pimg.sides == fds_save.img.sides;
        unsigned replaced = 0;
        NesFdsProfile layout = opt->profile == CYC_FDS_PROFILE_MESEN ? NES_FDS_PROFILE_MESEN099 : NES_FDS_PROFILE_MESEN2;
        NesFdsCrc crc = opt->stream_crc == CYC_FDS_CRC_MESEN ? NES_FDS_CRC_MESEN : NES_FDS_CRC_COMPUTED;
        for (unsigned s = 0; ok && s < pimg.sides; ++s) {
            size_t len = nes_fds_side_stream(&pimg, s, layout, crc, NULL, 0);
            uint8_t *stream = (uint8_t *)malloc(len);
            uint32_t cur_len;
            const uint8_t *cur = cyc_fds_side_stream(s, &cur_len);
            if (!stream || nes_fds_side_stream(&pimg, s, layout, crc, stream, len) != len) ok = false;
            else if (len != cur_len || memcmp(stream, cur, len)) {
                ok = cyc_fds_set_side_stream(s, stream, (uint32_t)len);
                replaced++;
            }
            free(stream);
        }
        free(ips);
        free(patched);
        if (!ok) { fprintf(stderr, "%s is not a Mesen disk save for this image\n", import_ips); return false; }
        printf("fds: Mesen save %s: %u side%s from it%s\n", import_ips, replaced, replaced == 1 ? "" : "s",
               path ? " (written to the disk save at the next save)" : "");
        fds_save.loaded = true;
        fds_save.load_source = 1;
        fds_save.load_sides = replaced;
        fds_save.load_bytes = (uint32_t)n;
        snprintf(fds_save.status, sizeof(fds_save.status), "MESEN SAVE LOADED");
    }
    return true;
}

/* The ring starts at power-on: record the load there. */
static void fds_save_powered_on(void)
{
    if (fds_save.loaded)
        cyc_ring_push_len(CYC_EV_FDS_LOAD, (uint16_t)fds_save.load_source, fds_save.load_sides, fds_save.load_bytes);
}

/* After each frame: save once the drive has stopped after a change. */
static void fds_save_frame(long frames_done)
{
    fds_save.frame = frames_done;
    if (!cyc_is_fds() || cyc_fds_disk_generation() == fds_save.saved_gen) { fds_save.dirty_since = -1; return; }
    if (fds_save.dirty_since < 0) {
        fds_save.dirty_since = frames_done;
        fds_save.idle = 0;
        if (fds_save.path) fds_status("UNSAVED", 0);
    }
    fds_save.idle = cyc_fds_motor_on() ? 0 : fds_save.idle + 1;
    if (fds_save.idle >= FDS_SAVE_IDLE_FRAMES) fds_save_flush(CYC_FDS_SAVE_IDLE);
    else if (frames_done - fds_save.dirty_since >= FDS_SAVE_MAX_FRAMES) fds_save_flush(CYC_FDS_SAVE_TIMEOUT);
}

/* cyc_host.h: the windowed host calls these. */
void cyc_host_frame_done(long frames_done) { fds_save_frame(frames_done); }
void cyc_host_disk_ejected(void) { fds_save_flush(CYC_FDS_SAVE_EJECT); }
const char *cyc_host_disk_save_status(void) { return fds_save.status; }
