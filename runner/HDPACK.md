# HD Texture Packs (Mesen HD Pack format)

Runner-level support for **Mesen HD Packs** — high-resolution tile/sprite
replacement driven by a `hires.txt` manifest + PNG sheets. Modeled on the
SNES recomp's MSU-1 wiring (opt-in config flag + env var + per-game support
hook). Source of truth for the format is Mesen2 `Core/NES/HdPacks/`.

## Architecture

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
- **Two-layer compositing** (HD background, then HD sprites over it) so original
  art never bleeds through transparent HD pixels.
- Arbitrary `<scale>`, HD SDL texture sizing, HD-aware screenshots.
- Toggles (env): `NESRECOMP_HDPACK_DEBUG` (unmatched BG→magenta, sprite→cyan),
  `NESRECOMP_HDPACK_HIDE_ORIGINALS` (`disableOriginalTiles`-style),
  `NESRECOMP_CHR_DUMP` (8 KB CHR snapshot for authoring CHR-RAM packs).

**NOT supported yet** (parsed and **skipped with a logged count**, never fatal):
- `<background>` full-screen layers (parallax / priority bands / alpha).
- `<condition>` / `[cond]` conditional tiles.
- `<bgm>` / `<sfx>` HD-pack audio; `<patch>` ROM patches; `<options>`
  (e.g. `disableOriginalTiles` — available only via the env toggle); HDR
  brightness > 1.0.

**Practical consequence — Zelda 1 and similar:** real *Legend of Zelda* (NES)
packs (e.g. "Zelda Remastered") are ~95% `<background>` + conditional tiles and
require a bundled **IPS ROM patch** (Zelda 1 Redux base). On stock Zelda with
this tile-only renderer, **nothing matches** — it needs the background+condition
subsystem above plus the ROM patch. Tile-based packs (e.g. the Super Mario Bros
pack) work fully. Zelda's CHR-RAM **content-match path is validated** here with a
synthetic pack generated from a runtime CHR dump (`tools/hdpack_gen.py
--chr-ram`); only the background/condition rendering is missing.

The loader structs are laid out to add backgrounds/conditions without a rewrite.

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
