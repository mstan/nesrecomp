/*
 * nes_rb_state.h -- the rollback snapshot and its digest.
 *
 * ONE domain, three consumers: the rollback ring (nes_netplay_rb.c), the
 * per-tick digest every peer publishes, and the determinism probe
 * (nes_rb_probe.c). The domain is the V7 save-state image (savestate.c --
 * CPU, work RAM, SRAM, CHR, OAM, palette, nametables, PPU registers and
 * latches, mapper, the runtime timing blob, the APU blob, controller ports,
 * render/zapper sidecars, frame count, the interrupted guest continuation and
 * every registered mod record) plus a rollback trailer (the logical input
 * seats past the two physical ports). The digest is computed over the SAME
 * serialized bytes, so "the digest covers exactly the snapshot" holds by
 * construction rather than by a list someone has to keep in step.
 *
 * A snapshot is taken at the top of the outermost frame callback, before the
 * NMI handler runs: the state BEFORE the tick that callback starts. Loading it
 * there and letting the callback continue replays that tick; the stale native
 * call stack is discarded at the callback's end (main_runner.c,
 * finish_frame_callback -> run_guest_execution). See docs/NETPLAY.md.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NesRbDigest {
    uint32_t master;   /* the whole image */
    uint32_t part[3];  /* cpu_wram, ppu, apu_io_mods (nes_rb_part_name) */
} NesRbDigest;

const char *nes_rb_part_name(int i);

/* Serialize the snapshot domain. Returns bytes written, 0 on failure; *need
 * (optional) receives the exact size, also when it did not fit. */
size_t nes_rb_state_save(uint8_t *buf, size_t cap, size_t *need);
/* Restore a snapshot. Validates before mutating. Requests the guest-resume
 * continuation (main_runner discards the stale host stack when the current
 * callback ends). Returns 1 on success. */
int nes_rb_state_load(const uint8_t *buf, size_t len);

/* The current machine's image, serialized once and cached until
 * nes_rb_state_invalidate() (the runner calls it whenever guest code is about
 * to run, and every load calls it). NULL on failure. */
const uint8_t *nes_rb_state_image(size_t *len);
void nes_rb_state_invalidate(void);

/* Digest of the current image (cached with it). */
void nes_rb_state_digest(NesRbDigest *out);
uint32_t nes_rb_state_digest_master(void);
/* Digest of an arbitrary image, same partitioning. */
void nes_rb_digest_image(const uint8_t *img, size_t len, NesRbDigest *out);

/* First differing byte of two images and its field name; -1 when equal. */
long nes_rb_state_first_diff(const uint8_t *a, size_t alen,
                             const uint8_t *b, size_t blen,
                             char *what, size_t what_cap);

/* Field name of one byte offset of an image. */
void nes_rb_state_describe(const uint8_t *img, size_t len, size_t off,
                           char *what, size_t cap);

/* Measured cost (microseconds), for the docs/NETPLAY.md evidence. */
typedef struct NesRbStateTiming {
    uint64_t saves, loads, digests;
    double   save_us_p50, save_us_p99, load_us_p50, load_us_p99;
    double   digest_us_p50, digest_us_p99;
    size_t   image_bytes;
} NesRbStateTiming;
void nes_rb_state_timing(NesRbStateTiming *out);
/* One line "RB_STATE_TIMING ..." to stderr. */
void nes_rb_state_log_timing(const char *tag);

#ifdef __cplusplus
}
#endif
