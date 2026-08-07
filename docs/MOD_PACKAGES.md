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
exclusive_group = "display-mode"
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
may read integer values with `nes_mod_get_option_int`, and any option's
committed value as a string with `nes_mod_option_value`:

```c
char character[64];
if (!nes_mod_option_value(package, feature, "character",
                          character, sizeof character))
    snprintf(character, sizeof character, "captain-falcon");  /* own default */
```

It writes a NUL-terminated value and returns 1. On failure — plan not
committed, ids unresolved, or the value too long for the buffer — it returns 0
and sets `out[0] = '\0'`, so a caller applies its own default instead of
treating an empty string as a selection.

Features with the same non-empty `exclusive_group` are mutually exclusive.
Enabling one in the launcher automatically disables the other selected feature
in that group, even when the features come from different packages. Validation
also rejects a hand-edited state file that enables more than one, so the
runtime can never activate incompatible presentation modes together.

## Conditional plugin activation

A `[[plugin]]` may be conditioned on one of its own feature's option values, so
one feature can offer several implementations instead of degenerating into one
pseudo-feature per value:

```toml
[[option]]
feature = "smash64-player"
id = "character"
label = "Character"
type = "choice"
default = "captain-falcon"

[[option.choice]]
value = "captain-falcon"
label = "Captain Falcon"

[[plugin]]
feature = "smash64-player"
id = "example.captain-falcon"
when_option = "character"
when_value = "captain-falcon"
```

`when_option` and `when_value` must both be present or both absent. The
referenced option must belong to the same feature, and the value must be one of
its declared choices — both are checked when the manifest loads, so a typo is a
package diagnostic rather than a feature that silently does nothing.

Unselected variants are skipped before the duplicate-plugin check, so sibling
implementations of one choice never collide with each other. A plugin without a
condition activates whenever its feature is enabled, which is the historical
behaviour and is unchanged.

Prefer conditions when the choice merely selects *which* implementation runs,
and `nes_mod_option_value` when a plugin must act on the value itself.

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
