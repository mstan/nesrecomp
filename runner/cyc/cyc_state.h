/*
 * cyc_state.h - the machine's state as a whole: save states, and the
 * snapshot a mod's isolated routine call is undone with (cyc_mod.h).
 *
 * A save state is the complete machine at a frame boundary - CPU, CPU RAM,
 * the cartridge (registers, work RAM, CHR RAM, mapper chips), PPU (with the
 * picture it just drew and what cyc_render.h captured of it), APU, and on
 * the FDS the drive, the disk as the game has written it and the HLE tier -
 * then the host's own section (cyc_state_set_host: the frame counter, the
 * Disk action) and one record per mod that keeps state of its own
 * (runner/include/mod_savestate.h). Loading validates everything first (the
 * program, the disk image, the FDS options, every section's size, each mod's
 * validator) and changes nothing unless it can apply all of it; compiled RAM
 * views are re-validated from RAM afterwards (they start UNKNOWN, as at
 * power-on), which is correct by construction.
 *
 * A state belongs to the executable that wrote it: the file records the
 * compiled program's identity and the size of every machine structure, and a
 * load refuses any difference. A run loaded from a state at frame N and one
 * that ran through frame N from power-on are the same machine: every later
 * --hash-out line matches (tools/cyc/test_cyc_state.py).
 *
 * Audio output filters and host presentation are not machine state and are
 * not saved (VRC7's sound chip, audio only, restarts from its registers).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the host's section ---- */
typedef struct {
    /* Append the host's bytes (a flat block) to out; returns its length, or
     * the length needed when out is NULL or cap is too small. */
    size_t (*save)(uint8_t *out, size_t cap);
    /* Validate (apply == false) or apply a block save() wrote. */
    bool (*load)(const uint8_t *data, size_t len, bool apply);
} CycStateHost;
void cyc_state_set_host(const CycStateHost *host);

/* Serialize to a malloc'd buffer (*out; the caller frees it). */
bool cyc_state_save(uint8_t **out, size_t *len, char *err, size_t err_len);
/* Load a buffer cyc_state_save wrote; on false nothing changed. */
bool cyc_state_load(const uint8_t *data, size_t len, char *err, size_t err_len);
/* Files: written to a temporary name then moved over path. */
bool cyc_state_save_file(const char *path, char *err, size_t err_len);
bool cyc_state_load_file(const char *path, char *err, size_t err_len);

/* ---- the in-process snapshot (cyc_mod.c) ----
 * Everything a routine running on the machine without its clock can change:
 * CPU, CPU RAM, the cartridge and its RAM, CHR RAM, PPU, APU, the FDS
 * media's drive state and HLE tier, the sound chip, compiled RAM views'
 * validity and statistics. Restoring puts all of it back bit for bit. */
typedef struct CycSnapshot CycSnapshot;
CycSnapshot *cyc_snapshot_new(void);
void cyc_snapshot_free(CycSnapshot *s);
void cyc_snapshot_take(CycSnapshot *s);
/* Unclocked mod scopes cannot change physical cartridge RAM size. Qualified
 * boards capture all accessible RAM banks; other machines use the full copy. */
void cyc_snapshot_take_isolated(CycSnapshot *s);
void cyc_snapshot_restore(const CycSnapshot *s);
/* The CPU RAM (2 KiB) and the cartridge's RAM (the FDS's PRG RAM, or work
 * RAM; *len bytes from $6000's first byte) as the snapshot holds them. */
const uint8_t *cyc_snapshot_cpu_ram(const CycSnapshot *s);
const uint8_t *cyc_snapshot_cart_ram(const CycSnapshot *s, size_t *len);

#ifdef __cplusplus
}
#endif
