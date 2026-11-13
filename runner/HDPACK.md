# HD Texture Packs (Mesen HD Pack format)

Shared support for **Mesen HD Packs**: tile/sprite and background replacement
driven by `hires.txt` and PNG assets. Cycle titles adopt the modern Mods package
path in [MOD_PACKAGES.md](../docs/MOD_PACKAGES.md#mesen-hd-pack-features).
Source of truth for the format is Mesen2 `Core/NES/HdPacks/`.

## Architecture

Cycle games opt in with `MODS HD_PACKS GAME_ID` in `nesrecomp_add_cycle_game`.
The common host resolves a committed `[[hd_pack]]` descriptor, applies an
optional verified IPS in memory before power-on, and loads its package assets.
The older explicit `runner/cyc/hdpack.cmake` folder/config adapter remains
available. The cycle adapter
(`cyc_hdpack.c`) observes the actual hardware PPU's pattern fetches, shifters,
pixel selection and color-output pipeline. Its content keys own their bytes,
so later CHR RAM writes cannot change a previously recorded tile. It preserves
mapped CHR ROM indices, fine scrolling, 8x16 sprite halves, flips, grayscale,
emphasis and up to four OAM-ordered sprites at each pixel, including native
transparent pixels and behind-background priority. It does not
read the PPU bus or advance CPU, PPU or APU clocks. It feeds the same
`hdpack_upscale` sampler used by the legacy renderer. Seventeen pointer-free
save records preserve its picture and fetch pipeline; validated loads require the
same HD enablement, scale and package asset/patch fingerprint. Rendering is
cached while paused and invalidated on state load; background scroll offsets
are computed once per frame. `tools/cyc/test_cyc_hdpack.py` checks CHR RAM
and banked CHR ROM, scrolling, sprite flips and priority, native fallback
pixels and fresh-process continuation on all four NTSC alignments.

The remaining renderer description below refers to the legacy backend.

### Qualified layered render service

The cycle Zelda target defines `NESRECOMP_HDPACK_LAYER_PLANS=1` after its
enabled Remastered gameplay qualification. This includes `hdpack_layered.inc`:
immutable tile-key groups and render-local frame-condition plans. Pixel-dependent
conditions and per-pixel layer order remain exact; derived plans are rebuilt
rather than serialized. Allocation failures use the direct compositor.

Other targets keep the original `hdpack.c` implementation by default. Broad
versions slowed the simple SMB1 LyonHrt pack, so their aggregate gains were
rejected. The qualified module leaves the default preprocessed program and
SMB1's compiled/runtime sections identical to its control. See the
[per-game results](../docs/validation/2026-10-28-hd-plan-results.md) for the
selected package, negative receipts and source/binary identities. New title
adopters must measure their real enabled packs before selecting this build
definition; it is not a user-facing setting.

`cyc_hdpack_plans` compares192 complete pictures against the retained direct
lookup/compositor, including both CHR dialects, scales1/2/3/10, conditional
priority/negation, aliases, nearby/origin/mirror tests, layer blends, clipping,
same-frame invalidation and fallback. Whole-machine HD tests retain the fetch,
bank, flip, native-output and current-build continuation checks above.

The native PPU renderer (`ppu_renderer.c`) is **unchanged in behavior** — it
still produces the authentic 256×240 (or widescreen) ARGB framebuffer. When a
pack is active it *additionally* records, per visible pixel, the tile identity
Mesen uses for matching (Mesen's `HdPpuPixelInfo`/`ScreenTiles` model):

```
HdPixel { tile_index, tile16*, p0..p3 (palette), off_x, off_y,
          is_sprite, hmirror, vmirror, has }
```

BG writes the record; a drawn sprite pixel overwrites it — so the side channel
always holds the *visible surface* tile. After the native pass, `hdpack_upscale`
consumes the side channel + native framebuffer and produces a `W·scale ×
240·scale` HD framebuffer: matched tiles blit the HD PNG block, unmatched
pixels nearest-upscale. main_runner presents this via an HD-sized SDL texture.

All HD logic is isolated in `hdpack.c`; the renderer change is a thin
record-on-write gated by `hdpack_recording()`.

## Tile matching (byte-exact with Mesen2 `HdData.h`)

Key = `HdTileKey { PaletteColors (u32), TileData[16], TileIndex, IsChrRamTile }`.

- **CHR-ROM games (SMB):** key = `(TileIndex, PaletteColors)`, where
  `TileIndex = absoluteCHRaddr/16`. For unbanked mapper-0 that is
  `chr_base/16 + tile_id` (256 + tile_id for the $1000 table). Hash =
  `TileIndex ^ PaletteColors`.
- **CHR-RAM games (Zelda):** key = the 16 raw CHR bytes + 4 palette bytes
  (20-byte memcmp). Hash = Mesen's rotate-add over those 20 bytes.

`PaletteColors` byte layout (color `c` → `(PaletteColors >> ((3-c)*8)) & 0x3F`):

| dialect | BG tile | sprite tile |
|---|---|---|
| modern `<ver>` 100–109 | `(pal[0]<<24)|(pal[tp+1]<<16)|(pal[tp+2]<<8)|pal[tp+3]` | `0xFF000000|(pal[so+1]<<16)|(pal[so+2]<<8)|pal[so+3]` |
| legacy `<ver>` 2–3 | `(pal[tp+1]<<16)|(pal[tp+2]<<8)|pal[tp+3]` (MSB 0) | same as BG (no 0xFF marker) |

`tp = pal_base*4` (BG sub-palette), `so = 0x10 + sprite_pal*4`. Sprite color 0
is transparent. `defaultTile=Y` registers the tile under a wildcard palette key
`0xFFFFFFFF`; lookup tries the exact palette first, then the wildcard.

Definitions keep their authored order, including conditional and
palette-independent definitions. CHR-ROM `<fallback>` aliases are consulted
when an exact key is absent; cycle packs may also request
`<options>automaticFallbackTiles` to alias identical nonblank CHR patterns to
an authored tile. Tile-presence conditions use the same aliases. A matching
transparent replacement reveals the layers below, and unmatched native
background color zero remains transparent to authored background layers.

## hires.txt grammar (subset implemented)

`<ver>`, `<scale>`, `<img>` (implicit 0,1,2… index), `<tile>`, `<overscan>`
(parsed, clipping TODO). Both dialects of `<tile>`:

- modern: `<tile>img,tileData,palData,X,Y,brightness,default[,chrBank,tileIdx]`
  — `tileData` ≥32 hex chars ⇒ CHR-RAM (16 bytes); else CHR-ROM index
  (decimal if ver ≤102, hex if ver ≥103). `palData` = hex `PaletteColors`.
- legacy (`<ver>`<100): `<tile>tileIdx,img,palR,palG,palB,X,Y,brightness,default[,16 dec CHR bytes]`
  — palette = 3 decimal NES indices → `(palR<<16)|(palG<<8)|palB`.

`X,Y` are absolute pixel coordinates of the HD tile's top-left in the sheet;
block size is `8·scale`. PNGs decoded RGBA → premultiplied ARGB8888.

## Scope & restrictions (READ THIS)

This is **tile-replacement** HD-pack support. It is **opt-in and fully
optional**: with no pack configured (no `NESRECOMP_HDPACK`, no
`[Display] HdPackEnabled`, no `<exe>/hdpack/`), `hdpack_active()` is false, no HD
texture/buffer is allocated, the per-pixel recorder is gated off, and the native
render path is **byte-identical to stock**. Safe to ship on by default off.

**Supported:**
- `<tile>` replacement — both dialects: legacy `<ver>` 2–3 and modern `<ver>`
  100–109.
- Both match modes — CHR-ROM `(tileIndex, palette)` and CHR-RAM 16-byte content
  (20-byte key memcmp), selected from the ROM's CHR type.
- `defaultTile` wildcard-palette keys, per-tile `brightness`, sprite H/V mirror,
  per-pixel transparency, premultiplied-alpha PNG sheets.
- Background priority bands 0–9, 10–19, 20–29 and 30–39, with behind-background
  sprites, the native BG tile and front sprites between the appropriate bands.
- Modern `<addition>` layers, matching exact or ignored palettes and canonical
  fallback keys. Source mirrors reverse displacement; copied tile offsets and
  full palette keys are preserved. Additions use a render-local four-sprite
  array, leaving the recorded frame unchanged across repeat presentations.
- Arbitrary `<scale>`, HD SDL texture sizing, HD-aware screenshots.
- Toggles (env): `NESRECOMP_HDPACK_DEBUG` (unmatched BG→magenta, sprite→cyan),
  `NESRECOMP_HDPACK_HIDE_ORIGINALS` (`disableOriginalTiles`-style),
  `NESRECOMP_CHR_DUMP` (8 KB CHR snapshot for authoring CHR-RAM packs).

The existing sampler also supports conditional tiles and full-screen
background layers, including CPU-memory comparisons, scroll checks, tile
presence checks and condition groups. Unsupported condition kinds are logged
and skipped rather than treated as unconditional matches. A missing image is
reported and leaves native pixels available.

The cycle adapter reads CPU RAM/ROM and PPU pattern, nametable and palette
conditions through observation APIs without advancing the machine. The
whole-machine tests cover these reads, palette mirroring and canonical CHR
keys; `cyc_hdpack_alpha` covers authored transparent backgrounds/sprites and
native transparent background pixels. `cyc_hdpack_sprites` additionally checks
native transparent sprite coverage, overlapping HD alpha, OAM priority and
mirrored/background additions with fresh-process save/load. The portable and
GPU job consumers share these resolved decisions; the 192-picture comparison
also exercises multiple sprite layers. Common pixel jobs remain 32 bytes;
optional 16-byte descriptors hold up to three more layers.

The cycle adapter also honors `disableSpriteLimit`: it snapshots additional
OAM/CHR display metadata once per scanline and considers all64 OAM entries,
retaining up to four sprite layers at a pixel. Actual fetched keys are reused
for hardware sprites. The guest retains its ordinary eight-sprite evaluation,
overflow, DMA and bus timing. `cyc_hdpack_sprite_limit` checks a ninth sprite
with CHR RAM/ROM,8/16px shapes and flips, exact unchanged guest output and
current-build continuation. The ordinary fetch suite also exercises this option.

### Replacement audio in cycle titles

Modern `<bgm>` and `<sfx>` tags use the NEA registers, including album selection,
loop points, pause/resume, independent volumes and overlapping sound effects.
Both normal and alternate register ranges are recognized. Every declared Ogg
asset must decode before the host exposes the NEA signature; a failed asset
leaves the ordinary cartridge/APU path available. Decoding uses
[stb_vorbis](https://github.com/nothings/stb), with its original MIT/public-domain
license retained in `runner/external/stb_vorbis.c`.

Immutable mono/stereo PCM is decoded once at pack activation, with a512MiB
decoded-PCM admission bound. The player admits up to4096 simultaneous effects.
Logical playback follows the emulated clock even when output is disabled.
Integer interpolation samples stereo enhancements on the APU output timeline;
the frontend first applies ordinary title audio hooks to native mono, then
adds the stereo enhancement and applies output volume. The original mono read
API and `--wav-out` retain native APU samples. `--wav-stereo-out` captures the
combined stereo output without opening a sound device.

Current-build states retain track selections, playback positions, fractional
phases, loop/pause flags, active effects and queued stereo output. The18th HD
save record holds logical NEA state. `cyc_hd_audio_test` covers completion,
repeated status polling, stereo, loop/pause, volume, overlapping effects, invalid
selections and exact restoration. Optional `NESRECOMP_HD_AUDIO_TRACE=1` logs
at most256 playback commands for diagnostics.

### Source image storage

Large background images can contain many identical regions. The loader stores
lossless128×128 source pages when their combined allocations and map are smaller
than a flat ARGB image; otherwise it retains the flat representation. Partial
edge pages are padded and clipped to the original dimensions. Both portable
composition and the resident GPU uploader read these same immutable pages.
Retained presentation jobs own the unique allocations through pack unload.

PNG decoding still needs a temporary full-image buffer. Cropped tile art owns
its pixels, so source sheet buffers are released after manifest parsing. There
is no generated disk cache or change to authored image files.

Mods packages apply verified IPS patches in memory. An optional compiled
program catalog also supports changed cartridge geometry; see
[PROJECTS.md](cyc/PROJECTS.md). The folder-only adapter does not apply patches.

Zelda Remastered's cycle preview uses its locally IPS-patched ROM and the
existing texture/background sampler. The tested pack contains one missing
image and some conditions outside the implemented subset; these limitations
remain visible in its loader log. Its audio patch still runs on the NES APU;
replacement music and sound files are not played by this sampler.

## Wiring (mirrors MSU-1)

| MSU-1 (SNES) | HD packs (NES) |
|---|---|
| `Config.msu1_enabled` / `msu1_dir` | `NesConfig.hdpack_enabled` / `hdpack_dir` |
| INI `[Sound] Msu1Enabled/Msu1Dir` | INI `[Display] HdPackEnabled/HdPackDir` |
| launcher exports `SNESRECOMP_MSU1` | launcher exports `NESRECOMP_HDPACK` |
| `gi.msu1_supported` / note | `game_hdpack_supported()` hook |

Per-game packs live in `<game>/hdpack/` (folder with `hires.txt`).

## Test packs

- Synthetic (deterministic bring-up): `tools/hdpack_gen.py` builds a modern
  `<ver>101` pack from a game's own CHR ROM — every tile replaced by its 4×
  upscaled+tinted self, `defaultTile=Y` (palette-independent). Proves the whole
  pipeline with zero third-party ambiguity.
- Real: lyonhrt SMB pack (`<ver>2` legacy) — `_ws-hdpack/_hdpacks/`.
