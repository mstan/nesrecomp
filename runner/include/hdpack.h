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
 *   - hosts publish immutable presentation jobs with hdpack_prepare_jobs().
 *     GPU consumers keep their authored source pixels resident and render at
 *     output density. The shared portable consumer is hdpack_render_jobs();
 *     hdpack_upscale() remains the pack-scale CPU compatibility entry point.
 *
 * All matching/parsing/rendering lives in hdpack.c; the renderer change is a
 * thin record-on-write. Byte-compatible with Mesen2 Core/NES/HdPacks.
 */
#pragma once
#include <stdint.h>
#include "hdpack_jobs.h"

/* A screen pixel may carry four OAM-ordered sprite layers. Native color zero
 * remains recorded because authored replacement art can extend into it. */
#define HD_MAX_SPRITES 4
typedef struct {
    int32_t index;
    const uint8_t *t16;
    uint32_t argb;
    uint8_t p0,p1,p2,p3,ox,oy,hm,vm,has,transparent,behind;
} HdSprite;

typedef struct {
    /* Background layer (recorded for every rendered BG pixel). */
    int32_t        bg_index;        /* CHR-ROM absolute tile index; <0 none */
    uint8_t        sp_count_hint;   /* 0=unknown/holey; 1..5=packed count+1 */
    const uint8_t *bg_t16;          /* -> 16 CHR bytes (CHR-RAM matching) */
    uint8_t        bg_p0, bg_p1, bg_p2, bg_p3;  /* backdrop + colors 1..3 (6-bit) */
    uint8_t        bg_ox, bg_oy;    /* within-tile pixel offset */
    uint8_t        bg_has;
    uint8_t        bg_transparent; /* native pattern color zero; reveal authored layers */
    uint32_t       bg_argb;         /* original BG color (fallback when unmatched) */
    uint32_t       backdrop;        /* universal bg color (transparent HD BG -> this) */

    HdSprite sp[HD_MAX_SPRITES];
} HdPixel;

/* A synchronous source avoids materializing another full HdPixel frame when
 * the host already records the same data in its native representation. Fill
 * the background and present sprites; a packed count permits unused sprite
 * slots to remain uninitialized. Referenced keys stay valid through preparation.
 * Lookups may request another position while matching the current pixel.
 * The reader must not mutate the pack or re-enter job preparation. No source
 * callback, context or pixel pointers are retained by the published jobs. */
typedef void (*HdPixelReader)(const void *context, size_t offset, HdPixel *out);
const HdJobFrame *hdpack_prepare_jobs_from(const uint32_t *native, int native_w,
                                          HdPixelReader reader, const void *context);
/* Additions and uncached neighboring-pixel conditions reuse the source. A
 * host with a costly reader can materialize its array once for these packs. */
int hdpack_reuses_pixel_source(void);

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

/* The framebuffer width changed (live aspect resize): re-size the per-pixel
 * side channel to native_w x 240. Returns 0 on success, <0 if the pack had
 * to be unloaded because the allocation failed. No-op when inactive. */
int  hdpack_resize(int native_w);

int  hdpack_active(void);    /* 1 if a pack is loaded */
int  hdpack_scale(void);     /* upscale factor (1 when inactive) */
const char *hdpack_directory(void); /* current asset root, or empty when inactive */

/* Side channel the renderer writes into (NULL when inactive). Indexed
 * [y*native_w + x]. */
HdPixel *hdpack_pixels(void);
int      hdpack_recording(void);   /* 1 == renderer should fill hdpack_pixels() */
int      hdpack_unlimited_sprites(void); /* pack-authored visual expansion only */

/* Clear the side channel's per-pixel `has` flags for a new frame. Called by the
 * renderer at the start of an actually-rendered frame. */
void hdpack_frame_begin(void);
/* Derived condition caches must be recomputed after restoring guest state. */
void hdpack_invalidate_conditions(void);

/* Produce the HD framebuffer from the native one + side channel.
 * native_fb is native_w x 240 ARGB8888; hd_buf is (native_w*scale) x
 * (240*scale) ARGB8888 and must be caller-allocated. */
void hdpack_upscale(const uint32_t *native_fb, int native_w, uint32_t *hd_buf);
