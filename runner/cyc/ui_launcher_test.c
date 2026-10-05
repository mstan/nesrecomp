/* ui_launcher_test.c - the cycle host's side of recomp-ui's launcher
 * (cyc_ui_launcher.c) with the launcher itself replaced by a scripted one:
 * what the host hands the launcher (the NES profile in host-owned binding
 * mode, the FDS media names, the image's SHA-256, every binding and its
 * default), and that every edit a player makes there -- each controller
 * button's key and pad binding, each host shortcut's, the input sources,
 * the display and audio settings -- comes back into the host's settings and
 * survives a config.ini save and reload. Also: a launcher that closes keeps
 * the edits and quits, one that cannot open leaves everything as it was.
 * And the FDS BIOS (cyc_fds_bios.h): the identity check, the lookup order,
 * the launcher's verdict for a disk / a cartridge / a wrong pick, and the pick
 * saved as config.ini [FDS] Bios, used by the next start, and cleared.
 * Needs recomp-ui's headers only (the launcher's ABI); no window. */
#include "cyc_fds_bios.h"
#include "cyc_input.h"
#include "cyc_settings.h"
#include "cyc_ui.h"
#include "launcher_profile.h"
#include "recomp_launcher.h"
#include "../../common/nes_cart.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#define CHDIR(p) _chdir(p)
#define GETCWD(b, n) _getcwd(b, n)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define RMDIR(p) rmdir(p)
#define CHDIR(p) chdir(p)
#define GETCWD(b, n) getcwd(b, n)
#endif

static unsigned checks;
static bool saves_enabled = true;
bool cyc_host_saves_enabled(void) { return saves_enabled; }
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

/* cyc_recomp.h's program metadata, as a compiled FDS program has it */
const char *cyc_native_program_name = "TestGame";
const char *cyc_native_display_name = NULL;
const uint32_t cyc_native_fds_bios_crc32 = 0x5E607DCFu;

/* ---- the scripted launcher ---- */
static int script_result;
static RecompLauncherCGameInfo seen_game;
static RecompLauncherCSettings seen_io;
static char seen_initial[256];
static void (*script_edit)(RecompLauncherCSettings *io);
static const char *script_rom;
static const char *script_bios;        /* pick_bios(): the player's Select BIOS... */

int recomp_launcher_run_window(const char *title, RecompLauncherCSettings *io, const RecompLauncherCGameInfo *game,
                               const char *assets, const char *initial_rom, char *out, size_t out_len)
{
    (void)title; (void)assets;
    seen_game = *game;
    seen_io = *io;
    snprintf(seen_initial, sizeof(seen_initial), "%s", initial_rom ? initial_rom : "");
    if (script_edit) script_edit(io);
    if (script_rom) snprintf(out, out_len, "%s", script_rom);
    return script_result;
}
void recomp_launcher_set_preserve_sdl(int preserve) { (void)preserve; }

/* The player's edits: every row of the Controls page and the settings. */
static void edit_everything(RecompLauncherCSettings *io)
{
    io->window_scale = 5;
    io->fullscreen = 1;
    io->integer_scale = 0;
    io->linear_filter = 1;
    io->enable_audio = 0;
    io->volume = 40;
    io->skip_launcher = 1;
    io->hdpack_enabled = 0;
    snprintf(io->hdpack_dir,sizeof io->hdpack_dir,"F:/HD Packs/Zelda");
#ifdef RECOMP_LAUNCHER_HAS_ZAPPER_SETTINGS
    io->zapper_mouse = -1;
    io->zapper_crosshair = -1;
#endif
    io->player_src[0] = 2;
    io->player_src[1] = 0;
    io->deadzone[0] = 12;
    snprintf(io->player_gamepad_guid[0], sizeof(io->player_gamepad_guid[0]), "030000005e0400008e02000000007801");
    for (int p = 0; p < 2; ++p)
        for (int b = 0; b < 8; ++b) {
            io->player_key_bind[p][b] = SDL_SCANCODE_A + p * 8 + b;
            io->player_pad_bind[p][b] = b == 6 ? RECOMP_LAUNCHER_PAD_AXIS(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1)
                                      : b == 7 ? RECOMP_LAUNCHER_PAD_BUTTON_COMBO((1 << 4) | (1 << 6))
                                               : RECOMP_LAUNCHER_PAD_BUTTON(b + p);
        }
    for (int i = 0; i < CYC_SC_COUNT; ++i) {
        io->assist_key_bind[i] = SDL_SCANCODE_F1 + i;
        io->assist_pad_bind[i] = RECOMP_LAUNCHER_PAD_BUTTON(SDL_CONTROLLER_BUTTON_Y + i);
    }
}

/* ---- the FDS BIOS: identity, lookup order, the launcher's verdict, config.ini ---- */
static void write_bytes(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    CHECK(f);
    CHECK(fwrite(data, 1, n, f) == n);
    fclose(f);
}
static void pick_bios(RecompLauncherCSettings *io) { snprintf(io->bios_path, sizeof(io->bios_path), "%s", script_bios); }
static void clear_bios(RecompLauncherCSettings *io) { io->bios_path[0] = 0; }

static void bios_checks(const char *dir)
{
    /* A scratch tree, and the current directory inside it so the lookup's
     * last candidate (bios/disksys.rom here) is under the test's control. */
    char root[1024], img_dir[1024], beside_dir[1024], good[1024], other[1024], wrong[1024], gone[1024];
    char image[1024], cart[1024], beside[1024], toml_bios[1024], toml[1024], cfg[1024], here[1024];
    snprintf(root, sizeof(root), "%s/ui_launcher_bios", dir);
    MKDIR(root);
    snprintf(img_dir, sizeof(img_dir), "%s/disks", root);
    MKDIR(img_dir);
    snprintf(beside_dir, sizeof(beside_dir), "%s/bios", img_dir);
    MKDIR(beside_dir);
    snprintf(good, sizeof(good), "%s/my disksys.rom", root);
    snprintf(other, sizeof(other), "%s/other.rom", root);
    snprintf(wrong, sizeof(wrong), "%s/wrong.rom", root);
    snprintf(gone, sizeof(gone), "%s/gone.rom", root);
    snprintf(image, sizeof(image), "%s/game.fds", img_dir);
    snprintf(cart, sizeof(cart), "%s/cart.nes", root);
    snprintf(beside, sizeof(beside), "%s/disksys.rom", beside_dir);
    snprintf(toml_bios, sizeof(toml_bios), "%s/t.rom", root);
    snprintf(toml, sizeof(toml), "%s/t.toml", root);
    snprintf(cfg, sizeof(cfg), "%s/config.ini", root);
    remove(beside);
    remove(gone);
    remove(cfg);

    static uint8_t bios[8192], bios2[8192];
    for (size_t i = 0; i < sizeof(bios); ++i) bios[i] = (uint8_t)(i * 7 + 3), bios2[i] = (uint8_t)(i * 5 + 1);
    const uint32_t crc = nes_crc32(0, bios, sizeof(bios)), crc2 = nes_crc32(0, bios2, sizeof(bios2));
    write_bytes(good, bios, sizeof(bios));
    write_bytes(other, bios2, sizeof(bios2));
    write_bytes(wrong, bios, 16);
    write_bytes(image, bios, 64);                            /* not parsed: the compiled CRC says "disk" */
    static const uint8_t ines[32] = { 'N', 'E', 'S', 0x1A, 1, 1 };
    write_bytes(cart, ines, sizeof(ines));

    /* identity: the compiled CRC, else <bios>.toml's, else the known disksys.rom */
    CycFdsBiosResult r;
    CHECK(cyc_fds_bios_check(good, crc, true, &r) == CYC_FDS_BIOS_OK && r.data && r.size == 8192 && r.crc == crc);
    CHECK(!memcmp(r.data, bios, sizeof(bios)));
    free(r.data);
    CHECK(cyc_fds_bios_check(other, crc, false, &r) == CYC_FDS_BIOS_WRONG && !r.data && r.crc == crc2);
    char want[64];
    snprintf(want, sizeof(want), "CRC %08X", (unsigned)crc2);
    CHECK(strstr(r.detail, "Not the FDS BIOS:") && strstr(r.detail, want));
    CHECK(cyc_fds_bios_check(wrong, crc, false, &r) == CYC_FDS_BIOS_WRONG && r.size == 16);
    CHECK(cyc_fds_bios_check(gone, crc, false, &r) == CYC_FDS_BIOS_MISSING);
    uint32_t size;
    CHECK(cyc_fds_bios_expected(good, 0, &size) == 0x5E607DCFu && size == 8192);    /* no .toml */
    CHECK(cyc_fds_bios_check(good, 0, false, &r) == CYC_FDS_BIOS_WRONG);
    write_bytes(toml_bios, bios, sizeof(bios));
    FILE *t = fopen(toml, "w");
    CHECK(t);
    fprintf(t, "size = 8192\ncrc32 = \"0x%08X\"\n", (unsigned)crc);
    fclose(t);
    CHECK(cyc_fds_bios_expected(toml_bios, 0, &size) == crc && size == 8192);
    CHECK(cyc_fds_bios_check(toml_bios, 0, false, &r) == CYC_FDS_BIOS_OK);

    /* the lookup: --fds-bios > config.ini > game.toml (headless); no file name is searched for */
    char orig[1024];
    CHECK(GETCWD(orig, (int)sizeof(orig)));
    CHECK(CHDIR(root) == 0);
    CycFdsBiosLookup in = { NULL, NULL, NULL, crc };
    CHECK(cyc_fds_bios_locate(&in, false, &r) == CYC_FDS_BIOS_MISSING && !r.source);
    CHECK(strstr(r.detail, "No FDS BIOS selected"));
    MKDIR("bios");
    write_bytes("bios/disksys.rom", bios, sizeof(bios));       /* the known names are never looked for */
    write_bytes(beside, bios, sizeof(bios));
    CHECK(cyc_fds_bios_locate(&in, false, &r) == CYC_FDS_BIOS_MISSING);
    remove("bios/disksys.rom");
    remove(beside);
    in.compiled_path = good;
    CHECK(cyc_fds_bios_locate(&in, false, &r) == CYC_FDS_BIOS_OK && !strcmp(r.source, "game.toml [fds] bios"));
    in.compiled_path = wrong;                                  /* the first file found decides */
    CHECK(cyc_fds_bios_locate(&in, false, &r) == CYC_FDS_BIOS_WRONG && !strcmp(r.path, wrong));
    in.saved_path = good;
    CHECK(cyc_fds_bios_locate(&in, true, &r) == CYC_FDS_BIOS_OK && !strcmp(r.path, good) && r.data);
    free(r.data);
    CHECK(!strcmp(r.source, "config.ini [FDS] Bios"));
    in.saved_path = gone;                                      /* a saved file that is gone: asked again */
    in.compiled_path = NULL;
    CHECK(cyc_fds_bios_locate(&in, false, &r) == CYC_FDS_BIOS_MISSING);
    CHECK(strstr(r.detail, "selected file is gone"));
    in.explicit_path = other;                                  /* --fds-bios is the only candidate */
    in.saved_path = good;
    CHECK(cyc_fds_bios_locate(&in, false, &r) == CYC_FDS_BIOS_WRONG && !strcmp(r.source, "--fds-bios"));
    in.explicit_path = gone;
    CHECK(cyc_fds_bios_locate(&in, false, &r) == CYC_FDS_BIOS_MISSING && r.source);

    /* the launcher: what it is handed, its verdict, the pick back into config.ini */
    CycFdsBiosLookup host = { NULL, NULL, NULL, crc };
    CycSettings s;
    cyc_settings_default(&s);
    const char *rom = image;
    script_edit = NULL;
    script_rom = NULL;
    script_result = RECOMP_LAUNCHER_RESULT_QUIT;
    CHECK(cyc_ui_launcher(&s, cfg, NULL, &rom, true, &host, NULL) == 1);
    CHECK(seen_game.has_bios == 1 && seen_game.host_persists_paths == 1);
    CHECK(seen_game.bios_name && !strcmp(seen_game.bios_name, "FDS BIOS"));
    CHECK(seen_game.num_bios_patterns == 3 && !strcmp(seen_game.bios_patterns[2], "*.*"));
    CHECK(seen_game.bios_verify_for_rom == cyc_ui_bios_verify && seen_game.bios_verify_ctx);
    CHECK(seen_io.bios_path[0] == 0);
    RecompLauncherCBiosVerify v;
    void *ctx = seen_game.bios_verify_ctx;
    CHECK(cyc_ui_bios_verify(ctx, "", image, &v) && !v.ok && !v.not_needed);     /* required, none found */
    CHECK(cyc_ui_bios_verify(ctx, other, image, &v) && !v.ok && strstr(v.detail, "Not the FDS BIOS:"));
    CHECK(strstr(v.detail, want));
    CHECK(cyc_ui_bios_verify(ctx, good, image, &v) && v.ok && !v.not_needed);
    write_bytes(beside, bios, sizeof(bios));                 /* a disksys.rom beside the disk: still asked */
    CHECK(cyc_ui_bios_verify(ctx, "", image, &v) && !v.ok);
    remove(beside);
    CycFdsBiosLookup cart_host = { NULL, NULL, NULL, 0 };  /* a build that also runs cartridges */
    CHECK(cyc_ui_bios_verify(&cart_host, "", cart, &v) && v.ok && v.not_needed);
    CycFdsBiosLookup cli = { good, NULL, NULL, crc };     /* --fds-bios: still checks a pick */
    CHECK(cyc_ui_bios_verify(&cli, "", image, &v) && v.ok && strstr(v.detail, "--fds-bios"));
    CHECK(cyc_ui_bios_verify(&cli, other, image, &v) && !v.ok);

    /* Select BIOS... -> config.ini [FDS] Bios, read back and handed in next time */
    script_bios = good;
    script_edit = pick_bios;
    CHECK(cyc_ui_launcher(&s, cfg, NULL, &rom, true, &host, NULL) == 1);
    CHECK(!strcmp(s.fds_bios, good));
    CHECK(cyc_settings_save(&s, cfg, NULL));
    CycSettings back;
    cyc_settings_default(&back);
    CHECK(cyc_settings_load(&back, cfg, stderr, NULL) && !strcmp(back.fds_bios, good));
    script_edit = NULL;
    CHECK(cyc_ui_launcher(&back, cfg, NULL, &rom, true, &host, NULL) == 1);
    CHECK(!strcmp(seen_io.bios_path, good));
    CycFdsBiosLookup start = { NULL, back.fds_bios, NULL, crc };  /* the next start uses it */
    CHECK(cyc_fds_bios_locate(&start, false, &r) == CYC_FDS_BIOS_OK && !strcmp(r.path, good));
    /* Clear: the key stays, empty */
    script_edit = clear_bios;
    CHECK(cyc_ui_launcher(&back, cfg, NULL, &rom, true, &host, NULL) == 1 && !back.fds_bios[0]);
    CHECK(cyc_settings_save(&back, cfg, NULL));
    cyc_settings_default(&s);
    snprintf(s.fds_bios, sizeof(s.fds_bios), "stale");
    CHECK(cyc_settings_load(&s, cfg, stderr, NULL) && !s.fds_bios[0]);
    script_edit = NULL;
    script_bios = NULL;

    CHECK(CHDIR(orig) == 0);
    remove(cfg);
    remove(good); remove(other); remove(wrong); remove(image); remove(cart); remove(toml_bios); remove(toml);
    snprintf(here, sizeof(here), "%s/bios", root);
    RMDIR(here); RMDIR(beside_dir); RMDIR(img_dir); RMDIR(root);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char cfg[1024], image[1024];
    snprintf(cfg, sizeof(cfg), "%s/ui_launcher_test.ini", dir);
    snprintf(image, sizeof(image), "%s/game.fds", dir);
    remove(cfg);

    /* what the host hands over */
    CycSettings s;
    cyc_settings_default(&s);
    const char *rom = image;
    script_result = RECOMP_LAUNCHER_RESULT_QUIT;
    CHECK(cyc_ui_launcher(&s, cfg, NULL, &rom, true, NULL, NULL) == 1);
    CHECK(!strcmp(seen_initial, image));
    CHECK(seen_game.settings_bindings == 1);
    CHECK(!strcmp(seen_game.theme, "nes") && !strcmp(seen_game.platform, "FAMICOM DISK SYSTEM"));
    CHECK(!strcmp(seen_game.rom_noun, "Disk") && seen_game.num_rom_patterns == 2);
    CHECK(!strcmp(seen_game.rom_patterns[0], "*.fds") && !strcmp(seen_game.rom_patterns[1], "*.qd"));
    CHECK(!strcmp(seen_game.name, "TestGame"));
    CHECK(seen_game.assist_binding_count == CYC_SC_COUNT);            /* Disk offered: an FDS program */
    CHECK(!strcmp(seen_game.assist_binding_labels[0], cyc_shortcut_label(CYC_SC_DISK)));
    CHECK(seen_game.assist_default_key_bind[0] == SDL_SCANCODE_D);
    CHECK(seen_game.assist_default_pad_bind[0] == RECOMP_LAUNCHER_PAD_BUTTON(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    CHECK(seen_game.default_settings && seen_game.default_settings->player_key_bind[0][4] == SDL_SCANCODE_Z);
    CHECK(seen_game.rom_cache_path && strstr(seen_game.rom_cache_path, "rom.cfg"));
    /* An external config path must not move a password save away from the exe. */
    char *base = SDL_GetBasePath();
    char password_path[1100];
    CHECK(base);
    snprintf(password_path, sizeof(password_path), "%stest-password.srm", base);
    SDL_free(base);
    CHECK(seen_game.password_save_path && !strcmp(seen_game.password_save_path, password_path));
    CHECK(!strcmp(seen_game.password_save_label, "Mantra"));
    saves_enabled = false;
    CHECK(cyc_ui_launcher(&s, cfg, NULL, &rom, true, NULL, NULL) == 1);
    CHECK(!seen_game.password_save_path && !seen_game.password_save_label);
    saves_enabled = true;
    /* the profile's button order (Up Down Left Right A B Start Select) */
    CHECK(seen_io.player_key_bind[0][0] == SDL_SCANCODE_UP && seen_io.player_key_bind[0][4] == SDL_SCANCODE_Z);
    CHECK(seen_io.player_key_bind[0][6] == SDL_SCANCODE_RETURN && seen_io.player_key_bind[0][7] == SDL_SCANCODE_BACKSLASH);
    CHECK(seen_io.player_pad_bind[0][5] == RECOMP_LAUNCHER_PAD_BUTTON(SDL_CONTROLLER_BUTTON_X));
    CHECK(seen_io.player_src[0] == 1 && seen_io.player_src[1] == 2 && seen_io.volume == 100);
#ifdef RECOMP_LAUNCHER_HAS_ZAPPER_SETTINGS
    CHECK(seen_io.zapper_mouse == 1 && seen_io.zapper_crosshair == 1);
#endif
#ifdef CYC_LAUNCHER_ROM_SHA256
    CHECK(seen_game.num_known_sha256 == 1 && seen_game.known_sha256[0][0] == 0xAB && seen_game.known_sha256[0][31] == 0x01);
#endif
    /* a cartridge: no Disk row, no disk media names */
    cyc_settings_default(&s);
    CHECK(cyc_ui_launcher(&s, cfg, NULL, &rom, false, NULL, NULL) == 1);
    CHECK(seen_game.assist_binding_count == CYC_SC_COUNT - 1);
    CHECK(strcmp(seen_game.assist_binding_labels[0], cyc_shortcut_label(CYC_SC_DISK)));
    CHECK(seen_game.num_rom_patterns == 0);
#ifdef NESRECOMP_CYCLE_HDPACK
    CHECK(seen_game.hdpack_supported == 1);
#else
    CHECK(seen_game.hdpack_supported == 0);
#endif
    CHECK(seen_io.hdpack_enabled == 1);

    /* every edit comes back, then survives config.ini */
    cyc_settings_default(&s);
    script_edit = edit_everything;
    script_rom = "F:/somewhere/else.fds";
    script_result = RECOMP_LAUNCHER_RESULT_LAUNCH;
    rom = image;
    CHECK(cyc_ui_launcher(&s, cfg, NULL, &rom, true, NULL, NULL) == 0);
    CHECK(!strcmp(rom, "F:/somewhere/else.fds"));
    CHECK(s.window_scale == 5 && s.fullscreen == 1 && s.integer_scale == 0 && s.linear_filter == 1);
    CHECK(s.audio_enabled == 0 && s.volume == 40 && s.skip_launcher == 1);
    CHECK(!s.hdpack_enabled && !strcmp(s.hdpack_dir,"F:/HD Packs/Zelda"));
#ifdef RECOMP_LAUNCHER_HAS_ZAPPER_SETTINGS
    CHECK(!s.zapper_mouse && !s.zapper_crosshair);
#endif
    CHECK(s.bind.source[0] == 2 && s.bind.source[1] == 0 && s.bind.deadzone[0] == 12);
    CHECK(!strcmp(s.bind.device[0], "030000005e0400008e02000000007801"));
    static const int SPEC_TO_CYC[8] = { 4, 5, 6, 7, 0, 1, 3, 2 };
    for (int p = 0; p < 2; ++p)
        for (int b = 0; b < 8; ++b) {
            CHECK(s.bind.button[p][SPEC_TO_CYC[b]].key == SDL_SCANCODE_A + p * 8 + b);
            int want = b == 6 ? CYC_PAD_AXIS(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1)
                     : b == 7 ? CYC_PAD_COMBO((1 << 4) | (1 << 6)) : CYC_PAD_BUTTON(b + p);
            CHECK(s.bind.button[p][SPEC_TO_CYC[b]].pad == want);
        }
    for (int i = 0; i < CYC_SC_COUNT; ++i) {
        CHECK(s.bind.shortcut[i].key == SDL_SCANCODE_F1 + i);
        CHECK(s.bind.shortcut[i].pad == CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_Y + i));
    }
    CHECK(cyc_settings_save(&s, cfg, NULL));
    CycSettings back;
    cyc_settings_default(&back);
    CHECK(cyc_settings_load(&back, cfg, stderr, NULL));
    CHECK(!memcmp(&back.bind, &s.bind, sizeof(s.bind)) && back.volume == 40 && back.skip_launcher == 1);
    CHECK(!back.hdpack_enabled && !strcmp(back.hdpack_dir,s.hdpack_dir));
#ifdef RECOMP_LAUNCHER_HAS_ZAPPER_SETTINGS
    CHECK(!back.zapper_mouse && !back.zapper_crosshair);
#endif
    /* the edited settings go back into the launcher next time */
    script_edit = NULL;
    script_rom = NULL;
    script_result = RECOMP_LAUNCHER_RESULT_QUIT;
    CHECK(cyc_ui_launcher(&back, cfg, NULL, &rom, true, NULL, NULL) == 1);
    CHECK(seen_io.assist_key_bind[CYC_SC_DISK] == SDL_SCANCODE_F1 && seen_io.volume == 40);

    /* a cartridge launcher's shortcut rows map around the missing Disk row */
    cyc_settings_default(&s);
    script_edit = edit_everything;
    CHECK(cyc_ui_launcher(&s, cfg, NULL, &rom, false, NULL, NULL) == 1);      /* closed: edits kept, quit */
    CHECK(s.bind.shortcut[CYC_SC_DISK].key == SDL_SCANCODE_D);    /* untouched */
    CHECK(s.bind.shortcut[CYC_SC_MENU].key == SDL_SCANCODE_F1);   /* row 0 there is Menu */
    /* unavailable: go on without it */
    cyc_settings_default(&s);
    script_edit = NULL;
    script_result = RECOMP_LAUNCHER_RESULT_UNAVAILABLE;
    rom = image;
    CHECK(cyc_ui_launcher(&s, cfg, NULL, &rom, true, NULL, NULL) == 2 && rom == image);
    CHECK(!seen_game.has_bios && !seen_game.bios_verify_for_rom);       /* no lookup: no BIOS card */
    remove(cfg);
    bios_checks(dir);
    printf("ui_launcher_test: %u checks passed\n", checks);
    return 0;
}
