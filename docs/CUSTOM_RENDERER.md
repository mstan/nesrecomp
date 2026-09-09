# Live-resizable output geometry and game-owned custom renderers

Two runner capabilities, both inert unless a game opts in:

1. **`nes_video`** — the framebuffer width (`g_render_width`) is a runtime
   property. A game can follow the window aspect, switch presets, or drop back
   to the stock 256-wide picture per frame.
2. **`ppu_renderer_set_custom_render`** — a game may take over compositing of
   the wide framebuffer, with the stock renderer still running underneath at
   stock geometry so every PPU side effect stays vanilla.

Source of truth: `runner/include/nes_video.h`, `runner/src/nes_video.c`,
`runner/src/main_runner.c`, the "Game-owned custom renderer" block in
`runner/include/nes_runtime.h`, and the tail of `runner/src/ppu_renderer.c`.

---

## 1. Render geometry (`nes_video`)

### Globals

| Global | Meaning |
|---|---|
| `g_render_width` | framebuffer width in pixels; always `256 + left + right` |
| `g_widescreen_left` / `g_widescreen_right` | configured margins (fixed geometry) |
| `g_ws_eff_left` / `g_ws_eff_right` | per-frame *effective* margins; pixels between effective and configured render black. `-1` = follow configured |

Historically `g_render_width` was fixed for the life of the process: a game set
`g_widescreen_left/right` once in `game_on_init()` and every buffer was a static
512-wide array. `nes_video` replaces that with a request/apply model.

### Modes

`NesAspectMode`: `NES_ASPECT_STOCK` (the vanilla 256-wide frame, 16:15 under
square pixels), `NES_ASPECT_16_9`, `NES_ASPECT_21_9`, `NES_ASPECT_32_9`,
`NES_ASPECT_FIT`.

`nes_video_aspect_from_name()` accepts `stock`/`4:3`/`off`/`0`, `16:9`,
`21:9`, `32:9`, `fit`/`adaptive`/`auto`. Separators are normalized, so
`16-9`, `16x9`, `16_9` all parse; case-insensitive.

### Width math

> **All of this is opt-in.** `nes_video_width_for()` has exactly two callers:
> `nes_video_set_aspect_mode()`, which a game must call itself, and
> `nes_video_on_window_resized()`, which acts only once the mode is already
> `NES_ASPECT_FIT`. A game that sets fixed margins instead (`g_widescreen_*`,
> `nes_video_request_width/_margins`, `--widescreen`) never reaches the aspect
> math and is unaffected by the widths in this section. In-tree, only
> `MetroidNESRecomp/mods/widescreen_plugin.c` opts in — itself a mod plugin.

`nes_video_width_for_aspect()`:

```
width = round_to_nearest_even(240.0 * aspect)   clamped to [256, NES_MAX_RENDER_WIDTH]
```

The presentation is **square pixels** (logical `g_render_width x 240` via
`SDL_RenderSetLogicalSize`), so a 240-row picture of aspect `A` is `240 * A`
columns wide. This follows snesrecomp's `SmCalculateViewport`.
`NES_MAX_RENDER_WIDTH` is **864** (32:9 at 240 rows is 853.3 px). Widths are
forced even (`w &= ~1`) so the stock 256 columns stay exactly centered under
symmetric margins.

`NES_ASPECT_STOCK` is an **explicit case returning exactly 256** — it does not
go through the formula. `NES_ASPECT_FIT` uses the window drawable aspect
(`out_w / out_h`) clamped to `[16:15, 32:9]` before applying the formula;
`NES_STOCK_ASPECT` (`= 256.0/240.0`) is that low clamp.

Resulting widths from `nes_video_width_for()`:

| Mode | aspect | width | margins (symmetric) |
|---|---|---|---|
| `NES_ASPECT_STOCK` | 16:15 (vanilla) | **256** | 0 + 256 + 0 |
| `NES_ASPECT_16_9` | 16:9 | 426 | 85 + 256 + 85 |
| `NES_ASPECT_21_9` | 21:9 | 560 | 152 + 256 + 152 |
| `NES_ASPECT_32_9` | 32:9 | 854 | 299 + 256 + 299 |
| `NES_ASPECT_FIT` | window, clamped `[16:15, 32:9]` | 256..854 | derived |

> **The pixel-shape trade-off.** A real NES puts 256 columns into a 4:3 raster
> with 8:7 non-square pixels. This module presents square pixels throughout,
> under which 256x240 is **16:15** and a true 4:3 frame is **320** columns.
> Two consequences, both deliberate:
>
> - `NES_ASPECT_STOCK` is 256, the vanilla picture, and is therefore slightly
>   narrow-looking versus a CRT. That is the price of never inventing picture
>   the game did not draw; SDL pillarboxes/letterboxes the rest.
> - Fit's low clamp is 16:15, not 4:3. A window at or below the vanilla aspect
>   gets exactly 256 columns — never a picture *narrower* than the game. A 4:3
>   window yields 320, which genuinely fills it with square pixels: 32 columns
>   per side of real extra widescreen content, not a stretched vanilla frame.
>   So a game in Fit mode starts widening as soon as the window passes 16:15.

A game that wants title/menu screens pillarboxed inside a *fixed* wide
framebuffer should use `g_ws_eff_left/right` (per frame) rather than
re-requesting geometry, since geometry changes destroy and re-create the SDL
texture.

### Pending / apply-after-present model

Requests are **queued**, never applied inline:

- `nes_video_set_aspect_mode(mode)` — sets the mode and requests the
  corresponding width using the last known drawable size.
- `nes_video_request_width(w)` — symmetric margins, `m = (clamp_even(w)-256)/2`.
- `nes_video_request_margins(l, r)` — explicit/asymmetric margins. If
  `256+l+r > NES_MAX_RENDER_WIDTH` the *larger* margin is shrunk first so the
  request stays satisfiable (`nes_video.c:92-97`).
- `nes_video_on_window_resized(out_w, out_h)` — called from the main loop's
  `SDL_WINDOWEVENT_SIZE_CHANGED` handler; re-derives the width only when the
  mode is `NES_ASPECT_FIT`.

All requests are **idempotent**: a request equal to the current geometry clears
the pending flag and is a no-op, so a game may re-assert its mode every frame
from `game_post_nmi()`.

`main_runner.c` drains the queue at exactly one point per frame —
`video_apply_pending()` (`main_runner.c:246`), called from `nes_vblank_callback()`
immediately after `SDL_RenderPresent` and before the next frame renders
(`main_runner.c:1595`). It:

1. `nes_video_commit(left, right)` → sets `g_widescreen_left/right` and
   `g_render_width`.
2. `memset(s_framebuf, 0, VIDEO_BUF_BYTES)` — stale narrower/wider content
   would sit misaligned under the new geometry until the next real render (a
   rendering-off frame keeps the previous picture on purpose), so start black.
3. Destroys and re-creates `s_texture` at the new width.
4. Re-creates the HD-pack side channel if a pack is active (below).
5. `SDL_RenderSetLogicalSize()` and a `[Video] render width N (nL + 256 + nR),
   aspect X` line.

**Nothing ever changes `g_render_width` while a frame is being rendered.**

### Pre-window immediate apply

Before the SDL window exists, `s_window_ready` is 0 and
`nes_video_request_margins()` calls `nes_video_commit()` **immediately**
(`nes_video.c:98-104`), exactly as the old `game_on_init()` assignments did, so
the first SDL texture is created at the right size. The fixed-width consumers
rely on this: SMB widescreen and the voxel first-person profile
(`voxel_screen_profile.c:230`, which now routes through
`nes_video_request_margins()` instead of assigning the globals directly).

`nes_video_mark_window_ready()` is called after window/renderer/texture
creation (`main_runner.c:1895`), immediately followed by a seed
`nes_video_on_window_resized()` with the real drawable size so `Fit` starts
correct. From that point on every request is queued.

### Buffers are allocated once

`video_alloc_buffers()` (`main_runner.c:231`) `calloc`s `s_framebuf`,
`s_present_buf` and `s_zapper_snapbuf` at
`VIDEO_BUF_BYTES = NES_MAX_RENDER_WIDTH * 240 * 4` on entry to
`nesrecomp_runner_run()`. They are never reallocated, so pointers handed out
earlier — the zapper snapshot buffer given to `runtime_set_zapper_snapshot()`,
the dot-PPU's target — stay valid across a resize. Only the SDL texture(s), the
renderer logical size and the HD-pack side channel are re-created.
`ppu_dot.c`'s `DOT_MAXW` is likewise `NES_MAX_RENDER_WIDTH`.

### HD packs across a resize

`video_apply_pending()` `realloc`s `s_hd_buf` to `g_render_width * s_hd_scale`
and calls `hdpack_resize(g_render_width)` (`hdpack.c:681`), which re-allocates
the per-pixel side channel. If either fails the pack is **unloaded** and output
drops to the native texture — the runner never presents a pack whose side
channel does not match the framebuffer width.

---

## 2. Game-owned custom renderer

### Interface (`nes_runtime.h:257-299`)

```c
typedef int (*NesCustomRenderFn)(uint32_t *out, int out_w, int out_h,
                                 int native_x0, const uint32_t *native,
                                 void *user);
int  ppu_renderer_set_custom_render(NesCustomRenderFn fn, void *user); /* 1 ok, 0 refused */
int  ppu_renderer_custom_render_active(void);
```

- `out` — `out_w x out_h` ARGB8888 framebuffer, `out_w == g_render_width`,
  `out_h == 240`.
- `native_x0` — the column in `out` where native column 0 sits; equals
  `g_widescreen_left`.
- `native` — the 256x240 stock frame just rendered, read-only.
- Return **nonzero** if the hook painted `out`. Return **0** to ask the engine
  to paste the native frame centered at `native_x0` (pillarboxed fallback —
  e.g. for title screens the hook leaves alone).

`ppu_renderer_custom_render_active()` is `fn != NULL && g_render_width > 256`.
At stock width the hook is dormant and the stock path runs unchanged.

### What `ppu_render_frame()` does when a hook is installed

(`ppu_renderer.c:1240-1268`)

1. If not active → `render_frame_native(framebuf)`, byte-identical to the
   pre-hook renderer.
2. Otherwise, save `g_render_width` / `g_widescreen_left` / `g_widescreen_right`
   / `g_ws_eff_left` / `g_ws_eff_right`; force them to `256 / 0 / 0 / -1 / -1`;
   run `render_frame_native(s_native_scratch)` into a private static
   `uint32_t s_native_scratch[256*240]`; restore. Every widescreen branch in the
   native renderer reduces to the vanilla path at margins 0, so **sprite-0 hit,
   mapper IRQ splits, the HD-pack side channel and the background-opacity
   snapshot are all exactly those of a vanilla 256-wide frame.** A
   rendering-disabled frame returns early and keeps the previous scratch
   content, mirroring the stock "CRT keeps the last picture" behavior.
3. Pre-fill the whole of `out` with opaque black `0xFF000000`.
4. Call the hook. Nonzero → done.
5. Zero → `memcpy` the 256-wide scratch row by row into `out` at `native_x0`.

### Constraints

- **Not compatible with the dot-PPU.** `ppu_renderer_set_custom_render()`
  returns 0 and logs `[Render] custom renderer refused: the dot-PPU publishes
  incrementally (unset NESRECOMP_DOT_PPU)` when `g_dot_ppu_on` is set. Check the
  return value.
- **`ppu_renderer_background_opaque()` is in NATIVE (256-wide) coordinates**
  while a hook is installed, because the stock pass ran at stock geometry. A
  hook that wants wide-space opacity must build its own coverage buffer for
  `ppu_renderer_draw_sprites_wide()`.
- **HD texture packs are bypassed** for a frame with an active hook: both the
  present path (`main_runner.c:1579`) and the screenshot path
  (`main_runner.c:1495`) test `&& !ppu_renderer_custom_render_active()`, because
  the pack's per-pixel side channel is native-space.
- With no hook installed (the default) the render path is unchanged and
  byte-identical.

### Sprite compositing helper

```c
typedef int (*NesSpritePlaceFn)(int oam_slot, int screen_x, int screen_y,
                                int *out_x, void *user);

void ppu_renderer_draw_sprites_wide(uint32_t *out, int out_w, int native_x0,
                                    const uint8_t *bg_opaque,
                                    NesSpritePlaceFn place, void *user);
```

Draws the current OAM — as snapshotted by the most recent `ppu_render_frame` —
into a wide framebuffer using the current CHR window, palette and PPUCTRL.
Details (`ppu_renderer.c:1270-1328`):

- Returns immediately unless `g_ppumask & 0x10` (sprites enabled).
- Iterates slots `63 → 0` so slot 0 lands on top.
- `screen_x` is `g_oam_x16[s]` when `g_ws_oam_sidecar` is set, else the raw OAM
  X byte.
- Honors the installed sprite-suppression hook
  (`ppu_renderer_sprite_suppressed`), 8x16 tall sprites, H/V flip, sprite
  palette, and PPUMASK bit 2 (leftmost 8 **native** columns clip sprites).
- `bg_opaque` is `out_w x 240` (1 = opaque, may be NULL) and drives
  behind-background priority (`attr` bit 5). It is indexed in `out` space.
- `place` receives the slot, the unwrapped screen X and OAM `Y+1`, and writes
  the destination X in `out` coordinates. Return 0 to **skip** the slot. NULL
  means `x + native_x0`.
- **No PPU side effects**: no sprite-0 hit, no IRQ service. It is a compositing
  helper, valid for frames without a mid-frame CHR/mask change — the stock pass
  keeps handling those.

---

## 3. Widescreen sprite-X sidecar

NES OAM X is a single byte, so a game's own 8-bit math wraps any sprite whose
screen X leaves `[0,255]`; in a widened viewport such a sprite would teleport to
the opposite edge. The sidecar keeps a parallel signed 16-bit screen X per OAM
slot that the renderer consumes instead of the raw byte
(`nes_runtime.h:445-483`).

State:

| Global | Role |
|---|---|
| `g_ws_oam_sidecar` | master enable; 0 = inert, byte-identical. Games set it in `game_on_init()` |
| `g_oam_x16[64]` | render-side sidecar, paired with `g_ppu_oam` |
| `g_ws_shadow_x16[64]` | shadow-OAM-side sidecar (the `$0200` page) |
| `g_ws_obj_true_rel` | context: true 16-bit screen X of the object being drawn |
| `g_ws_obj_rel8` | context: the 8-bit relative X the game itself computed |
| `g_ws_obj_ctx_valid` | context valid flag; game policy sets and clears it |
| `g_ws_obj_delta_min` / `g_ws_obj_delta_max` | accepted layout-offset window |

Population: every store in generated code funnels through `nes_write()`, so
writes to the shadow-OAM page are observed centrally. The tap is hardcoded to
the **`$0200` page, X bytes only** — `g_ws_oam_sidecar && (a & 0x0700) == 0x0200
&& (a & 3) == 3` (`runtime.c:1545`) → `ws_sidecar_track`, slot
`(a - 0x200) >> 2`. A game whose shadow OAM lives elsewhere gets no sidecar.
Game policy publishes a "current draw
object" context (typically from a `ram_read_hook` inside the game's own
relative-position routine). Each subsequent shadow-OAM X write re-derives the
unwrapped X as

```
wide = g_ws_obj_true_rel + (int8_t)(written_byte - g_ws_obj_rel8)
```

accepted only when `g_ws_obj_ctx_valid` and the delta lies in
`[g_ws_obj_delta_min, g_ws_obj_delta_max]` and the result is in `[-256, 512)`
(`runtime.c:1411-1426`). Otherwise the plain byte is recorded = vanilla
placement.

`g_ws_shadow_x16` is copied to `g_oam_x16` at OAM DMA (`$4014`) so it stays
paired with the OAM snapshot the renderer sees — but only when the DMA source
really is the tracked page (`val < 0x20 && (src & 0x0700) == 0x0200`,
`runtime.c:1569`); a DMA from anywhere else refills `g_oam_x16` from the plain
OAM X bytes. `$2004` writes update `g_oam_x16` directly with the plain byte
(`runtime.c:1687`), since a direct OAM write carries no draw context. None of
this state is in the savestate snapshot — it repopulates at the next DMA.

### The layout-offset window

`g_ws_obj_delta_min` / `g_ws_obj_delta_max` default to **-24 / 56**
(`main_runner.c:204-205`), which fits SMB's ≤5-tile object layouts. The window
exists so that HUD/static sprites written while a **stale** context is still
valid fall back to vanilla placement instead of being dragged by an unrelated
object's origin. A game with wider object frames (Metroid bosses) widens it in
`game_on_init()`:

```c
g_ws_obj_delta_min = -64;
g_ws_obj_delta_max = 120;
```

Widening trades false-negatives (a legitimately far-offset sub-sprite left
wrapped) for false-positives (an unrelated sprite dragged by a stale context).
Clear `g_ws_obj_ctx_valid` promptly and the window can stay tight.

---

## 4. Which approach for which game

**Classic re-expand widescreen** — the renderer draws extra background columns
into the margins from the nametables the game already wrote, plus the sprite-X
sidecar, plus per-frame gating of `g_ws_eff_left/right` so title/attract screens
stay pillarboxed. Use it when:

- the game's own nametables already contain more world than the 256-px viewport
  (horizontally-scrolling, vertically-mirrored games), and
- the extra columns are *correct* content, not guesses.

Worked reference: `SuperMarioBrosRecomp/WIDESCREEN.md` — four coordinated
layers (background margins, sprite-X sidecar, spawn/cull policy, per-frame
gating), margins capped `left ≤ 128, right ≤ 96`, gated on both the mod package
switch and `OperMode` (1 = game, 2 = victory; title/attract/game-over stay
vanilla and pillarboxed). Note SMB predates `nes_video` and picks its own
16:9 width of 428 (86 + 256 + 86) via `--widescreen 16:9`; `nes_video`'s
`NES_ASPECT_16_9` computes 426 (85 + 256 + 85) from the square-pixel formula.
The two numbers are both "16:9" to within a pixel of rounding; do not assume
they agree.

**Custom renderer** — use `ppu_renderer_set_custom_render()` when the wide
picture cannot be produced by widening the stock renderer's margins: the game
does not have the off-screen content in the nametables, the projection is not a
straight horizontal extension, or the HUD/status layout has to be re-placed
rather than shifted. The hook gets a clean 256x240 stock frame plus an empty
wide canvas and owns everything else. The cost is that HD packs and the dot-PPU
are unavailable, and `ppu_renderer_background_opaque()` is native-space.

---

## 5. Minimal game-side example

```c
/* mygame_ws.c */
#include "nes_runtime.h"
#include "nes_video.h"
#include "game_extras.h"

static uint8_t s_bg_opaque[NES_MAX_RENDER_WIDTH * 240];

static int place_hud(int slot, int screen_x, int screen_y, int *out_x, void *user) {
    (void)user; (void)screen_y;
    if (slot >= 56) {                 /* HUD slots: pin to the left margin edge */
        *out_x = screen_x;            /* out-space column 0.. */
        return 1;
    }
    return 0;                         /* world sprites: drawn by the wide pass */
}

static int mygame_render(uint32_t *out, int out_w, int out_h,
                         int native_x0, const uint32_t *native, void *user) {
    (void)user;
    if (!g_mod_widescreen_on) return 0;      /* 0 = engine pillarboxes `native` */

    /* 1. paint the wide background into `out` and record coverage. */
    memset(s_bg_opaque, 0, (size_t)out_w * out_h);
    mygame_paint_wide_background(out, out_w, out_h, native_x0, s_bg_opaque);

    /* 2. sprites, with the sidecar's unwrapped X. */
    ppu_renderer_draw_sprites_wide(out, out_w, native_x0, s_bg_opaque,
                                   place_hud, NULL);
    return 1;                                 /* we painted `out` */
}

void game_on_init(void) {
    g_ws_oam_sidecar   = 1;               /* enable the 16-bit sprite-X sidecar */
    g_ws_obj_delta_min = -64;             /* this game's object frames are wide */
    g_ws_obj_delta_max = 120;

    /* Mod activation / CLI: install the hook. Refused under the dot-PPU. */
    if (!ppu_renderer_set_custom_render(mygame_render, NULL)) {
        fprintf(stderr, "[MyGame] widescreen unavailable (dot-PPU active)\n");
        return;
    }
    /* Pre-window: applies immediately, so the first texture is the right size. */
    nes_video_set_aspect_mode(NES_ASPECT_FIT);
}

void game_post_nmi(uint64_t frame) {
    (void)frame;
    /* Re-assert the mode per frame: a request equal to the current geometry is
     * a no-op, so this costs nothing while the window aspect is unchanged. */
    nes_video_set_aspect_mode(NES_ASPECT_FIT);

    /* Pillarbox title/menu/attract screens WITHOUT changing geometry — a
     * geometry change destroys and re-creates the SDL texture, so per-frame
     * mode flapping is the wrong tool. -1 = follow the configured margins. */
    if (mygame_in_gameplay()) { g_ws_eff_left = -1; g_ws_eff_right = -1; }
    else                      { g_ws_eff_left =  0; g_ws_eff_right =  0; }
}
```

To uninstall, call `ppu_renderer_set_custom_render(NULL, NULL)` and
`nes_video_request_width(256)` or, equivalently,
`nes_video_set_aspect_mode(NES_ASPECT_STOCK)` — both request exactly 256 (see
the pixel-shape note in §1).

---

## 6. Invariants

1. `g_render_width == 256 + g_widescreen_left + g_widescreen_right`, always.
   Only `nes_video_commit()` writes these three; never assign them directly.
2. **Never change geometry mid-render.** Requests are queued; the single apply
   point is `video_apply_pending()`, after present and before the next render.
   The only exception is the pre-window path, which is before any render exists.
3. **Buffers are allocated once at `NES_MAX_RENDER_WIDTH`** (864) and never
   move. Any new width-sized buffer must be sized `NES_MAX_RENDER_WIDTH * 240`,
   not `g_render_width * 240`, and must not be reallocated on resize.
4. Widths are even and clamped to `[256, NES_MAX_RENDER_WIDTH]`. Symmetric
   margins are the default; asymmetric margins are available via
   `nes_video_request_margins()`.
5. A custom render hook must not touch PPU state, must not assume
   `native_x0 == (out_w-256)/2` (read the argument), and must tolerate being
   called with `out` full of opaque black.
6. `g_ws_eff_left/right` bound what is *drawn* this frame within the fixed
   geometry; they are clamped to the configured margins by the renderer and
   `-1` means "follow the configured margins".
