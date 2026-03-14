/*
 * nes_runtime.h — NES runtime interface
 * Shared between runner/ and generated/ code.
 * Generated code includes this; runner implements it.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ---- CPU State ---- */
typedef struct {
    uint8_t A, X, Y, S, P;  /* Accumulator, X, Y, Stack Pointer, Processor Status */
    /* Exploded flags for easier codegen */
    uint8_t N, V, D, I, Z, C;
} CPU6502State;

extern CPU6502State g_cpu;
extern uint8_t      g_ram[0x0800];     /* 2KB work RAM */
extern uint8_t      g_chr_ram[0x2000]; /* 8KB CHR RAM */
extern uint8_t      g_ppu_oam[0x100];  /* 64 sprites x 4 bytes */
extern uint8_t      g_ppu_pal[0x20];   /* Palette $3F00-$3F1F */
extern uint8_t      g_ppu_nt[0x1000];  /* Nametable RAM $2000-$2FFF */

/* ---- Memory Interface ---- */
uint8_t  nes_read(uint16_t addr);
void     nes_write(uint16_t addr, uint8_t val);
uint16_t nes_read16(uint16_t addr);       /* Read 16-bit little-endian */
uint16_t nes_read16zp(uint8_t zp_addr);  /* Zero-page 16-bit (wraps at $FF) */

/* ---- Dispatch ---- */
/* Called for JMP (indirect) — dispatch to the correct recompiled function */
void call_by_address(uint16_t addr);

/* Logging for dispatch misses */
void nes_log_dispatch_miss(uint16_t addr);

/* ---- Entry Points (defined in faxanadu_full.c) ---- */
void func_RESET(void);
void func_NMI(void);
void func_IRQ(void);

/* ---- PPU Interface ---- */
/* Called by runtime.c when PPU registers are written */
void ppu_write_reg(uint16_t reg, uint8_t val);
uint8_t ppu_read_reg(uint16_t reg);

/* Render one frame to the framebuffer */
/* framebuf: 256*240 ARGB8888 pixels */
void ppu_render_frame(uint32_t *framebuf);

/* Render OAM debug view: 8x8 grid of 64 sprite slots at 4x scale.
 * buf must be 256*256 ARGB8888 pixels. */
void ppu_render_oam_debug(uint32_t *buf);

/* ---- Mapper Interface ---- */
void mapper_write(uint16_t addr, uint8_t val);
void mapper_init(const uint8_t *prg_data, int prg_banks);

/* ---- Runtime Init ---- */
void runtime_init(void);

/* ---- VBlank Callback ---- */
/* Called by ppu_read_reg when simulated VBlank fires (game reading $2002 with bit7 set).
 * Runner implements this to call func_NMI() + render the frame.
 * This is the NES architectural fix: RESET never returns, so NMI must be
 * injected inline whenever the game reads $2002 during a VBlank period. */
void nes_vblank_callback(void);

/* PPU registers */
extern uint8_t g_ppuctrl;
extern uint8_t g_ppumask;
extern uint8_t g_ppustatus;
extern uint8_t g_ppuscroll_x;
extern uint8_t g_ppuscroll_y;

/* Split-screen: scroll/ppuctrl captured at sprite-0 hit (= HUD values).
 * g_spr0_split_active is 1 if a split occurred this frame. When active,
 * scanlines 0-15 use *_hud values; 16+ use g_ppuscroll_x/y + g_ppuctrl. */
extern uint8_t g_ppuscroll_x_hud;
extern uint8_t g_ppuscroll_y_hud;
extern uint8_t g_ppuctrl_hud;
extern int     g_spr0_split_active;

/* Frame counter incremented each VBlank */
extern uint64_t g_frame_count;

/* Current switchable PRG bank (set by mapper_write) */
extern int g_current_bank;

/* ---- Controller ---- */
/* Button bitmask: bit7=A, bit6=B, bit5=Select, bit4=Start,
 *                 bit3=Up, bit2=Down, bit1=Left, bit0=Right */
extern uint8_t g_controller1_buttons;

/* ---- Dispatch miss monitor ---- */
extern uint32_t g_miss_count_any;
extern uint16_t g_miss_last_addr;
extern uint64_t g_miss_last_frame;
#define MAX_MISS_UNIQUE 12
extern uint16_t g_miss_unique_addrs[MAX_MISS_UNIQUE];
extern int      g_miss_unique_count;

/* ---- Logger ---- */
void log_on_change(const char *label, uint32_t value);
