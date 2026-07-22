# Shadow Audio + Screen Enhancements (NES backport)

Backport of the gbarecomp "verified-enhancement" QoL layer to nesrecomp. All
work lives on the `feat/shadow-enhancements` branch / `_shadow_nesrecomp`
worktree (sibling of `nesrecomp/`), off a clean `master`. It does not touch any
other branch, worktree, or game project.

## Governing principle (the carve-out)

Faithfulness is the product; these are an opt-in layer on top. The one
permitted form of HLE here is a **verified-enhancement shadow**, allowed only
when ALL hold (recomp-template/PRINCIPLES.md, "Verified-Enhancement HLE Is
Allowed; Load-Bearing HLE Is Not"):

1. The emulated (canon) path keeps running and stays both the authoritative
   output and the verify oracle. The shadow is never ground truth.
2. The shadow is continuously, differentially checked against the canon stream
   and substitutes only after a proven window.
3. It reverts loudly (logs DEGRADED) the instant it stops matching.
4. It is opt-in and present-time, off by default; with it off the output is
   byte-identical (frame CRCs / smoke hashes / oracle comparisons stay on the
   raw canon).

Worst-case failure is "the user hears/sees the authentic NES output," and it
cannot mask a recompiler/APU/PPU bug because the canon path it shadows is still
the thing being diffed.

## What ports verbatim vs what is NES-specific

| Piece | Status | Notes |
|---|---|---|
| **`ShadowVerifier`** (envelope-correlation self-check, auto-gain, prove/strike/pause) | **DONE** — `runner/src/audio_shadow.{c,h}`, C, compiles clean | Engine-agnostic; algorithm identical to gbarecomp/snesrecomp, only the include guard / comments are renamed. Ported from JRickey/gba-recomp, © Jrickey, MIT OR Apache-2.0. |
| **APU float shadow render** | **DONE** — `runner/src/apu_shadow.{c,h}`, env-gated, default OFF, reverts loud | NES-specific: re-renders the canon APU's per-channel levels in float through the hardware's **nonlinear DAC mixer** (NESDev formula) instead of `apu.c`'s documented **linear approximation**, free of int16 requantization. Verified per output sample vs the canon int16 stream. |
| **Present-time palette LUT** | **DONE** (palette-swap model) — `runner/src/color_lut.{c,h}`, env-gated, default Raw=passthrough | NES-specific model — see "Video model" below. Swaps the runner's baked `g_nes_palette[64]` for an alternate measured NES palette present-time. |
| Full NTSC composite-artifact decoder | **NOT IMPLEMENTED — documented** | Strictly bigger model (emphasis bits, color bleed, dot crawl); deliberately not faked. See "Video model". |

## Video model — why a palette swap, not a panel/CRT LUT

The GBA color LUT is first-principles CIE colorimetry of a physical **LCD
panel**. The NES has no such panel: the 2C02 PPU emits a ~6-bit **color index**
that is decoded as an **NTSC composite signal**; the displayed RGB depends on
the TV/decoder, not a panel datasheet. A GBA-style BGR555→RGB888 panel LUT does
NOT apply.

This runner already collapses the composite signal into a single fixed **system
palette**: `g_nes_palette[64]` (ARGB8888) in `runner/src/ppu_renderer.c:35`. The
PPU resolves every pixel to an RGB value through that table, and the framebuffer
it produces is already palette-resolved ARGB. So the tractable, correct
enhancement is a **palette swap**: recognize each canon palette ARGB in the
finished frame and substitute the same index's RGB from an alternate
*measured/derived* NES palette — present-time, on a copy, leaving index
assignment (and emulation) untouched. Pixels that are not canon palette colors
(debug overlays, widescreen margin fills, the Zapper crosshair) are not in the
recognition table and pass through unchanged.

A full **NTSC-artifact decoder** (composite emphasis, color bleed, dot crawl,
per-scanline phase) is the genuinely higher-fidelity video model, but it is a
much larger subsystem and would require modeling the composite signal the PPU
emits — which this runner does not currently expose. Per the hard rule "never
guess hardware behavior," it is documented here and **not** implemented. The
right next step for it is to capture the PPU's per-pixel index + emphasis bits
(not the resolved RGB) and run an artifact decoder over that, still present-time
and still verified against the canon framebuffer.

The shipped palette tables (`2c02`, `fbx`) are transcribed public-domain
community approximations, chosen for taste, never correctness (they are
cosmetics over the canon index assignment). A measured-`.pal`-file loader is the
clean way to add precise colorimetry later.

## Integration points (file:line, on this branch)

### Audio (canon stream + shadow)
- **Canon mixer / stream:** `runner/src/apu.c` — `mix_sample()` (now
  `runner/src/apu.c:280`) is the authoritative linear-approximation mixer; it
  now also fills an `ApuChannelLevels` from `pulse_out/triangle_out/noise_out`
  + `s_dmc.output`. `apu_generate()` (`runner/src/apu.c:430`) produces the
  per-frame int16 stream consumed by SDL.
- **Output ring / consumer:** `runner/src/main_runner.c:638` —
  `apu_generate(s_audio_frame, 735)` → `SDL_QueueAudio` (44100 Hz mono int16,
  one ~735-sample frame per VBlank).
- **Shadow tap:** `runner/src/apu.c:523` (in `apu_generate`) routes each output
  sample through `apu_shadow_sample(canon, &lv)` **only when**
  `apu_shadow_enabled()`; otherwise it calls `mix_sample(NULL)` and the path is
  byte-identical to canon. `apu_shadow_init()` is armed from `apu_init()`
  (`runner/src/apu.c:324`).
- **Shadow render + verify:** `runner/src/apu_shadow.c` — `apu_shadow_sample()`
  computes the float nonlinear mix (`nonlinear_mix()`), feeds (canon, shadow)
  to `shadow_verifier_judge()`, returns canon while in probation / unproven,
  substitutes the gain-calibrated float once `shadow_verifier_proven()`, and
  logs a rate-limited `[apu_shadow] DEGRADED revert: …` on a failing window.
- **Gate:** env `NESRECOMP_AUDIO_SHADOW` (unset/`0` ⇒ OFF, default).

### Video (canon framebuffer + present-time LUT)
- **Canon palette:** `runner/src/ppu_renderer.c:35` — `g_nes_palette[64]`
  (ARGB8888). `ppu_render_frame()` resolves all pixels through it.
- **Framebuffer:** `runner/src/main_runner.c:58` — `s_framebuf[512*240]`, filled
  by `ppu_render_frame(s_framebuf)` at `runner/src/main_runner.c:651`.
- **Present / blit:** `runner/src/main_runner.c:757` — the main present site
  (`SDL_UpdateTexture` → `SDL_RenderCopy` → `SDL_RenderPresent`). The LUT is
  applied here: when `color_lut_is_passthrough()` (Raw, default) the raw
  `s_framebuf` is presented untouched (byte-identical); otherwise
  `color_lut_apply()` writes a swapped copy into `s_present_buf`
  (`runner/src/main_runner.c:61`) and that is presented.
- **Init:** `color_lut_init_from_env()` at `runner/src/main_runner.c:998` (just
  after texture creation).
- **Gate:** env `NESRECOMP_PALETTE={raw,2c02,fbx}` (unset/`raw` ⇒ passthrough,
  default).
- **NOT touched:** external-emulator presentation through `runner_present_framebuf`;
  those pixels do not come from `g_nes_palette`.

### Build
- `runner/runner.cmake` — explicit source list (NOT globbed); the three new
  files (`audio_shadow.c`, `apu_shadow.c`, `color_lut.c`) are appended there so
  every game project that `include()`s it picks them up.

## Compile status

All four affected translation units compile clean under
`gcc -std=c11 -Wall -Wextra -O2` (MSYS2 gcc 15.2):
`audio_shadow.c`, `apu_shadow.c`, `color_lut.c`, and the modified `apu.c`
(with `runner/include` on the include path). `main_runner.c` was not standalone-
compiled (needs SDL2 + the full runner include set); its changes only call the
already-compiled public APIs, signature-matched to the headers. No full game
build was performed (not required for this backport).

## Next steps

1. Wire into a game build (e.g. `SuperMarioBrosRecomp/`): confirm default-OFF is
   byte-identical (smoke CRCs unchanged), then A/B the enhancements.
2. APU shadow: confirm the canon int16 stream is mono — if a game build wants
   stereo, the verifier already takes stereo pairs; feed real L/R.
3. Palette LUT: add a measured-`.pal` file loader for precise colorimetry; keep
   Raw the default.
4. (Bigger) NTSC composite-artifact video model: expose per-pixel PPU index +
   emphasis from `ppu_render_frame`, run an artifact decoder present-time,
   verify against the canon framebuffer. Documented, not started.
5. Optional: surface shadow state (proven / pauses / last DEGRADED) over the TCP
   debug server for observability, mirroring gbarecomp.

## Attribution

`ShadowVerifier` (`audio_shadow.{c,h}`) ported from JRickey/gba-recomp
(`crates/gba-core/src/shadow.rs`) via the gbarecomp C++ port and the snesrecomp
C port, © Jrickey, MIT OR Apache-2.0, used with permission. The APU
nonlinear-mixer formula is the public NESDev-wiki APU mixer; the render +
substitution harness (`apu_shadow.c`) is ours. The present-path palette-swap
structure mirrors gbarecomp `color_lut.{h,cpp}`; the NES palette-index model and
the C palette-swap implementation are ours. Alternate palette tables are
public-domain community measurements.
