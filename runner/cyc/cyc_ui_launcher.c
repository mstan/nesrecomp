/*
 * cyc_ui_launcher.c - recomp-ui's pre-boot launcher for a cycle game (cyc_ui.h).
 *
 * The launcher edits a RecompLauncherCSettings; this file copies the host's
 * settings in and the edits back out, and the host persists them in its own
 * config.ini (recomp-ui's host-owned binding mode, GameInfo.settings_bindings):
 * every controller button and host shortcut, keyboard and controller.
 */
#include "cyc_ui.h"
#include "cyc_settings.h"

#include "cyc_input.h"
#include "cyc_recomp.h"
#include "launcher_profile.h"
#include "recomp_launcher.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CYC_GAME_PLAYERS
#define CYC_GAME_PLAYERS CYC_INPUT_PLAYERS
#endif

/* The NES profile's button order (recomp-ui consoles/nes/nes_profile.h
 * kNesPadButtons: Up Down Left Right A B Start Select) -> cyc_input.h's. */
static const int SPEC_TO_CYC[8] = { 4, 5, 6, 7, 0, 1, 3, 2 };

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(RECOMP_LAUNCHER_PAD_BUTTON(3) == CYC_PAD_BUTTON(3), "pad encoding");
_Static_assert(RECOMP_LAUNCHER_PAD_AXIS(5, 1) == CYC_PAD_AXIS(5, 1), "pad encoding");
_Static_assert(RECOMP_LAUNCHER_PAD_BUTTON_COMBO(9) == CYC_PAD_COMBO(9), "pad encoding");
_Static_assert(CYC_INPUT_PLAYERS <= RECOMP_LAUNCHER_MAX_PLAYERS, "players");
_Static_assert(CYC_SC_COUNT <= RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS, "shortcuts");
#endif

/* The shortcuts the launcher lists, in its order: Disk only for a disk program. */
static int shortcut_list(bool fds, int out[CYC_SC_COUNT])
{
    int n = 0;
    for (int i = 0; i < CYC_SC_COUNT; ++i)
        if (i != CYC_SC_DISK || fds) out[n++] = i;
    return n;
}

static void to_launcher(const CycSettings *s, RecompLauncherCSettings *io, const int *shortcuts, int count)
{
    io->window_scale = s->window_scale;
    io->fullscreen = s->fullscreen;
    io->integer_scale = s->integer_scale;
    io->linear_filter = s->linear_filter;
    io->enable_audio = s->audio_enabled;
    io->volume = s->volume;
    io->skip_launcher = s->skip_launcher;
    io->hdpack_enabled = s->hdpack_enabled;
    snprintf(io->hdpack_dir,sizeof io->hdpack_dir,"%s",s->hdpack_dir);
#ifdef RECOMP_LAUNCHER_HAS_ZAPPER_SETTINGS
    io->zapper_mouse = s->zapper_mouse ? 1 : -1;
    io->zapper_crosshair = s->zapper_crosshair ? 1 : -1;
#elif defined(CYC_GAME_ZAPPER_PORT)
#error Zapper builds require recomp-ui with host-owned Zapper settings
#endif
    snprintf(io->bios_path, sizeof(io->bios_path), "%s", s->fds_bios);
    for (int p = 0; p < CYC_INPUT_PLAYERS; ++p) {
        io->player_src[p] = s->bind.source[p];
        io->deadzone[p] = s->bind.deadzone[p];
        snprintf(io->player_gamepad_guid[p], sizeof(io->player_gamepad_guid[p]), "%s", s->bind.device[p]);
        for (int b = 0; b < 8; ++b) {
            io->player_key_bind[p][b] = s->bind.button[p][SPEC_TO_CYC[b]].key;
            io->player_pad_bind[p][b] = s->bind.button[p][SPEC_TO_CYC[b]].pad;
        }
    }
    for (int i = 0; i < count; ++i) {
        io->assist_key_bind[i] = s->bind.shortcut[shortcuts[i]].key;
        io->assist_pad_bind[i] = s->bind.shortcut[shortcuts[i]].pad;
    }
}

static void from_launcher(const RecompLauncherCSettings *io, CycSettings *s, const int *shortcuts, int count)
{
    s->window_scale = io->window_scale < 1 ? 1 : io->window_scale > 8 ? 8 : io->window_scale;
    s->fullscreen = io->fullscreen < 0 ? 0 : io->fullscreen > 2 ? 2 : io->fullscreen;
    s->integer_scale = io->integer_scale != 0;
    s->linear_filter = io->linear_filter != 0;
    s->audio_enabled = io->enable_audio != 0;
    s->volume = io->volume < 0 ? 0 : io->volume > 100 ? 100 : io->volume;
    s->skip_launcher = io->skip_launcher != 0;
    s->hdpack_enabled = io->hdpack_enabled != 0;
    snprintf(s->hdpack_dir,sizeof s->hdpack_dir,"%s",io->hdpack_dir);
#ifdef RECOMP_LAUNCHER_HAS_ZAPPER_SETTINGS
    s->zapper_mouse = io->zapper_mouse >= 0;
    s->zapper_crosshair = io->zapper_crosshair >= 0;
#endif
    snprintf(s->fds_bios, sizeof(s->fds_bios), "%s", io->bios_path);
    for (int p = 0; p < CYC_INPUT_PLAYERS; ++p) {
        s->bind.source[p] = io->player_src[p] < 0 ? 0 : io->player_src[p] > 2 ? 2 : io->player_src[p];
        s->bind.deadzone[p] = io->deadzone[p] < 0 ? 0 : io->deadzone[p] > 100 ? 100 : io->deadzone[p];
        snprintf(s->bind.device[p], sizeof(s->bind.device[p]), "%s", io->player_gamepad_guid[p]);
        for (int b = 0; b < 8; ++b) {
            s->bind.button[p][SPEC_TO_CYC[b]].key = io->player_key_bind[p][b];
            s->bind.button[p][SPEC_TO_CYC[b]].pad = io->player_pad_bind[p][b];
        }
    }
    for (int i = 0; i < count; ++i) {
        s->bind.shortcut[shortcuts[i]].key = io->assist_key_bind[i];
        s->bind.shortcut[shortcuts[i]].pad = io->assist_pad_bind[i];
    }
}

/* recomp-ui's BIOS verdict for the image the launcher has selected: the host's
 * own lookup (cyc_fds_bios.h) with the player's pick as the saved path, so the
 * launcher shows exactly what a start would use. A cartridge needs none. */
int cyc_ui_bios_verify(void *ctx, const char *bios_path, const char *rom_path, RecompLauncherCBiosVerify *out)
{
    const CycFdsBiosLookup *base = (const CycFdsBiosLookup *)ctx;
    memset(out, 0, sizeof(*out));
    if (!base) return 0;
    if (!base->compiled_crc && !cyc_fds_image_file(rom_path)) {
        out->ok = 1;
        out->not_needed = 1;
        return 1;
    }
    CycFdsBiosResult r;
    if (base->explicit_path && *base->explicit_path) {
        /* --fds-bios wins this run; a pick is still checked, and saved. */
        if (bios_path && *bios_path && cyc_fds_bios_check(bios_path, base->compiled_crc, false, &r) != CYC_FDS_BIOS_OK) {
            snprintf(out->detail, sizeof(out->detail), "%s", r.detail);
            return 1;
        }
        out->ok = cyc_fds_bios_locate(base, false, &r) == CYC_FDS_BIOS_OK;
        snprintf(out->detail, sizeof(out->detail), "%s (--fds-bios, this run)", r.detail);
        return 1;
    }
    CycFdsBiosLookup in = *base;
    in.saved_path = bios_path;
    out->ok = cyc_fds_bios_locate(&in, false, &r) == CYC_FDS_BIOS_OK;
    snprintf(out->detail, sizeof(out->detail), "%s", r.detail);
    return 1;
}

static int hex_nibble(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static const char *stem_of(const char *path, char *out, size_t n)
{
    const char *b = path, *s1 = strrchr(path, '/'), *s2 = strrchr(path, '\\');
    if (s1 && s1 + 1 > b) b = s1 + 1;
    if (s2 && s2 + 1 > b) b = s2 + 1;
    snprintf(out, n, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
    return out;
}

int cyc_ui_launcher(CycSettings *settings, const char *settings_path, const CycHostExtras *extras,
                    const char **rom_path, bool fds, const CycFdsBiosLookup *bios, const void *mods)
{
    static CycFdsBiosLookup bios_lookup;
    static const char *const BIOS_PATTERNS[] = { "*.rom", "*.bin", "*.*" };
    static RecompLauncherCSettings io, defaults;
    static RecompLauncherCGameInfo gi;
    static char out_rom[1024], name[256];
    static uint8_t sha[1][32];
    static const char *const FDS_PATTERNS[] = { "*.fds", "*.qd" };
    static const char *shortcut_labels[CYC_SC_COUNT];
    static int default_key[RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS], default_pad[RECOMP_LAUNCHER_MAX_ASSIST_BINDINGS];
    int shortcuts[CYC_SC_COUNT];
    int count = shortcut_list(fds, shortcuts);

    memset(&io, 0, sizeof(io));
    to_launcher(settings, &io, shortcuts, count);
    /* "Restore defaults" and the per-player / shortcut resets go back to these. */
    CycSettings d;
    cyc_settings_default(&d);
    memset(&defaults, 0, sizeof(defaults));
    to_launcher(&d, &defaults, shortcuts, count);
    for (int i = 0; i < count; ++i) {
        shortcut_labels[i] = cyc_shortcut_label(shortcuts[i]);
        default_key[i] = d.bind.shortcut[shortcuts[i]].key;
        default_pad[i] = d.bind.shortcut[shortcuts[i]].pad;
    }

    memset(&gi, 0, sizeof(gi));
    launcher_profile_apply("nes", &gi);
    if (cyc_native_display_name) snprintf(name, sizeof(name), "%s", cyc_native_display_name);
    else if (cyc_native_program_name) snprintf(name, sizeof(name), "%s", cyc_native_program_name);
    else stem_of(*rom_path ? *rom_path : "NES", name, sizeof(name));
    gi.name = name;
    gi.region = NULL;                 /* the identity is the image's SHA-256, not a region */
    gi.num_players = CYC_GAME_PLAYERS;
#ifdef CYC_GAME_ZAPPER_PORT
    gi.zapper = 1;
#endif
    gi.has_renderer = 0;              /* the cycle host has one SDL renderer */
#if defined(NESRECOMP_CYCLE_HDPACK) && !defined(NESRECOMP_CYCLE_HDPACK_MODS)
    gi.hdpack_supported = 1;
#else
    gi.hdpack_supported = 0;
#endif
    gi.has_integer_scale = 1;
    /* a game's view modes are a runtime-menu row (live), not a launcher toggle */
    gi.widescreen_supported = 0;
    (void)extras;
    gi.config_path = settings_path;
#ifdef CYC_GAME_PASSWORD_SAVE
    /* --config must not redirect an existing mantra away from the executable. */
    if (cyc_host_saves_enabled()) {
        static char password_path[1100];
        char *base = SDL_GetBasePath();
        if (base) {
            int n = snprintf(password_path, sizeof(password_path), "%s%s", base, CYC_GAME_PASSWORD_SAVE);
            SDL_free(base);
            if (n > 0 && (size_t)n < sizeof(password_path)) {
                gi.password_save_path = password_path;
#ifdef CYC_GAME_PASSWORD_SAVE_LABEL
                gi.password_save_label = CYC_GAME_PASSWORD_SAVE_LABEL;
#else
                gi.password_save_label = "Password";
#endif
            }
        }
    }
#endif
    /* the launcher's last-image memory beside config.ini, never in the cwd */
    static char rom_cache[1100];
    snprintf(rom_cache, sizeof(rom_cache), "%s", settings_path ? settings_path : "config.ini");
    char *cut = strrchr(rom_cache, '/'), *bcut = strrchr(rom_cache, '\\');
    if (bcut > cut) cut = bcut;
    if (cut) snprintf(cut + 1, sizeof(rom_cache) - (size_t)(cut + 1 - rom_cache), "rom.cfg");
    else snprintf(rom_cache, sizeof(rom_cache), "rom.cfg");
    gi.rom_cache_path = rom_cache;
    gi.settings_bindings = 1;         /* every binding is the host's (config.ini) */
    /* The Mods screen (a game built with mods; recomp-ui's RECOMP_UI_ENABLE_MODS). */
    gi.mods = (const RecompLauncherCModProvider *)mods;
    gi.default_settings = &defaults;
    gi.assist_binding_labels = shortcut_labels;
    gi.assist_binding_count = count;
    gi.assist_default_key_bind = default_key;
    gi.assist_default_pad_bind = default_pad;
    if (fds) {
        gi.platform = "FAMICOM DISK SYSTEM";
        gi.rom_noun = "Disk";
        gi.rom_patterns = FDS_PATTERNS;
        gi.num_rom_patterns = 2;
        gi.rom_filter_desc = "Famicom Disk System image (.fds, .qd)";
    }
    /* The FDS BIOS: the SYSTEM card and a "required" notice for a disk image
     * the lookup finds none for; a cartridge's verdict is "not needed". The
     * pick comes back in io.bios_path and is saved in config.ini by the host,
     * so the launcher writes no bios.cfg / rom.cfg sidecars. */
    if (bios) {
        bios_lookup = *bios;
        bios_lookup.saved_path = NULL;
        gi.has_bios = 1;
        gi.bios_name = "FDS BIOS";
        gi.bios_patterns = BIOS_PATTERNS;
        gi.num_bios_patterns = 3;
        gi.bios_filter_desc = "Famicom Disk System BIOS";
        gi.bios_verify_for_rom = cyc_ui_bios_verify;
        gi.bios_verify_ctx = &bios_lookup;
        gi.host_persists_paths = 1;
    }
#ifdef CYC_LAUNCHER_ROM_SHA256
    /* The image this program was compiled from, hashed the way the launcher
     * hashes (after an iNES header; FDS images whole). */
    {
        const char *hex = CYC_LAUNCHER_ROM_SHA256;
        bool ok = strlen(hex) == 64;
        for (int i = 0; ok && i < 32; ++i) {
            int hi = hex_nibble(hex[2 * i]), lo = hex_nibble(hex[2 * i + 1]);
            ok = hi >= 0 && lo >= 0;
            sha[0][i] = (uint8_t)(hi * 16 + lo);
        }
        if (ok) { gi.known_sha256 = (const uint8_t (*)[32])sha; gi.num_known_sha256 = 1; }
    }
#endif
    char title[300];
    snprintf(title, sizeof(title), "%s - Launcher", name);
#ifdef RECOMP_LAUNCHER_HAS_PRESERVE_SDL
    recomp_launcher_set_preserve_sdl(1);
#endif
    out_rom[0] = 0;
    int act = recomp_launcher_run_window(title, &io, &gi, ".", *rom_path ? *rom_path : "", out_rom, sizeof(out_rom));
    /* The edits come back whichever way the launcher closed. */
    from_launcher(&io, settings, shortcuts, count);
    if (act == RECOMP_LAUNCHER_RESULT_LAUNCH && out_rom[0]) *rom_path = out_rom;
    return act == RECOMP_LAUNCHER_RESULT_LAUNCH ? 0 : act == RECOMP_LAUNCHER_RESULT_QUIT ? 1 : 2;
}
