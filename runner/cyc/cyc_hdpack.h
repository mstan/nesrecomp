/* Optional HD texture presentation. Observes the real PPU's fetch and pixel
 * pipeline; never reads the PPU bus or advances the guest machine. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
void cyc_hdpack_config(int enabled,const char *directory);
void cyc_hdpack_power_on(void);
void cyc_hdpack_bg_fetch(uint16_t address,uint8_t value,bool high);
void cyc_hdpack_bg_reload(void);
void cyc_hdpack_bg_shift(void);
void cyc_hdpack_sprite_fetch(unsigned slot,uint16_t address,uint8_t value,bool high);
void cyc_hdpack_sprite_shift(unsigned slot);
void cyc_hdpack_clock(void);
void cyc_hdpack_pixel(unsigned bg_color,unsigned bg_palette,int sprite,unsigned sprite_color);
void cyc_hdpack_blank(uint8_t backdrop);
void cyc_hdpack_output(int x,int y);
const uint32_t *cyc_hdpack_present(const uint32_t *native,int *width,int *height);
#ifdef NESRECOMP_CYCLE_HDPACK_MODS
bool cyc_hdpack_mod_prepare(void);
const uint32_t *cyc_hdpack_mod_present(int *width,int *height);
#endif
