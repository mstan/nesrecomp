# Draft: macOS runner input and portable reference oracle

Base: upstream NESRecomp `13cd2d4`.

## Changes
- Disable Apple's press-and-hold accent panel before creating a game window;
  link CoreFoundation through the shared runner CMake integration.
- Default scripted screenshots to the current directory on POSIX while
  retaining `NESRECOMP_SHOT_DIR` overrides and the Windows default.
- Load libretro cores through SDL's portable shared-object API.
- Provide a standalone CMake oracle build, checked output handling,
  explicit completion records, and pinned-Mesen v13 cycle/PPU extraction.
- Link the runtime-boundary self-test with `-dead_strip` on Apple platforms
  (ld64 has no `--gc-sections`), so the existing self-test suite builds
  and runs on macOS via cmake + ctest.

## Validation
- CMake/AppleClang oracle build passes on macOS.
- Mesen 0.9.9: Guardian Legend headless run completes all 120 frames, exit 0,
  with an explicit `frame_limit` completion record.
- `tests/runtime_boundary` ctest (2 cases) passes on macOS with the fix.
- `launcher.c` and `input_script.c` compile-check clean with AppleClang.
- Held-key behavior was manually confirmed in the original pinned game build.
- This rebased runner integration still needs a full game build and manual
  held-key/screenshot verification; Windows/Linux have not been tested here.

The oracle changes include capture correctness needed by the macOS test
workflow, beyond just replacing the platform loader. Review those separately
from the runner commit. No ROM or proprietary core is included.
