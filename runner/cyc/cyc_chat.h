/*
 * cyc_chat.h - a live chat shown over the game and allowed to steer it.
 *
 * The window learns about chat only through its TCP server (cyc_tcp.h); a
 * bridge such as tools/twitch/twitch_bridge.mjs reads a Twitch channel and
 * turns messages and commands into these requests:
 *
 *   chat        {user, text, color "RRGGBB", ms}   a line in the chat panel
 *   chat_banner {text, color, ms}                  a big line across the picture
 *   chat_poll   {title, options "A|B", votes "3|1", ms, countdown, winner, y}
 *               a poll panel centred at y percent of the height, up for
 *               ms (counted down in its title unless countdown false), the
 *               winner (0-based) lit; no options takes it down
 *   chat_pad    {buttons, mode, frames, label}     player 1's input, per frame:
 *               mode press (held), tap (pressed 8 frames, released 8),
 *               block (masked off), invert (Left/Right and/or Up/Down
 *               swapped, as `buttons` names them)
 *   chat_ram    {addr, set|add|or|and, min, max, frames, every, if_addr,
 *               if_eq, label}                      a CPU RAM store as the CPU
 *               would make it (cyc_mod_poke): once now, or again every
 *               `every` frames for `frames` frames; add saturates to min..max;
 *               if_addr/if_eq skip the store while RAM[if_addr] != if_eq
 *   chat_call   {addr, a, x, y}                    one of the program's routines,
 *               now, its RAM effects kept (cyc_mod_call_commit)
 *   chat_config {show, side left|right, valign top|bottom, offset (percent
 *               of the window height from that edge), lines, ms, scale}
 *   chat_clear  {}                                 messages, banner, poll and every effect
 *   chat_state  {}                                 what is up and active
 *
 * A labelled effect is listed with its time left; a new effect with the same
 * label replaces it. Everything drawn here is presentation, in window pixels
 * over the picture and its shader, like the touch controls: the picture
 * (screenshots of it, hashes, comparisons) never contains it. Chat input and
 * RAM effects stay off while a netplay session runs.
 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_Renderer SDL_Renderer;

void cyc_chat_register_tcp(void);
/* Once per emulated frame, just before it runs: player 1's buttons after the
 * chat's input effects, with the RAM effects due this frame stored. */
uint8_t cyc_chat_frame(uint8_t pad);
/* Draw over everything presented so far, in window pixels (the renderer's
 * logical size, viewport, scale and blend mode are restored). */
void cyc_chat_draw(SDL_Renderer *ren);

#ifdef __cplusplus
}
#endif
