// launcher_capi.h — C-callable entry point for the RmlUi launcher.
//
// launcher.c (C) can't speak the C++ nes_launcher::run() API directly, so this
// shim wraps it: it creates its own SDL/GL window, runs the launcher, maps a
// plain-C settings struct in/out, and tears the window down — leaving launcher.c
// to seed/read the struct and pick up the chosen ROM path.

#ifndef NESRECOMP_LAUNCHER_CAPI_H
#define NESRECOMP_LAUNCHER_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mirrors nes_launcher::NesLauncherSettings as plain C (bools as int).
typedef struct NesLauncherCSettings {
    int  window_scale;      // 1..N
    int  fullscreen;        // 0 windowed, 1 borderless-desktop (legacy RmlUi launcher: no exclusive mode)
    int  integer_scale;     // bool
    int  linear_filter;     // bool
    int  renderer;          // game output: 0 accelerated, 1 software
    int  widescreen;        // experimental 16:9 (only meaningful if supported)
    int  volume;            // 0..100
    int  player_src[2];     // 0 none, 1 keyboard, 2 gamepad
    int  deadzone[2];       // 0..100
    int  skip_launcher;     // bool: boot straight to the game next time
    int  hdpack_enabled;    // bool: load a Mesen HD texture pack
    char hdpack_dir[512];   // folder containing the pack's hires.txt
} NesLauncherCSettings;

typedef struct NesLauncherCGameInfo {
    const char* name;
    const char* region;
    uint32_t    expected_crc;
    int         has_expected_crc;
    const char* mapper_board;   // optional "NROM-256" override; NULL => derive
    int         uses_sram;      // show SAVES panel
    const char* save_basename;  // saves/<save_basename>.srm
    int         widescreen_supported;  // show the experimental Widescreen toggle
    int         hdpack_supported;      // show the HD-pack panel (1=default; 0 hides it,
                                       // e.g. a stock build that must not load packs)

    // Optional game-specific "password / mantra" save (e.g. Faxanadu, which saves
    // via a password rather than battery SRAM). When password_save_path is non-NULL
    // the SAVES panel shows the password text (read-only, editable behind an edit
    // icon + a confirm step) instead of the binary SRAM file UI. The file is a
    // single line of text; the launcher reads it to display and rewrites it on
    // confirm. Independent of uses_sram.
    const char* password_save_path;   // abs path to the 1-line password file
    const char* password_save_label;  // panel label, e.g. "Password" / "Mantra"
} NesLauncherCGameInfo;

// Returns: 0 = LAUNCH (boot out_rom_path with the edited *io),
//          1 = QUIT (caller should exit),
//          2 = UNAVAILABLE (assets/GL failed — caller boots as if skipped).
int nes_launcher_run_window(const char* window_title,
                            NesLauncherCSettings* io,
                            const NesLauncherCGameInfo* game,
                            const char* assets_dir,
                            const char* initial_rom,
                            char* out_rom_path, size_t out_rom_path_len);

#ifdef __cplusplus
}
#endif

#endif // NESRECOMP_LAUNCHER_CAPI_H
