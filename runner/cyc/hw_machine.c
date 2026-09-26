/*
 * hw_machine.c - NESRecomp's NES machine around the CPU: the master clock,
 * hw.h (what the CPU sees of each cycle), the CPU memory map and open bus,
 * loading a cartridge (the mapper itself is hw_mapper.c), power-on and the
 * host interface (cyc_core.h).
 *
 * See hw_internal.h for the timing model.
 */
#include "hw_internal.h"

#include "cyc_core.h"
#include "cyc_trace.h"
#include "hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HwMachine  hw;
HwCart     hw_cart;
bool       hw_frame_done;
int        hw_dma_stalls;
/* What CPU RAM holds at power-on (cyc_core.h). It belongs to the machine, not
 * to a host: every host that links this implementation needs it, and the
 * cosimulation harnesses in tools/cyc are hosts too. tric_core.cpp defines
 * its own; the two are never linked together. */
CycRamInit cyc_ram_init = CYC_RAM_PATTERN;

static uint32_t frame_argb[256 * 240];
static void clock_cpu_devices(void)
{
    apu_cycle();
    if (hw_cart.watch_cpu) hw_cart_cpu_clock();
}

/* ------------------------------------------------------------------------- */
/* Master clock                                                              */
/* ------------------------------------------------------------------------- */

/* Tick 4: the CPU samples /NMI (the PPU's output) into its edge detector. */
static inline void sample_nmi(void)
{
    cpu_nmi_input(ppu_nmi_output());
}

static inline void run_tick(unsigned k)
{
    if (k == 4) sample_nmi();
    else if (k == 7) apu_sample_irq();
    unsigned q = (hw.align + k) & 3;
    if (q == 0) ppu_dot();
    else if (q == 2) ppu_half_dot();
    if (k == 0) clock_cpu_devices();
}

void hw_clock_run_ticks(int n)
{
    while (n-- > 0) run_tick(hw.tick++);
}

/* run_tick(1) through run_tick(11), unrolled for each alignment: the usual
 * case, when no register access has run part of the cycle. */
static void run_ticks_1_to_11(void)
{
    switch (hw.align) {
    case 0:
        ppu_half_dot();   /* 2 */
        sample_nmi();     /* 4 */
        ppu_dot();
        ppu_half_dot();   /* 6 */
        apu_sample_irq(); /* 7 */
        ppu_dot();        /* 8 */
        ppu_half_dot();   /* 10 */
        break;
    case 1:
        ppu_half_dot();   /* 1 */
        ppu_dot();        /* 3 */
        sample_nmi();     /* 4 */
        ppu_half_dot();   /* 5 */
        apu_sample_irq(); /* 7 */
        ppu_dot();
        ppu_half_dot();   /* 9 */
        ppu_dot();        /* 11 */
        break;
    case 2:
        ppu_dot();        /* 2 */
        sample_nmi();     /* 4 */
        ppu_half_dot();
        ppu_dot();        /* 6 */
        apu_sample_irq(); /* 7 */
        ppu_half_dot();   /* 8 */
        ppu_dot();        /* 10 */
        break;
    default:
        ppu_dot();        /* 1 */
        ppu_half_dot();   /* 3 */
        sample_nmi();     /* 4 */
        ppu_dot();        /* 5 */
        apu_sample_irq(); /* 7 */
        ppu_half_dot();
        ppu_dot();        /* 9 */
        ppu_half_dot();   /* 11 */
        break;
    }
}

/* run_tick(0) without the CPU's access. */
static inline void run_tick_0(void)
{
    if (hw.align == 0) ppu_dot();
    else if (hw.align == 2) ppu_half_dot();
    clock_cpu_devices();
}

/* ------------------------------------------------------------------------- */
/* hw.h                                                                      */
/* ------------------------------------------------------------------------- */

void hw_cycle_start(uint16_t addr, HwCycleKind kind)
{
    hw.cpu_addr = addr;
    hw.cpu_reading = kind == HW_READ;
    hw_dma_stalls = 0;
    for (;;) {
        if (hw.tick == 1) run_ticks_1_to_11();
        else
            while (hw.tick < 12) run_tick(hw.tick++);
        hw.tick = 0;
        if (!dma_wants_cycle()) return;
        /* RDY: a DMA takes this read cycle; the CPU holds its address. */
        dma_cycle();
        dma_end_of_cycle();
        if (cyc_trace_enabled) cyc_trace_cycle_end(false, hw.data_bus);
        hw.cycles++;
        run_tick(hw.tick++);
        hw_dma_stalls++;
    }
}

uint8_t hw_read(uint16_t addr)
{
    return hw_bus_read(addr);
}

uint8_t hw_read_rom(uint16_t addr, uint8_t value)
{
    hw.data_driven = 1;
    hw.data_bus = hw.internal_bus = value;
    if (cyc_trace_enabled) cyc_trace_access(addr, value, false);
    return value;
}

void hw_write(uint16_t addr, uint8_t value)
{
    hw_bus_write(addr, value);
}

void hw_cycle_finish(bool instruction_done)
{
    dma_end_of_cycle();
    if (cyc_trace_enabled) cyc_trace_cycle_end(instruction_done, hw.data_bus);
    hw.cycles++;
    if (hw.tick == 0) {
        run_tick_0();
        hw.tick = 1;
    } else {
        run_tick(hw.tick++);
    }
}

bool hw_irq_line(void) { return hw.irq_line != 0; }

unsigned hw_prg_bank(uint16_t addr) { return hw_cart.prg_off[(addr >> 12) & 7] >> 13; }
unsigned hw_prg_bank4(uint16_t addr) { return hw_cart.prg_off[(addr >> 12) & 7] >> 12; }

/* ------------------------------------------------------------------------- */
/* CPU memory map                                                            */
/* ------------------------------------------------------------------------- */

uint8_t hw_bus_read(uint16_t addr)
{
    hw.data_driven = 0;
    if (addr >= 0x8000) {
        hw.data_bus = hw_cart_prg_read(addr);
        hw.data_driven = 1;
    } else if (addr < 0x2000) {
        hw.data_bus = hw.ram[addr & 0x7FF];
        hw.data_driven = 1;
    } else if (addr < 0x4000) {
        hw.data_bus = ppu_read(addr);
        hw.data_driven = 1;
    } else if (addr >= 0x4020) {
        /* $4020-$7FFF: the cartridge's work RAM, if the board has any and the
         * mapper has it enabled. Otherwise nothing drives the bus. */
        uint8_t value = hw.data_bus; /* Partially driven cartridge reads. */
        if (hw_cart_cpu_read(addr, &value)) {
            hw.data_bus = value;
            hw.data_driven = 1;
        }
    }

    /* The 2A03 decodes its readable registers from the CPU's address bus and
     * the low bits of the accessed address, so a DMA reading while the CPU
     * holds $4015-$4017 can hit them (AccuracyCoin "DMC DMA Bus
     * Conflicts"). */
    if (hw.cpu_addr >= 0x4000 && hw.cpu_addr <= 0x401F && (addr < 0x2000 || addr >= 0x4000)) {
        unsigned reg = addr & 0x1F;
        if (reg == 0x15) {
            /* Driven on the internal bus only; the data bus keeps its value. */
            uint8_t status = apu_read_status();
            if (cyc_trace_enabled) cyc_trace_access(addr, status, false);
            return status;
        }
        if (reg == 0x16 || reg == 0x17) {
            uint8_t value = (uint8_t)(apu_read_controller((int)reg - 0x16) | (hw.data_bus & 0xE0));
            if (hw_oam_dma_active() && hw.data_driven) {
                /* A driven bus masks the controller bit. */
                if (cyc_trace_enabled) cyc_trace_access(addr, hw.data_bus, false);
                return hw.data_bus;
            }
            hw.data_bus = value;
        }
    }
    hw.internal_bus = hw.data_bus;
    if (cyc_trace_enabled) cyc_trace_access(addr, hw.data_bus, false);
    return hw.data_bus;
}

void hw_bus_write(uint16_t addr, uint8_t value)
{
    if (cyc_trace_enabled) cyc_trace_access(addr, value, true);
    if (addr < 0x2000) hw.ram[addr & 0x7FF] = value;
    else if (addr < 0x4000) ppu_write(addr, value);
    else if (addr <= 0x4017) apu_write(addr, value);
    else if (addr >= 0x4020) hw_cart_cpu_write(addr, value);
    /* $4018-$401F: the 2A03's test registers, not connected on a console. */
    hw.data_bus = value;
    hw.internal_bus = value;
}

/* ------------------------------------------------------------------------- */
/* Cartridge, power-on                                                       */
/* ------------------------------------------------------------------------- */

/* The bank tables index PRG and CHR with a power-of-two mask, which is what a
 * board's address lines do to a bank number past the end of the ROM. Images
 * whose size is not a power of two are padded so the mask stays in bounds. */
static uint32_t round_up_pow2(uint32_t n)
{
    uint32_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

static uint8_t *alloc_padded(const uint8_t *src, size_t len, uint32_t *out_alloc)
{
    uint32_t alloc = round_up_pow2((uint32_t)(len < 4096 ? 4096 : len));
    uint8_t *p = (uint8_t *)calloc(1, alloc);
    if (p && src) memcpy(p, src, len);
    *out_alloc = alloc;
    return p;
}

bool cyc_load_ines(const uint8_t *image, size_t size)
{
    NesCartInfo info;
    if (!nes_cart_image(image, size, &info) || !hw_cart_supports(info.mapper) ||
        !nes_cart_variant_supported(&info)) return false;
    uint32_t prg_alloc, chr_alloc;
    uint32_t chr_len = info.chr_size ? info.chr_size : info.chr_ram + info.chr_nvram;
    uint8_t *prg = alloc_padded(image + info.data_offset, info.prg_size, &prg_alloc);
    uint8_t *chr = alloc_padded(info.chr_size ? image + info.data_offset + info.prg_size : NULL,
                              chr_len, &chr_alloc);
    if (!prg || !chr) { free(prg); free(chr); return false; }
    free(hw_cart.prg);
    free(hw_cart.chr);
    memset(&hw_cart, 0, sizeof(hw_cart));
    hw_cart.info = info;
    hw_cart.prg = prg;
    hw_cart.prg_len = info.prg_size;
    hw_cart.prg_slots = prg_alloc / 4096;
    hw_cart.chr = chr;
    hw_cart.chr_len = chr_len;
    hw_cart.chr_pages = chr_alloc / 1024;
    hw_cart.chr_ram = !info.chr_size;
    hw_cart.mapper = info.mapper;
    hw_cart.wram_len = info.prg_ram + info.prg_nvram;
    hw_cart.mirroring = info.vertical ? HW_MIRROR_VERTICAL : HW_MIRROR_HORIZONTAL;
    hw_cart_power_on();
    return true;
}

void cyc_power_on(uint8_t ppu_alignment)
{
    static bool palette_ready;
    if (!palette_ready) {
        hw_palette_init();
        palette_ready = true;
    }
    memset(&hw, 0, sizeof(hw));
    hw.align = ppu_alignment & 3;
    /* CPU RAM at power-on: runs of $F0 and $0F (the pattern AccuracyCoin's
     * power-on page shows on the reference console; not defined by the
     * hardware). --ram-init selects zeros or ones instead, to compare a
     * program that reads uninitialized RAM against another emulator. */
    for (int i = 0; i < 0x800; i++) {
        bool bit1_clear = (i & 2) == 0, upper = (i & 0x1F) >= 0x10;
        hw.ram[i] = cyc_ram_init == CYC_RAM_ZEROS ? 0x00
                  : cyc_ram_init == CYC_RAM_ONES  ? 0xFF
                  : bit1_clear != upper ? 0xF0 : 0x0F;
    }
    hw_cart_power_on();
    ppu_power_on();
    apu_power_on();
    hw_frame_done = false;
    hw_dma_stalls = 0;

    /* Tick 0 of the first CPU cycle has no CPU access, and the PPU clock
     * starts at its alignment phase: its first dot is not on this tick. */
    if (hw.align == 2) ppu_half_dot();
    clock_cpu_devices();
    hw.tick = 1;
}

uint32_t cyc_prg_hash(void)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < hw_cart.prg_len; i++) h = (h ^ hw_cart.prg[i]) * 16777619u;
    return h;
}

const char *cyc_hw_name(void) { return "nesrecomp"; }

/* ------------------------------------------------------------------------- */
/* Host I/O                                                                  */
/* ------------------------------------------------------------------------- */

const uint8_t *cyc_cpu_ram(void) { return hw.ram; }

void cyc_set_controller(int port, uint8_t buttons) { hw_set_controller(port, buttons); }

uint64_t cyc_cycle_count(void) { return hw.cycles; }

const uint16_t *cyc_frame_index(void) { return hw_frame_index; }

const uint32_t *cyc_frame_argb(void)
{
    for (int i = 0; i < 256 * 240; i++) frame_argb[i] = hw_palette_argb[hw_frame_index[i] & 0x1FF];
    return frame_argb;
}

bool cyc_audio_enable(int sample_rate)
{
    apu_audio_enable(sample_rate > 0, sample_rate);
    return sample_rate > 0;
}

size_t cyc_audio_read(int16_t *out, size_t max) { return apu_audio_read(out, max); }

/* ------------------------------------------------------------------------- */
/* Comparison                                                                */
/* ------------------------------------------------------------------------- */

uint64_t cyc_mem_state_hash(void)
{
    return cyc_mem_hash(hw.cycles, hw.ram, ppu.ciram, hw_cart.info.four_screen ? 4096 : 2048, ppu.oam, ppu.palette, hw_cart.chr_ram ? hw_cart.chr : NULL,
                        hw_cart.chr_len, hw_cart.has_wram ? hw_cart.wram : NULL, hw_cart.wram_len,
                        hw_frame_index);
}

void cyc_mem_state_dump(void *file)
{
    cyc_mem_dump(file, hw.cycles, hw.ram, ppu.ciram, hw_cart.info.four_screen ? 4096 : 2048, ppu.oam, ppu.palette, hw_cart.chr_ram ? hw_cart.chr : NULL,
                 hw_cart.chr_len, hw_cart.has_wram ? hw_cart.wram : NULL, hw_cart.wram_len, hw_frame_index);
}

uint64_t cyc_hw_state_hash(void)
{
    uint64_t h = (uint64_t)ppu.scanline | (uint64_t)ppu.dot << 16 | (uint64_t)hw.tick << 32 |
                 (uint64_t)hw.align << 40;
    uint64_t acc = 0;
    acc = acc * 131 + hw.cpu_addr;
    acc = acc * 131 + hw.cpu_reading;
    acc = acc * 131 + hw.data_bus;
    acc = acc * 131 + hw.internal_bus;
    acc = acc * 131 + hw.data_driven;
    acc = acc * 131 + hw.irq_line;
    h = cyc_trace_mix(h, acc);
    h = hw_cart_state_hash(h);
    h = ppu_state_hash(h);
    return apu_state_hash(h);
}

void cyc_hw_state_dump(void *file)
{
    FILE *f = (FILE *)file;
    fprintf(f, "hw.tick %02X\nhw.align %02X\nhw.cycles %llu\nhw.cpu_addr %04X\nhw.cpu_reading %02X\n"
               "hw.data_bus %02X\nhw.internal_bus %02X\nhw.data_driven %02X\nhw.irq_line %02X\n",
            hw.tick, hw.align, (unsigned long long)hw.cycles, hw.cpu_addr, hw.cpu_reading, hw.data_bus,
            hw.internal_bus, hw.data_driven, hw.irq_line);
    hw_cart_state_dump(file);
    ppu_state_dump(file);
    apu_state_dump(file);
}
