/*
 * hw_machine.c - NESRecomp's NES machine around the CPU: the master clock,
 * hw.h (what the CPU sees of each cycle), the CPU memory map and open bus,
 * loading a cartridge (the mapper itself is hw_mapper.c), power-on and the
 * host interface (cyc_core.h).
 *
 * See hw_internal.h for the timing model.
 */
#include "hw_internal.h"
#include "cpu6502.h"

#include "cyc_core.h"
#include "cyc_mod.h"
#include "cyc_ring.h"
#include "cyc_trace.h"
#include "hw.h"
#include "hw_fds.h"
#include "../../common/nes_fds.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>

HwMachine  hw;
HwExtraTiming hw_extra_timing;
HwRegionTiming hw_region_timing;
static CycRegion region_request = CYC_REGION_NTSC;
static bool cpu_opcode_fetch;
HwZapper hw_zapper;
HwCart     hw_cart;
bool       hw_frame_done;
int        hw_observe_line = -1;
bool       hw_observe_hit;
bool       hw_frame_end_hit;
int        hw_dma_stalls;
uint8_t    hw_code_watch[HW_CODE_BYTES];
bool       hw_isolated;
HwNativeInterval hw_native_interval;
uint64_t hw_native_interval_cycles, hw_native_interval_spans;
#if NESRECOMP_ISOLATED_NATIVE
uint64_t hw_native_isolated_cycles, hw_native_isolated_spans;
#endif
uint64_t   hw_isolated_cycles, hw_isolated_budget, hw_isolated_io_writes;
static void no_code_write(unsigned phys, uint8_t value) { (void)phys; (void)value; }
void     (*hw_code_write)(unsigned phys, uint8_t value) = no_code_write;
/* What CPU RAM holds at power-on (cyc_core.h). It belongs to the machine, not
 * to a host: every host that links this implementation needs it, and the
 * cosimulation harnesses in tools/cyc are hosts too. tric_core.cpp defines
 * its own; the two are never linked together. */
CycRamInit cyc_ram_init = CYC_RAM_PATTERN;

static uint32_t frame_argb[256 * 240];
static void clock_cpu_devices(void)
{
    if (!hw_extra_timing.active) apu_clock();
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
    /* The cartridge's M2 clock comes at the CPU edge, before a coincident PPU
     * transition, in the oracle's CPU -> cartridge -> PPU order. MMC5 samples
     * the PPU /RD pin there, and a counter started by a PPU event on this tick
     * (the TC0690's delayed IRQ) first counts on the next CPU cycle. */
    if (k==0 && hw_cart.watch_cpu) hw_cart_cpu_clock();
    if (k == 4) sample_nmi();
    else if (k == 7) apu_sample_irq();
#if NESRECOMP_PPU_FRAME
    frame_ppu_ticks(k,k+1);
#else
    unsigned q = (hw.align + k) & 3;
    if (q == 0) ppu_dot();
    else if (q == 2) ppu_half_dot();
#endif
    if (k == 0) clock_cpu_devices();
    /* Boards clocked before the CPU's access, after the IRQ sample (FDS). */
    if (k == 11 && hw_cart.clock_late) hw_cart_cpu_clock_late();
}

/* PAL transitions use doubled units inside each master tick so the second
 * half of a /5 PPU dot occurs exactly 2.5 master clocks after its first half.
 * NTSC retains its existing unrolled /12 clock path. */
static inline void pal_ppu_edge(unsigned half_phase)
{
    if (half_phase == 0) ppu_dot();
    else if (half_phase == 5) ppu_half_dot();
}

static inline void run_pal_tick(unsigned k)
{
    if (k == 0 && hw_cart.watch_cpu) hw_cart_cpu_clock();
    /* 2A07 M2 is low for 6.5 clocks (2A03: 4.5); the CPU's sample
     * positions move two master clocks later relative to M2's falling edge. */
    if (k == 6) sample_nmi();
    else if (k == 9) apu_sample_irq();
    unsigned q = (2u * (hw_region_timing.phase + k)) % 10u;
    pal_ppu_edge(q);
    if (k == 0) clock_cpu_devices();
    pal_ppu_edge(q + 1);
    if (k == 15 && hw_cart.clock_late) hw_cart_cpu_clock_late();
}

void hw_clock_run_ticks(int n)
{
    if (hw_isolated) return;
    if (hw_pal()) {
        /* PPU register reads finish at the CPU's data sample. The $2000
         * latch delay (2 master clocks) belongs to the PPU and stays 2. */
        if (n == 7) n = 9;
        while (n-- > 0) run_pal_tick(hw.tick++);
    } else while (n-- > 0) run_tick(hw.tick++);
}

/* run_tick(1) through run_tick(11), unrolled for each alignment: the usual
 * case, when no register access has run part of the cycle. */
static void run_ticks_1_to_11(void)
{
    if (ppu_clock_cpu_tail(hw.align)) {
        if (hw_cart.clock_late) hw_cart_cpu_clock_late();
        return;
    }
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
    if (hw_cart.clock_late) hw_cart_cpu_clock_late();   /* 11 */
}

/* run_tick(0) without the CPU's access. */
static inline void run_tick_0(void)
{
    if (hw_cart.watch_cpu) hw_cart_cpu_clock();
#if NESRECOMP_PPU_FRAME
    frame_ppu_ticks(0,1);
#else
    if (hw.align == 0) ppu_dot();
    else if (hw.align == 2) ppu_half_dot();
#endif
    clock_cpu_devices();
}

/* ------------------------------------------------------------------------- */
/* hw.h                                                                      */
/* ------------------------------------------------------------------------- */

static bool native_masked_irq_enabled = true;
void hw_native_set_masked_irq(bool enabled)
{
    hw_native_flush();
    native_masked_irq_enabled = enabled;
}
/* Qualify a constant asserted APU frame IRQ only while the CPU masks IRQ.
 * The cursor watches I on every access; mapper IRQs remain excluded. */
static bool native_masked_irq_ready(void)
{
    return native_masked_irq_enabled && hw.irq_line && cpu.i &&
           hw_cart.mapper != 4 && apu_native_masked_irq_ready();
}

#if NESRECOMP_HEADROOM_COUNTERS
static void hr_interval_end(unsigned n, unsigned credit, unsigned site);
static inline void hr_classify(uint16_t addr, unsigned site);
static uint64_t hr_dma_cycles, hr_dma_oam_cycles;
static uint64_t hr_owner_full_fetches, hr_owner_native_fetches;
static unsigned hr_refusal;
unsigned hr_ppu_reason(bool known_zero);
unsigned hr_apu_block_reason(void);
enum { HR_R_DISABLED, HR_R_INT_PENDING, HR_R_OBSERVER, HR_R_TICK, HR_R_BOARD, HR_R_IRQ_LINE, HR_R_MMC3_IRQ_OUT,
       HR_R_NMI_EDGE, HR_R_NMI_INPUT, HR_R_APU_OAMDMA, HR_R_APU_DMC, HR_R_APU_FRAMEIRQ, HR_R_APU_OTHER,
       HR_R_PPU_BLANK, HR_R_PPU_GENERAL, HR_R_PPU_UNKNOWN, HR_R_PPU_IDLE_END, HR_R_PPU_MMC3_A12,
       HR_R_PPU_LINE_249, HR_R_PPU_LIMIT_NONZERO, HR_R_N };
#endif

static bool native_pal_intervals_enabled = true;
void hw_native_set_pal_intervals(bool enabled)
{
    hw_native_flush();
    native_pal_intervals_enabled = enabled;
}
static bool native_interval_board_allowed(void)
{
    /* PAL intervals need the FRAME PPU's five-tick dot cadence. They qualify
     * fixed-PRG NROM/CNROM only; CPU mapper writes flush pending work before
     * a CNROM CHR-bank change. Other PPU builds keep refusing PAL. */
    if (hw_pal() && !hw_isolated)
        return NESRECOMP_PPU_FRAME && native_pal_intervals_enabled &&
               (hw_cart.mapper == 0 || hw_cart.mapper == 3);
    return hw_cart.mapper == 0 || hw_cart.mapper == 1 || hw_cart.mapper == 4 || hw_cart.mapper == 66;
}

unsigned hw_native_interval_limit(void)
{
#if NESRECOMP_ISOLATED_NATIVE
    if (hw_isolated) {
        /* Memory-only guest service: no device clock, DMA or interrupt is
         * sampled. Stop credit at the call budget; the next ordinary access
         * must publish budget exhaustion at exactly the original cycle. */
        if (cyc_trace_enabled || cpu.nmi_edge || hw.irq_line ||
            (hw_cart.mapper != 0 && hw_cart.mapper != 1 &&
             hw_cart.mapper != 4 && hw_cart.mapper != 66) ||
            hw_isolated_cycles >= hw_isolated_budget) return 0;
        uint64_t remaining = hw_isolated_budget - hw_isolated_cycles;
        return remaining > 65535 ? 65535 : (unsigned)remaining;
    }
#endif
#define HR_REFUSE(reason) do { HR_COUNT(hr_refusal = reason); return 0; } while (0)
    if (hw.tick != 1) HR_REFUSE(HR_R_TICK);
    if (hw_isolated || cyc_trace_enabled ||
        hw_extra_timing.extra_scanlines || hw_cart.watch_cpu || hw_cart.clock_late ||
        !native_interval_board_allowed()) HR_REFUSE(HR_R_BOARD);
    bool masked_irq = hw.irq_line && native_masked_irq_ready();
    if (hw.irq_line && !masked_irq) HR_REFUSE(HR_R_IRQ_LINE);
    if (hw_cart.mapper == 4 && hw_cart.m.irq_out) HR_REFUSE(HR_R_MMC3_IRQ_OUT);
    if (cpu.nmi_edge) HR_REFUSE(HR_R_NMI_EDGE);
    if (cpu.nmi_input != ppu_nmi_output()) HR_REFUSE(HR_R_NMI_INPUT);
    if (!masked_irq && !apu_native_interval_ready()) {
        HR_COUNT(hr_refusal = HR_R_APU_OAMDMA + hr_apu_block_reason() - 1);
        return 0;
    }
    /* On extended SxROM, PPU A12 can also select CPU PRG/RAM signals. Such
     * boards need a bank-selection deadline before admitting CPU ROM reads. */
    if (hw_cart.mapper == 1 && (hw_cart.m.ctrl & 0x10) &&
        ((hw_cart.m.chr0 ^ hw_cart.m.chr1) & 0x1c)) HR_REFUSE(HR_R_BOARD);
    unsigned ppu_limit = ppu_native_interval_limit();
    unsigned limit = apu_native_interval_limit(ppu_limit);
    /* The owner admission bound also makes its packed host tally exact. */
    assert(limit <= UINT16_MAX);
    if (!limit) HR_COUNT(hr_refusal = ppu_limit ? HR_R_APU_DMC : HR_R_PPU_BLANK + hr_ppu_reason(true));
    return limit;
#undef HR_REFUSE
}

void hw_native_begin(void)
{
#if NESRECOMP_NATIVE_PRG_GUARDS
    hw_native_interval.checked_prg = false;
#endif
    hw_native_interval.ram = hw.ram;
#if NESRECOMP_ISOLATED_NATIVE
    hw_native_interval.enabled = NESRECOMP_NATIVE_INTERVALS && !cyc_trace_enabled &&
        ((!hw_isolated && (!hw_pal() || native_pal_intervals_enabled) && !hw_extra_timing.extra_scanlines)
         || hw_isolated
        ) &&
        native_interval_board_allowed() &&
        !hw_prg_data_modified && !cyc_mod_rom_read_overrides &&
        (!cyc_cpu_rts_observer || cyc_cpu_rts_observer == cyc_mod_return_observer);
#else
    hw_native_interval.enabled = NESRECOMP_NATIVE_INTERVALS && !hw_isolated && !cyc_trace_enabled &&
        (!hw_pal() || native_pal_intervals_enabled) && !hw_extra_timing.extra_scanlines &&
        native_interval_board_allowed() &&
        !hw_prg_data_modified && !cyc_mod_rom_read_overrides &&
        (!cyc_cpu_rts_observer || cyc_cpu_rts_observer == cyc_mod_return_observer);
#endif
    /* Armed instruction hooks are safe: compiled code yields to dispatch,
     * which closes this cursor before the scheduler invokes any callback.
     * Operand observers close at cpu_ram_read_value; registered RTS observers
     * close in the mod adapter. Unwrapped external RTS observers fall back. */
}

#if NESRECOMP_NATIVE_PRG_GUARDS
void hw_native_begin_checked_rom(void)
{
    hw_native_begin();
    if (!hw_prg_modified && !hw_prg_data_modified) return;
    hw_native_interval.checked_prg = true;
#if NESRECOMP_ISOLATED_NATIVE
    hw_native_interval.enabled = NESRECOMP_NATIVE_INTERVALS && !cyc_trace_enabled &&
        ((!hw_isolated && (!hw_pal() || native_pal_intervals_enabled) && !hw_extra_timing.extra_scanlines)
         || hw_isolated
        ) &&
        native_interval_board_allowed() &&
        !cyc_mod_rom_read_overrides &&
        (!cyc_cpu_rts_observer || cyc_cpu_rts_observer == cyc_mod_return_observer);
#else
    hw_native_interval.enabled = NESRECOMP_NATIVE_INTERVALS && !hw_isolated && !cyc_trace_enabled &&
        (!hw_pal() || native_pal_intervals_enabled) && !hw_extra_timing.extra_scanlines &&
        native_interval_board_allowed() &&
        !cyc_mod_rom_read_overrides &&
        (!cyc_cpu_rts_observer || cyc_cpu_rts_observer == cyc_mod_return_observer);
#endif
    /* Unsupported mappers can encode RAM/open-bus flags in prg_off. Never
     * form a ROM pointer for those mappings, including on scalar fallback. */
    if (hw_native_interval.enabled)
        for (unsigned slot = 0; slot < 8; ++slot)
            hw_native_interval.prg[slot] = hw_cart.prg + hw_cart.prg_off[slot];
}
#endif

static void hw_native_flush_site(unsigned site)
{
    (void)site;
    unsigned n = hw_native_pending_cycles();
#if NESRECOMP_HEADROOM_COUNTERS
    unsigned fetched = hw_native_pending_fetches();
#endif
    if (n) {
        HR_COUNT(if (!hw_isolated) hr_interval_end(n, hw_native_interval.credit, site));
        hw_native_interval_run(n);
        hw.cpu_addr = hw_native_interval.addr;
        hw.cpu_reading = hw_native_interval.kind != HW_WRITE;
        cpu_opcode_fetch = hw_native_interval.kind == HW_FETCH;
        hw.data_bus = hw.internal_bus = hw_native_interval.value;
        hw.data_driven = hw_native_interval.driven;
#if NESRECOMP_ISOLATED_NATIVE
        if (hw_isolated) {
            hw_native_isolated_cycles += n;
            ++hw_native_isolated_spans;
        } else {
#endif
        hw_native_interval_cycles += n;
        ++hw_native_interval_spans;
#if NESRECOMP_ISOLATED_NATIVE
        }
#endif
        HR_COUNT(hr_owner_native_fetches += fetched);
    }
    hw_native_interval.pending = hw_native_interval.credit = 0;
    hw_native_interval.irq_mask = NULL;
}

void hw_native_flush(void) { hw_native_flush_site(HR_S_NONE); }

void hw_native_end(void)
{
    hw_native_flush();
    hw_native_interval.enabled = false;
}

bool hw_native_prepare(void)
{
    hw_native_flush();
    HR_COUNT(hr_refusal = !hw_native_interval.enabled ? HR_R_DISABLED :
        (cpu.do_nmi | cpu.do_irq | cpu.do_reset) ? HR_R_INT_PENDING : HR_R_OBSERVER);
    if ((cpu.do_nmi | cpu.do_irq | cpu.do_reset) ||
        (cyc_cpu_rts_observer && cyc_cpu_rts_observer != cyc_mod_return_observer) ||
        (hw_prg_data_modified
#if NESRECOMP_NATIVE_PRG_GUARDS
         && !hw_native_interval.checked_prg
#endif
        ) || cyc_mod_rom_read_overrides) return false;
    hw_native_interval.credit = hw_native_interval_limit();
    hw_native_interval.irq_mask = hw_native_interval.credit && hw.irq_line ? &cpu.i : NULL;
    hw_native_interval.driven = hw.data_driven;
    return hw_native_interval.credit != 0;
}

void hw_native_interval_run(unsigned cycles)
{
#if NESRECOMP_ISOLATED_NATIVE
    if (hw_isolated) {
        hw_isolated_cycles += cycles;
        if (hw_isolated_cycles > hw_isolated_budget) hw_frame_done = true;
        return;
    }
#endif
    ppu_native_interval_run(cycles, hw.align);
    if (hw_pal()) hw_region_timing.phase = (uint8_t)((hw_region_timing.phase + cycles) % 5u);
    apu_native_interval_run(cycles);
    /* Both sampled inputs are proven constant throughout this interval. */
    cpu_nmi_input(ppu_nmi_output());
    apu_sample_irq();
    hw.cycles += cycles;
    hw_dma_stalls = 0;
}

void hw_cycle_start(uint16_t addr, HwCycleKind kind)
{
    unsigned site = HR_S_NONE;
#if NESRECOMP_HEADROOM_COUNTERS
    /* The caller chooses its site before pending time is published. Sample
     * PPU allowance after that flush, before starting the physical cycle. */
    site = (unsigned)kind >> 2;
    kind = (HwCycleKind)((unsigned)kind & 3u);
#endif
#if NESRECOMP_NATIVE_INTERVALS
    hw_native_flush_site(site);
#endif
    HR_COUNT(if (!hw_isolated) hr_classify(addr, site));
    hw.cpu_addr = addr;
    hw.cpu_reading = kind == HW_READ || kind == HW_FETCH;
    cpu_opcode_fetch = kind == HW_FETCH;
    HR_COUNT(if (site && cpu_opcode_fetch) ++hr_owner_full_fetches);
    hw_dma_stalls = 0;
    if (hw_isolated) {
        /* a mod's isolated routine call: memory only, no time (hw_internal.h) */
        if (++hw_isolated_cycles > hw_isolated_budget) hw_frame_done = true;
        return;
    }
    for (;;) {
        if (hw_pal()) {
            while (hw.tick < 16) run_pal_tick(hw.tick++);
            hw_region_timing.phase = (uint8_t)((hw_region_timing.phase + 1) % 5);
        } else if (hw.tick == 1) run_ticks_1_to_11();
        else while (hw.tick < 12) run_tick(hw.tick++);
        hw.tick = 0;
        if ((hw_pal() && !cpu_opcode_fetch) || !dma_wants_cycle()) return;
        /* RDY: a DMA takes this read cycle; the CPU holds its address. */
        HR_COUNT(++hr_dma_cycles; if (hw_oam_dma_active()) ++hr_dma_oam_cycles);
        dma_cycle();
        dma_end_of_cycle();
        if (cyc_trace_enabled) cyc_trace_cycle_end(false, hw.data_bus);
        hw.cycles++;
        if (hw_pal()) run_pal_tick(hw.tick++); else run_tick(hw.tick++);
        hw_dma_stalls++;
    }
}

uint8_t hw_read(uint16_t addr)
{
    return hw_bus_read(addr);
}

/* A cheat's read substitution (cyc_mod.h), applied last so it wins over the
 * board: a cheat the mapper can undo is not a cheat. Empty for every game that
 * has not asked for one, so this costs a load and a branch per ROM read. */
static inline uint8_t hw_rom_read_override(uint16_t addr, uint8_t value)
{
    const CycModReadOverride *t = cyc_mod_rom_read_overrides;
    if (!t) return value;
    for (unsigned i = 0, n = cyc_mod_rom_read_override_count(); i < n; i++) {
        if (t[i].qualified) {
            uint32_t offset = hw_cart.prg_off[(addr >> 12) & 7] + (addr & 0xFFFu);
            if (offset < hw_cart.prg_len && offset == t[i].prg_offset && value == t[i].compare) return t[i].value;
        } else if (t[i].addr == addr) return t[i].value;
    }
    return value;
}

uint8_t hw_read_rom(uint16_t addr, uint8_t value)
{
    if (hw_prg_data_modified) value = hw_cart_prg_read(addr);
    hw_cart_cpu_read_snoop(addr,value);
    value = hw_rom_read_override(addr, value);
    hw.data_driven = 1;
    hw.data_bus = hw.internal_bus = value;
    if (cyc_trace_enabled) cyc_trace_access(addr, value, false);
    return value;
}

uint8_t hw_read_code_ram(uint16_t addr, uint8_t value)
{
    hw.data_driven = 1;
    hw.data_bus = hw.internal_bus = value;
    if (cyc_trace_enabled) cyc_trace_access(addr, value, false);
    return value;
}

void hw_write(uint16_t addr, uint8_t value)
{
    /* an isolated routine that talks to a device (cyc_mod.h): counted, so a
     * committed call can refuse (its device effects would be lost) */
    if (hw_isolated && addr >= 0x2000 &&
        !(hw_cart.mapper == NES_FDS_MAPPER ? addr >= 0x6000 && addr < 0xE000 : addr >= 0x6000 && addr < 0x8000))
        hw_isolated_io_writes++;
    hw_bus_write(addr, value);
}

void hw_cycle_finish(bool instruction_done)
{
    if (hw_isolated) return;
    dma_end_of_cycle();
    if (cyc_trace_enabled) cyc_trace_cycle_end(instruction_done, hw.data_bus);
    hw.cycles++;
    if (hw_pal()) {
        run_pal_tick(hw.tick++);
    } else if (hw.tick == 0) {
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
        hw.data_driven = !(hw_cart.prg_off[addr>>12&7]&MMC5_PRG_OPEN);
        hw_cart_cpu_read_snoop(addr,hw.data_bus);
        hw.data_bus = hw_rom_read_override(addr,hw.data_bus);
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
            if (hw_zapper.port == reg - 0x15)
                value = (uint8_t)((hw.data_bus & 0xE0) | (hw_zapper.trigger ? 0x10 : 0) |
                                  (hw_zapper.dots < hw_zapper.light_until ? 0 : 0x08));
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
    if (hw_cart.mapper==5 && addr<0x4020) hw_cart_cpu_write(addr,value);
    if (addr < 0x2000) hw_code_store(&hw.ram[addr & 0x7FF], addr & 0x7FF, value);
    else if (addr < 0x4000) ppu_write(addr, value);
    else if (addr <= 0x4017) apu_write(addr, value);
    else if (addr >= 0x4020) {
#if NESRECOMP_PPU_FRAME
        frame_ppu_mapper_changed();
#endif
        hw_cart_cpu_write(addr, value);
    }
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

bool hw_prg_modified;
bool hw_prg_data_modified;
static bool prg_hash_valid;
static uint32_t prg_hash_cached;

#if NESRECOMP_NATIVE_PRG_GUARDS
uint64_t hw_prg_guard_epoch;
static bool prg_guard_untracked, prg_guard_exhausted;

void hw_prg_guard_write(bool untracked)
{
    prg_guard_untracked |= untracked;
    if (hw_prg_guard_epoch == UINT64_MAX) prg_guard_exhausted = true;
    else ++hw_prg_guard_epoch;
}
void hw_prg_guard_load(void)
{
    prg_guard_untracked = false;
    hw_prg_guard_write(false); /* never reuse an epoch from a previous image */
}
const uint8_t *hw_prg_guard_image(size_t *size)
{
    *size = hw_cart.prg_len;
    if (!hw_prg_guard_epoch || prg_guard_untracked || prg_guard_exhausted ||
        (hw_cart.mapper != 0 && hw_cart.mapper != 1 && hw_cart.mapper != 4 && hw_cart.mapper != 66))
        return NULL;
    return hw_cart.prg;
}
#endif

bool cyc_load_ines(const uint8_t *image, size_t size)
{
    NesCartInfo info;
    if (!nes_cart_image(image, size, &info) || !hw_cart_supports(info.mapper) ||
        !nes_cart_variant_supported(&info)) return false;
    /* An iNES header cannot describe a disk; the RAM Adapter comes from
     * cyc_load_fds(). */
    if (info.mapper == NES_FDS_MAPPER) return false;
    uint32_t prg_alloc, chr_alloc;
    uint32_t chr_len = info.chr_size ? info.chr_size : info.chr_ram + info.chr_nvram;
    /* A board with both CHR ROM and CHR RAM (TQROM) keeps the RAM chip after
     * the padded ROM, outside the range ROM bank numbers wrap within. */
    uint32_t mixed_ram = info.chr_size ? info.chr_ram : 0;
    uint8_t *prg = alloc_padded(image + info.data_offset, info.prg_size, &prg_alloc);
    uint8_t *chr = alloc_padded(info.chr_size ? image + info.data_offset + info.prg_size : NULL,
                              chr_len, &chr_alloc);
    if (chr && mixed_ram) {
        uint8_t *grown = (uint8_t *)realloc(chr, (size_t)chr_alloc + mixed_ram);
        if (grown) memset(grown + chr_alloc, 0, mixed_ram);
        else free(chr);
        chr = grown;
    }
    if (!prg || !chr) { free(prg); free(chr); return false; }
    free(hw_cart.prg);
    free(hw_cart.chr);
    memset(&hw_cart, 0, sizeof(hw_cart));
    hw_cart.info = info;
    hw_prg_modified = false;
    hw_prg_data_modified = false;
    prg_hash_valid = false;
#if NESRECOMP_NATIVE_PRG_GUARDS
    hw_prg_guard_load();
#endif
    hw_cart.prg = prg;
    hw_cart.prg_len = info.prg_size;
    hw_cart.prg_slots = prg_alloc / 4096;
    hw_cart.chr = chr;
    hw_cart.chr_len = chr_len;
    hw_cart.chr_pages = chr_alloc / 1024;
    hw_cart.chr_ram = !info.chr_size;
    hw_cart.chr_ram_base = mixed_ram ? chr_alloc : 0;
    hw_cart.chr_ram_len = mixed_ram ? mixed_ram : hw_cart.chr_ram ? chr_len : 0;
    hw_cart.mapper = info.mapper;
    if (info.mapper==153) memset(hw_cart.wram,255,8192);
    nes_eeprom_init(&hw_cart.eeprom[0],info.mapper==157?256:(info.mapper==16 || info.mapper==159)?info.prg_nvram:0);
    nes_eeprom_init(&hw_cart.eeprom[1],info.mapper==157?info.prg_nvram:0);
    hw_cart.wram_len = info.prg_ram + info.prg_nvram;
    hw_cart.mirroring = info.vertical ? HW_MIRROR_VERTICAL : HW_MIRROR_HORIZONTAL;
    hw_cart_power_on();
    return true;
}

/* The RAM Adapter: the BIOS is the board's whole PRG ROM, fixed at
 * $E000-$FFFF; the disk sides are the drive's media (hw_fds.c). */
bool cyc_load_fds(const uint8_t *bios, size_t bios_size, const uint8_t *image, size_t image_size,
                  const CycFdsOptions *options)
{
    CycFdsOptions defaults;
    if (!options) { cyc_fds_default_options(&defaults); options = &defaults; }
    if (!bios || bios_size != NES_FDS_BIOS_BYTES) return false;
    uint32_t prg_alloc, chr_alloc;
    uint8_t *prg = alloc_padded(bios, bios_size, &prg_alloc);
    uint8_t *chr = alloc_padded(NULL, NES_FDS_CHR_RAM_BYTES, &chr_alloc);
    if (!prg || !chr || !cyc_fds_load_media(image, image_size, options)) { free(prg); free(chr); return false; }
    free(hw_cart.prg);
    free(hw_cart.chr);
    memset(&hw_cart, 0, sizeof(hw_cart));
    nes_fds_cart_info(&hw_cart.info);
    hw_prg_modified = false;
    hw_prg_data_modified = false;
    prg_hash_valid = false;
    hw_cart.prg = prg;
    hw_cart.prg_len = NES_FDS_BIOS_BYTES;
    hw_cart.prg_slots = prg_alloc / 4096;
    hw_cart.chr = chr;
    hw_cart.chr_len = NES_FDS_CHR_RAM_BYTES;
    hw_cart.chr_pages = chr_alloc / 1024;
    hw_cart.chr_ram = 1;
    hw_cart.chr_ram_len = NES_FDS_CHR_RAM_BYTES;
    hw_cart.mapper = NES_FDS_MAPPER;
    hw_cart.wram_len = NES_FDS_PRG_RAM_BYTES;
    hw_cart.mirroring = HW_MIRROR_VERTICAL;
    hw_cart_power_on();
    return true;
}

const uint8_t *cyc_cart_ram(size_t *len)
{
    *len = hw_cart.has_wram ? hw_cart.wram_len : 0;
    return hw_cart.has_wram ? hw_cart.wram : NULL;
}

const uint8_t *cyc_ppu_ciram(size_t *len) { *len = hw_cart.info.four_screen ? 4096 : 2048; return ppu.ciram; }
const uint8_t *cyc_ppu_palette(void) { return ppu.palette; }
const uint8_t *cyc_ppu_oam(void) { return ppu.oam; }
const uint8_t *cyc_chr_ram(size_t *len)
{
    *len = hw_cart.chr_ram_len;
    return hw_cart.chr_ram_len ? hw_cart.chr + hw_cart.chr_ram_base : NULL;
}

void cyc_power_on(uint8_t ppu_alignment)
{
    static bool palette_ready;
    if (!palette_ready) {
        hw_palette_init();
        palette_ready = true;
    }
    memset(&hw_native_interval, 0, sizeof(hw_native_interval));
    hw_native_interval_cycles = hw_native_interval_spans = 0;
    memset(&hw, 0, sizeof(hw));
    memset(&hw_extra_timing, 0, sizeof(hw_extra_timing));
    memset(&hw_region_timing, 0, sizeof(hw_region_timing));
    hw_region_timing.region = (uint8_t)region_request;
    hw_region_timing.phase = hw_pal() ? ppu_alignment % 5 : 0;
    memset(&hw_zapper, 0, sizeof(hw_zapper));
    hw_zapper.x = hw_zapper.y = -1;
    hw.align = ppu_alignment & 3;
    /* Traced sessions share this process when returning to the lobby. A new
     * power-on must not inherit the previous match's diagnostic history:
     * these fields are included in cycle snapshots and rollback digests. */
    cyc_trace_hash = 0;
    cyc_trace_cycle = 0;
    cyc_ring_reset();
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
     * starts at its alignment phase: its first dot is not on this tick. The
     * cartridge's first M2 clock comes with the first CPU cycle, as in the
     * oracle; a free-running divider (the N163 sound generator) keeps the
     * oracle's phase, which CPU-readable RAM exposes. */
    if (hw_pal()) {
        unsigned q = 2u * hw_region_timing.phase;
        /* As on NTSC, no first-dot transition precedes the reset cycle. */
        if (q + 1 == 5) ppu_half_dot();
    } else if (hw.align == 2) ppu_half_dot();
    apu_cycle();
    hw.tick = 1;
}

#include "../../common/nes_nvram.h"
static NesNvram nvram_region(unsigned region) { return nes_nvram_region(&hw_cart.info,hw_cart.wram,hw_cart.chr,hw_cart.eeprom,hw_cart.exram,region); }
bool cyc_scan_barcode(const char *digits, unsigned cycles_per_module) {
    return hw_cart.mapper==157 && nes_barcode_scan(&hw_cart.barcode,digits,cycles_per_module,hw.cycles);
}
size_t cyc_nvram_size(unsigned region) { NesNvram n=nvram_region(region); return n.size[0]+n.size[1]+n.size[2]; }
bool cyc_nvram_export(unsigned region, void *buffer, size_t size) {
    return nes_nvram_transfer(nvram_region(region),buffer,size,false);
}
bool cyc_nvram_import(unsigned region, const void *buffer, size_t size) {
    return nes_nvram_transfer(nvram_region(region),(void *)buffer,size,true);
}

uint32_t cyc_prg_hash(void)
{
    /* A mod can retain a writable PRG pointer; keep hashing that domain on
     * every request. Ordinary cartridges and FDS BIOS are immutable. */
    if(prg_hash_valid&&!hw_prg_modified&&!hw_prg_data_modified)return prg_hash_cached;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < hw_cart.prg_len; i++) h = (h ^ hw_cart.prg[i]) * 16777619u;
    if(!hw_prg_modified&&!hw_prg_data_modified){prg_hash_cached=h;prg_hash_valid=true;}
    return h;
}

const char *cyc_hw_name(void) { return "nesrecomp"; }

/* ------------------------------------------------------------------------- */
/* Host I/O                                                                  */
/* ------------------------------------------------------------------------- */

const uint8_t *cyc_cpu_ram(void) { return hw.ram; }

bool cyc_debug_peek(uint16_t addr, uint8_t *value)
{
    if (addr < 0x2000) { *value = hw.ram[addr & 0x7FF]; return true; }
    if (addr >= 0x8000) {
        uint32_t offset = hw_cart.prg_off[(addr >> 12) & 7] | (addr & 4095);
        if (offset & MMC5_PRG_OPEN) return false;
        *value = (offset & MMC5_PRG_RAM) ? hw_cart.wram[offset & 0x1ffff] : hw_cart.prg[offset];
        return true;
    }
    if (addr >= 0x6000 && hw_cart.has_wram && hw_cart.wram_len) {
        *value = hw_cart.mapper == NES_FDS_MAPPER ? hw_cart.wram[addr - 0x6000]
                                                   : hw_cart.wram[(hw_cart.wram_bank + (addr & 0x1FFF)) % hw_cart.wram_len];
        return true;
    }
    return false;
}

void cyc_set_controller(int port, uint8_t buttons) { hw_set_controller(port, buttons); }

uint64_t cyc_cycle_count(void)
{
#if NESRECOMP_NATIVE_INTERVALS
#if NESRECOMP_ISOLATED_NATIVE
    if (hw_isolated) return hw.cycles; /* guest service time is not host/device time */
#endif
    return hw.cycles + hw_native_pending_cycles();
#else
    return hw.cycles;
#endif
}

bool cyc_set_region(CycRegion region)
{
    if ((region != CYC_REGION_NTSC && region != CYC_REGION_PAL) ||
        (region == CYC_REGION_PAL && cyc_is_fds())) return false;
    region_request = region;
    return true;
}
CycRegion cyc_region(void) { return (CycRegion)hw_region_timing.region; }
CycRegion cyc_cart_region(void) { return hw_cart.info.nes2 && hw_cart.info.timing == 1 ? CYC_REGION_PAL : CYC_REGION_NTSC; }
double cyc_cpu_hz(void) { return hw_pal() ? 26601712.5 / 16.0 : 21477272.0 / 12.0; }
double cyc_frame_seconds(void) { return hw_pal() ? (341.0 * 312.0 * 5.0) / 26601712.5 : 1.0 / 60.0988; }

/* The added lines are blank (rendering is off above scanline 239) and the APU
 * is paused across them, so they are inert for any mapper whose IRQ is not
 * clocked by the PPU -- such a board just sees CPU time the game can spend
 * before its NMI, which is the whole point of the enhancement.
 *
 * MMC3 (4) counts rising edges of PPU A12 (mmc3_clock_irq), so it is affected
 * by what the PPU does on those lines and stays supported for its own sake.
 * MMC1 (1) has no IRQ counter. Its serial register responds to CPU writes
 * (bit 7 resets that register), so it needs no PPU-clocked IRQ adjustment.
 * UxROM (2) also has no IRQ counter; bank selection responds to CPU writes.
 *
 * Only consulted when extra_scanlines is non-zero: at stock timing (0) this
 * changes nothing for any game. */
bool cyc_extra_scanlines_supported(void) { return hw_cart.mapper == 1 || hw_cart.mapper == 2 || hw_cart.mapper == 4; }
unsigned cyc_extra_scanlines(void) { return hw_extra_timing.extra_scanlines; }

bool cyc_zapper_attach(unsigned port)
{
    if (port > 2) return false;
    memset(&hw_zapper, 0, sizeof(hw_zapper));
    hw_zapper.port = (uint8_t)port;
    hw_zapper.x = hw_zapper.y = -1;
    return true;
}

void cyc_set_zapper(int x, int y, bool trigger)
{
    bool inside = x >= 0 && x < 256 && y >= 0 && y < 240;
    hw_zapper.x = inside ? (int16_t)x : -1;
    hw_zapper.y = inside ? (int16_t)y : -1;
    hw_zapper.trigger = trigger;
}

void cyc_zapper_state(CycZapperState *out)
{
    out->port = hw_zapper.port;
    out->x = hw_zapper.x; out->y = hw_zapper.y;
    out->trigger = hw_zapper.trigger != 0;
    out->light = hw_zapper.dots < hw_zapper.light_until;
}

void hw_zapper_pixel(int x, int y, uint16_t color)
{
    if (!hw_zapper.port || hw_zapper.x < 0 || abs(x - hw_zapper.x) > 4 || abs(y - hw_zapper.y) > 4) return;
    uint32_t rgb = hw_palette_argb[color & 0x1FF];
    unsigned brightness = (77 * ((rgb >> 16) & 255) + 150 * ((rgb >> 8) & 255) + 29 * (rgb & 255)) >> 8;
    /* Optical approximation: small aperture, 20-scanline decay. Only PPU
     * pixels can charge it; crosshairs and compositors cannot. Polarity and
     * scanline locality: NESdev Zapper research, Mesen2 Input/Zapper.h. */
    if (brightness >= 85) hw_zapper.light_until = hw_zapper.dots + 20u * 341u;
}
bool cyc_set_extra_scanlines(unsigned lines)
{
    if (lines > 262 || (lines && !cyc_extra_scanlines_supported())) return false;
    hw_extra_timing.extra_scanlines = (uint16_t)lines;
    return true;
}

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

void cyc_set_audio_synthesis(CycAudioSynthesis synthesis) { apu_set_audio_synthesis(synthesis); }

CycAudioSynthesis cyc_audio_synthesis(void) { return apu_audio_synthesis(); }

void cyc_set_console(CycConsole console) { apu_set_console((int)console); }

CycConsole cyc_console(void) { return (CycConsole)apu_console(); }

const char *cyc_console_name(CycConsole console)
{
    return console == CYC_CONSOLE_NES ? "nes" : console == CYC_CONSOLE_FAMICOM ? "famicom" : "default";
}

/* ------------------------------------------------------------------------- */
/* Comparison                                                                */
/* ------------------------------------------------------------------------- */

uint64_t cyc_mem_state_hash(void)
{
    uint64_t h = cyc_mem_hash(hw.cycles, hw.ram, ppu.ciram, hw_cart.info.four_screen ? 4096 : 2048, ppu.oam, ppu.palette,
                        hw_cart.chr_ram_len ? hw_cart.chr + hw_cart.chr_ram_base : NULL, hw_cart.chr_ram_len, hw_cart.has_wram ? hw_cart.wram : NULL, hw_cart.wram_len,
                        hw_frame_index);
    for (unsigned chip=0;chip<2;++chip)
        for (unsigned i=0;i<hw_cart.eeprom[chip].size;++i) h=cyc_trace_mix(h,hw_cart.eeprom[chip].data[i]);
    if (hw_cart.mapper==5) for (unsigned i=0;i<1024;++i) h=cyc_trace_mix(h,hw_cart.exram[i]);
    if (hw_cart.mapper==19) for (unsigned i=0;i<128;++i) h=cyc_trace_mix(h,hw_cart.exram[i]);
    if (hw_cart.mapper==NES_FDS_MAPPER) h=fds_media_hash(h);
    return h;
}

void cyc_mem_state_dump(void *file)
{
    cyc_mem_dump(file, hw.cycles, hw.ram, ppu.ciram, hw_cart.info.four_screen ? 4096 : 2048, ppu.oam, ppu.palette,
                 hw_cart.chr_ram_len ? hw_cart.chr + hw_cart.chr_ram_base : NULL, hw_cart.chr_ram_len, hw_cart.has_wram ? hw_cart.wram : NULL, hw_cart.wram_len, hw_frame_index);
    /* Cartridge-internal RAM that cyc_mem_state_hash also covers. */
    unsigned n = hw_cart.mapper==5 ? 1024 : hw_cart.mapper==19 ? 128 : 0;
    for (unsigned i = 0; i < n; ++i)
        fprintf((FILE *)file, "%s%02X%s", i % 32 ? " " : i ? "\nexram " : "exram ", hw_cart.exram[i], i + 1 == n ? "\n" : "");
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
    if (hw_extra_timing.extra_scanlines || hw_extra_timing.active)
        h = cyc_trace_mix(h, (uint64_t)hw_extra_timing.extra_scanlines |
                         (uint64_t)hw_extra_timing.line << 16 | (uint64_t)hw_extra_timing.active << 32);
    if (hw_zapper.port) {
        h = cyc_trace_mix(h, hw_zapper.dots);
        h = cyc_trace_mix(h, hw_zapper.light_until);
        h = cyc_trace_mix(h, (uint16_t)hw_zapper.x | (uint64_t)(uint16_t)hw_zapper.y << 16 |
                         (uint64_t)hw_zapper.port << 32 | (uint64_t)hw_zapper.trigger << 40);
    }
    if (hw_pal()) h = cyc_trace_mix(h, hw_region_timing.region | (uint64_t)hw_region_timing.phase << 8);
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
    if (hw_pal()) fprintf(f, "region.pal 1\nregion.phase %u\n", hw_region_timing.phase);
    hw_cart_state_dump(file);
    fprintf(f, "extra.scanlines %u\nextra.line %u\nextra.active %u\n",
            hw_extra_timing.extra_scanlines, hw_extra_timing.line, hw_extra_timing.active);
    ppu_state_dump(file);
    apu_state_dump(file);
}

#if NESRECOMP_HEADROOM_COUNTERS
static const char *const hr_r_names[HR_R_N] = { "disabled", "int_pending", "observer", "tick_not_1", "board",
    "irq_line", "mmc3_irq_out", "nmi_edge", "nmi_input", "apu_oamdma", "apu_dmc", "apu_frameirq", "apu_other",
    "ppu_blank", "ppu_general", "ppu_unknown", "ppu_idle_end", "ppu_mmc3_a12", "ppu_line_249", "ppu_limit_nonzero" };
static const char *const hr_s_names[HR_S_N] = { "none", "ram_r", "ram_w", "rom_fold", "fetch_rom", "code_ram",
    "io_r", "io_w", "watch_w", "fetch_interp" };
static const char *const hr_ior_names[10] = { "2002", "2007", "ppu_other", "4015", "4016_7", "apu_io_other",
    "4020_5fff", "6000_7fff", "rom_8000", "ram" };
static const char *const hr_iow_names[21] = { "2000", "2001", "2002", "2003", "2004", "2005", "2006", "2007",
    "4014", "apu_regs", "4016", "4018_5fff", "6000_7fff",
    "8000", "8001", "a000", "a001", "c000", "c001", "e000", "e001" };
static uint64_t hr_full_cycles, hr_full_site[HR_S_N], hr_full_reason[HR_R_N], hr_io_r[10], hr_io_w[21], hr_full_ctx[3];
static uint64_t hr_ppu_x[HR_R_N][7];
static uint64_t hr_iv_hist[17], hr_iv_end[5], hr_iv_ctx[3], hr_iv_cycles;
/* Record each classified event once. The report derives the original
 * overlapping site/refusal/PPU and length/end-reason marginal tables. */
static uint64_t hr_full_joint[HR_S_N][HR_R_N][7], hr_iv_joint[17][5];
uint64_t hr_ins, hr_frames;
static uint64_t hr_ins_ctx[3], hr_cyc_ctx[3], hr_nmi_count, hr_irq_count;
static int hr_ctx, hr_last_ctx, hr_sp, hr_pending_pop, hr_last_valid, hr_inited;
static int hr_stack[16];
static uint64_t hr_last_t;
static uint32_t hr_last_key;
#define HR_KEYS 0x48000u
static uint64_t *hr_pc_cyc;
static uint32_t *hr_pc_ins;
int hr_pc_active;
static uint64_t hr_ctx_time, hr_ctx_ins, hr_owner_rebase;
static uint64_t hr_ctx_interval, hr_ctx_dma, hr_dma_ctx[3];
unsigned hr_apu_block_reason(void);
void hr_ppu_print(void);
void hr_apu_print(void);


static uint64_t hr_owner_fetch_total(void)
{
    return hr_owner_rebase + hr_owner_full_fetches + hr_owner_native_fetches + hw_native_pending_fetches();
}

static void hr_context_sync(void)
{
    hr_ins = hr_owner_fetch_total();
    uint64_t now = hw.cycles + hw_native_pending_cycles();
    uint64_t interval = hw_native_interval_cycles + (hw_isolated ? 0 : hw_native_pending_cycles());
    if (now >= hr_ctx_time) hr_cyc_ctx[hr_ctx] += now - hr_ctx_time;
    if (interval >= hr_ctx_interval) hr_iv_ctx[hr_ctx] += interval - hr_ctx_interval;
    hr_dma_ctx[hr_ctx] += hr_dma_cycles - hr_ctx_dma;
    hr_ins_ctx[hr_ctx] += hr_ins - hr_ctx_ins;
    hr_ctx_time = now; hr_ctx_ins = hr_ins;
    hr_ctx_interval = interval; hr_ctx_dma = hr_dma_cycles;
}
static inline void hr_classify(uint16_t addr, unsigned s)
{
    switch (s) {
    case HR_S_RAM_R: case HR_S_RAM_W: case HR_S_ROM_FOLD: case HR_S_FETCH_ROM: case HR_S_CODE_RAM:
        { unsigned r = hw_native_interval.enabled ? hr_refusal : HR_R_DISABLED;
          ++hr_full_joint[s][r][hr_ppu_reason(false)]; }
        break;
    case HR_S_IO_R: {
        unsigned c;
        if (addr < 0x2000) c = 9;
        else if (addr < 0x4000) c = (addr & 7) == 2 ? 0 : (addr & 7) == 7 ? 1 : 2;
        else if (addr == 0x4015) c = 3;
        else if (addr == 0x4016 || addr == 0x4017) c = 4;
        else if (addr < 0x4020) c = 5;
        else if (addr < 0x6000) c = 6;
        else if (addr < 0x8000) c = 7;
        else c = 8;
        ++hr_io_r[c];
        break; }
    case HR_S_IO_W: {
        unsigned c;
        if (addr < 0x4000) c = addr & 7;
        else if (addr == 0x4014) c = 8;
        else if (addr == 0x4016) c = 10;
        else if (addr < 0x4018) c = 9;
        else if (addr < 0x6000) c = 11;
        else if (addr < 0x8000) c = 12;
        else c = 13 + (((addr >> 13) & 3) * 2 + (addr & 1));
        ++hr_io_w[c];
        break; }
    default: ++hr_full_site[s]; break;
    }
}

static void hr_interval_end(unsigned n, unsigned credit, unsigned site)
{
    unsigned b = 0;
    while (b < 16 && (1u << (b + 1)) <= n) ++b;
    unsigned end = site ? (site == HR_S_IO_R ? 1 : site == HR_S_IO_W ? 2 : 3) :
        (credit == 0 ? 0 : 4);
    ++hr_iv_joint[b][end];
}

static char *hr_output;
static size_t hr_output_size, hr_output_used;
void hr_print(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (hr_output) {
        size_t room = hr_output_size - hr_output_used;
        int n = vsnprintf(hr_output + hr_output_used, room, format, args);
        if (n > 0) hr_output_used += (size_t)n < room ? (size_t)n : room - 1;
    } else vprintf(format, args);
    va_end(args);
}
size_t hr_report(char *out, size_t size)
{
    if (!out || !size) return 0;
    hr_output = out; hr_output_size = size; hr_output_used = 0;
    hr_dump();
    hr_output = NULL;
    return hr_output_used;
}
void hr_dump(void)
{
    hr_context_sync();
    /* Derive overlapping totals at observation time, rather than updating
     * the same event through several counters in the hot paths. */
    memset(hr_ppu_x, 0, sizeof(hr_ppu_x));
    static const unsigned sites[] = { HR_S_RAM_R, HR_S_RAM_W, HR_S_ROM_FOLD, HR_S_FETCH_ROM, HR_S_CODE_RAM };
    for (unsigned k = 0; k < sizeof(sites) / sizeof(sites[0]); ++k) {
        unsigned site = sites[k];
        hr_full_site[site] = 0;
        for (unsigned reason = 0; reason < HR_R_N; ++reason)
            for (unsigned ppu = 0; ppu < 7; ++ppu) {
                uint64_t n = hr_full_joint[site][reason][ppu];
                hr_full_site[site] += n;
                hr_ppu_x[reason][ppu] += n;
            }
    }
    hr_full_site[HR_S_IO_R] = hr_full_site[HR_S_IO_W] = 0;
    for (unsigned k = 0; k < 10; ++k) hr_full_site[HR_S_IO_R] += hr_io_r[k];
    for (unsigned k = 0; k < 21; ++k) hr_full_site[HR_S_IO_W] += hr_io_w[k];
    memset(hr_iv_hist, 0, sizeof(hr_iv_hist));
    memset(hr_iv_end, 0, sizeof(hr_iv_end));
    for (unsigned length = 0; length < 17; ++length)
        for (unsigned end = 0; end < 5; ++end) {
            hr_iv_hist[length] += hr_iv_joint[length][end];
            hr_iv_end[end] += hr_iv_joint[length][end];
        }
    hr_full_cycles = 0;
    for (unsigned i = 0; i < HR_S_N; ++i) hr_full_cycles += hr_full_site[i];
    for (unsigned i = 0; i < HR_R_N; ++i) {
        hr_full_reason[i] = 0;
        for (unsigned j = 0; j < 7; ++j) hr_full_reason[i] += hr_ppu_x[i][j];
    }
    for (unsigned i = 0; i < 3; ++i)
        hr_full_ctx[i] = hr_cyc_ctx[i] - hr_iv_ctx[i] - hr_dma_ctx[i];
    hr_iv_cycles = hw_native_interval_cycles + (hw_isolated ? 0 : hw_native_pending_cycles());
    hr_print("HR_frames frames=%llu nmi=%llu irq=%llu\n", (unsigned long long)hr_frames, (unsigned long long)hr_nmi_count, (unsigned long long)hr_irq_count);
    hr_print("HR_guest instructions=%llu main=%llu nmi=%llu irq=%llu cycles_main=%llu cycles_nmi=%llu cycles_irq=%llu\n",
           (unsigned long long)hr_ins, (unsigned long long)hr_ins_ctx[0], (unsigned long long)hr_ins_ctx[1],
           (unsigned long long)hr_ins_ctx[2], (unsigned long long)hr_cyc_ctx[0], (unsigned long long)hr_cyc_ctx[1],
           (unsigned long long)hr_cyc_ctx[2]);
    hr_print("HR_full cycles=%llu ctx_main=%llu ctx_nmi=%llu ctx_irq=%llu dma=%llu dma_oam=%llu\n",
           (unsigned long long)hr_full_cycles, (unsigned long long)hr_full_ctx[0], (unsigned long long)hr_full_ctx[1],
           (unsigned long long)hr_full_ctx[2], (unsigned long long)hr_dma_cycles, (unsigned long long)hr_dma_oam_cycles);
    hr_print("HR_full_site");
    for (unsigned i = 0; i < HR_S_N; ++i) hr_print(" %s=%llu", hr_s_names[i], (unsigned long long)hr_full_site[i]);
    hr_print("\nHR_full_reason");
    for (unsigned i = 0; i < HR_R_N; ++i) hr_print(" %s=%llu", hr_r_names[i], (unsigned long long)hr_full_reason[i]);
    hr_print("\nHR_io_read");
    for (unsigned i = 0; i < 10; ++i) hr_print(" %s=%llu", hr_ior_names[i], (unsigned long long)hr_io_r[i]);
    hr_print("\nHR_io_write");
    for (unsigned i = 0; i < 21; ++i) hr_print(" w%s=%llu", hr_iow_names[i], (unsigned long long)hr_io_w[i]);
    hr_print("\nHR_interval cycles=%llu ctx_main=%llu ctx_nmi=%llu ctx_irq=%llu end_limit=%llu end_io_read=%llu end_io_write=%llu end_cs_other=%llu end_other=%llu\n",
           (unsigned long long)hr_iv_cycles, (unsigned long long)hr_iv_ctx[0], (unsigned long long)hr_iv_ctx[1],
           (unsigned long long)hr_iv_ctx[2], (unsigned long long)hr_iv_end[0], (unsigned long long)hr_iv_end[1],
           (unsigned long long)hr_iv_end[2], (unsigned long long)hr_iv_end[3], (unsigned long long)hr_iv_end[4]);
    hr_print("HR_interval_len_log2");
    for (unsigned i = 0; i < 17; ++i) hr_print(" b%u=%llu", i, (unsigned long long)hr_iv_hist[i]);
    hr_print("\n");
    hr_print("HR_reason_x_ppu (rows: primary reason; cols: ppu blank general unknown idle_end mmc3_a12 line_249 limit_nonzero)\n");
    for (unsigned i = 0; i < HR_R_N; ++i) { if (!hr_full_reason[i]) continue; hr_print("HR_x %s", hr_r_names[i]); for (unsigned j = 0; j < 7; ++j) hr_print(" %llu", (unsigned long long)hr_ppu_x[i][j]); hr_print("\n"); }
    hr_ppu_print();
    hr_apu_print();
    fflush(stdout);
    const char *path = getenv("HR_PCHIST");
    if (path && *path && hr_pc_active && !hr_output) {
        FILE *f = fopen(path, "w");
        if (f) {
            for (uint32_t k = 0; k < HR_KEYS; ++k)
                if (hr_pc_ins[k] || hr_pc_cyc[k])
                    fprintf(f, "%05X %llu %u\n", k, (unsigned long long)hr_pc_cyc[k], hr_pc_ins[k]);
            fclose(f);
        }
    }
}

void hr_int_enter(uint16_t vector)
{
    hr_context_sync();
    if (vector == 0xFFFA) ++hr_nmi_count; else if (vector == 0xFFFE) ++hr_irq_count;
    if (hr_sp < 16) hr_stack[hr_sp++] = hr_ctx;
    hr_ctx = vector == 0xFFFA ? 1 : vector == 0xFFFE ? 2 : 0;
}

void hr_fetch_pc(uint16_t pc)
{
    uint64_t now = hw.cycles + hw_native_pending_cycles();
    if (hr_last_valid && now >= hr_last_t && now - hr_last_t < 4096)
        hr_pc_cyc[hr_last_key] += now - hr_last_t;
    uint32_t key = pc < 0x8000 ? pc : 0x8000u + (((hw_cart.prg_off[(pc >> 12) & 7] >> 12) & 0x3f) << 12 | (pc & 0xfffu));
    ++hr_pc_ins[key];
    hr_last_key = key; hr_last_t = now; hr_last_valid = 1;
}
void hr_int_leave(void)
{
    hr_context_sync();
    hr_ctx = hr_sp ? hr_stack[--hr_sp] : 0;
}
void hr_clock_cut(void) { hr_context_sync(); }
void hr_clock_resume(void)
{
    /* A load may discard an unpublished cursor, while observed host work
     * remains cumulative. Rebase ownership exactly like physical clocks. */
    hr_owner_rebase += hr_ins - hr_owner_fetch_total();
    /* Restored emulated timestamps are not newly executed host work. */
    hr_ctx_time = hw.cycles + hw_native_pending_cycles();
    hr_ctx_ins = hr_ins;
    hr_ctx_interval = hw_native_interval_cycles;
    hr_ctx_dma = hr_dma_cycles;
    hr_last_valid = 0;
}
void hr_start(void)
{
    if (!hr_inited) {
        hr_inited = 1;
        atexit(hr_dump);
        const char *path = getenv("HR_PCHIST");
        if (path && *path) {
            hr_pc_cyc = calloc(HR_KEYS, sizeof(*hr_pc_cyc));
            hr_pc_ins = calloc(HR_KEYS, sizeof(*hr_pc_ins));
            hr_pc_active = hr_pc_cyc && hr_pc_ins;
        }
    }
    hr_ins = hr_owner_fetch_total();
    hr_ctx = hr_sp = hr_last_valid = 0;
    hr_ctx_time = hw.cycles + hw_native_pending_cycles();
    hr_ctx_ins = hr_ins;
    hr_ctx_interval = hw_native_interval_cycles;
    hr_ctx_dma = hr_dma_cycles;
}
#endif
