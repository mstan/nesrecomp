# recomp-net integration

NESRecomp exposes optional two-player, delay-synchronised netplay through
`recomp-net`. It is deliberately title opt-in: existing games do not build or
link networking code until their CMake target enables it.

## Title integration

After including `runner/runner.cmake` and creating the executable target:

```cmake
set(NESRECOMP_NET_ICE ON CACHE BOOL "" FORCE) # optional WAN/ICE support
target_compile_definitions(MyGame PRIVATE
    NESRECOMP_GAME_PLAYERS=2
    NESRECOMP_GAME_VERSION="my-game-net-v1")
nesrecomp_enable_recomp_net(MyGame)
```

The game version is the multiplayer compatibility pin. Change it whenever a
build can no longer remain deterministic with the previous release.

When the target also uses recomp-ui, the launcher automatically gains the
Netplay page and the complete host/join/room flow. The bridge combines the
WebSocket matchmaking service with recomp-net's local lobby registry, exposes
local interfaces and STUN-discovered external IPv4 addresses, and hands LAN or
ICE session details to the runner.

## Runtime contract

The runner admits exactly one simulation tick at each outermost VBlank. It
samples one local controller, waits until recomp-net publishes both delayed
inputs, and then writes those bytes to the emulated `$4016` and `$4017`
controller streams. Nested VBlank callbacks reuse the published inputs and do
not advance the network timeline.

Each input packet also carries a canonical CRC32 of CPU, RAM/SRAM, PPU, mapper,
APU, timing, and controller-shift state. A mismatch, input-stream mismatch,
peer disconnect, or Escape ends the match and returns the launcher to the room.
Quick save/load is disabled while a netplay session is active.

Match settings are host-owned. The initial NES integration synchronises input
delay and the title's widescreen toggle. This spike supports exactly two peers,
delay sync, and no spectators or rollback.

## Headless LAN bring-up

The launcher is the normal path. For automated two-process testing, both peers
can instead use environment variables:

```text
NES_NETPLAY=1
NES_NET_SLOT=0|1
NES_NET_SESSION=4242
NES_NET_BIND=127.0.0.1:17771
NES_NET_PEER=127.0.0.1:17772
NES_NET_TRANSPORT=lan|ice
NES_NET_INPUT_DELAY=2
NES_NET_INPUT_PLAYER=0|1
```

Use inverse bind/peer ports for the second local process. `NES_NET_TRANSPORT`
defaults to automatic selection: lobby-backed public peers use ICE and private
or direct endpoints use LAN.

## Dependencies

`lib/recomp-net` is pinned as a submodule. ICE builds fetch libjuice v1.5.9 when
it is not installed locally. On Windows, the NESRecomp build supplies a narrow
native mutex/error-code compatibility shim for the pinned recomp-net sources.
