# Draft: macOS runner input and portable reference oracle

Base: upstream NESRecomp `a39d9cd`.

## Changes
- Disable Apple's press-and-hold accent panel before creating a game window;
  link CoreFoundation through the shared runner CMake integration.
- Default scripted screenshots to the current directory on POSIX while
  retaining `NESRECOMP_SHOT_DIR` overrides and the Windows default.
- Load libretro cores through SDL's portable shared-object API.
- Provide a standalone CMake oracle build, checked output handling,
  explicit completion records, and pinned-Mesen v13 cycle/PPU extraction.

## Validation
- CMake/AppleClang oracle build passes on macOS.
- Mesen 0.9.9: Guardian Legend headless run completes all 120 frames, exit 0,
  with an explicit `frame_limit` completion record.
- Held-key behavior was manually confirmed in the original pinned game build.
- This rebased runner integration still needs a full game build and manual
  held-key/screenshot verification; Windows/Linux have not been tested here.

The oracle changes include capture correctness needed by the macOS test
workflow, beyond just replacing the platform loader. Review those separately
from the runner commit. No ROM or proprietary core is included.
