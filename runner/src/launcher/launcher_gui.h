// launcher_gui.h — RmlUi pre-boot launcher for nesrecomp games.
//
// Shown in its own SDL/OpenGL window before the recompiled game boots, modeled
// structurally on the snesrecomp / psxrecomp launchers. The user picks/verifies
// a ROM (iNES metadata + CRC32 badge), chooses per-player input devices, manages
// the SRAM save, tunes display/audio settings, then presses PLAY. Chosen values
// are written back into the caller-owned NesLauncherSettings and the resolved ROM
// path; the game's launcher.c maps them onto config.ini / keybinds.ini.
//
// Game-AGNOSTIC: edits the shared subset below, never a game's private state.
// This module does NOT own the window/GL context — the caller passes an
// already-current GL 3.3 core context (kept as a pure overlay).

#pragma once

#include <cstddef>
#include <cstdint>

struct SDL_Window;

namespace nes_launcher {

enum class Result {
    Launch,       // user pressed PLAY — boot out_rom_path with the edited settings
    Quit,         // user closed the window — caller should exit
    Unavailable,  // launcher could not initialise (assets/GL) — caller boots as if skipped
};

// Per-player input source. Mirrors the dashboard's DEVICE dropdown.
enum class InputSource : int {
    None     = 0,
    Keyboard = 1,
    Gamepad  = 2,
};

// Editable settings subset. Seeded by the caller from its config, mutated in
// place, read back on Result::Launch. A game ignores fields it doesn't use.
struct NesLauncherSettings {
    // --- Display ---
    int  window_scale  = 3;       // 1..N integer scale (NES native 256x240)
    int  fullscreen    = 0;       // 0 windowed, 1 borderless-desktop
    bool integer_scale = true;    // snap to whole-pixel multiples
    bool linear_filter = false;   // bilinear vs nearest
    int  renderer      = 0;       // game output: 0 accelerated, 1 software
    bool widescreen    = false;   // experimental 16:9 (per-game)

    // --- Audio ---
    int  volume        = 100;     // 0..100 (game maps to its own scale)

    // --- HD texture pack (Mesen-format) ---
    bool hdpack_enabled = false;  // load an HD pack at boot
    char hdpack_dir[512] = {0};   // folder containing the pack's hires.txt

    // --- Controllers (2 players) ---
    InputSource player_src[2] = { InputSource::Keyboard, InputSource::Gamepad };
    int  deadzone[2]   = { 30, 30 };   // 0..100 percent of stick range

    // --- Launcher behaviour ---
    bool skip_launcher = false;   // boot straight to the game next time (pass
                                  // --launcher or set SkipLauncher=0 to undo)
};

// Static facts about the game being configured. Drives the title, the ROM
// verification badge, the mapper-board label, and the SAVES panel.
struct GameInfo {
    const char* name             = nullptr;  // "The Legend of Zelda"
    const char* region           = nullptr;  // "NTSC-U (USA)"

    // ROM verification: CRC32 over the post-iNES-header bytes. 0 / false => the
    // "ROM verified" badge is suppressed (any readable iNES ROM is accepted).
    uint32_t    expected_crc     = 0;
    bool        has_expected_crc = false;

    // Optional human board name override (e.g. "NROM-256"). When null, the
    // launcher derives a name from the iNES mapper number ("Mapper 1 (MMC1)").
    const char* mapper_board     = nullptr;

    // SRAM. uses_sram drives the SAVES panel: when false the panel shows "This
    // game does not use SRAM". When true, save_basename names the save file the
    // panel manages (saves/<save_basename>.srm) — the same stem the runtime
    // save_ram backend uses, so the launcher and runtime agree on one file.
    bool        uses_sram        = false;
    const char* save_basename    = nullptr;  // e.g. "zelda"

    // Whether this game has an experimental widescreen path (gates the Settings
    // → Widescreen toggle). Set per-game (e.g. SMB); default hidden.
    bool        widescreen_supported = false;

    // Whether HD texture packs are offered for this build (gates the entire
    // Settings → HD Texture Pack panel). Default true; a stock build that must
    // not load packs (e.g. unpatched Zelda) sets this false.
    bool        hdpack_supported = true;

    // Optional game-specific "password / mantra" save (e.g. Faxanadu). When
    // password_save_path is set, the SAVES panel shows the password text
    // (read-only + edit/confirm) rather than the binary SRAM file. The file is a
    // single line of text. Independent of uses_sram.
    const char* password_save_path  = nullptr;
    const char* password_save_label = nullptr;  // e.g. "Password"
};

// Run the launcher loop to completion. `gl_context` is an SDL_GLContext (void*)
// already created and current on `window`. `io` is seeded with the effective
// settings and, on Result::Launch, updated in place. `assets_dir` holds
// launcher.rml / fonts / img. On Result::Launch, `out_rom_path` receives the
// ROM to boot. `initial_rom` may be a cached path (rom.cfg) to pre-populate the
// dashboard, or null/empty.
Result run(SDL_Window* window, void* gl_context,
           NesLauncherSettings& io, const GameInfo& game,
           const char* assets_dir, const char* initial_rom,
           char* out_rom_path, size_t out_rom_path_len);

} // namespace nes_launcher
