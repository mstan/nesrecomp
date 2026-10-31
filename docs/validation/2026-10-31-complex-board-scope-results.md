# NES30: complex-board workload scope

Defer this service implementation: none of the five current campaign titles
uses a qualifying complex board or expansion-audio service. Actual ROM headers
and full SHA256 hashes confirm SMB1 NROM0, SMB3 MMC3/4, Metroid and Zelda MMC1/1,
and Gumshoe GxROM66. Prepared title snapshots are distinguished from the
current selected SMB1/Zelda heads; the ROMs are unchanged.

The engine does support MMC5, VRC6/VRC7, Namco and FME7. This decision concerns
the five-title campaign, not the engine's full game catalog. Current expansion
audio dispatch is gated to5/85/69/19/20/VRC6, with separate CPU/PPU/IRQ/read
service gates. MMC3 board4 is distinct from MMC5 board5. No relevant runtime
cost is inferred for the five titles, and no generic callback cleanup or
all-mapper matrix is introduced merely to rank a candidate.

There are no new builds, launches or speedup claims. A consuming supported
title would reopen the native event/board/audio-service opportunity while
preserving PCM, register/IRQ/read timing, current state and portable execution.
FDS remains owner-parked with NES29. This is a scoped conditional disposition,
not architectural exhaustion. Final fresh CLI/five-game builds and owner
approval remain separate. [Verified inventory and source evidence](2026-10-31-complex-board-scope-results.json).
