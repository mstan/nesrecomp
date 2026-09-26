This directory vendors unmodified `emu2413.c`, `emu2413.h`, and `LICENSE` from
https://github.com/digital-sound-antiques/emu2413 at commit
`11676f6c43af7a53a0a940f8faea57eed73a22ba` (MIT license).
`revision.json` records the SHA-256 of each downloaded file.

`hw_vrc7.inc` selects the VRC7 patch ROM and six audible channels, filters the
register map, applies the board reset/oscillator rules, and schedules one update
per 72 resonator clocks. The wrapper uses the exposed `OPLL` state to avoid a
second integer-rate adjustment, preserve vibrato across the board's sound reset,
and suppress the generic OPLL diagnostic phase-reset path's DC residue.
Keep the upstream source files unchanged when updating this dependency.
