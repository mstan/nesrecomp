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
extern int          g_bail_active;     /* set by stack_bail_func return; checked at JSR sites */

/* ---- Write breakpoints ---- */
/* Set g_write_bp_addr to a RAM address (0-$07FF) to enable.
 * When nes_write hits that address, it calls the callback before writing.
 * Set to 0xFFFF to disable. */
extern uint16_t g_write_bp_addr;
extern uint8_t  g_write_bp_match_val; /* only break if val matches (0xFF = any) */
extern int      g_write_bp_block;     /* set to 1 in callback to block the write */
typedef void (*write_bp_callback_t)(uint16_t addr, uint8_t old_val, uint8_t new_val);
extern write_bp_callback_t g_write_bp_callback;
extern uint8_t      g_sram[0x2000];    /* 8KB battery-backed SRAM ($6000-$7FFF) */
extern uint8_t      g_chr_ram[0x2000]; /* 8KB CHR RAM/ROM */
extern int          g_chr_is_rom;      /* 1 = CHR ROM (ignore $2007 writes to $0000-$1FFF) */
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
int call_by_address(uint16_t addr);  /* returns 1 on hit, 0 on miss */

/* Logging for dispatch misses */
void nes_log_dispatch_miss(uint16_t addr);
void nes_log_inline_miss(uint16_t dispatch_pc, uint8_t a_val);

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

/* Check elapsed time and fire VBlank if >=16ms has passed.
 * Called from generated JMP instructions to ensure games with tight idle
 * loops (no memory reads) still receive timely NMI callbacks. */
void maybe_trigger_vblank(int cycles);
void maybe_fire_pending_vblank(void);
void runtime_set_vblank_firing(int active);
int  runtime_get_vblank_depth(void);

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
extern int     g_spr0_reads_ctr;

/* Frame counter incremented each VBlank */
extern uint64_t g_frame_count;

/* Current switchable PRG bank (set by mapper_write) */
extern int g_current_bank;

/* MMC3 8KB bank alignment flags.
 * The recompiler uses 16KB banks, but MMC3 switches 8KB banks.
 * When R6 is odd, $8000-$9FFF contains the upper 8KB of the 16KB bank,
 * so dispatch addresses in that range need +$2000 offset.
 * When R7 is even, $A000-$BFFF contains the lower 8KB of the 16KB bank,
 * so dispatch addresses in that range need -$2000 offset. */
extern int g_mmc3_r6_odd;     /* 1 if R6 is odd — $8000 addresses need +$2000 */
extern int g_mmc3_r7_even;   /* 1 if R7 is even — $A000 addresses need -$2000 */
extern int g_mmc3_bank_a000; /* R7/2 — 16KB bank index for $A000-$BFFF dispatch */

/* ---- Controller ---- */
/* Button bitmask: bit7=A, bit6=B, bit5=Select, bit4=Start,
 *                 bit3=Up, bit2=Down, bit1=Left, bit0=Right */
extern uint8_t g_controller1_buttons;
extern uint8_t g_controller2_buttons;

/* ---- State accessors for debug ring buffer ---- */
/* These expose private statics from runtime.c for exhaustive state capture. */
void    runtime_get_vblank_state(uint32_t *ops_count, int *vblank_depth);
void    runtime_set_vblank_state(uint32_t ops_count, int vblank_depth);
void    runtime_get_controller_shift(uint8_t *shift1, uint8_t *shift2, uint8_t *strobe);
void    runtime_set_controller_shift(uint8_t shift1, uint8_t shift2, uint8_t strobe);
uint8_t runtime_get_ppudata_buf(void);
void    runtime_set_ppudata_buf(uint8_t val);
uint16_t runtime_get_ppuaddr(void);
void     runtime_set_ppuaddr(uint16_t addr);
extern uint8_t g_oamaddr;

/* ---- Dispatch miss monitor ---- */
extern uint32_t g_miss_count_any;
extern uint16_t g_miss_last_addr;
extern uint64_t g_miss_last_frame;
#define MAX_MISS_UNIQUE 12
extern uint16_t g_miss_unique_addrs[MAX_MISS_UNIQUE];
extern int      g_miss_unique_count;

/* ---- Exe directory (for writing logs next to the binary) ---- */
extern char g_exe_dir[260];  /* set once at startup by launcher */

/* ---- Logger ---- */
void log_on_change(const char *label, uint32_t value);
