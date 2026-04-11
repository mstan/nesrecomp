# NESRecomp Modding Framework

The NESRecomp runner provides game-agnostic systems for overriding text
and tile graphics at runtime.  Games built on NESRecomp inherit these
capabilities automatically.

---

## Tile Override System (`override_chr`)

Intercepts CHR RAM writes at the PPU register level and allows
replacement of tile data via PNG files.

### Architecture

```
ROM -> game code -> PPU $2006/$2007 writes -> [HOOK] -> g_chr_ram -> render
                                                 |
                                          override_chr.c
                                          checks manifest,
                                          applies replacement
```

The system tracks "transfers" -- contiguous sequences of `$2007` writes
to CHR address space (`$0000-$1FFF`).  Each transfer is a discrete game
asset (player sprites, font tiles, background tileset, etc.).

Transfers are identified by PPU destination address + content CRC.
When a transfer matches a manifest entry, the replacement data is
written to `g_chr_ram` instead.

### Components

| File                          | Role                                    |
|-------------------------------|-----------------------------------------|
| `runner/src/override_chr.c`   | Session tracking, dump, manifest, overrides |
| `runner/src/chr_codec.c`      | PNG <-> NES 2bpp CHR conversion         |
| `runner/include/override_chr.h` | Public API                            |

### Runtime hooks (in `runtime.c`)

- `chr_override_on_ppuaddr(addr)` -- called when `$2006` pair completes
- `chr_override_on_chr_write(addr, val)` -- called on each `$2007` CHR write
- `chr_override_frame_end()` -- called at frame boundary to flush pending transfers

### Game integration (`extras.c`)

```c
void game_on_init(void) {
    /* Auto-detect tiles/manifest.json next to exe */
    /* ... or respond to --tile-dump / --tiles CLI flags */
    chr_override_init();
    chr_override_set_dump(1);              /* dump mode */
    chr_override_load_manifest("tiles");   /* load overrides */
}

void game_on_frame(uint64_t frame_count) {
    chr_override_reload_if_changed();      /* hot reload */
}

void game_post_nmi(uint64_t frame_count) {
    chr_override_frame_end();              /* flush transfers */
}
```

### CHR codec (`chr_codec.c`)

Handles conversion between NES 2bpp CHR format and PNG:

- **Encode (CHR -> PNG)**: 4-color grayscale, tiles arranged in a grid
  sized to fit the exact tile count (no phantom tiles).
- **Decode (PNG -> CHR)**: Fixed palette nearest-match to ensure
  lossless round-trips:
  - `#000000` -> index 0
  - `#555555` -> index 1
  - `#AAAAAA` -> index 2
  - `#FFFFFF` -> index 3
- **Disk cache**: `.chr.bin` files next to PNGs, regenerated when PNG
  is newer.

### Non-tile-aligned transfers

Some games write partial tile data (transfers not aligned to 16-byte
tile boundaries).  The system handles this by splitting:

- **Lead bytes**: partial tile data before the first complete tile
- **Tile data**: complete 8x8 tiles -> PNG (user-editable)
- **Trail bytes**: partial tile data after the last complete tile

The manifest records `lead_bytes` and `trail_bytes`.  At load time,
the PNG provides tile data and the companion `.bin` provides the
partial bytes, reconstructed to the exact original transfer size.

### Manifest format

```json
{
  "overrides": [
    {
      "ppu_addr": "0x0400",
      "length": 1280,
      "crc": "0xD13AF8B1",
      "lead_bytes": 0,
      "trail_bytes": 0,
      "file": "asset_0000_addr0400.png"
    }
  ]
}
```

### CLI conventions

Games should implement these flags in `game_handle_arg`:

| Flag                 | Description                                       |
|----------------------|---------------------------------------------------|
| `--tile-dump`        | Dump tile assets as PNGs to `tiles/`.             |
| `--tiles DIR`        | Load tile overrides from DIR (default: `tiles`).  |
| `--tile-compile DIR` | Batch pre-compile PNGs to `.chr.bin` cache files. |

Auto-detection: check for `tiles/manifest.json` next to the executable
on startup.  If present, enable tile overrides without requiring a CLI
flag.

---

## Text Override System (`override_text`)

Replaces in-game text strings via JSON-driven PRG ROM patching.

### Architecture

Two intercept mechanisms:

1. **PRG ROM patch** -- writes replacement bytes directly into the
   runtime's PRG ROM shadow buffer.  Works for any rendering path.
2. **PPU DMA buffer scan** -- scans the `$0500` write buffer each
   frame before NMI drains it.

### Game integration

Games register their text encodings (character -> tile byte mappings)
and load a JSON override file:

```c
void game_on_init(void) {
    text_override_init();
    text_override_register_encoding("MY_ENC", my_encode_fn, 0x00);
    text_override_load_json("text_overrides.json");
}

void game_on_frame(uint64_t frame_count) {
    text_override_reload_if_changed();  /* hot reload */
    text_override_apply();              /* DMA buffer scan */
}
```

### JSON format

```json
[
  {
    "bank": 12,
    "addr": "9DBC",
    "encoding": "MY_ENC",
    "source": "ORIGINAL",
    "replacement": "MODIFIED"
  }
]
```

Hot reload is supported: save the JSON file and changes appear in-game
within ~1 second.
