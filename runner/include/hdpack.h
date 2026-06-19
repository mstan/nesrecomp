/*
 * hdpack.h — Mesen HD Pack support (high-res tile/sprite replacement).
 *
 * A pack is a folder containing `hires.txt` (manifest) + PNG sheets. The
 * runner loads it, indexes the replacement tiles by Mesen's HdTileKey, and
 * renders matched tiles at the pack's scale factor.
 *
 * Integration model (see runner/HDPACK.md):
 *   - ppu_renderer.c records the visible tile per pixel into the side channel
 *     (hdpack_pixels()) while doing the normal native render, gated by
 *     hdpack_recording().
 *   - main_runner.c calls hdpack_upscale() to turn the native framebuffer +
 *     side channel into a width*scale x 240*scale HD framebuffer, presented
 *     through an HD-sized SDL texture.
 *
 * All matching/parsing/rendering lives in hdpack.c; the renderer change is a
 * thin record-on-write. Byte-compatible with Mesen2 Core/NES/HdPacks.
 */
#pragma once
#include <stdint.h>

/* Per-pixel record of the visible BG + topmost sprite tile, filled by the native
 * render pass. Two layers so the upscaler can composite HD sprites over the HD
 * background (Mesen's model) instead of over the original art. */
typedef struct {
    /* Background layer (recorded for every rendered BG pixel). */
    int32_t        bg_index;        /* CHR-ROM absolute tile index; <0 none */
    const uint8_t *bg_t16;          /* -> 16 CHR bytes (CHR-RAM matching) */
    uint8_t        bg_p0, bg_p1, bg_p2, bg_p3;  /* backdrop + colors 1..3 (6-bit) */
    uint8_t        bg_ox, bg_oy;    /* within-tile pixel offset */
    uint8_t        bg_has;
    uint32_t       bg_argb;         /* original BG color (fallback when unmatched) */
    uint32_t       backdrop;        /* universal bg color (transparent HD BG -> this) */

    /* Topmost sprite layer (recorded where a sprite pixel was drawn). */
    int32_t        sp_index;
    const uint8_t *sp_t16;
    uint8_t        sp_p1, sp_p2, sp_p3;
    uint8_t        sp_ox, sp_oy, sp_hm, sp_vm;
    uint8_t        sp_has;
    uint32_t       sp_argb;         /* original sprite color (fallback when unmatched) */
} HdPixel;

/* Load the pack in `dir` (expects dir/hires.txt). native_w is the runner's
 * framebuffer width (g_render_width), used to size the per-pixel side channel.
 * is_chr_ram_game selects the CHR-RAM (content) vs CHR-ROM (index) match mode.
 * Returns 0 on success (pack active), <0 on error (pack stays inactive). */
int  hdpack_load(const char *dir, int is_chr_ram_game, int native_w);

/* Convenience: resolve the pack directory from the NESRECOMP_HDPACK env var or
 * g_nes_config (hdpack_enabled + hdpack_dir), then hdpack_load(). No-op if no
 * pack is configured. Returns 0 if a pack was loaded, <0 otherwise. */
int  hdpack_load_from_config(int is_chr_ram_game, int native_w);

void hdpack_unload(void);

int  hdpack_active(void);    /* 1 if a pack is loaded */
int  hdpack_scale(void);     /* upscale factor (1 when inactive) */

/* Side channel the renderer writes into (NULL when inactive). Indexed
 * [y*native_w + x]. */
HdPixel *hdpack_pixels(void);
int      hdpack_recording(void);   /* 1 == renderer should fill hdpack_pixels() */

/* Clear the side channel's per-pixel `has` flags for a new frame. Called by the
 * renderer at the start of an actually-rendered frame. */
void hdpack_frame_begin(void);

/* Produce the HD framebuffer from the native one + side channel.
 * native_fb is native_w x 240 ARGB8888; hd_buf is (native_w*scale) x
 * (240*scale) ARGB8888 and must be caller-allocated. */
void hdpack_upscale(const uint32_t *native_fb, int native_w, uint32_t *hd_buf);
