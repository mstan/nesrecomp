# README showcase sources

Feature/catalog review: **21 October 2026**. Beta describes NESRecomp;
individual game and enhancement qualifications remain separate.

- `super-mario-bros-four-player.png` is a fresh headless capture from the
  [published v1.12.3 Windows release](https://github.com/mstan/SuperMarioBrosNESRecomp/releases/tag/v1.12.3),
  checked against its published SHA-256 sum. The stock World ROM, shipped
  co-op package, four-player selection and an input schedule drive World 1-1.
  Capture uses `--mods-root`, `--coop 4`, `--frames 324` and `--present-out`.
  The last option captures the final composition, including all four actors;
  `--screenshot` captures the underlying console picture instead. No player
  position or game-success flags are patched.
- `super-mario-bros-ultrawide.png` is a separate 32:9 single-player capture
  from the maintained local game build. It uses the original area loader and
  scripted controller input. It does not depict co-op plus widescreen.
- `metroid-widescreen.png` comes from the
  [Metroid game gallery](https://github.com/mstan/MetroidNESRecomp/blob/main/docs/screenshots/metroid-widescreen.png).
- `zelda-voxel.webp` is the existing `docs/assets/voxel-3d.webp` capture from
  [The Legend of Zelda project](https://github.com/mstan/LegendOfZeldaNESRecomp).
  It shows the optional presentation renderer, separate from Mario co-op.

Automated launches used dummy video/audio and isolated writable folders.
Logs/input schedules are retained in the task workspace; ROMs, snapshots,
settings and binaries are not included in this documentation change.

The [Mario co-op guide](https://github.com/mstan/SuperMarioBrosNESRecomp/blob/master/docs/SIMULTANEOUS_COOP.md)
defines its shared 4:3 camera and mutually exclusive display/player mods.
The [netplay notes](https://github.com/mstan/SuperMarioBrosNESRecomp/blob/master/docs/NETPLAY.md)
and [cycle-host integration](../runner/cyc/PROJECTS.md) distinguish measured
multi-process/interactive checks from remaining separate-machine/WAN and
campaign qualification. A local screenshot is not evidence of a WAN match.
