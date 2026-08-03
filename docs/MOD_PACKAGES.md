# NESRecomp mod packages and trusted plugins

NES mod support is an explicit per-game build feature:

```cmake
set(NESRECOMP_ENABLE_MODS ON CACHE BOOL "" FORCE)
include(${NESRECOMP_ROOT}/runner/runner.cmake)
```

The normal default is `OFF`. Opting in compiles the package runtime and opens
recomp-ui's compile-time Mods gate. The launcher shows Mods only after the game
also initializes a valid provider.

## Product and trust model

- A **package** is the installation, update, provenance, and trust boundary.
- A **feature** is independently enabled and may own boolean, choice, or
  bounded-integer options.
- A **trusted plugin** is game-owned native behavior already statically linked
  into the executable and registered under a stable ID.

`.nesmod` archives are ZIP files with a root `manifest.toml`. Archives contain
data only. They cannot provide native code, symbols, DLLs, or library paths.
The loader accepts stored and DEFLATE entries, verifies ZIP CRCs, rejects
encrypted or unsafe paths, caps archives at 4096 files and 256 MiB expanded,
stages extraction, validates the manifest, and publishes a version atomically.

The selected game image remains a stock ROM. Targets use the lowercase CRC32
of all bytes after the 16-byte iNES header, matching NESRecomp's existing ROM
verification and allowing equivalent header variants.

## Package layout

Installed and preloaded packages use the same executable-relative catalog:

```text
mods/
  state.toml
  packages/
    example.display/
      1.0.0/
        manifest.toml
```

Built-in features should default to disabled.

## Manifest format 1

```toml
format_version = 1
id = "example.display"
version = "1.0.0"
name = "Example Display Enhancements"
author = "Example Author"
description = "Game-specific presentation features."
license = "MIT"
resolver = "declarative"
save_compatibility = "shared"

[[target]]
game_id = "example-game-us"
rom_crc32 = "0123abcd"

[[feature]]
id = "widescreen"
name = "Widescreen"
description = "Enables the game's surveyed widescreen implementation."
group = "Display"
default_enabled = false

[[plugin]]
feature = "widescreen"
id = "example.widescreen"
```

Options are feature-owned:

```toml
[[option]]
feature = "widescreen"
id = "margin"
label = "Side margin"
type = "integer"
default = 64
min = 16
max = 128
step = 8
```

Supported types are `boolean`, `choice`, and bounded `integer`. Trusted plugins
may read integer values with `nes_mod_get_option_int`.

## Plugin registration

Game code registers trusted behavior before `main()`:

```c
#include "mod_runtime.h"

static void enable_widescreen(void) {
    GameDisplay_SetWidescreenEnabled(1);
}

static void reset_display_mods(void) {
    GameDisplay_SetWidescreenEnabled(0);
}

NES_MOD_CONSTRUCTOR(register_display_mods) {
    nes_mod_register_reset_callback(reset_display_mods);
    nes_mod_register_activation_plugin(
        "example.widescreen", enable_widescreen);
}
```

Mod-enabled game targets must define string literals for
`NESRECOMP_GAME_ID` and `NESRECOMP_GAME_ROM_CRC32`. On Play, the runtime
revalidates the selected stock ROM, resolves enabled features, rejects missing
or multiply claimed plugin IDs, persists state, runs every reset callback, and
then activates the resolved plugins. Netplay launches clear the in-session plan
without overwriting the user's offline selections.

Format 1 intentionally limits executable behavior to trusted activation
plugins. Guarded guest-ROM writes, assets, and interpreter hooks can extend the
same package contract later, but must retain pre-boot validation and the
no-arbitrary-code archive boundary.
