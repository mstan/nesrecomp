/* cyc_run.c - see cyc_run.h. */
#include "cyc_run.h"

#include "cyc_core.h"
#include "cyc_recomp.h"
#include "hw_internal.h"

bool      cyc_run_native = true;
uint64_t  cyc_run_native_cycles;
uint32_t *cyc_run_miss;
uint32_t *cyc_run_ram_miss;
uint8_t  *cyc_run_ram_opcodes;
uint64_t  cyc_run_interp_rom_cycles;
uint64_t  cyc_run_interp_ram_cycles;
uint64_t  cyc_run_interp_other_cycles;

/* A miss is recorded per (bank, slot, offset in slot): the recompiler needs to
 * know which bank was mapped when the instruction ran, because that is what it
 * compiles a block for. On NROM every slot has one bank and this collapses to
 * the CPU address. */
size_t cyc_run_miss_slots(void) { return (size_t)hw_cart.prg_slots * 4 * 0x2000; }

unsigned cyc_run_miss_index(uint16_t pc) {
    unsigned slot = (pc >> 13) & 3;
    return ((hw_prg_bank(pc) * 4 + slot) << 13) | (pc & 0x1FFF);
}

void cyc_run_miss_decode(unsigned index, unsigned *bank, uint16_t *addr) {
    unsigned slot = (index >> 13) & 3;
    *bank = index >> 15;
    *addr = (uint16_t)(0x8000 + (slot << 13) + (index & 0x1FFF));
}

void cyc_run_power_on(void) {
    cpu_power_on();
}

void cyc_run_frame(void) {
    hw_frame_done = false;
    if (cpu.power_on) cpu_power_on_sequence();
    while (!hw_frame_done) {
        if (cpu.jammed) {
            cpu_jam_cycle();
        } else if (cyc_run_native && cyc_native_has(cpu.pc)) {
            uint64_t before = cyc_cycle_count();
            cyc_native_run();
            cyc_run_native_cycles += cyc_cycle_count() - before;
        } else {
            uint16_t pc = cpu.pc;
            uint64_t before = cyc_cycle_count();
            if (cyc_run_miss && pc >= 0x8000) cyc_run_miss[cyc_run_miss_index(pc)]++;
            else if (cyc_run_ram_miss && pc < 0x2000) {
                cyc_run_ram_miss[pc]++;
                if (cyc_run_ram_opcodes) {
                    uint8_t op = cyc_cpu_ram()[pc & 0x7FF];
                    cyc_run_ram_opcodes[(pc & 0x7FF) * 32 + (op >> 3)] |= (uint8_t)(1u << (op & 7));
                }
            }
            cpu_interp_step();
            uint64_t took = cyc_cycle_count() - before;
            if (pc < 0x2000) cyc_run_interp_ram_cycles += took;
            else if (pc >= 0x8000) cyc_run_interp_rom_cycles += took;
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
