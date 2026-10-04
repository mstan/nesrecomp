/*
 * cyc_sdl.c - SDL2 window and audio for NESRecomp --cycle-accurate builds.
 *
 * Input is actions (cyc_input.h): each player's controller buttons and the
 * host shortcuts, every one with a keyboard and a controller binding kept in
 * config.ini (cyc_settings.h) and edited by recomp-ui's launcher. Defaults:
 *   player 1 keyboard: Z = A, X = B, Backslash = Select, Return = Start, arrows
 *   controllers: A = A, X = B, Back = Select, Start, D-pad (and the left stick)
 *   Disk: D / LB   Menu: Escape / RB   Fast-forward: Tab / RT (held)
 *   Screenshot: F12 (the picture only, cyc_shot_NNNN.png)   Fullscreen: F11
 *
 * Disk (FDS only, cyc_disk_action.h): the first press shows a toast with the
 * drive's state; each press while it is up swaps to the next side; after it
 * times out the next press shows the state again.
 *
 * Menu (builds with recomp-ui, cyc_ui.h): recomp-ui's runtime menu. The game
 * pauses while it is open; it holds the display and audio settings, the Disk
 * Drive section (an FDS image only: the side in the drive, eject / insert,
 * the HLE options), the shortcut list and Quit.
 *
 * The toast and the menu are presentation: drawn after the picture, never
 * into cyc_frame_argb(), so screenshots, --hash-out and every comparison see
 * the machine's picture only.
 *
 * Dev builds (NESRECOMP_DEV_UI, CYC_DEV_UI) add the drive bar under the
 * picture and the dev keys: F1 eject / insert the selected side, F3 select
 * the next side (drive empty), F4 drive bar on/off, F2 recompiled code /
 * interpreter (live; both produce the same machine), F6 + n the HLE axis n of
 * nes_fds_hle_axes() (F6 auto swap, F7 fast load, ...), and the coverage in
 * the title bar. Production builds have none of them.
 *
 * Fast load: while the drive is loading (cyc_fds_hle_loading), frames run back
 * to back without pacing and without audio, and the window shows the newest
 * one about every 1/60 s. The machine runs the same frames either way.
 *
 * The picture: the game's own (cyc_host_extras.h present), else the engine's
 * (cyc_render.h): the machine's 256x240 frame, or a game compositor's at the
 * width cyc_video.h settles on. A width change (a new mode, or Fit following a
 * window resize) is applied after the present and before the next picture.
 *
 * Save states (cyc_state.h): the Save state / Load state shortcuts (F8 / F9)
 * and the menu's rows use one slot, <exe dir>/saves/<image stem>.cycstate
 * (FDS retains .state); TCP
 * save_state / load_state take any path.
 */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include "cyc_core.h"
#include "cyc_disk_action.h"
#include "cyc_fds_bios.h"
#include "cyc_host.h"
#include "cyc_host_extras.h"
#include "cyc_hooks.h"
#include "cyc_input.h"
#include "cyc_mod.h"
#include "cyc_overlay.h"
#include "cyc_png.h"
#include "cyc_recomp.h"
#include "cyc_render.h"
#include "cyc_run.h"
#include "cyc_session.h"
#include "cyc_settings.h"
#include "cyc_ring.h"
#include "cyc_state.h"
#include "cyc_tcp.h"
#include "cyc_video.h"
#include "../../common/nes_cart.h"
#include "../../common/nes_fds.h"
#ifdef CYC_WITH_RECOMP_UI
#include "cyc_ui.h"
#include "recomp_runtime_ui.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0755)
#endif

#define AUDIO_RATE 48000
#define BAR_ROWS   15     /* logical rows under the 240 of the picture (dev builds) */

/* ---- options and settings ---- */

static CycSettings s_set;
static bool s_zapper_override, s_zapper_release;
static char        s_set_path[1024];
static bool        s_fds;
static const CycHostExtras *s_extras;
static const char *s_present_out;
static long        s_present_every;
static long        s_exit_after = -1;
static const char *s_vpad_path;
static int         s_tcp_port;
static int         s_key_hold[SDL_NUM_SCANCODES];   /* host loops a TCP-held key stays down */
static bool        s_hidden;
static bool        s_pause_unfocused;
static char        s_image[1024];                   /* the image running (the save state slot's name) */

void cyc_sdl_image_path(const char *path) { snprintf(s_image, sizeof(s_image), "%s", path ? path : ""); }

void cyc_sdl_option(const char *name, const char *value)
{
    if (!strcmp(name, "--virtual-pad")) s_vpad_path = value;
    else if (!strcmp(name, "--exit-after")) s_exit_after = atol(value);
    else if (!strcmp(name, "--config")) snprintf(s_set_path, sizeof(s_set_path), "%s", value);
    else if (!strcmp(name, "--tcp")) s_tcp_port = atoi(value);
    else if (!strcmp(name, "--hidden")) s_hidden = true;
    else if (!strcmp(name, "--pause-unfocused")) s_pause_unfocused = true;
}

void cyc_sdl_present_out(const char *path, long every)
{
    s_present_out = path;
    s_present_every = every;
}

static void game_load(void *ctx, const char *key, const char *value)
{
    (void)ctx;
    if (s_extras && s_extras->load_setting) s_extras->load_setting(s_extras->ctx, key, value);
}
static void game_save(void *ctx, FILE *f)
{
    (void)ctx;
    if (s_extras && s_extras->save_settings) s_extras->save_settings(s_extras->ctx, f);
}
static const CycSettingsGame GAME_KEYS = { NULL, game_load, game_save };

static void save_settings(void)
{
    if (!s_set_path[0]) return;
    if (!cyc_settings_save(&s_set, s_set_path, &GAME_KEYS))
        fprintf(stderr, "cannot write the settings file %s\n", s_set_path);
}

/* A disk image, from the compiled program or the file itself. */
static bool looks_fds(const char *path)
{
    return cyc_native_fds_bios_crc32 || cyc_fds_image_file(path);
}

/* <exe dir>/<name>: the directory config.ini is in. */
static void exe_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s", cyc_settings_default_path());
    char *cut = strrchr(out, '/'), *bcut = strrchr(out, '\\');
    if (bcut > cut) cut = bcut;
    if (cut) snprintf(cut + 1, n - (size_t)(cut + 1 - out), "%s", name);
    else snprintf(out, n, "%s", name);
}

int cyc_sdl_prelaunch(const char **rom_path, const char *cli_bios, NesFdsHleAsk *saved_hle,
                      const char **saved_bios)
{
    s_extras = cyc_session_extras();
    if (!s_set_path[0]) snprintf(s_set_path, sizeof(s_set_path), "%s", cyc_settings_default_path());
    cyc_settings_default(&s_set);
    bool had_config = cyc_settings_load(&s_set, s_set_path, stderr, &GAME_KEYS);
    /* Preserve legacy Zapper choices without modifying the old keybind file. */
    CycSettings legacy;
    cyc_settings_default(&legacy);
    char legacy_path[1100];
    exe_path(legacy_path, sizeof(legacy_path), "keybinds.ini");
    if (cyc_settings_load(&legacy, legacy_path, NULL, NULL)) {
        if (!(s_set.zapper_keys & 1) && (legacy.zapper_keys & 1)) s_set.zapper_mouse = legacy.zapper_mouse;
        if (!(s_set.zapper_keys & 2) && (legacy.zapper_keys & 2)) s_set.zapper_crosshair = legacy.zapper_crosshair;
    }
    if (!had_config) save_settings();
    s_fds = looks_fds(*rom_path);
    /* The mod catalog beside the executable, for a game built with mods. */
    char mods[1100], err[600];
    exe_path(mods, sizeof(mods), "mods");
    if (!cyc_session_mods_init(mods, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        cyc_sdl_error_box("Mods", err);
        return 1;
    }
#ifdef CYC_WITH_RECOMP_UI
    const char *no = getenv("NESRECOMP_NO_LAUNCHER");
    if (!(no && *no && *no != '0') && !s_set.skip_launcher) {
        /* The launcher's BIOS state runs the host's own lookup (cyc_fds_bios.h);
         * its pick comes back in s_set.fds_bios. PLAY commits the Mods
         * screen's selection (the provider's commit) before it returns. */
        const CycFdsBiosLookup bios = { cli_bios, NULL, NULL, cyc_native_fds_bios_crc32 };
        int r = cyc_ui_launcher(&s_set, s_set_path, s_extras, rom_path, s_fds, &bios, cyc_session_mods_provider());
        save_settings();
        if (r == 1) return 1;
        s_fds = looks_fds(*rom_path);
    }
#else
    (void)cli_bios;
#endif
    if (!cyc_session_mods_start(*rom_path, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        cyc_sdl_error_box("Mods", err);
        return 1;
    }
    if (s_extras && s_extras->set_view_mode && s_set.view_mode) s_extras->set_view_mode(s_extras->ctx, s_set.view_mode);
    *saved_hle = s_set.fds_hle;
    *saved_bios = s_set.fds_bios[0] ? s_set.fds_bios : NULL;
    return 0;
}

bool cyc_sdl_error_box(const char *title, const char *text)
{
    /* Never on a hidden or offscreen window (the native box would still reach
     * the desktop). */
    const char *drv = getenv("SDL_VIDEODRIVER");
    if (s_hidden || (drv && (!strcmp(drv, "dummy") || !strcmp(drv, "offscreen")))) return false;
    /* SDL_ShowSimpleMessageBox needs no SDL_Init. */
    return SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, NULL) == 0;
}

/* ---- the window ---- */

static SDL_Window   *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;
static int           s_tex_w, s_tex_h;
static bool          s_bar;          /* dev builds: the drive bar */
static long          s_frames_done;
static bool          s_running = true;
static bool          s_have_menu;

static uint64_t now_ms(void) { return SDL_GetTicks64(); }
static long     frames_done(void) { return s_frames_done; }
static void     request_quit(void) { s_running = false; }

static const uint32_t *picture(int *w, int *h)
{
    if (s_extras && s_extras->present) {
        const uint32_t *p = s_extras->present(s_extras->ctx, w, h);
        if (p && *w > 0 && *h > 0) return p;
    }
    return cyc_render_present(w, h);
}

/* ---- save states: one slot beside the executable ---- */

static char s_note_title[64], s_note_body[128];
static uint64_t s_note_until;

static void note(const char *title, const char *body)
{
    snprintf(s_note_title, sizeof(s_note_title), "%s", title);
    snprintf(s_note_body, sizeof(s_note_body), "%s", body ? body : "");
    s_note_until = SDL_GetTicks64() + 2500;
    printf("[cyc state] f=%ld %s%s%s\n", s_frames_done, title, body && *body ? ": " : "", body ? body : "");
    fflush(stdout);
}

static void state_slot(char *out, size_t n)
{
    char stem[512];
    const char *base = s_image, *s1 = strrchr(s_image, '/'), *s2 = strrchr(s_image, '\\');
    if (s1 && s1 + 1 > base) base = s1 + 1;
    if (s2 && s2 + 1 > base) base = s2 + 1;
    snprintf(stem, sizeof(stem), "%s", *base ? base : "game");
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem) *dot = 0;
    char dir[1100];
    exe_path(dir, sizeof(dir), "saves");
    make_dir(dir);
    snprintf(out, n, "%s/%s.%s", dir, stem, s_fds ? "state" : "cycstate");
}

static bool save_state_to(const char *path, char *err, size_t n)
{
    return cyc_state_save_file(path, err, n);
}

static bool load_state_from(const char *path, char *err, size_t n)
{
    if (!cyc_state_load_file(path, err, n)) return false;
    s_frames_done = (long)cyc_ring_frame;
    cyc_session_state_loaded();
    return true;
}

static bool save_state_slot(void)
{
    char path[1200], err[256];
    state_slot(path, sizeof(path));
    bool ok = save_state_to(path, err, sizeof(err));
    note(ok ? "STATE SAVED" : "STATE NOT SAVED", ok ? NULL : err);
    return ok;
}

static bool load_state_slot(void)
{
    char path[1200], err[256];
    state_slot(path, sizeof(path));
    bool ok = load_state_from(path, err, sizeof(err));
    note(ok ? "STATE LOADED" : "STATE NOT LOADED", ok ? NULL : err);
    return ok;
}

static int logical_h(void) { return s_tex_h + (s_bar ? BAR_ROWS : 0); }

static void apply_settings(void)
{
    if (!s_win) return;
    Uint32 fs = s_set.fullscreen == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : s_set.fullscreen == 2 ? SDL_WINDOW_FULLSCREEN : 0;
    if ((SDL_GetWindowFlags(s_win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != fs)
        SDL_SetWindowFullscreen(s_win, fs);
    if (!fs) SDL_SetWindowSize(s_win, s_tex_w * s_set.window_scale, logical_h() * s_set.window_scale);
    SDL_RenderSetLogicalSize(s_ren, s_tex_w, logical_h());
    SDL_RenderSetIntegerScale(s_ren, s_set.integer_scale ? SDL_TRUE : SDL_FALSE);
    if (s_tex) SDL_SetTextureScaleMode(s_tex, s_set.linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
}

static bool ensure_texture(int w, int h)
{
    if (s_tex && w == s_tex_w && h == s_tex_h) return true;
    if (s_tex) SDL_DestroyTexture(s_tex);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    s_tex_w = w;
    s_tex_h = h;
    if (s_tex) SDL_SetTextureScaleMode(s_tex, s_set.linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_RenderSetLogicalSize(s_ren, s_tex_w, logical_h());
    return s_tex != NULL;
}

/* ---- controllers ---- */

static SDL_GameController *s_pads[CYC_INPUT_MAX_PADS];
static char s_pad_guid[CYC_INPUT_MAX_PADS][40];
static int  s_pad_count;

static void open_pads(void)
{
    for (int i = 0; i < s_pad_count; ++i) SDL_GameControllerClose(s_pads[i]);
    s_pad_count = 0;
    for (int i = 0; i < SDL_NumJoysticks() && s_pad_count < CYC_INPUT_MAX_PADS; ++i) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController *c = SDL_GameControllerOpen(i);
        if (!c) continue;
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(c)), s_pad_guid[s_pad_count],
                                  sizeof(s_pad_guid[0]));
        printf("[cyc input] controller %d: %s (%s)\n", s_pad_count, SDL_GameControllerName(c), s_pad_guid[s_pad_count]);
        s_pads[s_pad_count++] = c;
    }
    fflush(stdout);
}

static void read_frame(CycInputFrame *f)
{
    memset(f, 0, sizeof(*f));
    int n = 0;
    const Uint8 *real = SDL_GetKeyboardState(&n);
    /* the TCP server's held keys are down too (cyc_tcp.h `key`) */
    static uint8_t keys[SDL_NUM_SCANCODES];
    for (int i = 0; i < SDL_NUM_SCANCODES; ++i) keys[i] = (uint8_t)((i < n && real[i]) || s_key_hold[i] > 0);
    f->keys = keys;
    f->key_count = SDL_NUM_SCANCODES;
    f->pad_count = s_pad_count;
    for (int i = 0; i < s_pad_count; ++i) {
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
            if (SDL_GameControllerGetButton(s_pads[i], (SDL_GameControllerButton)b)) f->pad_buttons[i] |= 1u << b;
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; ++a)
            f->pad_axes[i][a] = SDL_GameControllerGetAxis(s_pads[i], (SDL_GameControllerAxis)a);
        f->pad_guid[i] = s_pad_guid[i];
    }
}

/* ---- --virtual-pad: an SDL virtual game controller on a schedule ---- */

typedef struct { long frame; uint32_t buttons; int16_t axes[SDL_CONTROLLER_AXIS_MAX]; } VpadStep;
static VpadStep     *s_vpad;
static int           s_vpad_count, s_vpad_next;
static SDL_Joystick *s_vpad_joy;

static bool vpad_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", path); return false; }
    char line[256];
    int cap = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        char *end;
        long frame = strtol(p, &end, 10);
        if (end == p) continue;
        VpadStep st;
        memset(&st, 0, sizeof(st));
        st.frame = frame;
        for (char *tok = strtok(end, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
            if (!strcmp(tok, "-")) break;
            int v;
            if (!cyc_pad_parse(tok, &v)) { fprintf(stderr, "%s: unknown controller input %s\n", path, tok); fclose(f); return false; }
            if (CYC_PAD_IS_BUTTON(v)) st.buttons |= 1u << (v - 1);
            else if (CYC_PAD_IS_COMBO(v)) st.buttons |= (uint32_t)(v - 1000);
            else if (CYC_PAD_IS_AXIS(v)) st.axes[(v - 100) / 2] = ((v - 100) & 1) ? 32767 : -32768;
        }
        if (s_vpad_count == cap) {
            cap = cap ? cap * 2 : 32;
            s_vpad = (VpadStep *)realloc(s_vpad, sizeof(VpadStep) * (size_t)cap);
        }
        s_vpad[s_vpad_count++] = st;
    }
    fclose(f);
    return true;
}

static bool vpad_attach(void)
{
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.name = "NESRecomp virtual controller";
    int index = SDL_JoystickAttachVirtualEx(&desc);
    if (index < 0 || !(s_vpad_joy = SDL_JoystickOpen(index))) {
        fprintf(stderr, "virtual controller: %s\n", SDL_GetError());
        return false;
    }
    printf("[cyc input] virtual controller attached (%d steps)\n", s_vpad_count);
    return true;
}

static void vpad_tick(long frame)
{
    if (!s_vpad_joy) return;
    bool changed = false;
    while (s_vpad_next < s_vpad_count && s_vpad[s_vpad_next].frame <= frame) {
        const VpadStep *st = &s_vpad[s_vpad_next++];
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
            SDL_JoystickSetVirtualButton(s_vpad_joy, b, (st->buttons >> b) & 1);
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; ++a) SDL_JoystickSetVirtualAxis(s_vpad_joy, a, st->axes[a]);
        changed = true;
    }
    if (changed) {
        printf("[cyc input] f=%ld virtual controller step %d\n", frame, s_vpad_next);
        fflush(stdout);
    }
}

/* ---- the TCP debug server (cyc_tcp.h): every window check without a desktop ----
 *
 * Input goes in where a player's does: `key` holds an SDL key (the keyboard
 * state the bindings read, plus the key events the menu and dev keys read),
 * `pad` holds buttons of an SDL virtual game controller, `action` holds an
 * action itself. Screenshots come from the picture or from the renderer after
 * everything the window draws (the toast, the menu, the dev bar). */

static int      s_pad_hold[SDL_CONTROLLER_BUTTON_MAX];
static int      s_act_hold[CYC_INPUT_PLAYERS][CYC_INPUT_BUTTONS];
static int      s_sc_hold[CYC_SC_COUNT];
static char     s_ui_shot[1024];
static int      s_ui_shot_id = -1;
static char     s_toast_title[64], s_toast_body[256];
static bool     s_toast_up;
static bool menu_open(void);

static void push_key(SDL_Scancode sc, bool down)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    e.key.windowID = s_win ? SDL_GetWindowID(s_win) : 0;
    SDL_PushEvent(&e);
}

static long hold_frames(const char *line, long def)
{
    long f = def;
    cyc_tcp_long(line, "frames", &f);
    return f < 1 ? 1 : f > 36000 ? 36000 : f;
}

static void tcp_key(int id, const char *line)
{
    char name[64];
    int sc;
    if (!cyc_tcp_str(line, "name", name, sizeof(name)) || !cyc_key_parse(name, &sc) || !sc) {
        cyc_tcp_err(id, "key: name must be an SDL key name (\"D\", \"Return\", \"Escape\", \"F1\")");
        return;
    }
    if (!s_key_hold[sc]) push_key((SDL_Scancode)sc, true);
    s_key_hold[sc] = (int)hold_frames(line, 3);
    cyc_tcp_ok(id, NULL);
}

static bool vpad_attach(void);
static void open_pads(void);
static void tcp_pad(int id, const char *line)
{
    char names[128];
    if (!cyc_tcp_str(line, "buttons", names, sizeof(names))) {
        cyc_tcp_err(id, "pad: buttons is SDL controller button names joined by + (\"leftshoulder\", \"a+start\")");
        return;
    }
    int v;
    if (!cyc_pad_parse(names, &v) || !(CYC_PAD_IS_BUTTON(v) || CYC_PAD_IS_COMBO(v))) {
        cyc_tcp_err(id, "pad: unknown button names");
        return;
    }
    if (!s_vpad_joy) {
        if (!vpad_attach()) { cyc_tcp_err(id, "pad: no virtual controller"); return; }
        SDL_PumpEvents();
        open_pads();
    }
    uint32_t mask = CYC_PAD_IS_BUTTON(v) ? 1u << (v - 1) : (uint32_t)(v - 1000);
    long frames = hold_frames(line, 3);
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
        if (mask & (1u << b)) {
            s_pad_hold[b] = (int)frames;
            SDL_JoystickSetVirtualButton(s_vpad_joy, b, 1);
        }
    cyc_tcp_ok(id, NULL);
}

static void tcp_action(int id, const char *line)
{
    char name[32];
    long player = 1;
    cyc_tcp_long(line, "player", &player);
    if (!cyc_tcp_str(line, "name", name, sizeof(name))) { cyc_tcp_err(id, "action: name"); return; }
    int frames = (int)hold_frames(line, 3);
    for (int s = 0; s < CYC_SC_COUNT; ++s)
        if (!strcmp(name, cyc_shortcut_key(s))) { s_sc_hold[s] = frames; cyc_tcp_ok(id, NULL); return; }
    for (int b = 0; b < CYC_INPUT_BUTTONS; ++b)
        if (!strcmp(name, cyc_button_key(b)) && player >= 1 && player <= CYC_INPUT_PLAYERS) {
            s_act_hold[player - 1][b] = frames;
            cyc_tcp_ok(id, NULL);
            return;
        }
    cyc_tcp_err(id, "action: a shortcut (disk, menu, fast_forward, screenshot, fullscreen) or a button (a ... right)");
}

static void tcp_state(int id, const char *line)
{
    (void)line;
    char f[2048], t[160], b[400], h[96], save[96];
    cyc_tcp_quote(s_toast_up ? s_toast_title : "", t, sizeof(t));
    cyc_tcp_quote(s_toast_up ? s_toast_body : "", b, sizeof(b));
    cyc_tcp_quote(s_fds ? cyc_host_hle_text() : "", h, sizeof(h));
    cyc_tcp_quote(s_fds ? cyc_host_disk_save_status() : "", save, sizeof(save));
    char hint[64], hq[96];
    cyc_binding_hint(&s_set.bind.shortcut[CYC_SC_DISK], hint, sizeof(hint));
    cyc_tcp_quote(hint, hq, sizeof(hq));
    int w, hh;
    picture(&w, &hh);
    snprintf(f, sizeof(f),
             "\"frame\":%ld,\"menu_open\":%s,\"fds\":%s,\"side\":%d,\"sides\":%u,\"motor\":%s,\"writing\":%s,"
             "\"toast\":{\"visible\":%s,\"title\":%s,\"body\":%s},\"hle\":%s,\"disk_save\":%s,\"disk_binding\":%s,"
             "\"picture\":[%d,%d],\"dev_ui\":%s,\"recomp_ui\":%s,\"native\":%s",
             s_frames_done, menu_open() ? "true" : "false", s_fds ? "true" : "false", s_fds ? cyc_fds_side() : -1,
             s_fds ? cyc_fds_side_count() : 0, s_fds && cyc_fds_motor_on() ? "true" : "false",
             s_fds && cyc_fds_writing() ? "true" : "false", s_toast_up ? "true" : "false", t, b, h, save, hq, w, hh,
#ifdef CYC_DEV_UI
             "true",
#else
             "false",
#endif
#ifdef CYC_WITH_RECOMP_UI
             "true",
#else
             "false",
#endif
             cyc_run_native ? "true" : "false");
    /* the controllers the window reads, and the buttons it sees held */
    size_t at = strlen(f);
    at += (size_t)snprintf(f + at, sizeof(f) - at, ",\"pads\":[");
    for (int i = 0; i < s_pad_count && at < sizeof(f) - 64; ++i) {
        uint32_t held = 0;
        for (int bt = 0; bt < SDL_CONTROLLER_BUTTON_MAX; ++bt)
            if (SDL_GameControllerGetButton(s_pads[i], (SDL_GameControllerButton)bt)) held |= 1u << bt;
        at += (size_t)snprintf(f + at, sizeof(f) - at, "%s{\"guid\":\"%s\",\"buttons\":%u}", i ? "," : "", s_pad_guid[i], held);
    }
    snprintf(f + at, sizeof(f) - at, "]");
    cyc_tcp_ok(id, f);
}

static void tcp_screenshot(int id, const char *line)
{
    char layer[16] = "picture", path[1024];
    cyc_tcp_str(line, "layer", layer, sizeof(layer));
    if (!cyc_tcp_str(line, "path", path, sizeof(path))) { cyc_tcp_err(id, "screenshot: path"); return; }
    if (!strcmp(layer, "picture")) {
        int w, h;
        const uint32_t *pic = picture(&w, &h);
        if (!cyc_write_png(path, pic, w, h)) { cyc_tcp_err(id, "cannot write the file"); return; }
        char q[1100], fl[1200];
        cyc_tcp_quote(path, q, sizeof(q));
        snprintf(fl, sizeof(fl), "\"path\":%s,\"size\":[%d,%d]", q, w, h);
        cyc_tcp_ok(id, fl);
    } else if (!strcmp(layer, "ui")) {
        snprintf(s_ui_shot, sizeof(s_ui_shot), "%s", path);   /* read back after the next draw */
        s_ui_shot_id = id;
    } else {
        cyc_tcp_err(id, "screenshot: layer is picture (the game alone) or ui (what the window presents)");
    }
}

static void tcp_menu(int id, const char *line)
{
#ifdef CYC_WITH_RECOMP_UI
    bool want;
    if (!s_have_menu) { cyc_tcp_err(id, "menu: this window has no runtime menu"); return; }
    if (cyc_tcp_bool(line, "open", &want) && want != menu_open()) cyc_ui_toggle_menu();
    char in[16];
    if (cyc_tcp_str(line, "nav", in, sizeof(in))) {
        static const char *const NAMES[] = { "toggle", "back", "up", "down", "left", "right", "accept" };
        int k = -1;
        for (int i = 0; i < 7; ++i) if (!strcmp(in, NAMES[i])) k = i;
        if (k < 0) { cyc_tcp_err(id, "menu: nav is toggle, back, up, down, left, right or accept"); return; }
        cyc_ui_nav(k, false);
    }
    cyc_tcp_ok(id, menu_open() ? "\"open\":true" : "\"open\":false");
#else
    (void)line;
    cyc_tcp_err(id, "menu: built without recomp-ui");
#endif
}

static void tcp_disk(int id, const char *line)
{
    char what[16] = "press";
    long side = -1;
    cyc_tcp_str(line, "do", what, sizeof(what));
    cyc_tcp_long(line, "side", &side);
    if (!s_fds) { cyc_tcp_err(id, "disk: not a disk program"); return; }
    uint64_t now = SDL_GetTicks64();
    bool ok = true;
    if (!strcmp(what, "press")) ok = cyc_host_disk_press(now, s_frames_done) != CYC_DISK_PRESS_NONE;
    else if (!strcmp(what, "choose")) ok = side >= 0 && cyc_host_disk_choose(now, s_frames_done, (unsigned)side) != CYC_DISK_PRESS_NONE;
    else if (!strcmp(what, "eject")) ok = cyc_host_disk_eject(s_frames_done);
    else if (!strcmp(what, "insert")) ok = side >= 0 && cyc_host_disk_insert(s_frames_done, (unsigned)side);
    else { cyc_tcp_err(id, "disk: do is press, choose, eject or insert"); return; }
    if (ok) cyc_tcp_ok(id, NULL);
    else cyc_tcp_err(id, "disk: refused (see the drive state)");
}

static void tcp_hle(int id, const char *line)
{
    char word[32], value[16];
    if (!cyc_tcp_str(line, "axis", word, sizeof(word)) || !cyc_tcp_str(line, "value", value, sizeof(value))) {
        cyc_tcp_err(id, "hle: axis (auto-swap, fast-load, ...) and value (on, off, default)");
        return;
    }
    unsigned n;
    const NesFdsHleAxis *ax = nes_fds_hle_axes(&n);
    for (unsigned a = 0; a < n; ++a) {
        if (strcmp(word, ax[a].word)) continue;
        int8_t v = !strcmp(value, "on") ? 1 : !strcmp(value, "off") ? 0 : -1;
        *nes_fds_hle_ask_axis(&s_set.fds_hle, &ax[a]) = v;       /* what the menu row does */
        cyc_host_hle_user_set(a, v);
        save_settings();
        char q[96], fl[160];
        cyc_tcp_quote(cyc_host_hle_text(), q, sizeof(q));
        snprintf(fl, sizeof(fl), "\"hle\":%s", q);
        cyc_tcp_ok(id, fl);
        return;
    }
    cyc_tcp_err(id, "hle: unknown axis");
}

static void tcp_read_ram(int id, const char *line)
{
    long addr = 0, len = 1;
    cyc_tcp_long(line, "addr", &addr);
    cyc_tcp_long(line, "len", &len);
    if (len < 1 || len > 4096 || addr < 0 || addr + len > 0x10000) { cyc_tcp_err(id, "read_ram: addr, len (<= 4096)"); return; }
    char *hex = (char *)malloc((size_t)len * 2 + 32);
    if (!hex) return;
    size_t at = (size_t)sprintf(hex, "\"hex\":\"");
    for (long i = 0; i < len; ++i) {
        uint8_t v = 0;
        cyc_debug_peek((uint16_t)(addr + i), &v);
        at += (size_t)sprintf(hex + at, "%02X", v);
    }
    sprintf(hex + at, "\"");
    cyc_tcp_ok(id, hex);
    free(hex);
}

static void tcp_ring(int id, const char *line)
{
    char path[1024];
    long first = 0, last = -1;
    if (!cyc_tcp_str(line, "path", path, sizeof(path))) { cyc_tcp_err(id, "ring_dump: path [first, last]"); return; }
    cyc_tcp_long(line, "first", &first);
    cyc_tcp_long(line, "last", &last);
    FILE *f = fopen(path, "w");
    if (!f) { cyc_tcp_err(id, "cannot write the file"); return; }
    cyc_ring_dump(f, (uint32_t)first, last < 0 ? UINT32_MAX : (uint32_t)last);
    fclose(f);
    cyc_tcp_ok(id, NULL);
}

static void tcp_state_file(int id, const char *line, bool save)
{
    char path[1024], err[256];
    if (!cyc_tcp_str(line, "path", path, sizeof(path))) { cyc_tcp_err(id, save ? "save_state: path" : "load_state: path"); return; }
    if (save ? save_state_to(path, err, sizeof(err)) : load_state_from(path, err, sizeof(err))) {
        char f[64];
        snprintf(f, sizeof(f), "\"frame\":%ld", s_frames_done);
        cyc_tcp_ok(id, f);
    } else {
        cyc_tcp_err(id, err);
    }
}
static void tcp_save_state(int id, const char *line) { tcp_state_file(id, line, true); }
static void tcp_load_state(int id, const char *line) { tcp_state_file(id, line, false); }

static void tcp_video(int id, const char *line)
{
    char mode[16];
    if (cyc_tcp_str(line, "mode", mode, sizeof(mode))) {
        int m;
        if (!nes_video_geometry_parse(mode, &m)) { cyc_tcp_err(id, "video: mode is stock, 16:9, 21:9, 32:9 or fit"); return; }
        cyc_video_set_mode(m);
    }
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(s_ren, &ow, &oh);
    char f[200];
    snprintf(f, sizeof(f), "\"mode\":\"%s\",\"width\":%d,\"native_x0\":%d,\"drawable\":[%d,%d],\"compositor\":%s",
             nes_video_geometry_name(cyc_video_mode()), cyc_video_width(), cyc_video_native_x0(), ow, oh,
             cyc_render_has_compositor() ? "true" : "false");
    cyc_tcp_ok(id, f);
}

static void tcp_window_size(int id, const char *line)
{
    long w = 0, h = 0;
    if (!cyc_tcp_long(line, "w", &w) || !cyc_tcp_long(line, "h", &h) || w < 64 || h < 60 || w > 8192 || h > 8192) {
        cyc_tcp_err(id, "window_size: w, h");
        return;
    }
    SDL_SetWindowSize(s_win, (int)w, (int)h);
    cyc_tcp_ok(id, NULL);
}

static void tcp_mod_stats(int id, const char *line)
{
    (void)line;
    CycHookStats hs;
    CycModStats ms;
    CycRenderStats rs;
    cyc_hooks_stats(-1, &hs);
    cyc_mod_stats(&ms);
    cyc_render_stats(&rs);
    char f[512];
    snprintf(f, sizeof(f),
             "\"hook_sites\":%u,\"armed\":%s,\"fired\":%llu,\"handled\":%llu,\"mismatched\":%llu,\"scopes\":%llu,"
             "\"calls\":%llu,\"call_cycles\":%llu,\"call_failures\":%llu,\"composed\":%llu,\"pillarboxed\":%llu,"
             "\"native\":%llu",
             cyc_native_hook_site_count, cyc_hooks_armed ? "true" : "false", (unsigned long long)hs.fired,
             (unsigned long long)hs.handled, (unsigned long long)hs.mismatched, (unsigned long long)ms.scopes,
             (unsigned long long)ms.calls, (unsigned long long)ms.cycles, (unsigned long long)ms.failures,
             (unsigned long long)rs.composed, (unsigned long long)rs.pillarboxed, (unsigned long long)rs.native);
    cyc_tcp_ok(id, f);
}

static void tcp_quit(int id, const char *line)
{
    (void)line;
    cyc_tcp_ok(id, NULL);
    s_running = false;
}

static void tcp_ping(int id, const char *line)
{
    (void)line;
    char f[64];
    snprintf(f, sizeof(f), "\"frame\":%ld", s_frames_done);
    cyc_tcp_ok(id, f);
}

static void zapper_window_point(int wx, int wy, int *x, int *y)
{
    float lx, ly;
    SDL_RenderWindowToLogical(s_ren, wx, wy, &lx, &ly);
    int w, h;
    picture(&w, &h);
    lx -= (float)(w - 256) / 2.0f;
    if (lx < 0 || lx >= 256 || ly < 0 || ly >= 240) *x = *y = -1;
    else { *x = (int)lx; *y = (int)ly; }
}

static void zapper_mouse_frame(void)
{
    CycZapperState gun;
    cyc_zapper_state(&gun);
    if (!gun.port || s_zapper_override) return;
    int wx, wy, x = -1, y = -1;
    Uint32 buttons = SDL_GetMouseState(&wx, &wy);
    bool trigger = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    if (s_zapper_release) {
        if (!trigger) s_zapper_release = false;
        trigger = false;
    }
    bool aiming = s_set.zapper_mouse && (SDL_GetWindowFlags(s_win) & SDL_WINDOW_MOUSE_FOCUS);
    if (aiming) zapper_window_point(wx, wy, &x, &y);
    cyc_set_zapper(x, y, aiming && trigger);
}

static void tcp_zapper(int id, const char *line)
{
    CycZapperState gun;
    cyc_zapper_state(&gun);
    if (!gun.port) { cyc_tcp_err(id, "no Zapper attached"); return; }
    bool mouse, trigger = gun.trigger;
    long x = gun.x, y = gun.y, wx, wy;
    bool set = cyc_tcp_long(line, "x", &x);
    set |= cyc_tcp_long(line, "y", &y);
    set |= cyc_tcp_bool(line, "trigger", &trigger);
    if (cyc_tcp_long(line, "window_x", &wx) && cyc_tcp_long(line, "window_y", &wy)) {
        int px, py;
        zapper_window_point((int)wx, (int)wy, &px, &py);
        x = px; y = py; set = true;
    }
    if (set) { s_zapper_override = true; cyc_set_zapper((int)x, (int)y, trigger); }
    if (cyc_tcp_bool(line, "mouse", &mouse)) s_zapper_override = !mouse;
    cyc_zapper_state(&gun);
    char fields[256];
    snprintf(fields, sizeof(fields), "\"port\":%u,\"aim\":[%d,%d],\"trigger\":%s,\"light\":%s,\"mouse\":%s,"
             "\"mouse_enabled\":%s,\"crosshair\":%s",
             gun.port, gun.x, gun.y, gun.trigger ? "true" : "false", gun.light ? "true" : "false",
             s_zapper_override ? "false" : "true", s_set.zapper_mouse ? "true" : "false",
             s_set.zapper_crosshair ? "true" : "false");
    cyc_tcp_ok(id, fields);
}

static void tcp_setup(void)
{
    int port = s_tcp_port;
    const char *env = getenv("NESRECOMP_CYC_TCP");
    if (!port && env && *env) port = atoi(env);
    if (!port) {
        /* debug.ini beside the executable turns it on at the documented port (TCP.md) */
        char ini[1100];
        snprintf(ini, sizeof(ini), "%s", cyc_settings_default_path());
        char *cut = strrchr(ini, '/');
        if (cut) snprintf(cut + 1, sizeof(ini) - (size_t)(cut + 1 - ini), "debug.ini");
        FILE *f = fopen(ini, "r");
        if (f) { fclose(f); port = 4370; }
    }
    if (port <= 0) return;
    cyc_tcp_register("ping", "liveness; the frame counter", tcp_ping);
    cyc_tcp_register("zapper", "aim x/y, trigger, mouse true; optional window_x/window_y", tcp_zapper);
    cyc_tcp_register("state", "frame, menu, drive, toast, HLE, disk save, bindings, build", tcp_state);
    cyc_tcp_register("key", "hold an SDL key through the bindings: name, frames", tcp_key);
    cyc_tcp_register("pad", "hold virtual game controller buttons: buttons (a+start), frames", tcp_pad);
    cyc_tcp_register("action", "hold an action: name (a..right, disk, menu, ...), player, frames", tcp_action);
    cyc_tcp_register("disk", "the drive: do press|choose|eject|insert, side", tcp_disk);
    cyc_tcp_register("menu", "the runtime menu: open true|false, nav up|down|left|right|accept|back|toggle", tcp_menu);
    cyc_tcp_register("hle", "an HLE axis as the menu sets it: axis, value on|off|default", tcp_hle);
    cyc_tcp_register("screenshot", "layer picture (the game) | ui (what the window presents), path", tcp_screenshot);
    cyc_tcp_register("read_ram", "CPU address space bytes: addr, len", tcp_read_ram);
    cyc_tcp_register("ring_dump", "the always-on event ring to a file: path, first, last", tcp_ring);
    cyc_tcp_register("save_state", "write a save state (cyc_state.h): path", tcp_save_state);
    cyc_tcp_register("load_state", "load a save state: path", tcp_load_state);
    cyc_tcp_register("video", "the presented width: mode stock|16:9|21:9|32:9|fit (optional)", tcp_video);
    cyc_tcp_register("window_size", "resize the window (Fit follows its drawable): w, h", tcp_window_size);
    cyc_tcp_register("mod_stats", "hook sites, isolated calls, compositor counts", tcp_mod_stats);
    cyc_tcp_register("quit", "close the window (the disk save is written first)", tcp_quit);
    if (s_extras && s_extras->tcp_setup) s_extras->tcp_setup(s_extras->ctx);
    if (cyc_tcp_start(port)) printf("[cyc tcp] listening on 127.0.0.1:%d\n", port);
    else fprintf(stderr, "[cyc tcp] cannot listen on port %d\n", port);
    fflush(stdout);
}

/* Once per host loop: the held TCP inputs count down and let go. */
static void tcp_tick(void)
{
    for (int sc = 0; sc < SDL_NUM_SCANCODES; ++sc)
        if (s_key_hold[sc] && --s_key_hold[sc] == 0) push_key((SDL_Scancode)sc, false);
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
        if (s_pad_hold[b] && --s_pad_hold[b] == 0 && s_vpad_joy) SDL_JoystickSetVirtualButton(s_vpad_joy, b, 0);
    for (int p = 0; p < CYC_INPUT_PLAYERS; ++p)
        for (int b = 0; b < CYC_INPUT_BUTTONS; ++b)
            if (s_act_hold[p][b]) --s_act_hold[p][b];
    for (int s = 0; s < CYC_SC_COUNT; ++s)
        if (s_sc_hold[s]) --s_sc_hold[s];
}

/* ---- the dev build's drive bar (under the picture, never in it) ---- */

#ifdef CYC_DEV_UI
static void bar_text(int x, int y, const char *s)
{
    for (; *s; ++s, x += 4) {
        const uint8_t *g = cyc_overlay_glyph(*s);
        if (!g) continue;
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 3; ++c)
                if (g[r] & (4 >> c)) {
                    SDL_Rect px = { x + c, y + r, 1, 1 };
                    SDL_RenderFillRect(s_ren, &px);
                }
    }
}

static void draw_drive_bar(bool loading)
{
    int y0 = s_tex_h;
    SDL_Rect bar = { 0, y0, s_tex_w, BAR_ROWS };
    SDL_SetRenderDrawColor(s_ren, 24, 24, 32, 255);
    SDL_RenderFillRect(s_ren, &bar);
    char text[64], name[24];
    int side = cyc_fds_side();
    bool motor = cyc_fds_motor_on();
    SDL_Rect lamp = { 3, y0 + 2, 5, 5 };           /* red while the motor runs, as on the real drive */
    if (motor) SDL_SetRenderDrawColor(s_ren, 255, 48, 32, 255);
    else SDL_SetRenderDrawColor(s_ren, 70, 20, 20, 255);
    SDL_RenderFillRect(s_ren, &lamp);
    SDL_SetRenderDrawColor(s_ren, 230, 230, 230, 255);
    if (side >= 0) {
        cyc_disk_side_name(name, sizeof(name), side);
        snprintf(text, sizeof(text), "%s%s", name, motor ? " MOTOR" : "");
    } else {
        cyc_disk_side_name(name, sizeof(name), (int)cyc_host_disk_selected());
        snprintf(text, sizeof(text), "EMPTY - F1 INSERTS %s", name);
    }
    bar_text(11, y0 + 2, text);
    CycFdsHleStatus hle;
    cyc_fds_hle_status(&hle);
    char htext[64];
    if (hle.swap_target >= 0) {
        cyc_disk_side_name(name, sizeof(name), hle.swap_target);
        snprintf(htext, sizeof(htext), "AUTO SWAP TO %s", name);
    } else {
        snprintf(htext, sizeof(htext), "%s%s", cyc_host_hle_text(), loading ? " LOADING" : "");
    }
    SDL_SetRenderDrawColor(s_ren, 255, 210, 90, 255);
    bar_text(11, y0 + 9, htext);
    const char *save = cyc_host_disk_save_status();
    if (save && *save) {
        SDL_SetRenderDrawColor(s_ren, 150, 200, 255, 255);
        bar_text(s_tex_w - 2 - (int)strlen(save) * 4, y0 + 2, save);
    }
}

/* F1-F12 as the dev build uses them; false for any other key. */
static bool dev_key(SDL_Scancode sc)
{
    char msg[96] = "";
    unsigned axes;
    nes_fds_hle_axes(&axes);
    if (sc == SDL_SCANCODE_F2) {
        cyc_run_native = !cyc_run_native;
        snprintf(msg, sizeof(msg), "%s (F2)", cyc_run_native ? "recompiled code" : "interpreter only");
    } else if (sc == SDL_SCANCODE_F1 && s_fds) {
        if (cyc_fds_side() >= 0) cyc_host_disk_eject(s_frames_done);
        else cyc_host_disk_insert(s_frames_done, cyc_host_disk_selected());
    } else if (sc == SDL_SCANCODE_F3 && s_fds) {
        if (cyc_fds_side() < 0 && cyc_fds_side_count()) {
            cyc_host_disk_select((cyc_host_disk_selected() + 1) % cyc_fds_side_count());
            snprintf(msg, sizeof(msg), "select side %u (F3)", cyc_host_disk_selected());
        }
    } else if (sc == SDL_SCANCODE_F4 && s_fds) {
        s_bar = !s_bar;
        apply_settings();
        snprintf(msg, sizeof(msg), "drive bar %s (F4)", s_bar ? "shown" : "hidden");
    } else if (s_fds && sc >= SDL_SCANCODE_F6 && sc < SDL_SCANCODE_F6 + (int)axes && sc <= SDL_SCANCODE_F10) {
        snprintf(msg, sizeof(msg), "%s (F%d)", cyc_host_hle_toggle((int)(sc - SDL_SCANCODE_F6)), 6 + (int)(sc - SDL_SCANCODE_F6));
    } else {
        return false;
    }
    if (msg[0]) printf("[cyc dev] f=%ld %s\n", s_frames_done, msg);
    fflush(stdout);
    return true;
}
#endif

/* ---- presentation ---- */

static void present_shot(long frame)
{
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(s_ren, &w, &h);
    uint32_t *px = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!px) return;
    if (SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, px, w * 4) == 0) {
        char path[1024];
        const char *base = s_present_out, *dot = strrchr(base, '.');
        int stem = (int)(dot ? (size_t)(dot - base) : strlen(base));
        snprintf(path, sizeof(path), "%.*s_%05ld%s", stem, base, frame, dot ? dot : ".png");
        if (!cyc_write_png(path, px, w, h)) fprintf(stderr, "cannot write %s\n", path);
    }
    free(px);
}

static void show(bool loading, const char *toast_title, const char *toast_body)
{
    int w, h;
    const uint32_t *pic = picture(&w, &h);
    if (!ensure_texture(w, h)) return;
#ifndef CYC_WITH_RECOMP_UI
    /* no runtime UI: the toast goes into a copy of the picture */
    static uint32_t *copy;
    static size_t copy_len;
    if (toast_title) {
        size_t need = (size_t)w * (size_t)h;
        if (need > copy_len) { free(copy); copy = (uint32_t *)malloc(need * 4); copy_len = copy ? need : 0; }
        if (copy) {
            memcpy(copy, pic, need * 4);
            cyc_overlay_toast(copy, w, h, toast_title, toast_body);
            pic = copy;
        }
    }
#else
    (void)toast_title;
    (void)toast_body;
#endif
    SDL_UpdateTexture(s_tex, NULL, pic, w * 4);
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderClear(s_ren);
    SDL_Rect dst = { 0, 0, w, h };
    SDL_RenderCopy(s_ren, s_tex, NULL, &dst);
    CycZapperState gun;
    cyc_zapper_state(&gun);
    if (gun.port && s_set.zapper_crosshair && gun.x >= 0 && !menu_open()) {
        int x = gun.x + (w - 256) / 2, y = gun.y;
        SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
        SDL_Rect back = {x - 5, y - 1, 11, 3}; SDL_RenderFillRect(s_ren, &back);
        back.x = x - 1; back.y = y - 5; back.w = 3; back.h = 11; SDL_RenderFillRect(s_ren, &back);
        SDL_SetRenderDrawColor(s_ren, 255, 255, 255, 255);
        SDL_RenderDrawLine(s_ren, x - 4, y, x + 4, y);
        SDL_RenderDrawLine(s_ren, x, y - 4, x, y + 4);
    }
#ifdef CYC_DEV_UI
    if (s_bar) draw_drive_bar(loading);
#else
    (void)loading;
#endif
#ifdef CYC_WITH_RECOMP_UI
    cyc_ui_render();
#endif
    if (s_ui_shot_id >= 0) {
        /* the TCP server's `screenshot layer ui`: everything drawn this frame */
        int ow = 0, oh = 0;
        SDL_GetRendererOutputSize(s_ren, &ow, &oh);
        uint32_t *px = (uint32_t *)malloc((size_t)ow * (size_t)oh * 4);
        /* ReadPixels(NULL) reads only the current picture viewport. Capture
         * the whole window, including pillarboxes and the full-size menu. */
        SDL_Rect viewport;
        SDL_RenderGetViewport(s_ren, &viewport);
        SDL_RenderSetViewport(s_ren, NULL);
        bool ok = px && SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, px, ow * 4) == 0 &&
                  cyc_write_png(s_ui_shot, px, ow, oh);
        SDL_RenderSetViewport(s_ren, &viewport);
        free(px);
        char q[1100], fl[1200];
        cyc_tcp_quote(s_ui_shot, q, sizeof(q));
        snprintf(fl, sizeof(fl), "\"path\":%s,\"size\":[%d,%d]", q, ow, oh);
        if (ok) cyc_tcp_ok(s_ui_shot_id, fl);
        else cyc_tcp_err(s_ui_shot_id, SDL_GetError());
        s_ui_shot_id = -1;
    }
}

/* ---- the loop ---- */

static void menu_nav_key(const SDL_KeyboardEvent *k)
{
#ifdef CYC_WITH_RECOMP_UI
    int in = -1;
    switch (k->keysym.scancode) {
    case SDL_SCANCODE_UP: in = RECOMP_RUNTIME_UI_INPUT_UP; break;
    case SDL_SCANCODE_DOWN: in = RECOMP_RUNTIME_UI_INPUT_DOWN; break;
    case SDL_SCANCODE_LEFT: in = RECOMP_RUNTIME_UI_INPUT_LEFT; break;
    case SDL_SCANCODE_RIGHT: in = RECOMP_RUNTIME_UI_INPUT_RIGHT; break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: in = RECOMP_RUNTIME_UI_INPUT_ACCEPT; break;
    case SDL_SCANCODE_BACKSPACE: in = RECOMP_RUNTIME_UI_INPUT_BACK; break;
    case SDL_SCANCODE_ESCAPE:
        /* Escape backs out unless it is the Menu shortcut (which then closes) */
        if (s_set.bind.shortcut[CYC_SC_MENU].key != SDL_SCANCODE_ESCAPE) in = RECOMP_RUNTIME_UI_INPUT_BACK;
        break;
    default: break;
    }
    if (in >= 0) cyc_ui_nav(in, k->repeat != 0);
#else
    (void)k;
#endif
}

static void menu_nav_button(int button)
{
#ifdef CYC_WITH_RECOMP_UI
    if (s_set.bind.shortcut[CYC_SC_MENU].pad == CYC_PAD_BUTTON(button)) return;   /* the toggle */
    int in = -1;
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: in = RECOMP_RUNTIME_UI_INPUT_UP; break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: in = RECOMP_RUNTIME_UI_INPUT_DOWN; break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: in = RECOMP_RUNTIME_UI_INPUT_LEFT; break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: in = RECOMP_RUNTIME_UI_INPUT_RIGHT; break;
    case SDL_CONTROLLER_BUTTON_A: in = RECOMP_RUNTIME_UI_INPUT_ACCEPT; break;
    case SDL_CONTROLLER_BUTTON_B: in = RECOMP_RUNTIME_UI_INPUT_BACK; break;
    default: break;
    }
    if (in >= 0) cyc_ui_nav(in, false);
#else
    (void)button;
#endif
}

static bool menu_open(void)
{
#ifdef CYC_WITH_RECOMP_UI
    return s_have_menu && cyc_ui_menu_open();
#else
    return false;
#endif
}

int cyc_sdl_main(const char *title_in, int scale)
{
    /* a path names the game by its file's stem */
    char title[256];
    const char *base = title_in, *s1 = strrchr(title_in, '/'), *s2 = strrchr(title_in, '\\');
    if (s1 && s1 + 1 > base) base = s1 + 1;
    if (s2 && s2 + 1 > base) base = s2 + 1;
    snprintf(title, sizeof(title), "%s", base);
    char *dot = strrchr(title, '.');
    if (dot && dot != title && base != title_in) *dot = 0;
    SDL_SetMainReady();
    /* A hidden window never has the keyboard focus, and SDL drops controller
     * input for an unfocused application unless told otherwise. */
    if (s_hidden) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!s_set_path[0]) cyc_settings_default(&s_set);   /* prelaunch did not run */
    if (scale > 0) s_set.window_scale = scale;
    s_fds = cyc_is_fds();
#ifdef CYC_DEV_UI
    s_bar = s_fds;
#endif
    int w, h;
    picture(&w, &h);
    s_tex_w = w;
    s_tex_h = h;
    s_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w * s_set.window_scale,
                             logical_h() * s_set.window_scale,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (s_hidden ? SDL_WINDOW_HIDDEN : 0));
    s_ren = s_win ? SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    /* offscreen / headless video drivers have no accelerated renderer */
    if (s_win && !s_ren) s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    if (!s_ren || !ensure_texture(w, h)) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    apply_settings();
    /* From here a width request waits for the frame boundary; Fit follows
     * the drawable from the first frame. */
    cyc_video_window_ready();
    {
        int ow = 0, oh = 0;
        SDL_GetRendererOutputSize(s_ren, &ow, &oh);
        cyc_video_window_resized(ow, oh);
    }
#ifdef CYC_WITH_RECOMP_UI
    CycUiHost host = { &s_set, s_extras, s_fds, apply_settings, save_settings, request_quit, frames_done, now_ms, title,
                       save_state_slot, load_state_slot, cyc_session_mods_provider(), s_image, cyc_session_mods_reapply };
    s_have_menu = cyc_ui_init(s_win, s_ren, &host);
#endif

    /* Audio is queued as frames produce it; fast forward and the menu drop it. */
    SDL_AudioSpec want = { 0 }, have;
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev && cyc_audio_enable(have.freq)) SDL_PauseAudioDevice(dev, 0);
    else if (!dev) fprintf(stderr, "SDL audio: %s (continuing without sound)\n", SDL_GetError());

    if (s_vpad_path && vpad_load(s_vpad_path)) vpad_attach();
    tcp_setup();
    open_pads();

    char disk_hint[64];
    const double frame_seconds = cyc_host_frame_seconds();
    const Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 next = SDL_GetPerformanceCounter();
    Uint64 fps_mark = next, shown = 0;
    uint64_t native_mark = cyc_run_native_cycles;
    uint64_t cycles_mark = cyc_cycle_count();
    int frames = 0, shot = 0;
    long loops = 0;
    bool prev[CYC_SC_COUNT] = { false };
    bool hold_input = false;     /* after the menu closes: until every key is up */
    bool was_open = false;
    SDL_SetWindowTitle(s_win, title);

    while (s_running) {
        vpad_tick(s_frames_done);
        cyc_tcp_poll();
        tcp_tick();
        SDL_Event ev;
        bool open = menu_open();
        while (SDL_PollEvent(&ev)) {
#ifdef CYC_WITH_RECOMP_UI
            cyc_ui_process_event(&ev);
#endif
            if (ev.type == SDL_CONTROLLERDEVICEREMOVED ||
                (!open && (!s_pause_unfocused || s_hidden || (SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS)))) {
                int player = -1;
                if (ev.type == SDL_CONTROLLERAXISMOTION) {
                    CycInputFrame ef; CycInputState es;
                    read_frame(&ef); cyc_input_eval(&s_set.bind, &ef, &es);
                    for (int p = 0; p < CYC_INPUT_PLAYERS; ++p)
                        if (es.player_pad[p] >= 0 && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pads[es.player_pad[p]])) == ev.caxis.which)
                            player = p;
                }
                cyc_session_event(&ev, player);
            }
            if (ev.type == SDL_QUIT) s_running = false;
            else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                int ow = 0, oh = 0;
                SDL_GetRendererOutputSize(s_ren, &ow, &oh);
                cyc_video_window_resized(ow, oh);
            }
            else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) open_pads();
            else if (ev.type == SDL_KEYDOWN) {
#ifdef CYC_DEV_UI
                if (!ev.key.repeat && dev_key(ev.key.keysym.scancode)) continue;
#endif
                if (open) menu_nav_key(&ev.key);
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN && open) {
                menu_nav_button(ev.cbutton.button);
            }
        }

        /* the actions */
        CycInputFrame in;
        CycInputState st;
        read_frame(&in);
        cyc_input_eval(&s_set.bind, &in, &st);
        for (int p = 0; p < CYC_INPUT_PLAYERS; ++p)            /* TCP-held actions */
            for (int b = 0; b < CYC_INPUT_BUTTONS; ++b)
                if (s_act_hold[p][b]) st.buttons[p] |= (uint8_t)(0x80 >> b);
        for (int i = 0; i < CYC_SC_COUNT; ++i)
            if (s_sc_hold[i]) st.shortcut[i] = true;
        bool edge[CYC_SC_COUNT];
        for (int i = 0; i < CYC_SC_COUNT; ++i) {
            edge[i] = st.shortcut[i] && !prev[i];
            prev[i] = st.shortcut[i];
        }
        uint64_t now = now_ms();
        if (edge[CYC_SC_MENU] && s_have_menu) {
#ifdef CYC_WITH_RECOMP_UI
            cyc_ui_toggle_menu();
#endif
            printf("[cyc ui] f=%ld menu %s\n", s_frames_done, menu_open() ? "open" : "closed");
            fflush(stdout);
        }
        if (edge[CYC_SC_DISK] && s_fds) cyc_host_disk_press(now, s_frames_done);
        if (edge[CYC_SC_FULLSCREEN]) {
            s_set.fullscreen = s_set.fullscreen ? 0 : 1;
            apply_settings();
            save_settings();
        }
        if (edge[CYC_SC_SAVE_STATE] && !menu_open()) save_state_slot();
        if (edge[CYC_SC_LOAD_STATE] && !menu_open()) load_state_slot();
        if (edge[CYC_SC_SCREENSHOT]) {
            char name[64];
            int pw, ph;
            const uint32_t *pic = picture(&pw, &ph);
            snprintf(name, sizeof(name), "cyc_shot_%04d.png", shot++);
            if (cyc_write_png(name, pic, pw, ph)) printf("saved %s\n", name);
        }
        open = menu_open();
        if (was_open && !open) {
            hold_input = true;
            s_zapper_release = true;
        }
        was_open = open;

        /* the toast */
        const char *toast_title = NULL, *toast_body = NULL;
        static char tt[64], tb[256];
        CycDiskToast toast;
        if (s_fds && cyc_disk_action_toast(cyc_host_disk_action(), now, &toast)) {
            cyc_binding_hint(&s_set.bind.shortcut[CYC_SC_DISK], disk_hint, sizeof(disk_hint));
            cyc_disk_toast_text(&toast, disk_hint, tt, sizeof(tt), tb, sizeof(tb));
            toast_title = tt;
            toast_body = tb;
        }
        if (!toast_title && s_note_until > now) {
            toast_title = s_note_title;
            toast_body = s_note_body;
        }
        s_toast_up = toast_title != NULL;
        if (s_toast_up) {
            snprintf(s_toast_title, sizeof(s_toast_title), "%s", tt);
            snprintf(s_toast_body, sizeof(s_toast_body), "%s", tb);
        }
#ifdef CYC_WITH_RECOMP_UI
        cyc_ui_set_toast(toast_title, toast_body);
#endif

        bool inactive = s_pause_unfocused && !s_hidden && !(SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS);
        bool loading = false, fast = false;
        if (!open && !inactive) {
            uint8_t pad0 = st.buttons[0], pad1 = st.buttons[1];
            if (hold_input) {
                if (pad0 || pad1) pad0 = pad1 = 0;
                else hold_input = false;
            }
            cyc_host_disk_frame(now, s_frames_done);
            uint8_t buttons[2] = {pad0, pad1};
            cyc_session_input(buttons);
            cyc_set_controller(0, buttons[0]);
            cyc_set_controller(1, buttons[1]);
            zapper_mouse_frame();
            cyc_session_frame_begin();
            cyc_run_frame();
            cyc_session_frame_end();
            frames++;
            cyc_host_frame_done(++s_frames_done);
            loading = s_fds && cyc_host_frame_unpaced();
            fast = st.shortcut[CYC_SC_FAST_FORWARD] || loading;
        }
        int16_t pcm[4096];
        size_t n;
        while ((n = cyc_audio_read(pcm, 4096)) > 0) {
            /* Keep latency bounded: skip a frame's audio if ~100 ms are queued. */
            if (!dev || fast || open || inactive || !s_set.audio_enabled || SDL_GetQueuedAudioSize(dev) >= (Uint32)(have.freq / 10) * 2)
                continue;
            if (s_set.volume < 100)
                for (size_t i = 0; i < n; ++i) pcm[i] = (int16_t)(pcm[i] * s_set.volume / 100);
            SDL_QueueAudio(dev, pcm, (Uint32)(n * sizeof(int16_t)));
        }

        /* A fast-loaded frame is shown only if 1/60 s passed since the last one. */
        Uint64 tnow = SDL_GetPerformanceCounter();
        if (!loading || tnow - shown >= (Uint64)(frame_seconds * (double)freq)) {
            show(loading, toast_title, toast_body);
            if (s_present_out && s_present_every > 0 && loops % s_present_every == 0) present_shot(s_frames_done);
            SDL_RenderPresent(s_ren);
            shown = tnow;
            /* the one point a new width applies: after this present, before the next picture */
            cyc_video_apply_pending();
        }
        loops++;
        tnow = SDL_GetPerformanceCounter();
#ifdef CYC_DEV_UI
        if (tnow - fps_mark >= freq) {
            double secs = (double)(tnow - fps_mark) / (double)freq;
            uint64_t cycles = cyc_cycle_count() - cycles_mark;
            double native_pct = cycles ? 100.0 * (double)(cyc_run_native_cycles - native_mark) / (double)cycles : 0.0;
            char t[256];
            snprintf(t, sizeof(t), "%s - %.1f%% of CPU cycles recompiled%s - %.0f fps", title, native_pct,
                     cyc_run_native ? "" : " [interpreter only, F2]", frames / secs);
            SDL_SetWindowTitle(s_win, t);
            fps_mark = tnow;
            native_mark = cyc_run_native_cycles;
            cycles_mark = cyc_cycle_count();
            frames = 0;
        }
#else
        (void)fps_mark; (void)native_mark; (void)cycles_mark; (void)frames;
#endif
        if (s_exit_after >= 0 && s_frames_done >= s_exit_after) s_running = false;

        if (fast) {
            next = tnow;
            continue;
        }
        next += (Uint64)(frame_seconds * (double)freq);
        if (next > tnow) {
            Uint32 ms = (Uint32)((next - tnow) * 1000 / freq);
            if (ms > 1) SDL_Delay(ms - 1);
            while (SDL_GetPerformanceCounter() < next) {}
        } else if (tnow - next > freq / 4) {
            next = tnow; /* fell behind: don't try to catch up */
        }
    }

    save_settings();
    cyc_tcp_stop();
#ifdef CYC_WITH_RECOMP_UI
    if (s_have_menu) cyc_ui_shutdown();
#endif
    if (dev) SDL_CloseAudioDevice(dev);
    for (int i = 0; i < s_pad_count; ++i) SDL_GameControllerClose(s_pads[i]);
    if (s_vpad_joy) SDL_JoystickClose(s_vpad_joy);
    SDL_DestroyTexture(s_tex);
    SDL_DestroyRenderer(s_ren);
    SDL_DestroyWindow(s_win);
    SDL_Quit();
    return 0;
}
