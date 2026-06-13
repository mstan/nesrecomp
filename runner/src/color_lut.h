// color_lut.h — present-time NES palette re-map (palette-index -> RGB swap).
//
// PRESENT-TIME ONLY. This never touches the emulation, the PPU, or the
// differential-verify path — frame hashes / smoke CRCs / oracle comparisons
// stay defined on the raw ARGB8888 framebuffer the PPU renders through the
// canon `g_nes_palette[64]`. The re-map is applied to a COPY of the frame at
// SDL-upload time, and it defaults to Raw (exact passthrough), so default
// behavior and every hashed/verified frame are byte-identical unless an
// alternate NES palette is opted in via NESRECOMP_PALETTE={raw,2c02,...}.
//
// ── Why this model and not a panel/CRT LUT ─────────────────────────
// NES video is NOT an RGB framebuffer at the hardware boundary: the PPU emits
// a ~6-bit color index decoded as an NTSC composite signal. There is no
// physical RGB panel to colorimetrically model (unlike GBA's LCD). The runner
// already collapses that composite signal into a single fixed "system palette"
// (`g_nes_palette[64]`, an RGB approximation of 2C02 output). The right,
// tractable enhancement here is therefore a PALETTE swap: replace the runner's
// baked-in palette RGB with a different *measured/derived* NES palette, leaving
// pixel-for-pixel index assignment untouched. A full NTSC-artifact decoder
// (composite emphasis, color bleed, dot crawl) is a strictly bigger model and
// is documented (NOT implemented) in docs/SHADOW_ENHANCEMENTS.md.
//
// Because the framebuffer the PPU produces is ALREADY palette-resolved ARGB,
// the swap works by recognizing each canon palette ARGB value and substituting
// the alternate palette's ARGB for the same index. Pixels that are not canon
// palette colors (debug overlays, widescreen margin fills, crosshair) are not
// in the recognition table and pass through unchanged.
//
// ── Attribution ───────────────────────────────────────────────────
// The present-path swap structure mirrors the gbarecomp color_lut.{h,cpp}
// (which ports JRickey/gba-recomp screen color science, © Jrickey, MIT OR
// Apache-2.0). The alternate NES palette tables are public-domain community
// measurements (see comments in color_lut.c). The NES-specific palette-index
// model is ours.

#ifndef NESRECOMP_COLOR_LUT_H
#define NESRECOMP_COLOR_LUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The canon system palette the PPU renders through (defined in ppu_renderer.c).
extern const uint32_t g_nes_palette[64];

// Which palette to present. Raw = the runner's baked-in g_nes_palette
// (exact passthrough; default).
typedef enum {
  NES_PALETTE_RAW = 0,   // passthrough (g_nes_palette), byte-identical
  NES_PALETTE_2C02,      // alternate measured 2C02 RGB approximation
  NES_PALETTE_FBX,       // "FirebrandX"-style consumer-CRT measured palette
} NesPaletteKind;

// Parse a config/env token; returns false (and leaves *out) if unrecognized.
bool nes_palette_kind_from_name(const char* name, NesPaletteKind* out);

// Initialize the present-time color LUT from the environment variable
// NESRECOMP_PALETTE (default = raw). Safe to call once at startup. Reads the
// canon g_nes_palette to build the recognition map, so call after that table
// is linked (it has static storage, so always available).
void color_lut_init_from_env(void);

// Explicitly select a palette (overrides env). Rebuilds the LUT.
void color_lut_set(NesPaletteKind kind);

// True if the active LUT is exact passthrough (Raw). When true, callers SHOULD
// skip color_lut_apply entirely and present the raw framebuffer, guaranteeing
// byte-identical output.
bool color_lut_is_passthrough(void);

// Map a width*height ARGB8888 frame `src` into `dst` (which must hold
// width*height uint32_t). For each pixel: if its ARGB matches a canon palette
// entry, substitute the active palette's ARGB for that index; otherwise copy
// unchanged. No-op-equivalent (still copies) when passthrough — callers should
// gate on color_lut_is_passthrough() to truly skip.
void color_lut_apply(const uint32_t* src, uint32_t* dst, int width, int height);

#ifdef __cplusplus
}
#endif

#endif  // NESRECOMP_COLOR_LUT_H
