/*
 * ppu_dot.h — cycle-driven, per-scanline PPU renderer (Phase 3, EXPERIMENTAL).
 *
 * Opt-in via the NESRECOMP_DOT_PPU environment variable. When OFF (the
 * default) every entry point is a no-op and the per-frame renderer in
 * ppu_renderer.c is used unchanged — the build is byte-identical.
 *
 * Model (see ACCURACY_PHASE_PLAN.md §Phase 3):
 *   - The dot clock is SLAVED to the existing frame driver (s_ops_count /
 *     OPS_PER_FRAME in runtime.c), it does NOT replace the NMI/VBlank timing.
 *   - The dot clock free-runs through BOTH the NMI handler and the main loop,
 *     with a fixed VBlank phase offset: cycle 0 of the frame budget = scanline
 *     241 (VBlank start), so visible scanline 0 begins ~21 scanlines later.
 *     Many games (SMB included) run their sprite-0 split spin-wait INSIDE the
 *     NMI handler, which on hardware overruns VBlank into the top visible
 *     scanlines — so rendering must proceed during the NMI. The HUD paints at
 *     the early scroll, sprite-0 fires when the beam crosses it (self-syncing,
 *     no predictor), and the playfield paints at the post-hit scroll.
 *   - Rendering targets a back buffer; the completed frame is copied to the
 *     presentation framebuffer at the frame boundary (double-buffered so the
 *     mid-NMI present never tears).
 *   - sprite-0 hit / sprite overflow / MMC3 scanline IRQ are owned here, fired
 *     as the dot clock sweeps each scanline (replacing the render-time hack in
 *     ppu_renderer.c when the dot path is active).
 */
#ifndef PPU_DOT_H
#define PPU_DOT_H

#include <stdint.h>

/* 1 if the dot-accurate PPU is engaged this run (env NESRECOMP_DOT_PPU set and
 * not in a fall-back mode such as widescreen). Read at every CPU instruction
 * boundary, so it is a plain cached global. */
extern int g_dot_ppu_on;

/* Read the env flag and register the framebuffer. Call once at startup. */
void ppu_dot_init(uint32_t *framebuf);

/* Advance the dot clock to the cycle position `ops` (the per-frame CPU cycle
 * accumulator s_ops_count). Renders any newly-uncovered visible scanlines into
 * the back buffer and fires the per-scanline MMC3 IRQ. Called from
 * maybe_trigger_vblank after the cycle count is updated (during NMI and main
 * loop alike). No-op when g_dot_ppu_on is 0. */
void ppu_dot_advance(uint32_t ops);

/* Called at the very START of nes_vblank_callback, before the NMI handler runs
 * (so PPU memory still reflects the just-completed frame). Flushes any visible
 * scanlines not yet painted (safety net), copies the completed frame from the
 * back buffer to the presentation framebuffer, then arms the next frame's
 * visible region (cursor 0, scroll tracking reset). No-op when off. */
void ppu_dot_frame_boundary(void);

/* Render the full visible frame from the current PPU state into `buf`
 * (g_render_width x 240), synchronously and without side effects — for the
 * Zapper light probe, which needs a mid-frame display snapshot. */
void ppu_dot_render_snapshot(uint32_t *buf);

#endif /* PPU_DOT_H */
