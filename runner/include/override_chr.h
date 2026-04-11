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

/* Get dump statistics. */
void chr_override_get_dump_stats(int *unique_snapshots, int *total_switches);
