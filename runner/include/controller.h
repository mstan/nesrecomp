#pragma once
#include <stdint.h>
#include <SDL.h>

/*
 * controller.h — SDL2 game-controller (gamepad) support.
 *
 * Cross-platform via SDL's SDL_GameController API: Xbox, PlayStation, Switch
 * Pro and generic pads are recognized through SDL's built-in mapping database
 * (on Windows this sits on top of XInput/DirectInput). The first connected pad
 * drives NES port 1, the second drives port 2; hotplug is supported.
 *
 * Controller input is OR'd with the keyboard in main_runner, so both work at
 * the same time and a controller never has to be configured to start playing.
 */

/* Initialize the game-controller subsystem and open any already-connected
 * pads. Call once, after SDL_Init. Safe to call with no controllers attached. */
void controller_init(void);

/* Feed SDL events here so device add/remove (hotplug) is handled. */
void controller_handle_event(const SDL_Event *ev);

/* Return the NES controller byte for player 1 or 2 from the assigned pad, or 0
 * if no pad is assigned to that player. Bit layout matches keybinds_read_player:
 * A=0x80 B=0x40 SELECT=0x20 START=0x10 UP=0x08 DOWN=0x04 LEFT=0x02 RIGHT=0x01. */
uint8_t controller_read_player(int player);

/* Number of controllers currently connected. */
int controller_count(void);

/* Close all controllers and release resources. */
void controller_shutdown(void);
