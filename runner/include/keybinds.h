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

typedef struct {
    PlayerBinds  p1;
    PlayerBinds  p2;
    ZapperBinds  zapper;
    GamepadBinds pad1;
    GamepadBinds pad2;
} KeyBinds;

/* Initialize keybinds from INI file next to exe. Generates defaults if missing. */
void keybinds_init(const char *exe_path);

/* Get current keybind configuration */
const KeyBinds *keybinds_get(void);

/* Read NES controller byte for player 1 or 2 from SDL keyboard state */
uint8_t keybinds_read_player(const uint8_t *keys, int player);

/* Get the gamepad bindings for player 1 or 2. */
const GamepadBinds *keybinds_get_pad(int player);

/* Returns 1 if the Zapper mouse mode is enabled in keybinds.ini */
int keybinds_zapper_mouse(void);

/* Returns 1 if the Zapper crosshair should be drawn */
int keybinds_zapper_crosshair(void);
