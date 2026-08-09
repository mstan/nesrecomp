# nesrecomp tests

Reserved for synthetic ROM pattern tests (future).

Per-game regression tests live in each game repo's `tests/` directory.
The orchestrator script lives in `nesrecomp-release/run-tests.sh`.

The focused native self-tests remain standalone CMake projects, matching the
historical mapper test:

```sh
cmake -S tests/mod_runtime -B build/mod-runtime-tests
cmake --build build/mod-runtime-tests --config Release
ctest --test-dir build/mod-runtime-tests -C Release --output-on-failure
```

`external_rom_gate_selftest` generates synthetic 16 MiB fixtures at runtime;
it never requires or distributes a commercial ROM. It covers N64 z64/v64/n64
normalization, raw hashing, missing and wrong files, the PLAY-time rehash and
stale-plan clear, and package-scoped resource-ID uniqueness.
