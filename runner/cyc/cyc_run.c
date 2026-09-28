/* cyc_run.c - see cyc_run.h. */
#include "cyc_run.h"

#include "cyc_core.h"
#include "cyc_ramview.h"
#include "cyc_recomp.h"
#include "cyc_ring.h"
#include "hw_fds.h"
#include "hw_internal.h"

bool      cyc_run_native = true;
uint64_t  cyc_run_native_cycles;
uint32_t *cyc_run_miss;
uint32_t *cyc_run_ram_miss;
uint8_t  *cyc_run_ram_opcodes;
uint64_t  cyc_run_interp_rom_cycles;
uint64_t  cyc_run_interp_ram_cycles;
uint64_t  cyc_run_interp_prg_ram_cycles;
uint64_t  cyc_run_interp_other_cycles;

/* An instruction start from $6000 up that no compiled view covers:
 * cartridge RAM (the FDS PRG RAM, MMC5 RAM windows) or ROM mapped below
 * $8000 (mapper 40), which the compiler's window does not reach. */
static bool is_prg_ram(uint16_t pc) { return pc >= 0x6000 && !hw_prg_is_rom(pc); }

/* A miss is recorded per (bank, slot, offset in slot): the recompiler needs to
 * know which bank was mapped when the instruction ran, because that is what it
 * compiles a block for. On NROM every slot has one bank and this collapses to
 * the CPU address. */
size_t cyc_run_miss_slots(void) { return (size_t)hw_cart.prg_slots * 8 * 0x1000; }

unsigned cyc_run_miss_index(uint16_t pc) {
    unsigned slot = (pc >> 12) & 7;
    return ((hw_prg_bank4(pc) * 8 + slot) << 12) | (pc & 0xFFF);
}

void cyc_run_miss_decode(unsigned index, unsigned *bank, uint16_t *addr) {
    unsigned slot = (index >> 12) & 7;
    *bank = index >> 15;
    *addr = (uint16_t)(0x8000 + (slot << 12) + (index & 0xFFF));
}

void cyc_run_power_on(void) {
    cpu_power_on();
    cyc_ramview_power_on();
    cyc_run_native_cycles = cyc_run_interp_rom_cycles = cyc_run_interp_ram_cycles = 0;
    cyc_run_interp_prg_ram_cycles = cyc_run_interp_other_cycles = 0;
}

void (*cyc_run_observer)(void);
void (*cyc_run_entry_observer)(void);

static void run_until_stop(void);

void cyc_run_frame(void) {
    hw_frame_done = hw_frame_end_hit = false;
    /* The FDS boot skip puts the machine the BIOS's intro leaves back at
     * power-on instead of running the reset sequence (cyc_fds_skip.c). */
    if (cpu.power_on && !cyc_fds_skip_power_on()) cpu_power_on_sequence();
    for (;;) {
        run_until_stop();
        bool observe = hw_observe_hit, entry = hw_entry_hit;
        if (!observe && !entry) break;
        /* An observation point (or the FDS game's first instruction): look,
         * then go on unless the frame also ended (an OAM DMA can carry the
         * CPU from scanline 240 into VBlank). */
        hw_observe_hit = hw_entry_hit = false;
        if (entry && cyc_is_fds()) cyc_fds_skip_boundary();
        if (entry && cyc_run_entry_observer) cyc_run_entry_observer();
        if (observe && cyc_run_observer) cyc_run_observer();
        if (hw_frame_end_hit) break;
        hw_frame_done = false;
    }
    cyc_ramview_frame_end();
    if (cyc_is_fds()) {
        fds_audio_frame_end();
        fds_hle_frame_end();
        fds_boot_frame_end();
    }
    cyc_ring_frame++;
}

static void run_until_stop(void) {
    int view;
    while (!hw_frame_done) {
        if (cpu.jammed) {
            cpu_jam_cycle();
        } else if (cyc_run_native && cyc_native_has(cpu.pc)) {
            uint64_t before = cyc_cycle_count();
            cyc_native_run();
            cyc_run_native_cycles += cyc_cycle_count() - before;
        } else if (cyc_run_native && (view = cyc_ramview_find(cpu.pc)) >= 0) {
            /* Code in RAM that a compiled view covers, as RAM holds it now. */
            uint64_t before = cyc_cycle_count();
            cyc_ramview_run(view);
            uint64_t took = cyc_cycle_count() - before;
            cyc_run_native_cycles += took;
            cyc_ramview_stats.native_cycles += took;
        } else {
            uint16_t pc = cpu.pc;
            uint64_t before = cyc_cycle_count();
            bool ram = pc < 0x2000 || is_prg_ram(pc);
            if (cyc_run_miss && hw_prg_is_rom(pc)) cyc_run_miss[cyc_run_miss_index(pc)]++;
            else if (cyc_run_ram_miss && ram) {
                cyc_run_ram_miss[pc]++;
                uint8_t op;
                if (cyc_run_ram_opcodes && cyc_debug_peek(pc, &op))
                    cyc_run_ram_opcodes[(size_t)pc * 32 + (op >> 3)] |= (uint8_t)(1u << (op & 7));
            }
            if (ram) cyc_ramview_interp(pc);
            cpu_interp_step();
            uint64_t took = cyc_cycle_count() - before;
            if (pc < 0x2000) cyc_run_interp_ram_cycles += took;
            else if (hw_prg_is_rom(pc)) cyc_run_interp_rom_cycles += took;
            else if (is_prg_ram(pc)) cyc_run_interp_prg_ram_cycles += took;
            else cyc_run_interp_other_cycles += took;
        }
    }
}

void cyc_cpu_state(CycCpuState *out) {
    out->pc = cpu.pc;
    out->a = cpu.a;
    out->x = cpu.x;
    out->y = cpu.y;
    out->s = cpu.s;
    out->p = (uint8_t)(cpu_get_p(0) & 0xCF);
    out->do_nmi = cpu.do_nmi;
    out->do_irq = cpu.do_irq;
}
