/*
 * cyc_input.h - the windowed cycle host's input actions and their bindings.
 *
 * Every input the player has is an action with a keyboard binding and a
 * controller binding:
 *
 *   the controller buttons of each player (A B Select Start Up Down Left Right),
 *   read from the keyboard or from a game controller as the player's source
 *   says (config.ini [Input] PlayerNSource: 0 none, 1 keyboard, 2 gamepad);
 *
 *   host shortcuts, read from the keyboard and from every open controller:
 *   Disk (the FDS side swap, cyc_disk_action.h; offered only for a disk
 *   program), Menu (the runtime menu), Fast-forward (held), Screenshot,
 *   Fullscreen, Save state, Load state.
 *
 * A keyboard binding is an SDL scancode (0: unbound). A controller binding
 * uses recomp-ui's portable encoding (recomp_launcher.h RECOMP_LAUNCHER_PAD_*),
 * so the launcher's binding editor edits these values directly: 0 unbound,
 * 1 + button, 100 + axis * 2 + positive, 1000 + a mask of buttons held together
 * (a chord). In config.ini they are SDL's own names: "Z", "Left Shift",
 * "a", "leftshoulder", "lefttrigger+", "leftx-", "back+start", "none".
 *
 * A key or button bound to a shortcut is the shortcut's: while the shortcut is
 * held it does not also press a controller button.
 *
 * cyc_input_eval() is a pure function of the bindings and one frame of device
 * state, so the whole mapping is unit-tested without a window
 * (runner/cyc/input_test.c).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#ifndef CYC_INPUT_PLAYERS
#define CYC_INPUT_PLAYERS 2
#endif
#if CYC_INPUT_PLAYERS < 2 || CYC_INPUT_PLAYERS > 4
#error CYC_INPUT_PLAYERS must be between 2 and 4
#endif
#define CYC_INPUT_BUTTONS 8      /* NES bit order, MSB first: A B Select Start Up Down Left Right */
#define CYC_INPUT_MAX_PADS 8
#define CYC_INPUT_PAD_AXES 6     /* SDL_CONTROLLER_AXIS_MAX */

typedef enum {
    CYC_SC_DISK,
    CYC_SC_MENU,
    CYC_SC_FAST_FORWARD,
    CYC_SC_SCREENSHOT,
    CYC_SC_FULLSCREEN,
    CYC_SC_SAVE_STATE,       /* the save state slot (cyc_state.h) */
    CYC_SC_LOAD_STATE,
    CYC_SC_COUNT
} CycShortcut;

/* recomp-ui's portable controller-binding encoding (recomp_launcher.h). */
#define CYC_PAD_BUTTON(code) (1 + (code))
#define CYC_PAD_AXIS(code, positive) (100 + (code) * 2 + ((positive) ? 1 : 0))
#define CYC_PAD_COMBO(mask) (1000 + (int)(mask))
#define CYC_PAD_IS_BUTTON(v) ((v) > 0 && (v) < 100)
#define CYC_PAD_IS_AXIS(v) ((v) >= 100 && (v) < 1000)
#define CYC_PAD_IS_COMBO(v) ((v) >= 1000)

typedef struct {
    int key;   /* SDL_Scancode, 0 = unbound */
    int pad;   /* portable encoding, 0 = unbound */
} CycBinding;

typedef struct {
    int  source[CYC_INPUT_PLAYERS];            /* 0 none, 1 keyboard, 2 gamepad */
    char device[CYC_INPUT_PLAYERS][40];        /* preferred controller GUID ("" = any free one) */
    int  deadzone[CYC_INPUT_PLAYERS];          /* 0..100 percent of full stick travel */
    CycBinding button[CYC_INPUT_PLAYERS][CYC_INPUT_BUTTONS];
    CycBinding shortcut[CYC_SC_COUNT];
} CycBindings;

void cyc_bindings_default(CycBindings *b);

/* Names: settings keys ("a", "disk") and player-facing labels. */
const char *cyc_button_key(int button);        /* "a" "b" "select" "start" "up" "down" "left" "right" */
const char *cyc_button_label(int button);      /* "A" ... "Right" */
const char *cyc_shortcut_key(int shortcut);    /* "disk" "menu" "fast_forward" "screenshot" "fullscreen" "save_state" "load_state" */
const char *cyc_shortcut_label(int shortcut);  /* "Disk (swap side)" ... */

/* Text forms (config.ini): false when the text names nothing SDL knows. */
void cyc_key_text(int scancode, char *out, size_t n);
bool cyc_key_parse(const char *text, int *scancode);
void cyc_pad_text(int binding, char *out, size_t n);
bool cyc_pad_parse(const char *text, int *binding);
/* A short label for toasts and the bar: "D", "LB", "RT", "BACK+START". */
void cyc_binding_hint(const CycBinding *b, char *out, size_t n);

/* One frame of device state. */
typedef struct {
    const uint8_t *keys;                       /* SDL_GetKeyboardState */
    int            key_count;
    int            pad_count;
    uint32_t       pad_buttons[CYC_INPUT_MAX_PADS];            /* bit n: SDL_GameControllerButton n */
    int16_t        pad_axes[CYC_INPUT_MAX_PADS][CYC_INPUT_PAD_AXES];
    const char    *pad_guid[CYC_INPUT_MAX_PADS];
} CycInputFrame;

typedef struct {
    uint8_t buttons[CYC_INPUT_PLAYERS];        /* NES pad bits, opposite directions resolved */
    bool    shortcut[CYC_SC_COUNT];            /* held this frame */
    int     player_pad[CYC_INPUT_PLAYERS];     /* index into the frame's pads, -1 none */
} CycInputState;

/* Which pad each gamepad-source player reads: its device GUID if connected,
 * else the first connected pad no earlier player took. */
void cyc_input_assign(const CycBindings *b, const CycInputFrame *f, int player_pad[CYC_INPUT_PLAYERS]);

void cyc_input_eval(const CycBindings *b, const CycInputFrame *f, CycInputState *out);

#ifdef __cplusplus
}
#endif
