/* Adapter for existing presentation-only voxel profiles. The picture and
 * memories come from the cycle machine; no function-level CPU is linked. */
#pragma once
#include <SDL.h>
#include "../include/config.h"
#include "../include/nes_video.h"
#define SCREEN_HEIGHT 240
#define TILE_SIZE 8
extern uint8_t *cyc_presentation_ram;
#define g_ram cyc_presentation_ram
extern uint8_t g_ppu_oam[256], g_ppu_nt[2048], g_ppu_pal[32], g_chr_ram[8192];
extern uint8_t g_ppuctrl, g_ppumask, g_controller1_buttons;
extern uint32_t g_nes_palette[64];
extern int g_render_width, g_widescreen_left, g_widescreen_right, g_ws_eff_left, g_ws_eff_right;
void cyc_presentation_sync(void);
void cyc_presentation_event(const SDL_Event *event, int player);
