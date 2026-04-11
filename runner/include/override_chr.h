/*
 * override_chr.h — CHR (sprite/tile) override and dump system.
 *
 * Game-agnostic: works with any mapper that uses the mapper_chr_callback hook.
 * Provides:
 *   1. CHR dump — write g_chr_ram snapshots to chr_dump/ on each bank switch
 *   2. CHR override — load pre-encoded binary overrides from chr/manifest.json
 *   3. Hot reload — monitor manifest + referenced files for changes
 *
 * Usage from game extras.c:
 *   game_on_init():
 *     chr_override_init();
 *     chr_override_set_dump(1);              // enable dump mode
 *     chr_override_load_manifest("chr");     // load overrides (optional)
 *
 *   game_on_frame():
 *     chr_override_reload_if_changed();      // hot reload polling
 */
#pragma once
#include <stdint.h>

/* Returns 1 if the CHR override system is active (init was called).
 * Runtime.c checks this to avoid calling hooks when inactive. */
int chr_override_active(void);

/* Initialize the CHR override system.  Registers the mapper callback.
 * Call once from game_on_init() after mapper_init_chr(). */
void chr_override_init(void);

/* Enable/disable CHR dump mode.  When enabled, each unique CHR bank
 * configuration is written to chr_dump/snapshot_NNNN.bin (8KB raw).
 * A summary CSV (chr_dump/index.csv) maps snapshot number to frame
 * and bank register state.  Dumps are deduplicated by content hash. */
void chr_override_set_dump(int enable);

/* Load CHR override manifest from the given directory.
 * Looks for <dir>/manifest.json.  Overrides are pre-encoded .bin files.
 * Returns number of overrides loaded, or -1 on error. */
int chr_override_load_manifest(const char *dir);

/* Poll for manifest/override file changes (~1s interval).
 * Call from game_on_frame(). */
void chr_override_reload_if_changed(void);

/* Batch-compile all PNGs in dir to .chr.bin cache files.
 * Standalone operation — does not require the game to be running.
 * Returns count of PNGs compiled, or -1 on error. */
int chr_override_compile_dir(const char *dir);

/* ── CHR RAM transfer tracking (for CHR RAM games) ────────────────────────
 * These are called from runtime.c at the $2006/$2007 write points.
 * They track individual DMA-style transfers into CHR RAM and capture
 * each as a discrete asset (e.g. "David's sprites", "town tileset").
 *
 * A transfer starts when $2006 sets an address in $0000-$1FFF.
 * It ends when $2006 sets a new address, or a frame boundary is hit.
 * Each unique transfer (same dest + same content) is one asset. */

/* Called when $2006 pair completes (PPU address is set).
 * new_addr = the full 14-bit PPU address just written. */
void chr_override_on_ppuaddr(uint16_t new_addr);

/* Called on each $2007 write to CHR RAM ($0000-$1FFF).
 * addr = mapped CHR address, val = byte written. */
void chr_override_on_chr_write(uint16_t addr, uint8_t val);

/* Called at frame boundary (end of VBlank / start of rendering).
 * Flushes any in-progress transfer and applies overrides. */
void chr_override_frame_end(void);

/* Get dump statistics. */
void chr_override_get_dump_stats(int *unique_snapshots, int *total_switches);
