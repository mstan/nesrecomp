#pragma once
#include <stdint.h>
#include <SDL.h>

typedef struct {
    SDL_Scancode a, b, select, start;
    SDL_Scancode up, down, left, right;
} PlayerBinds;

typedef struct {
    int mouse_enabled;     /* 1 if mouse controls the Zapper */
    int crosshair;         /* 1 to show crosshair at aim point */
} ZapperBinds;

/* Gamepad bindings. btn_mask is indexed by NES button in this order:
 *   0=A 1=B 2=Select 3=Start 4=Up 5=Down 6=Left 7=Right
 * (matching the bit order of keybinds_read_player). Each entry is a set of
 * SDL_GameControllerButton values OR'd together as (1u << SDL_CONTROLLER_BUTTON_*),
 * so more than one physical button can map to the same NES button. */
typedef struct {
    uint32_t btn_mask[8];
    int      deadzone;     /* left-stick deadzone, 0..32767 */
    int      analog_dpad;  /* 1 = left analog stick also drives the d-pad */
} GamepadBinds;

typedef enum {
    NES_CAMERA_LOOK_UP = 0,
    NES_CAMERA_LOOK_DOWN,
    NES_CAMERA_LOOK_LEFT,
    NES_CAMERA_LOOK_RIGHT,
    NES_CAMERA_ROLL_LEFT,
    NES_CAMERA_ROLL_RIGHT,
    NES_CAMERA_ZOOM_IN,
    NES_CAMERA_ZOOM_OUT,
    NES_CAMERA_SPRITE_SMALLER,
    NES_CAMERA_SPRITE_LARGER,
    NES_CAMERA_RESET,
    NES_CAMERA_TOGGLE,
    NES_CAMERA_BIND_COUNT
} NesCameraBindAction;

typedef struct {
    PlayerBinds  p1;
    PlayerBinds extra[3];
    ZapperBinds  zapper;
    GamepadBinds pad1;
    GamepadBinds pad2;
    GamepadBinds extra_pad[2];
    SDL_Scancode camera[NES_CAMERA_BIND_COUNT];
} KeyBinds;

/* Initialize keybinds from INI file next to exe. Generates defaults if missing. */
void keybinds_init(const char *exe_path);
/* Load existing bindings or defaults, without creating a file. */
void keybinds_init_readonly(const char *exe_path);

/* Get current keybind configuration */
const KeyBinds *keybinds_get(void);

/* Read a logical seat from SDL keyboard state. Games opting into four-seat
 * input may bind each seat; legacy games retain the P1-only keyboard policy. */
uint8_t keybinds_read_player(const uint8_t *keys, int player);

/* Get the gamepad bindings for a one-based logical seat. */
const GamepadBinds *keybinds_get_pad(int player);

/* Returns 1 if the Zapper mouse mode is enabled in keybinds.ini */
int keybinds_zapper_mouse(void);

/* Returns 1 if the Zapper crosshair should be drawn */
int keybinds_zapper_crosshair(void);

/* Optional Voxel/3D camera keys. The launcher exposes these only while an
 * enabled mod advertises camera controls, but the runner always keeps stable
 * defaults so older keybinds.ini files remain immediately usable. */
SDL_Scancode keybinds_camera_key(NesCameraBindAction action);
int keybinds_camera_action_for_scancode(SDL_Scancode scancode);
