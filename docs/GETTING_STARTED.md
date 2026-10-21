# Your first NESRecomp port

Use this guide to build the tools and establish a small, repeatable native
game baseline. No AI assistant is required. The worked path uses Windows x64,
Visual Studio 2022, PowerShell and one stock cartridge ROM.

For playing an existing port, use the [game list](../README.md#game-projects)
and that project's release instructions instead.

## 1. Install the tools and clone

Install Git for Windows, Python 3.11 or newer, and Visual Studio 2022 with
**Desktop development with C++** and its CMake tools. Open **Developer
PowerShell for VS 2022**. Check `git --version`, `python --version` and
`cmake --version` before continuing.

Use a development drive for the checkout and output. Replace the example
paths with yours:

```powershell
New-Item -ItemType Directory -Force D:\NESDev\Temp | Out-Null
$env:TEMP = 'D:\NESDev\Temp'
$env:TMP = $env:TEMP
Set-Location D:\NESDev
git clone --recurse-submodules https://github.com/mstan/nesrecomp.git
Set-Location nesrecomp
cmake -S recompiler -B build/recompiler -G 'Visual Studio 17 2022' -A x64
cmake --build build/recompiler --config Release --parallel
```

The recompiler is `build\recompiler\Release\NESRecomp.exe`. Keep your own
`.nes` ROM outside the repository, including its iNES/NES 2.0 header.
Cartridges need no BIOS. Famicom Disk System is a separate integration with
disk and BIOS requirements; start with an ordinary cartridge for this guide.

## 2. Describe the cartridge

First check for an [existing game project](../README.md#game-projects).
For a first, simple NROM cartridge, make a separate project folder and create
this starting `game.toml`:

```powershell
New-Item -ItemType Directory -Force D:\NESDev\MyFirstPort | Out-Null
@'
[game]
output_prefix = "my-first-port"
cycle_accurate = true
'@ | Set-Content D:\NESDev\MyFirstPort\game.toml -Encoding ascii
```

The compiler reads the cartridge header and records the ROM identity in its
generated metadata. More complex games need additional analysis and banking
configuration; use a nearby game's current configuration as a format
reference. Addresses and bank-switch rules copied from another game are not
valid defaults.

Start with a supported, simple board. The
[mapper reference](../runner/cyc/MAPPERS.md) describes implemented hardware
and its qualification limits. A registered mapper number alone does not
prove that every cartridge using it works.

## 3. Build a baseline host

The cycle backend is the default. Its standalone project can generate code
and build a headless host from a ROM and game configuration, before you add
a custom window or title-specific hooks:

```powershell
cmake -S runner/cyc/project -B D:\NESDev\MyFirstPort\build `
  -G 'Visual Studio 17 2022' -A x64 `
  -DNESRECOMP_ROM='D:/ROMs/My Game.nes' `
  -DNESRECOMP_GAME_CONFIG='D:/NESDev/MyFirstPort/game.toml' `
  -DNESRECOMP_HEADLESS=ON
cmake --build D:\NESDev\MyFirstPort\build --config Release --parallel
& D:\NESDev\MyFirstPort\build\Release\nes_game.exe `
  'D:\ROMs\My Game.nes' --frames 600 `
  --screenshot D:\NESDev\MyFirstPort\boot.png
```

Open `boot.png` to inspect the result. A successful compile or screenshot is
an initial observation, not proof of a working port. Use the
[cycle-project reference](../runner/cyc/PROJECTS.md) when filling the
configuration and integrating `nesrecomp_add_cycle_game` into your own
`CMakeLists.txt`. It documents the required paths and host hooks.

## 4. Establish one working scene

Drive boot, title selection and the first playable scene with the host's
`--input` schedule. The format is documented in
[`cyc_host.c`](../runner/cyc/cyc_host.c): each line names a frame and the
buttons held from then on. Compare with an independent emulator. Keep logs
and screenshots for passed, failed and untested behavior.

Resolve ROM identity, banking, interrupt and dispatch failures before adding
enhancements. Uncovered code can use the interpreter while you identify its
real control flow. Generated code belongs in build output; fix its inputs or
the generator instead of editing it by hand.

Once that baseline works, integrate the SDL desktop host and launcher using
the cycle-project guide. Add one feature at a time and rerun the same route.
Widescreen needs actual scenery/object/HUD integration. Four controller
slots do not create multiplayer in a game that has none.

Keep one stock revision for the first port. Mods and alternate content need
explicit game integration and compatible saves; arbitrary ROM variants do
not become supported by changing the filename. ROMs, extracted assets,
generated code and personal saves stay out of Git.
