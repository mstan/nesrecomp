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
void call_by_address(uint16_t addr);

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
/* framebuf: g_render_width*240 ARGB8888 pixels */
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

/* Check elapsed time and fire VBlank if >=16667us (1/60s) has passed.
 * Uses high-resolution timer (QPC on Windows, clock_gettime on POSIX).
 * In turbo mode, timing is bypassed — fires immediately on every call.
 * Called from generated JMP instructions to ensure games with tight idle
 * loops (no memory reads) still receive timely NMI callbacks. */
void maybe_trigger_vblank(void);

/* PPU registers */
extern uint8_t g_ppuctrl;
extern uint8_t g_ppumask;
extern uint8_t g_ppustatus;
extern uint16_t g_ppuaddr;
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

/* ---- Widescreen ---- */
typedef enum { ASPECT_4_3 = 0, ASPECT_16_9 = 1, ASPECT_21_9 = 2 } AspectRatio;
extern AspectRatio g_aspect_ratio;
extern int g_render_width;      /* 256, 427, or 560 */
extern int g_widescreen_left;   /* extra px on left margin */
extern int g_widescreen_right;  /* extra px on right margin */
void widescreen_set(AspectRatio ar);

/* Nametable column freshness tracking for widescreen stale-tile detection.
 * Each of the 64 virtual NT columns (512px / 8) gets stamped with the current
 * generation when written. Scene transitions bump the generation, making old
 * columns stale. The renderer blanks stale columns in widescreen margins. */
extern uint32_t g_nt_generation;
extern uint32_t g_nt_col_gen[64];

/* World scroll position (absolute, not mod 512).  Set by game extras each
 * frame from game RAM.  Used by the renderer to clamp widescreen margins:
 * left margin blanked where world_x < 0, right blanked beyond write cursor. */
extern int g_ws_world_scroll;

/* Shadow nametable for widescreen runahead.  Filled by game extras each frame
 * by running the column-write pipeline ahead.  The renderer uses this for
 * right-margin pixels (sx >= 256) instead of g_ppu_nt. */
extern uint8_t g_shadow_nt[0x1000];
extern int     g_shadow_nt_valid;
extern int     g_runahead_mode;  /* when set, PPU $2007 writes go to g_shadow_nt */

/* Extended OAM for widescreen sprite runahead.  Filled by game extras by
 * running the game engine with a shifted camera so entities at screen_x 256+
 * get rendered into the 0-255 OAM range.  The renderer draws these at
 * OAM_X + 256 + g_widescreen_left. */
extern uint8_t g_ppu_oam_ext[0x100];
extern int     g_ext_oam_valid;
extern int     g_suppress_vblank;  /* when set, maybe_trigger_vblank() is a no-op */

/* PPU latch save/restore for runahead state preservation */
void runtime_get_latch_state(uint8_t *ppuaddr_latch, uint8_t *scroll_latch);
void runtime_set_latch_state(uint8_t ppuaddr_latch, uint8_t scroll_latch);

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
