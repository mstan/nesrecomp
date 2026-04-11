/*
 * chr_codec.h — NES 2bpp CHR encode/decode (PNG <-> CHR).
 *
 * Pure conversion logic, no file I/O policy.  Used by override_chr.c
 * for runtime JIT PNG->CHR conversion with disk caching.
 */
#pragma once
#include <stdint.h>

/*
 * chr_decode_png — Load a PNG and convert to NES 2bpp CHR data.
 *
 * Reads the PNG, quantizes to 4 colors by luminance, encodes as 2bpp CHR.
 * Tiles are read left-to-right, top-to-bottom from the image (8x8 grid).
 *
 *   png_path:    path to PNG file
 *   out_data:    receives malloc'd CHR buffer (caller must free)
 *   out_size:    receives size in bytes (multiple of 16)
 *
 * Returns 0 on success, -1 on error (message printed to stderr).
 */
int chr_decode_png(const char *png_path, uint8_t **out_data, int *out_size);

/*
 * chr_load_cached — Load CHR data from PNG with .bin disk cache.
 *
 * If <png_path>.bin exists and is newer than the PNG, loads the .bin directly.
 * Otherwise decodes the PNG, writes the .bin cache, and returns the data.
 *
 * Cache file name: replaces .png extension with .chr.bin
 *   e.g. "chr/player_tiles.png" -> "chr/player_tiles.chr.bin"
 *
 *   png_path:    path to PNG file
 *   out_data:    receives malloc'd CHR buffer (caller must free)
 *   out_size:    receives size in bytes
 *
 * Returns 0 on success, -1 on error.
 */
int chr_load_cached(const char *png_path, uint8_t **out_data, int *out_size);

/*
 * chr_write_png — Convert NES 2bpp CHR data to a PNG file.
 *
 * Tiles are arranged 16 per row, grayscale palette (black/dark/light/white).
 *
 *   png_path:    output PNG path
 *   chr_data:    raw CHR data (must be multiple of 16 bytes)
 *   chr_size:    size in bytes
 *
 * Returns 0 on success, -1 on error.
 */
int chr_write_png(const char *png_path, const uint8_t *chr_data, int chr_size);
