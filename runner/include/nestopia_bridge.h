/*
 * nestopia_bridge.h — Nestopia libretro core bridge for NES recomp
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  nestopia_bridge_init(const char *rom_path);
void nestopia_bridge_run_frame(uint8_t buttons);
void nestopia_bridge_get_ram(uint8_t *out);       /* 2KB work RAM */
void nestopia_bridge_get_sram(uint8_t *out);      /* 8KB save RAM */
void nestopia_bridge_get_framebuf_argb(uint32_t *out);  /* 256x240 ARGB */
void nestopia_bridge_write_ram(uint16_t addr, uint8_t val);
void nestopia_bridge_get_vram(uint8_t *out, int *out_size); /* PPU VRAM */
void nestopia_bridge_shutdown(void);

/* Read a byte from Nestopia's CPU address space (any address $0000-$FFFF) */
uint8_t nestopia_bridge_cpu_read(uint16_t addr);

/* PPU register extraction — reads Nestopia's internal PPU state */
typedef struct {
    uint8_t ctrl;       /* $2000 PPUCTRL */
    uint8_t mask;       /* $2001 PPUMASK */
    uint8_t scroll_x;   /* Coarse X + fine X combined (from v register) */
    uint8_t scroll_y;   /* Coarse Y + fine Y combined (from v register) */
} NestopiaPpuRegs;

void nestopia_bridge_get_ppu_regs(NestopiaPpuRegs *out);

/* Full PPU internal state — t, v, w, fine_x, status, oam_addr, scanline */
typedef struct {
    uint16_t t;          /* temporary VRAM addr (scroll latch) — rendering scroll source */
    uint16_t v;          /* current VRAM addr (scroll address) */
    uint8_t  w;          /* write toggle (shared $2005/$2006 latch) */
    uint8_t  fine_x;     /* fine X scroll (3 bits) */
    uint8_t  status;     /* PPUSTATUS ($2002) */
    uint8_t  oam_addr;   /* OAM address */
    int      scanline;   /* current scanline */
    uint8_t  scroll_x_from_t; /* scroll X derived from t (actual rendering scroll) */
    uint8_t  scroll_y_from_t; /* scroll Y derived from t */
    uint8_t  scroll_x_from_v; /* scroll X derived from v */
    uint8_t  scroll_y_from_v; /* scroll Y derived from v */
} NestopiaPpuInternals;

void nestopia_bridge_get_ppu_internals(NestopiaPpuInternals *out);

/* PPU VRAM extraction — reads Nestopia's internal PPU memory */
void nestopia_bridge_get_chr_ram(uint8_t *out, int len);    /* CHR pattern tables (up to 8KB) */
void nestopia_bridge_get_nametable(uint8_t *out, int len);  /* Nametable (up to 4KB, mirrored) */
void nestopia_bridge_get_palette(uint8_t *out);             /* Palette RAM (32 bytes) */
void nestopia_bridge_get_oam(uint8_t *out);                 /* OAM (256 bytes) */

/* CPU register extraction — reads Nestopia's internal CPU state */
typedef struct {
    uint8_t a, x, y, sp, p;
    uint16_t pc;
} NestopiaCpuRegs;

void nestopia_bridge_get_cpu_regs(NestopiaCpuRegs *out);

/* Returns actual PPU mirroring: 2=vertical, 3=horizontal, 0=single-screen, -1=unknown */
int nestopia_bridge_get_mirroring(void);

/* Returns 1 if Nestopia is initialized and loaded, 0 otherwise */
int nestopia_bridge_is_loaded(void);

/* MMC3 mapper state extraction — valid only for MMC3/TxROM games.
 * Returns valid=0 if the loaded board is not MMC3. */
typedef struct {
    int      valid;         /* 1 if board is MMC3, 0 otherwise */
    uint8_t  bank_select;   /* regs.ctrl0 & 0x07 */
    uint8_t  regs[8];       /* chr/prg bank registers (from banks.chr/prg) */
    uint8_t  prg[4];        /* PRG bank mapping */
    uint8_t  irq_latch;
    uint8_t  irq_counter;
    uint8_t  irq_reload;
    uint8_t  irq_enabled;
} NestopiaMapperState;

void nestopia_bridge_get_mapper_state(NestopiaMapperState *out);

#ifdef __cplusplus
}
#endif
