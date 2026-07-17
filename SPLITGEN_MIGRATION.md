# Split-gen migration (real parallel-compilable generated C)

The recompiler now emits the generated game code as **genuinely independent
translation units** instead of the old single umbrella that textually
`#include`d its per-bank parts:

- `<prefix>_full_decls.h` — shared declarations (includes, flag macros,
  `extern uint16_t g_rti_target`, symbol aliases, all `func_*` + `*_body`
  forward declarations).
- `<prefix>_full_bankNN.c` — per-PRG-bank standalone TUs; banks over
  ~1 MiB sub-shard further into `<prefix>_full_bankNN_partPP.c`.
- `<prefix>_full.c` — a tiny umbrella TU (the `g_rti_target` definition +
  `func_RESET/NMI/IRQ`).

`func_XXXX_body` helpers are now **externally linked** and forward-declared.
This is required, not cosmetic: a `_body` can be called directly (the
inline-pointer JSR idiom) from another function that may now live in a
different TU. It was only safe before because everything was one TU.

Measured on Metroid: 13 MB single TU → 18 parallel TUs (none > 1.5 MB);
`-j16` compile **29.5s → 10.2s** (~2.9×). Generated function bytes unchanged.

## Migrating a game to the split output

A game keeps building on its committed monolithic `<prefix>_full.c` until it
is bumped to this framework and regenerated. To migrate a game:

1. **CMake** — replace the explicit generated listing

   ```cmake
   generated/<prefix>_full.c
   generated/<prefix>_dispatch.c
   ```

   with a glob that matches the umbrella, bank TUs, sub-shards, and dispatch:

   ```cmake
   file(GLOB GAME_GEN CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/generated/<prefix>_full*.c"
        "${CMAKE_SOURCE_DIR}/generated/<prefix>_dispatch.c")
   ```

   and add `${GAME_GEN}` to the executable.

2. **recomp_stack** — the framework promoted `recomp_stack.c/.h` into the
   shared runner sources (single source of truth). If the game repo still
   ships its own local `recomp_stack.c/.h`, delete them and drop the
   `recomp_stack.c` line from its `CMakeLists.txt`, or the link fails with
   duplicate symbols. The shared header is a compatible superset.

See `MetroidNESRecomp` for a worked example.
