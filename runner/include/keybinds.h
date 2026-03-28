#pragma once
#include <stdint.h>
#include <SDL.h>

typedef struct {
    SDL_Scancode a, b, select, start;
    SDL_Scancode up, down, left, right;
} PlayerBinds;

typedef struct {
    PlayerBinds p1;
    PlayerBinds p2;
} KeyBinds;

/* Initialize keybinds from INI file next to exe. Generates defaults if missing. */
void keybinds_init(const char *exe_path);

/* Get current keybind configuration */
const KeyBinds *keybinds_get(void);

/* Read NES controller byte for player 1 or 2 from SDL keyboard state */
uint8_t keybinds_read_player(const uint8_t *keys, int player);
