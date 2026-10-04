# Porting game features to the cycle host

`runner/cyc/cyc_host_extras.h` supplies optional game callbacks. Leaving them
NULL keeps ordinary cycle games unchanged. `input` receives the final NES
controller bytes after host mapping and before latching, including headless
runs with no input script. Password entry can use the original guest input
path. `event` receives SDL gameplay events with the owning controller player
index; host menus and unfocused paused windows block gameplay keys/axes.
Controller removal is forwarded to clear held camera input.

`present` supplies the game picture to the window and headless
`--present-out`. Native `--screenshot`, frame logs and machine hashes keep
using the hardware picture/state. Presentation callbacks must return a valid
ARGB buffer and must not mutate the machine.

`cyc_mod_set_return_hook` installs an optional trusted observer after an
actual RTS, including the scheduler's return for a handled function hook.
Registers, return PC and stack are available through `cyc_mod_regs`; the
observer replaces no CPU bus cycles. Generated code and the interpreter use
the same boundary. The observer is NULL until installed.

Isolated scopes suppress hooks and return observation by default.
`cyc_mod_allow_isolated_hooks(true)` explicitly re-enables both after a
successful `cyc_mod_isolate_begin`. Permission is cleared at scope end;
scopes still do not nest. Trusted callbacks must maintain their own
speculative host state and guards, since the machine snapshot does not
restore game-owned C data.

`cyc_mod_call_commit_hooked` is the opt-in counterpart to
`cyc_mod_call_commit`: it runs enabled hooks/observers while computing a
memory-only guest routine's committed result. Device-register stores still
refuse the commit. A caller must restore or discard speculative host side
effects on failure. The ordinary commit and isolated call defaults are
unchanged.

`cyc_presentation.c` is an optional adapter for existing voxel profiles.
Link it and compile the shared voxel sources with
`NESRECOMP_CYCLE_PRESENTATION`. It exposes the cycle machine's data and
displayed OAM, CHR, palette and geometry through the old presentation names,
without linking the function-level CPU. Its controller ownership adapter is
event-scoped. `keybinds_init_readonly` reads existing camera bindings and uses
defaults without creating a legacy INI.

Validation includes the real and handled RTS paths, native/interpreter mod
parity, exact isolated snapshot restoration, per-scope permission reset and
memory-only commit refusal. Metroid additionally exercises original password
entry/capture, both-region gameplay, complete enhanced state replay and SDL
voxel camera/menu behavior. Game sidecars should register pure validators
with `nes_mod_register_savestate_validator` to reject incompatible options or
corrupt packets before machine restoration.
