#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Game capability, independent of the NES's two physical controller ports. */
#ifndef NESRECOMP_INPUT_SEATS
#define NESRECOMP_INPUT_SEATS 2
#endif
#if NESRECOMP_INPUT_SEATS < 2 || NESRECOMP_INPUT_SEATS > 4
#error NESRECOMP_INPUT_SEATS must be between 2 and 4
#endif

/* One-based seats. Published once per outer frame after script overrides. */
extern uint8_t g_logical_input[4];
uint8_t nes_input_seat(int seat);
/* Optional host game actions (bit 0 L, bit 1 R), published with the seat.
 * These are distinct from the NES byte. Version 2 online seat payloads carry
 * both bits, and rollback snapshots/digests retain them for all four seats. */
extern uint32_t g_game_input[4];
uint32_t nes_input_game_buttons(int seat);
#ifdef __cplusplus
}
#endif
