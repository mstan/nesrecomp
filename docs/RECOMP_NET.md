# recomp-net integration

*Replaced 2026-09-25.* This page used to describe a two-player delay-sync spike
("exactly two peers, delay sync, and no spectators or rollback", a CRC32 of the
state carried in every input packet, a private lobby client). That is gone:
every NES netplay match now runs recomp-net's **rollback episode driver** with
2-4 seats, and the lobby is recomp-ui's shared backend. The design, the replay
model and the measured capability matrix are in [NETPLAY.md](NETPLAY.md); this
page is the integration checklist.

NESRecomp netplay is title opt-in: a game builds no networking code until its
CMake target enables it.

## Title integration

After `include(runner/runner.cmake)`, `add_executable()`, and -- when the game
links the launcher -- `include(recomp-ui/recomp_ui.cmake)`:

```cmake
target_compile_definitions(MyGame PRIVATE
    NESRECOMP_GAME_PLAYERS=4          # lobby seat ceiling (2..4)
    NESRECOMP_INPUT_SEATS=4)          # seats the runner routes (2..4)
nesrecomp_enable_recomp_net(MyGame)   # recomp-net + retcomm-rbengine
```

`NESRECOMP_GAME_VERSION` defaults to `git describe --tags --always --dirty` of
the game repository; the runtime appends the executable's SHA-256, so two
builds from different trees never share an identity (`game_version`), and the
loaded ROM's SHA-256 is the `content_fingerprint`. The driver's IDENT handshake
compares both in-band as well.

Pinned libraries: `lib/recomp-net` (RetroPortingToolKit, 03ee1b1: main a9d20e2
+ sparse-seat seal fix + lobby WebSocket backlog) and `lib/retcomm-rbengine`
(2a03e73, the snapshot ring). The game pins recomp-ui with the shared netplay
backend (b688ca7 or later; SMB pins 9edc243).

### Per-game checklist

1. **Seats.** Seat i's pad byte reaches the guest as `nes_input_seat(i+1)`:
   seats 0/1 are the two controller ports, seats 2/3 are `g_logical_input`
   (read by a multi-player mod). Nothing else may read a local device while a
   session is active; the runner applies the published rows right before the
   NMI, after every local source.
2. **Session configuration.** Every setting a match may vary is a key
   (`nes_session_config.h`): `get`, `apply`, `restore`, optionally an `offer`
   (what the host proposes; read it from the offline selection, since a match
   commits no mods) and one `finalize` rule for cross-key exclusions. The
   engine registers `widescreen`. Anything not registered cannot differ,
   because a netplay launch commits no mods.
3. **Snapshot domain.** Architectural state a mod keeps outside guest RAM must
   be a mod save-state record (`mod_savestate.h`); it is then in the rollback
   snapshot and the digest automatically. Prove it with `NES_RB_PROBE` over
   the mod's scenes (0 divergences, 0 symmetry failures).
4. **Local-only features.** `nes_mod_set_local_only()` still refuses an online
   launch; use it only for a feature that genuinely cannot run networked.

### Soft-return / rematch checklist

- A match ends in the waiting room, never in a dead process: Escape, a peer
  gone, a refusal (`boot_digest_mismatch`, `mod_set_not_agreed`), or a
  drained match return with `nes_netplay_last_error()` shown as the room's
  `last_error`.
- A rematch is a cold boot in the same process: `nesrecomp_runner_run` reloads
  the ROM and calls `runtime_session_reset()` / `runtime_init()`; the session
  configuration is restored to the player's own values on shutdown. Measured
  by `tools/rb_lobby.sh` (boot parts equal across matches; offline Play after
  a rematch equals a fresh process).
- A guest's SRAM persistence goes to `saves/netplay/`, and the match runs the
  host's SRAM, transferred before boot.

## Headless bring-up

The launcher is the normal path. For automated runs every peer can be driven by
the environment instead (a positional ROM skips the launcher):

```text
NES_NETPLAY=1
NES_NET_SLOT=0..3            NES_NET_SLOTS=2..4        NES_NET_OCCUPIED=<mask>
NES_NET_BIND=127.0.0.1:9701  NES_NET_PEER=127.0.0.1:9700   (seat 0 of >2 seats hubs)
NES_NET_DELAY=8              NES_NET_PREDICTION=12     NES_NET_MODE=rollback|delay
NES_NET_SESSION_ID=<n>       NES_NET_RELAY=1           NES_NET_SPECTATOR=1 + _SLOT
NES_NET_SESSION_CONFIG='nes-session/1;coop=4:player;widescreen=0;'
NES_NET_TEST_PAD=<seed>      NES_NET_MATCH_TICKS=<n>   NES_NET_EXIT_ON_RETURN=1
NES_NET_SHOT_TICK=<n> + NES_NET_SHOT_PATH=<png>        NES_NET_SCREENSHOT=<png>
```

The earlier spike's names `NES_NET_SESSION` (the session id) and
`NES_NET_INPUT_DELAY` are now `NES_NET_SESSION_ID` and `NES_NET_DELAY`;
`NES_NET_TRANSPORT=ice` is refused (ICE is not wired in the NES host -- online
play goes through the lobby server's UDP input relay). Rollback knobs are the
driver's (`NES_RB_*`, e.g. `NES_RB_SNAP_DEPTH`, `NES_RB_TIP_RUNWAY`,
`NES_RB_FORCE_MISPREDICT`); `tools/rb_loopback.sh`, `tools/rb_sweep.sh` and
`tools/rb_lobby.sh` drive them.

## Dependencies

ICE (`NESRECOMP_NET_ICE`) still builds libjuice into recomp-net, but the NES
host does not start an ICE session; on Windows the build supplies the narrow
mutex/error-code shim for it. Windows and macOS builds of the rollback stack
have not been verified.
