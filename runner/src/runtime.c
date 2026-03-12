/*
 * runtime.c — NES memory map, PPU register stubs, hardware I/O
 */
#include "nes_runtime.h"
#include "mapper.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

CPU6502State g_cpu;
uint8_t      g_ram[0x0800];
uint8_t      g_chr_ram[0x2000];
uint8_t      g_ppu_oam[0x100];
uint8_t      g_ppu_pal[0x20];
uint8_t      g_ppu_nt[0x1000]; /* 4KB nametable RAM: $2000-$2FFF */

uint8_t g_ppuctrl     = 0;
uint8_t g_ppumask     = 0;
uint8_t g_ppustatus   = 0;
uint8_t g_oamaddr     = 0;
uint8_t g_ppuscroll_x = 0;
uint8_t g_ppuscroll_y = 0;
uint16_t g_ppuaddr    = 0;
static int g_ppuaddr_latch = 0;
static int g_scroll_latch  = 0;
static int s_spr0_reads    = 0; /* sprite-0 hit simulation counter, reset at each VBlank */

uint64_t g_frame_count = 0;

/* ---- Controller state ---- */
uint8_t g_controller1_buttons = 0;
static uint8_t s_ctrl1_shift   = 0;
static bool    s_ctrl1_strobe  = false;

static FILE *s_ppu_trace = NULL;

static void ppu_trace_init(void) {
    s_ppu_trace = fopen("C:/temp/ppu_trace.csv", "w");
    if (s_ppu_trace) {
        fprintf(s_ppu_trace, "DIR,ADDR,VALUE,PC,FRAME\n");
        fflush(s_ppu_trace);
    }
}

static void ppu_trace_write(uint16_t reg, uint8_t val) {
    static uint32_t s_trace_count = 0;
    if (s_ppu_trace && s_trace_count < 50000) {
        fprintf(s_ppu_trace, "W,$%04X,$%02X,PC=?,F=%llu\n",
                reg, val, (unsigned long long)g_frame_count);
        fflush(s_ppu_trace);
        s_trace_count++;
    }
}

void runtime_init(void) {
    memset(&g_cpu, 0, sizeof(g_cpu));
    memset(g_ram,     0, sizeof(g_ram));
    memset(g_chr_ram, 0, sizeof(g_chr_ram));
    memset(g_ppu_oam, 0, sizeof(g_ppu_oam));
    memset(g_ppu_pal, 0, sizeof(g_ppu_pal));
    memset(g_ppu_nt,  0, sizeof(g_ppu_nt));
    g_cpu.S = 0xFD;
    g_cpu.I = 1;
    ppu_trace_init();
}

/* Wall-clock VBlank simulation: fires NMI every ~16ms (60Hz).
 * Uses clock() which on MSVC gives millisecond resolution.
 * This fires regardless of how many memory operations the game performs,
 * so it works for both read-heavy and write-heavy game loops. */
static bool s_vblank_firing = false;

extern int g_turbo;

static void maybe_trigger_vblank(void) {
    if (s_vblank_firing) return;
    static clock_t s_last = 0;
    clock_t now = clock();
    /* CLOCKS_PER_SEC/60 = period for 60Hz. On MSVC CLOCKS_PER_SEC=1000 → 16 ticks.
     * In turbo mode skip the throttle — fire on every call. */
    if (!g_turbo && (now - s_last) < (CLOCKS_PER_SEC / 60)) return;
    s_last = now;
    s_vblank_firing = true;
    /* Set VBlank (bit7), clear sprite-0 hit (bit6) — standard NES VBlank start.
     * Also reset sprite-0 read counter so main-loop $2002 reads between VBlanks
     * don't poison the count and cause the first spin-wait to loop forever. */
    g_ppustatus = (g_ppustatus & ~0x40) | 0x80;
    s_spr0_reads = 0;
    nes_vblank_callback();
    s_vblank_firing = false;
    /* Update s_last so we don't immediately re-fire after a long callback */
    s_last = clock();
}

uint8_t nes_read(uint16_t addr) {
    maybe_trigger_vblank();
    if (addr <= 0x1FFF) return g_ram[addr & 0x07FF];
    if (addr >= 0x2000 && addr <= 0x3FFF) return ppu_read_reg(0x2000 + (addr & 7));
    if (addr >= 0x4000 && addr <= 0x401F) {
        if (addr == 0x4016) {
            if (s_ctrl1_strobe) return 0x40 | (g_controller1_buttons >> 7);
            uint8_t bit = (s_ctrl1_shift & 0x80) ? 1 : 0;
            s_ctrl1_shift <<= 1;
            return 0x40 | bit;
        }
        if (addr == 0x4017) return 0x40; /* controller 2 not connected */
        return 0;
    }
    if (addr >= 0x8000 && addr <= 0xBFFF) {
        const uint8_t *bank = mapper_get_switchable_bank();
        return bank ? bank[addr - 0x8000] : 0xFF;
    }
    if (addr >= 0xC000) {
        const uint8_t *bank = mapper_get_fixed_bank();
        return bank ? bank[addr - 0xC000] : 0xFF;
    }
    return 0xFF;
}

void nes_write(uint16_t addr, uint8_t val) {
    maybe_trigger_vblank();

    if (addr <= 0x1FFF) { g_ram[addr & 0x07FF] = val; return; }
    if (addr >= 0x2000 && addr <= 0x3FFF) { ppu_write_reg(0x2000 + (addr & 7), val); return; }
    if (addr == 0x4014) {
        uint16_t src = (uint16_t)val << 8;
        for (int i = 0; i < 256; i++) g_ppu_oam[i] = nes_read(src + i);
        return;
    }
    if (addr == 0x4016) {
        if (val & 1) {
            s_ctrl1_strobe = true;
        } else if (s_ctrl1_strobe) {
            s_ctrl1_strobe = false;
            s_ctrl1_shift  = g_controller1_buttons; /* latch on falling edge */
        }
        return;
    }
    if (addr >= 0x4000 && addr <= 0x401F) return;
    if (addr >= 0x8000) { mapper_write(addr, val); return; }
}

uint16_t nes_read16(uint16_t addr) {
    return (uint16_t)nes_read(addr) | ((uint16_t)nes_read(addr + 1) << 8);
}

uint16_t nes_read16zp(uint8_t zp) {
    return (uint16_t)g_ram[zp] | ((uint16_t)g_ram[(uint8_t)(zp + 1)] << 8);
}

/* Map a PPU address in $2000-$2FFF to a physical offset in g_ppu_nt[].
 * MMC1 supports 4 mirroring modes; only 2KB of physical NT RAM is used.
 *   Mode 0: one-screen lower  — all 4 virtual NTs → physical NT0 (offset 0)
 *   Mode 1: one-screen upper  — all 4 virtual NTs → physical NT1 (offset 0x400)
 *   Mode 2: vertical          — $2000/$2800→NT0, $2400/$2C00→NT1
 *   Mode 3: horizontal        — $2000/$2400→NT0, $2800/$2C00→NT1
 * virtual_idx: 0=$2000, 1=$2400, 2=$2800, 3=$2C00 */
static int nt_phys_offset(uint16_t a) {
    int virt = (a >> 10) & 3;
    int phys;
    switch (mapper_get_mirroring()) {
        case 0:  phys = 0;          break; /* one-screen lower */
        case 1:  phys = 1;          break; /* one-screen upper */
        case 2:  phys = virt & 1;   break; /* vertical:   $2000/$2800→0, $2400/$2C00→1 */
        case 3:  phys = virt >> 1;  break; /* horizontal: $2000/$2400→0, $2800/$2C00→1 */
        default: phys = virt & 1;   break;
    }
    return phys * 0x400 + (a & 0x3FF);
}

void ppu_write_reg(uint16_t reg, uint8_t val) {
    ppu_trace_write(reg, val);
    switch (reg) {
        case 0x2000: g_ppuctrl = val; break;
        case 0x2001: g_ppumask = val; break;
        case 0x2003: g_oamaddr = val; break;
        case 0x2004: g_ppu_oam[g_oamaddr++] = val; break;
        case 0x2005:
            if (!g_scroll_latch) g_ppuscroll_x = val;
            else                 g_ppuscroll_y = val;
            g_scroll_latch ^= 1;
            break;
        case 0x2006:
            if (!g_ppuaddr_latch) g_ppuaddr = (uint16_t)val << 8;
            else                   g_ppuaddr |= val;
            g_ppuaddr_latch ^= 1;
            break;
        case 0x2007: {
            uint16_t a = g_ppuaddr & 0x3FFF;
            if      (a >= 0x3F00) g_ppu_pal[a & 0x1F] = val;
            else if (a >= 0x2000) g_ppu_nt[nt_phys_offset(a)] = val; /* nametable w/ mirroring */
            else                  g_chr_ram[a] = val;                 /* CHR RAM */
            g_ppuaddr += (g_ppuctrl & 0x04) ? 32 : 1;
            break;
        }
    }
}

uint8_t ppu_read_reg(uint16_t reg) {
    switch (reg) {
        case 0x2002: {
            uint8_t s = g_ppustatus;
            g_ppustatus &= ~0x80;  /* clear VBlank flag (standard NES) */
            g_scroll_latch  = 0;
            g_ppuaddr_latch = 0;
            /* Simulate sprite-0 hit (bit6) for split-screen spin-waits.
             * After 3 consecutive reads that find bit6 clear, set sprite-0 hit.
             * s_spr0_reads is file-scope and reset at each VBlank start so that
             * main-loop reads between VBlanks cannot pre-arm the counter. */
            {
                if (s & 0x40) {
                    s_spr0_reads = 0;
                } else {
                    if (++s_spr0_reads >= 3) {
                        g_ppustatus |= 0x40;
                        s_spr0_reads = 0;
                    }
                }
            }
            return s;
        }
        case 0x2004: return g_ppu_oam[g_oamaddr];
        case 0x2007: {
            uint16_t a = g_ppuaddr & 0x3FFF;
            g_ppuaddr += (g_ppuctrl & 0x04) ? 32 : 1;
            if (a >= 0x3F00) return g_ppu_pal[a & 0x1F];
            if (a >= 0x2000) return g_ppu_nt[nt_phys_offset(a)]; /* mirrored */
            return g_chr_ram[a];
        }
    }
    return 0;
}

void nes_log_dispatch_miss(uint16_t addr) {
    static uint32_t last = 0xFFFFFFFF;
    uint32_t key = ((uint32_t)g_current_bank << 16) | addr;
    if (key != last) {
        printf("[Dispatch] MISS: no func for $%04X bank=%d\n", addr, g_current_bank);
        last = key;
    }
}
