# NESRecomp mod packages and trusted plugins

NES mod support is an explicit per-game build feature:

```cmake
set(NESRECOMP_ENABLE_MODS ON CACHE BOOL "" FORCE)
include(${NESRECOMP_ROOT}/runner/runner.cmake)
```

The normal default is `OFF`. Opting in compiles the package runtime and opens
recomp-ui's compile-time Mods gate. The launcher shows Mods only after the game
also initializes a valid provider.

Cycle projects use `nesrecomp_add_cycle_game(... MODS GAME_ID "my-game")`.
Add `HD_PACKS` to that call to provide the framework's shared HD pack plugin.

## Mesen HD pack features

HD packs use the same installation, feature selection and persisted state as
other Mods. They are disabled by default in archives made by the importer.
The common cycle host loads their assets and presents the real PPU's recorded
tiles; games do not need HD-specific frame or activation callbacks.

```toml
[[feature]]
id = "hd-pack"
name = "HD textures"
group = "Display"
default_enabled = false
exclusive_group = "display-mode"

[[hd_pack]]
feature = "hd-pack"
directory = "pack"
# Optional, geometry-preserving IPS against the target's stock iNES image:
# patch = "pack/sound.ips"
# patched_rom_crc32 = "01234567"
```

`directory` and `patch` are relative to the installed package root. The
directory contains `hires.txt` and the creator's assets. The runtime refuses
parent/absolute paths, links, missing files, multiple selected HD packs and
unsupported executables. A patch requires both fields above, preserves the
header, trainer and cartridge size, and must produce the declared payload
CRC32. It changes an in-memory copy; the original ROM remains untouched.
Patched PRG uses the cycle interpreter so compiled stock opcodes cannot run
against changed code. Native execution remains available with the Mod off.

Pack identity includes package/version, target, all pack files and the patch.
Cycle saves refuse a different pack or patch before changing the machine.
Installed folders can move with the executable without changing identity.
Select a pack in the launcher before starting; the running game's menu makes
HD and conflicting display modes read-only until the next launch. HD packs
currently require local play. Game display plugins that cannot combine with
HD presentation must use the same `display-mode` exclusion group.

Create a local archive from an existing pack:

```sh
python tools/package_hdpack.py --pack "/path/to/pack" --rom "/path/to/stock.nes" --game-id "my-game" --id "my-game.hd" --name "HD textures" --author "Original creator" --license "Original pack terms" --notice "/path/to/original-readme.txt" --out "my-game-hd.nesmod"
```

The importer verifies a Mesen `<patch>` declaration's original ROM SHA-1,
checks its IPS and records the patched CRC. It bundles local assets and
preserved notices, with no ROM. Install the archive through **Mods**, then
enable its feature. This is the adoption path for cycle titles; the older
folder/config adapter remains for explicit legacy integrations. Replacement
pack music/sounds and some Mesen conditions remain unsupported. An installed
pack should be tested against its original and unmatched-pixel presentation,
state continuation and the game's other display features.

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
verification and allowing equivalent header variants. A Famicom Disk System
image is identified by its side data: the bytes after a 16-byte fwNES
`FDS` header, or the whole of a headerless image (its first side begins
with the disk info block, `*NINTENDO-HVC*`). Games on the cycle backend
(`runner/cyc`) use the same packages and runtime; see
[runner/cyc/README.md, Game mods](../runner/cyc/README.md#game-mods).

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

## Required owner ROMs

A feature may require a second, user-owned source ROM without treating that
ROM as package content or as the game image being launched:

```toml
[[external_rom]]
feature = "smash64-player"
id = "smash-64-us-v1"
label = "Super Smash Bros. 64 ROM"
description = "Select a legally owned USA v1.0 dump."
format = "n64"
identity = "Super Smash Bros. (USA), NTSC-U v1.0 (NALE)"
size = 16777216
normalized_sha1 = "e2929e10fccc0aa84e5776227e798abc07cedabf"
```

Resource IDs are unique across their whole package because persisted paths
and provider callbacks address them at package scope. `format = "n64"` gives
the launcher `.z64`/`.v64`/`.n64` filters and tells the runtime to detect N64
byte order from the magic and normalize the bytes in memory. `format = "raw"`
hashes the selected file without a byte-order transform and uses a generic ROM
picker. `format = "nes"` accepts either a headerless cartridge payload or an
iNES 1.0/2.0 `.nes` image without a trainer. The 16-byte iNES header is removed
before verification, and `size` describes the normalized PRG+CHR payload; this
lets equivalent iNES 1.0 and 2.0 headers identify the same cartridge revision.
`normalized_sha1` may be one string or an array of explicitly supported
revision hashes. The runtime compares the normalized SHA-1 and exact size declared by
the package, then displays the manifest's `identity` as the verified revision.
SHA-1 is used as a ROM-revision identity, not as a security primitive. The
selected path is stored in `mods/state.toml`; the ROM is never copied into or
packaged with the mod.

A resource declared with `required = false` is optional: leaving it
unselected never blocks enabling or PLAY, and the plugin simply receives
`NULL` for it. Once a path is selected, it is verified exactly like a required
resource, and the launcher offers a Clear button to return to the unselected
state.

Required owner ROMs are fail-closed at every activation boundary. A missing,
changed, or unsupported file prevents both package-level and feature-level
enable operations, invalidates a hand-edited or legacy enabled state, and is
reopened and rehashed when PLAY commits the activation plan. UI status alone
is never activation authority.

An activated, trusted game plugin may obtain the selected owner-ROM *path*
after that successful PLAY commit:

```c
const char* source = nes_mod_external_rom_path(
    "com.example.smash64", "smash64-player", "smash-64-us-v1");
if (!source) return;  /* no committed, verified resource for this feature */
```

`nes_mod_external_rom_path` returns `NULL` before a successful commit, for an
inactive feature, and for any package/feature/resource outside the committed
activation plan. It returns a runtime-owned path snapshot, not a pending
launcher selection, so a later UI edit cannot change what an already committed
plugin reads. The pointer remains valid until the next initialize or commit.
It exposes no ROM bytes; trusted game code is responsible for opening the
user-owned file. Native plugins are intentionally trusted code, so this is an
activation-scope gate rather than a sandbox between plugins.

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

`exclusive_groups = ["display-mode", "player-controller"]` adds memberships
when one feature conflicts with several independent feature families. It can
be used with or without the older singular `exclusive_group`; both contribute
to the same set. Enabling a feature disables every overlapping selection, and
commit also rejects conflicts introduced by editing `state.toml` directly.

A trusted plugin that requires local play calls
`nes_mod_set_local_only("Feature name", 1)` during activation. These requests
are cleared before the next activation pass. An online launch is rejected
before connecting while any request remains active.

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

## Function-entry hooks

An activation plugin can flip a global switch, but some mods need to take over
a specific guest subroutine *while enabled* and leave it untouched otherwise.
`replace_func` in `game.toml` cannot express that — it is a compile-time
substitution that makes codegen drop the original body, so there is nothing to
fall back to.

A game opts specific entries in:

```toml
[[mod_function_hook]]
addr = 0xB0E9           # PlayerCtrlRoutine
# bank = 0              # optional; omitted means any bank
```

Codegen then emits, as the first statement of that entry:

```c
void func_B0E9_b0(void) { /* PlayerCtrlRoutine */
    if (nes_mod_function_entry(0xB0E9u)) return;  /* trusted opt-in game-mod hook */
    ...
```

Nonzero skips the original body; zero runs it unchanged. The check precedes
the stack-tracking push, so a handling mod owns the whole call including its
frame and needs no push/pop bookkeeping.

For routines also reached by a native branch or fallthrough within a shared C
body, add `include_internal = true`. This moves the gate to the original
instruction label, covering those paths and ordinary calls exactly once.
A nonzero callback handles the remainder of that native subroutine and returns
from the current body; codegen balances its debug stack tracking. Use this only
at verified subroutine boundaries, with CPU registers and guest scratch RAM
preserved as required by the original caller. The default remains entry-only.

Register the implementation like any other trusted plugin — archives still
select behavior only by stable id, never by supplying native code:

```c
#include "mod_function_hooks.h"

static int take_over_player(uint16_t addr) { (void)addr; ...; return 1; }

NES_MOD_CONSTRUCTOR(register_hooks) {
    nes_mod_register_function_entry_plugin(
        "example.player-control", 0xB0E9, take_over_player);
}
/* Hooks start disabled. Activation plugins enable, reset callbacks disable: */
nes_mod_set_function_hook_enabled("example.player-control", 1);
nes_mod_disable_all_function_hooks();
```

Empty by default: a title that declares no hooks emits no callback and its
generated code is byte-identical. A declared hook that matches no emitted
entry is reported by codegen (`[[mod_function_hook]] ... matched no emitted
function entry`) rather than silently never firing — worth heeding, since the
6502 address alone does not tell you which bank the entry landed in.

This is intentionally narrow. It is not a general per-instruction mod
dispatcher.

On the cycle backend (`runner/cyc`) a site is an instruction boundary rather
than a C function entry, and it names its plugin and its code:

```toml
[[mod_function_hook]]
id = "example.area-parser"   # the plugin registered for this site
addr = 0x6E39
length = 8                   # content key: CRC32 of the 8 bytes at addr
crc32 = 0x24933765           # (or bytes = "A5 0E 48 20 ...")
```

The key is what makes an address in RAM mean one routine: a disk program
loads different files at the same address, and the site fires only while its
bytes are there (compiled RAM views, ROM banks and the interpreter alike).
`length` + `crc32` keeps game bytes out of the repository. A callback runs
before the site's instruction with the machine as the program left it, and
can read, write and call the program's own routines in isolation (`cyc_mod.h`);
nonzero returns from the routine as its RTS would. A plugin registered for an
id the program does not declare refuses to start. See
[runner/cyc/README.md, Game mods](../runner/cyc/README.md#game-mods).

Games with additional local players can compile with `NESRECOMP_INPUT_SEATS=4`
and read one-based seats through `logical_input.h`. Seats 1/2 retain the NES
controller ports; seats 3/4 are host inputs. `HOLD` and `RELEASE` input-script
commands accept an optional final player number. Match the launcher's
`NESRECOMP_GAME_PLAYERS` capability, and save any additional architectural input
state in the game's mod record. Titles that omit the capability keep two seats.

Register a pure `nes_mod_register_savestate_validator` alongside a mod's save
hook when mode/configuration mismatches must be rejected before restoring core
state. It receives `NULL, 0` for an absent record. Activation plugins may call
`nes_mod_set_local_only` to reject requested online sessions before connection;
the requirement is cleared and rebuilt on each activation pass.

Mod-enabled game targets must define string literals for
`NESRECOMP_GAME_ID` and `NESRECOMP_GAME_ROM_CRC32`. On Play, the runtime
revalidates the selected stock ROM, resolves enabled features, rejects missing
or multiply claimed plugin IDs, persists state, runs every reset callback, and
then activates the resolved plugins. Netplay launches clear the in-session plan
without overwriting the user's offline selections.

Format 1 limits executable behavior to trusted activation plugins. HD assets
and their optional geometry-preserving IPS use the shared, pre-boot validated
HD adapter described above. Further guest writes and interpreter hooks must
retain that validation and the no-arbitrary-code archive boundary.
