/* cyc_run.c - see cyc_run.h. */
#include "cyc_run.h"

#include "cyc_core.h"
#include "cyc_hooks.h"
#include "cyc_mod.h"
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
}

void (*cyc_run_observer)(void);

static void run_until_stop(void);

void cyc_run_frame(void) {
    hw_frame_done = hw_frame_end_hit = false;
    if (cpu.power_on) cpu_power_on_sequence();
    for (;;) {
        run_until_stop();
        if (!hw_observe_hit) break;
        /* An observation point: look, then go on unless the frame also
         * ended (an OAM DMA can carry the CPU from scanline 240 into VBlank). */
        hw_observe_hit = false;
        if (cyc_run_observer) cyc_run_observer();
        if (hw_frame_end_hit) break;
        hw_frame_done = false;
    }
    cyc_ramview_frame_end();
    cyc_hooks_frame_end();
    cyc_mod_frame_end();
    if (cyc_is_fds()) {
        fds_audio_frame_end();
        fds_hle_frame_end();
    }
    cyc_ring_frame++;
}

/* One dispatch at an instruction boundary: recompiled ROM code, a compiled
 * RAM view, or the interpreter. */
static void dispatch(void) {
    int view;
    {
        if (cpu.jammed) {
            cpu_jam_cycle();
        } else if (cyc_run_native && !hw_prg_modified && cyc_native_has(cpu.pc)) {
            uint64_t before = cyc_cycle_count();
            cyc_native_run();
            cyc_run_native_cycles += cyc_cycle_count() - before;
        } else if (cyc_run_native && !hw_prg_modified && (view = cyc_ramview_find(cpu.pc)) >= 0) {
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

static void run_until_stop(void) {
    while (!hw_frame_done) {
        /* Mod hook sites (cyc_hooks.h): the callbacks run at the boundary
         * before the site's instruction, whichever path would run it; the
         * compiled instruction then lets execution through once. */
        if (cyc_hooks_armed) {
            cyc_hook_hit = false;
            if (cyc_hooks_due(cpu.pc)) {
                uint16_t at = cpu.pc;
                cyc_hooks_fire(at);
                if (cpu.pc == at) cyc_hook_passed = at;
                continue;
            }
        }
        dispatch();
        cyc_hook_passed = -1;
    }
}

/* A mod's isolated call (cyc_mod.c): run until the routine returns to stop_pc
 * with the stack back at entry_s, hw_isolated set (no clocks; hw_frame_done
 * rises when the cycle budget runs out). */
int cyc_run_isolated(uint16_t stop_pc, uint8_t entry_s) {
    while (!(cpu.pc == stop_pc && cpu.s == entry_s)) {
        if (hw_frame_done) return CYC_MOD_FAIL_BUDGET;
        if (cpu.jammed) return CYC_MOD_FAIL_JAM;
        if ((int8_t)(cpu.s - entry_s) > 0) return CYC_MOD_FAIL_STACK;   /* returned past its caller */
        dispatch();
    }
    return 0;
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
