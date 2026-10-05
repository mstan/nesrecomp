/* input_test.c - the windowed cycle host's input actions (cyc_input.h) and
 * their persistence (cyc_settings.h): defaults, the text forms of every
 * binding kind, keyboard and controller evaluation, shortcut ownership,
 * controller assignment, and a config.ini round trip (every binding changed,
 * saved, reloaded into a fresh default set, compared field by field), plus
 * damaged and foreign files. Links SDL for its key and controller names only;
 * no window or device is opened. */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include "cyc_input.h"
#include "cyc_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static uint8_t keys[SDL_NUM_SCANCODES];

static CycInputFrame frame(int pads)
{
    CycInputFrame f;
    memset(&f, 0, sizeof(f));
    f.keys = keys;
    f.key_count = SDL_NUM_SCANCODES;
    f.pad_count = pads;
    return f;
}

static void text_forms(void)
{
    char t[64];
    int v;
    cyc_key_text(SDL_SCANCODE_Z, t, sizeof(t));
    CHECK(!strcmp(t, "Z") && cyc_key_parse(t, &v) && v == SDL_SCANCODE_Z);
    cyc_key_text(SDL_SCANCODE_RSHIFT, t, sizeof(t));
    CHECK(!strcmp(t, "Right Shift") && cyc_key_parse(t, &v) && v == SDL_SCANCODE_RSHIFT);
    cyc_key_text(0, t, sizeof(t));
    CHECK(!strcmp(t, "None") && cyc_key_parse(t, &v) && v == 0);
    CHECK(cyc_key_parse("rshift", &v) && v == SDL_SCANCODE_RSHIFT);       /* the runner's alias */
    CHECK(cyc_key_parse("  escape", &v) && v == SDL_SCANCODE_ESCAPE);
    CHECK(!cyc_key_parse("NoSuchKey", &v));
    /* every scancode SDL names round-trips */
    for (int sc = 1; sc < SDL_NUM_SCANCODES; ++sc) {
        const char *name = SDL_GetScancodeName((SDL_Scancode)sc);
        if (!name || !*name) continue;
        cyc_key_text(sc, t, sizeof(t));
        int back = -1;
        CHECK(cyc_key_parse(t, &back));
        CHECK(back == (int)SDL_GetScancodeFromName(name));
    }
    /* every controller button, every axis direction, chords */
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) {
        cyc_pad_text(CYC_PAD_BUTTON(b), t, sizeof(t));
        CHECK(cyc_pad_parse(t, &v) && v == CYC_PAD_BUTTON(b));
    }
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; ++a)
        for (int pos = 0; pos < 2; ++pos) {
            cyc_pad_text(CYC_PAD_AXIS(a, pos), t, sizeof(t));
            CHECK(cyc_pad_parse(t, &v) && v == CYC_PAD_AXIS(a, pos));
        }
    uint32_t chord = (1u << SDL_CONTROLLER_BUTTON_BACK) | (1u << SDL_CONTROLLER_BUTTON_START);
    cyc_pad_text(CYC_PAD_COMBO(chord), t, sizeof(t));
    CHECK(!strcmp(t, "back+start") && cyc_pad_parse(t, &v) && v == CYC_PAD_COMBO(chord));
    CHECK(cyc_pad_parse("lefttrigger+", &v) && v == CYC_PAD_AXIS(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1));
    CHECK(cyc_pad_parse("LeftX-", &v) && v == CYC_PAD_AXIS(SDL_CONTROLLER_AXIS_LEFTX, 0));
    CHECK(cyc_pad_parse("a, b", &v) && v == CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_A));   /* runner list: first */
    CHECK(cyc_pad_parse("a+a", &v) && v == CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_A));     /* one button, once */
    CHECK(cyc_pad_parse("none", &v) && v == 0 && cyc_pad_parse("", &v) && v == 0);
    CHECK(!cyc_pad_parse("paddle9", &v) && !cyc_pad_parse("a+paddle9", &v) && !cyc_pad_parse("nosuchaxis+", &v));
    /* hints */
    CycBinding hb = { SDL_SCANCODE_D, CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) };
    cyc_binding_hint(&hb, t, sizeof(t));
    CHECK(!strcmp(t, "D / LB"));
    hb.key = 0;
    cyc_binding_hint(&hb, t, sizeof(t));
    CHECK(!strcmp(t, "LB"));
    hb.pad = CYC_PAD_AXIS(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1);
    cyc_binding_hint(&hb, t, sizeof(t));
    CHECK(!strcmp(t, "RT"));
    hb.pad = 0;
    cyc_binding_hint(&hb, t, sizeof(t));
    CHECK(!strcmp(t, "DISK"));
}

static void defaults(void)
{
    CycBindings b;
    cyc_bindings_default(&b);
    CHECK(b.source[0] == 1 && b.source[1] == 2);
    CHECK(b.button[0][0].key == SDL_SCANCODE_Z && b.button[0][1].key == SDL_SCANCODE_X);
    CHECK(b.button[0][2].key == SDL_SCANCODE_BACKSLASH && b.button[0][3].key == SDL_SCANCODE_RETURN);
    CHECK(b.button[1][0].key == 0);                          /* player 2 has no keyboard layout */
    CHECK(b.shortcut[CYC_SC_DISK].key == SDL_SCANCODE_D);
    CHECK(b.shortcut[CYC_SC_DISK].pad == CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    CHECK(b.shortcut[CYC_SC_MENU].key == SDL_SCANCODE_ESCAPE);
    /* no shortcut default collides with a controller default, a dev key (F1-F7)
     * or another shortcut */
    for (int s = 0; s < CYC_SC_COUNT; ++s) {
        const CycBinding *sb = &b.shortcut[s];
        CHECK(!(sb->key >= SDL_SCANCODE_F1 && sb->key <= SDL_SCANCODE_F7));
        for (int p = 0; p < CYC_INPUT_PLAYERS; ++p)
            for (int i = 0; i < CYC_INPUT_BUTTONS; ++i) {
                CHECK(!sb->key || sb->key != b.button[p][i].key);
                CHECK(!sb->pad || sb->pad != b.button[p][i].pad);
            }
        for (int o = 0; o < s; ++o) {
            CHECK(!sb->key || sb->key != b.shortcut[o].key);
            CHECK(!sb->pad || sb->pad != b.shortcut[o].pad);
        }
    }
}

static void evaluation(void)
{
    CycBindings b;
    cyc_bindings_default(&b);
    CycInputState st;
    memset(keys, 0, sizeof(keys));
    CycInputFrame f = frame(0);
    cyc_input_eval(&b, &f, &st);
    CHECK(!st.buttons[0] && !st.buttons[1] && st.player_pad[0] == -1 && st.player_pad[1] == -1);
    keys[SDL_SCANCODE_Z] = keys[SDL_SCANCODE_RETURN] = keys[SDL_SCANCODE_LEFT] = 1;
    cyc_input_eval(&b, &f, &st);
    CHECK(st.buttons[0] == (0x80 | 0x10 | 0x02) && st.buttons[1] == 0);
    keys[SDL_SCANCODE_RIGHT] = 1;                            /* left + right cancel */
    cyc_input_eval(&b, &f, &st);
    CHECK(st.buttons[0] == (0x80 | 0x10));
    keys[SDL_SCANCODE_UP] = keys[SDL_SCANCODE_DOWN] = 1;     /* ...and up + down */
    cyc_input_eval(&b, &f, &st);
    CHECK(st.buttons[0] == (0x80 | 0x10));
    keys[SDL_SCANCODE_DOWN] = 0;
    cyc_input_eval(&b, &f, &st);
    CHECK(st.buttons[0] == (0x80 | 0x10 | 0x08));
    memset(keys, 0, sizeof(keys));

    /* player 2 on the first controller; player 1's keyboard does not read it */
    f = frame(1);
    f.pad_guid[0] = "pad-a";
    f.pad_buttons[0] = (1u << SDL_CONTROLLER_BUTTON_A) | (1u << SDL_CONTROLLER_BUTTON_DPAD_UP);
    cyc_input_eval(&b, &f, &st);
    CHECK(st.player_pad[0] == -1 && st.player_pad[1] == 0);
    CHECK(st.buttons[0] == 0 && st.buttons[1] == (0x80 | 0x08));
    /* the left stick works the D-pad past the deadzone only */
    f.pad_buttons[0] = 0;
    f.pad_axes[0][SDL_CONTROLLER_AXIS_LEFTX] = 32767 * 29 / 100;
    cyc_input_eval(&b, &f, &st);
    CHECK(st.buttons[1] == 0);
    f.pad_axes[0][SDL_CONTROLLER_AXIS_LEFTX] = 32767 * 31 / 100;
    f.pad_axes[0][SDL_CONTROLLER_AXIS_LEFTY] = -32768;
    cyc_input_eval(&b, &f, &st);
    CHECK(st.buttons[1] == (0x01 | 0x08));
    memset(f.pad_axes, 0, sizeof(f.pad_axes));

    /* player 1 on a controller: its named device wins over order */
    b.source[0] = 2;
    snprintf(b.device[0], sizeof(b.device[0]), "pad-b");
    f = frame(2);
    f.pad_guid[0] = "pad-a";
    f.pad_guid[1] = "pad-b";
    f.pad_buttons[1] = 1u << SDL_CONTROLLER_BUTTON_X;        /* NES B on pad-b */
    f.pad_buttons[0] = 1u << SDL_CONTROLLER_BUTTON_START;
    cyc_input_eval(&b, &f, &st);
    CHECK(st.player_pad[0] == 1 && st.player_pad[1] == 0);
    CHECK(st.buttons[0] == 0x40 && st.buttons[1] == 0x10);
    /* a named device that is not connected: the first free one */
    snprintf(b.device[0], sizeof(b.device[0]), "gone");
    cyc_input_eval(&b, &f, &st);
    CHECK(st.player_pad[0] == 0 && st.player_pad[1] == 1);
    b.source[1] = 0;                                         /* none */
    cyc_input_eval(&b, &f, &st);
    CHECK(st.buttons[1] == 0 && st.player_pad[1] == -1);
    cyc_bindings_default(&b);

    /* shortcuts: from the keyboard and from any controller */
    f = frame(2);
    keys[SDL_SCANCODE_D] = 1;
    cyc_input_eval(&b, &f, &st);
    CHECK(st.shortcut[CYC_SC_DISK] && !st.shortcut[CYC_SC_MENU]);
    keys[SDL_SCANCODE_D] = 0;
    f.pad_buttons[1] = 1u << SDL_CONTROLLER_BUTTON_LEFTSHOULDER;   /* a pad no player reads */
    cyc_input_eval(&b, &f, &st);
    CHECK(st.shortcut[CYC_SC_DISK]);
    f.pad_buttons[1] = 0;
    f.pad_axes[0][SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 20000;
    cyc_input_eval(&b, &f, &st);
    CHECK(st.shortcut[CYC_SC_FAST_FORWARD]);
    f.pad_axes[0][SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 10000;       /* under half travel */
    cyc_input_eval(&b, &f, &st);
    CHECK(!st.shortcut[CYC_SC_FAST_FORWARD]);
    memset(f.pad_axes, 0, sizeof(f.pad_axes));

    /* a key bound to a shortcut and a button is the shortcut's */
    b.shortcut[CYC_SC_DISK].key = SDL_SCANCODE_Z;
    f = frame(0);
    keys[SDL_SCANCODE_Z] = 1;
    cyc_input_eval(&b, &f, &st);
    CHECK(st.shortcut[CYC_SC_DISK] && st.buttons[0] == 0);
    keys[SDL_SCANCODE_Z] = 0;
    /* ...and a chord's buttons are the shortcut's only while the whole chord is held */
    cyc_bindings_default(&b);
    b.shortcut[CYC_SC_MENU].pad =
        CYC_PAD_COMBO((1u << SDL_CONTROLLER_BUTTON_BACK) | (1u << SDL_CONTROLLER_BUTTON_START));
    f = frame(1);
    f.pad_buttons[0] = 1u << SDL_CONTROLLER_BUTTON_START;
    cyc_input_eval(&b, &f, &st);
    CHECK(!st.shortcut[CYC_SC_MENU] && st.buttons[1] == 0x10);
    f.pad_buttons[0] |= 1u << SDL_CONTROLLER_BUTTON_BACK;
    cyc_input_eval(&b, &f, &st);
    CHECK(st.shortcut[CYC_SC_MENU] && st.buttons[1] == 0);
    /* an axis bound to a player button */
    cyc_bindings_default(&b);
    b.button[1][0].pad = CYC_PAD_AXIS(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1);
    f = frame(1);
    f.pad_axes[0][SDL_CONTROLLER_AXIS_TRIGGERLEFT] = 30000;
    cyc_input_eval(&b, &f, &st);
    CHECK(st.buttons[1] == 0x80);
    memset(keys, 0, sizeof(keys));
}

typedef struct { int loads; char value[32]; } GameKeys;
static void game_load(void *ctx, const char *key, const char *value)
{
    GameKeys *g = ctx;
    if (!strcmp(key, "HudMode")) { g->loads++; snprintf(g->value, sizeof(g->value), "%s", value); }
}
static void game_save(void *ctx, FILE *f) { fprintf(f, "HudMode = %s\n", ((GameKeys *)ctx)->value); }

static void persistence(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/input_test_config.ini", dir);
    remove(path);
    CycSettings s, back;
    cyc_settings_default(&s);
    CHECK(!cyc_settings_load(&s, path, NULL, NULL));               /* no file: defaults stay */
    /* change every field */
    s.window_scale = 5; s.fullscreen = 2; s.integer_scale = 0; s.linear_filter = 1;
    s.audio_enabled = 0; s.volume = 35; s.skip_launcher = 1;
    s.hdpack_enabled = 0;
    snprintf(s.hdpack_dir,sizeof s.hdpack_dir,"F:/HD Packs/Zelda");
    s.zapper_mouse = 0; s.zapper_crosshair = 0;
    s.fds_hle.auto_swap = 1; s.fds_hle.fast_load = 0;
    for (int p = 0; p < CYC_INPUT_PLAYERS; ++p) {
        s.bind.source[p] = p == 0 ? 2 : 0;
        snprintf(s.bind.device[p], sizeof(s.bind.device[p]), "03000000de280000ff1100000100000%d", p);
        s.bind.deadzone[p] = 12 + p;
        for (int i = 0; i < CYC_INPUT_BUTTONS; ++i) {
            s.bind.button[p][i].key = SDL_SCANCODE_A + i + p * 8;
            s.bind.button[p][i].pad = i % 3 == 0 ? CYC_PAD_BUTTON(i + p)
                                    : i % 3 == 1 ? CYC_PAD_AXIS(i % 6, p)
                                                 : CYC_PAD_COMBO((1u << i) | (1u << (i + 3)));
        }
    }
    for (int i = 0; i < CYC_SC_COUNT; ++i) {
        s.bind.shortcut[i].key = SDL_SCANCODE_F1 + i;
        s.bind.shortcut[i].pad = i == 2 ? 0 : CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_Y + i);
    }
    CHECK(cyc_settings_save(&s, path, NULL));
    cyc_settings_default(&back);
    CHECK(cyc_settings_load(&back, path, stderr, NULL));
    CHECK(back.window_scale == 5 && back.fullscreen == 2 && back.integer_scale == 0 && back.linear_filter == 1);
    CHECK(back.audio_enabled == 0 && back.volume == 35 && back.skip_launcher == 1);
    CHECK(!back.hdpack_enabled && !strcmp(back.hdpack_dir,s.hdpack_dir));
    CHECK(!back.zapper_mouse && !back.zapper_crosshair && back.zapper_keys == 3);
    CHECK(back.fds_hle.auto_swap == 1 && back.fds_hle.fast_load == 0);
    CHECK(!memcmp(&back.bind, &s.bind, sizeof(s.bind)));
    /* the file is what a person can read and edit */
    FILE *f = fopen(path, "r");
    char all[8192];
    size_t n = fread(all, 1, sizeof(all) - 1, f);
    fclose(f);
    all[n] = 0;
    CHECK(strstr(all, "[Keyboard.Shortcuts]\ndisk = F1\n"));
    CHECK(strstr(all, "[Gamepad.Shortcuts]\ndisk = y\n"));
    CHECK(strstr(all, "[FDS]") && strstr(all, "AutoSwap = on\n") && strstr(all, "FastLoad = off\n"));
    /* "default" goes back to saying nothing */
    s.fds_hle = NES_FDS_HLE_ASK_NONE;
    CHECK(cyc_settings_save(&s, path, NULL));
    cyc_settings_default(&back);
    back.fds_hle.auto_swap = 1;
    cyc_settings_load(&back, path, NULL, NULL);
    CHECK(back.fds_hle.auto_swap == -1 && back.fds_hle.fast_load == -1);

    /* a game's view mode and its own [Game] keys round-trip through its hooks */
    s.view_mode = 2;
    GameKeys gk = { 0 };
    snprintf(gk.value, sizeof(gk.value), "wide");
    CycSettingsGame game = { &gk, game_load, game_save };
    CHECK(cyc_settings_save(&s, path, &game));
    GameKeys gk2 = { 0 };
    CycSettingsGame game2 = { &gk2, game_load, game_save };
    cyc_settings_default(&back);
    CHECK(cyc_settings_load(&back, path, stderr, &game2));
    CHECK(back.view_mode == 2 && gk2.loads == 1 && !strcmp(gk2.value, "wide"));
    CHECK(!memcmp(&back.bind, &s.bind, sizeof(s.bind)));      /* the game's keys disturbed nothing */

    /* damaged values keep their defaults and are reported; unknown sections
     * and keys are ignored; the runner's keybinds.ini names parse */
    f = fopen(path, "w");
    fputs("[Display]\nWindowScale = lots\nFullscreen = 9\n[Unknown]\nx = y\n"
          "[Keyboard.Player1]\na = NoSuchKey\nb = rshift\n[Gamepad.Player1]\nstart = a, b\nup = paddle9\n"
          "[Keyboard.Player7]\na = Q\n[FDS]\nAutoSwap = maybe\nFastLoad = on\n"
          "[Zapper]\nMouse = false\nCrosshair = true\n", f);
    fclose(f);
    char logpath[1024];
    snprintf(logpath, sizeof(logpath), "%s/input_test_log.txt", dir);
    FILE *log = fopen(logpath, "w");
    cyc_settings_default(&back);
    CHECK(cyc_settings_load(&back, path, log, NULL));
    fclose(log);
    CycSettings d;
    cyc_settings_default(&d);
    CHECK(back.window_scale == d.window_scale && back.fullscreen == 2);         /* clamped */
    CHECK(back.bind.button[0][0].key == d.bind.button[0][0].key);             /* kept */
    CHECK(back.bind.button[0][1].key == SDL_SCANCODE_RSHIFT);
    CHECK(back.bind.button[0][3].pad == CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_A));
    CHECK(back.bind.button[0][4].pad == d.bind.button[0][4].pad);
    CHECK(back.fds_hle.auto_swap == -1 && back.fds_hle.fast_load == 1);
    CHECK(!back.zapper_mouse && back.zapper_crosshair && back.zapper_keys == 3);
    log = fopen(logpath, "r");
    n = fread(all, 1, sizeof(all) - 1, log);
    fclose(log);
    all[n] = 0;
    CHECK(strstr(all, "WindowScale = lots") && strstr(all, "a = NoSuchKey") && strstr(all, "up = paddle9") &&
          strstr(all, "AutoSwap = maybe"));
    remove(path);
    remove(logpath);
}

int main(int argc, char **argv)
{
    text_forms();
    defaults();
    evaluation();
    persistence(argc > 1 ? argv[1] : ".");
    printf("input_test: %u checks passed\n", checks);
    return 0;
}
