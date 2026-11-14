/* Optional HD texture presentation. Observes the real PPU's fetch and pixel
 * pipeline; never reads the PPU bus or advances the guest machine. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "hdpack_jobs.h"
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
/* Whole-picture provider: consume tile inputs directly, without clocking the
 * fetch/shifter pipeline. These products are presentation-only. A retained
 * prefix uses the existing serialized pipeline.bg[] tile records. */
void cyc_hdpack_native_begin(void);
void cyc_hdpack_native_tile(uint16_t address,uint8_t lo,uint8_t hi,bool sprite);
void cyc_hdpack_native_background(unsigned x,unsigned tile_x,unsigned palette,unsigned color,uint8_t backdrop);
void cyc_hdpack_native_sprite(unsigned x,unsigned tile_x,unsigned attr,unsigned color);
void cyc_hdpack_native_hold(unsigned slot,unsigned tile_x);
void cyc_hdpack_native_shift(unsigned count);
void cyc_hdpack_native_prefix(unsigned x,unsigned slot,unsigned palette,unsigned color);
void cyc_hdpack_native_output(unsigned first,unsigned end,unsigned line);
const uint32_t *cyc_hdpack_present(const uint32_t *native,int *width,int *height);
/* Borrowed immutable frame, held until the next preparation/reset. Does not
 * allocate or draw an enhanced framebuffer. */
const HdJobFrame *cyc_hdpack_jobs(const uint32_t *native);
/* Fixed native-size enhanced preview for state thumbnails, at any GPU scale. */
const uint32_t *cyc_hdpack_thumbnail(const uint32_t *native);
#ifdef NESRECOMP_CYCLE_HDPACK_MODS
bool cyc_hdpack_mod_prepare(void);
const uint32_t *cyc_hdpack_mod_present(int *width,int *height);
#endif
