/* cpu6502.c - the CPU sequences that are not instruction templates. See
 * cpu6502.h. */
#include "cpu6502.h"

#include <string.h>

Cpu6502 cpu;
void (*cyc_cpu_rts_observer)(void);
uint8_t (*cyc_cpu_ram_read_policy)(uint16_t pc, uint16_t addr, uint8_t value);

void cpu_power_on(void) {
    hr_start();
    memset(&cpu, 0, sizeof(cpu));
    cpu.do_reset = 1;
    cpu.power_on = 1;
}

/* The NMI edge detector: clocked once per cycle, DMA cycles included, with
 * the level of the (inverted) /NMI pin. A change from released to asserted
 * between two samples latches a pending NMI, which the next poll sees even if
 * the line has been released again by then. */
void cpu_nmi_input(bool asserted) {
    if (asserted && !cpu.nmi_input) cpu.nmi_edge = 1;
    cpu.nmi_input = asserted;
}

void cpu_power_on_sequence(void) {
    /* The register contents at power-on are not defined by the hardware.
     * NESRecomp starts from zero with the reset sequence one cycle in, which
     * is also where TriCNES starts. */
    cpu.power_on = 0;
    hw_cycle_start(0, HW_IDLE);
    hw_cycle_finish(false);
    cpu_interrupt(false);
}

/* Interrupt entry. Hardware interrupts (NMI, IRQ, reset) replace the fetched
 * opcode with BRK and keep the program counter on the interrupted instruction;
 * a BRK opcode advances past its padding byte and pushes P with B set. Reset
 * turns the three pushes into reads that still decrement S. The vector is
 * chosen after the poll in the fourth cycle, so an NMI arriving during an IRQ
 * or BRK sequence takes it over. */
void cpu_interrupt(bool brk) {
    uint16_t pc = cpu.pc;
    if (brk) {
        /* The padding byte; RTI returns past it. */
        cpu_read((uint16_t)(pc + 1), 0);
        pc = (uint16_t)(pc + 2);
    } else {
        cpu_read(pc, 0);
    }
    if (cpu.do_reset) {
        cpu_read((uint16_t)(0x100 | cpu.s), 0);
        cpu.s--;
        cpu_read((uint16_t)(0x100 | cpu.s), 0);
        cpu.s--;
        cpu_read((uint16_t)(0x100 | cpu.s), CYC_POLL);
        cpu.s--;
    } else {
        cpu_write((uint16_t)(0x100 | cpu.s), (uint8_t)(pc >> 8), 0);
        cpu.s--;
        cpu_write((uint16_t)(0x100 | cpu.s), (uint8_t)pc, 0);
        cpu.s--;
        /* The 6502 polls after this push; a stack write cannot change the
         * interrupt lines, so polling before it is the same. */
        cpu_write((uint16_t)(0x100 | cpu.s), cpu_get_p(brk), CYC_POLL);
        cpu.s--;
    }
    uint16_t vector = cpu.do_nmi ? 0xFFFA : cpu.do_reset ? 0xFFFC : 0xFFFE;
    if (!brk) hr_int_enter(vector);
    if (cpu.do_nmi) cpu.nmi_edge = 0; /* taking the vector acknowledges the edge */
    uint8_t lo = cpu_read(vector, 0);
    uint8_t hi = cpu_read((uint16_t)(vector + 1), CYC_DONE);
    cpu.pc = (uint16_t)(lo | hi << 8);
    cpu.do_reset = cpu.do_nmi = cpu.do_irq = 0;
    cpu.i = 1;
}

void cpu_jam(void) {
    cpu_read((uint16_t)(cpu.pc + 1), 0);
    cpu_read(0xFFFF, 0);
    cpu_read(0xFFFE, 0);
    cpu_read(0xFFFE, 0);
    cpu.jammed = 1;
}

void cpu_jam_cycle(void) {
    cpu_read(0xFFFF, 0);
}

#if NESRECOMP_NATIVE_WAIT_LOOPS
#include "hw_internal.h"
#include "cyc_recomp.h"
#include "cyc_hooks.h"
#include <stdio.h>
#include <stdlib.h>
static uint64_t wait_cycles, wait_iterations, wait_spans;
static uint64_t wait_attempts[3], wait_entries[3];
static void wait_report(void) {
    if (getenv("NESRECOMP_WAIT_REPORT"))
        fprintf(stderr, "native_wait: cycles=%llu iterations=%llu spans=%llu jump_attempts=%llu poll_attempts=%llu copy_attempts=%llu jump_entries=%llu poll_entries=%llu copy_entries=%llu\n",
                (unsigned long long)wait_cycles,
                (unsigned long long)wait_iterations,
                (unsigned long long)wait_spans,
                (unsigned long long)wait_attempts[0], (unsigned long long)wait_attempts[1],
                (unsigned long long)wait_attempts[2], (unsigned long long)wait_entries[0],
                (unsigned long long)wait_entries[1], (unsigned long long)wait_entries[2]);
}

void cpu_native_wait_jump(uint16_t pc, uint8_t high)
{
    ++wait_attempts[0];
    if ((pc & 0xffu) > 0xfdu || pc < 0x8000 || !hw_native_interval.enabled ||
        hw_isolated || cyc_trace_enabled ||
        hw_prg_modified || hw_prg_data_modified ||
        (cpu.do_nmi | cpu.do_irq | cpu.do_reset)) return;
    /* The compiler excludes immutable declared hook sites. No registers or
     * flags change, and no RAM operand observer is involved in a JMP-self. */
    ++wait_entries[0];
    hw_native_flush();
    while (!hw_frame_done && !(cpu.do_nmi | cpu.do_irq | cpu.do_reset)) {
        unsigned cycles = hw_native_interval_limit();
        cycles -= cycles % 3;
        if (!cycles) break;
        if (!wait_spans) atexit(wait_report);
        /* One iteration ends with the last ROM operand read.
         * The shared device interval proves that no observer can distinguish
         * its internal bus phases; publish the final phase exactly once. */
        hw_native_interval.pending = cycles;
        hw_native_interval.addr = (uint16_t)(pc + 2);
        hw_native_interval.kind = HW_READ;
        hw_native_interval.value = high;
        hw_native_interval.driven = 1;
        hw_native_flush();
        wait_cycles += cycles;
        wait_iterations += cycles / 3;
        ++wait_spans;
    }
}

/* One immutable LDA-zp/BPL-back operation. Complete quiet iterations use the
 * shared device deadline; boundary instructions use the ordinary CPU bus API.
 * There is no second DMA, interrupt or partial-instruction sequencer. */
bool cpu_native_wait_poll(uint16_t pc, uint8_t zp, uint8_t tail)
{
    ++wait_attempts[1];
    if ((pc & 0xffu) > 0xfbu || pc < 0x8000 || !hw_native_interval.enabled ||
        hw_isolated || cyc_trace_enabled || cyc_cpu_ram_read_policy ||
        hw_prg_modified || hw_prg_data_modified ||
        (cpu.do_nmi | cpu.do_irq | cpu.do_reset) || (hw.ram[zp] & 0x80)) return false;
    if (hw_cart.mapper==1 && (hw_cart.m.ctrl&0x10) &&
        ((hw_cart.m.chr0^hw_cart.m.chr1)&0x1c)) return false;
    if (cyc_hooks_armed)
        for (uint32_t i=0;i<cyc_native_hook_site_count;++i)
            if (cyc_native_hook_sites[i].addr==pc ||
                cyc_native_hook_sites[i].addr==(uint16_t)(pc+2)) return false;
    ++wait_entries[1];
    for (;;) {
        hw_native_flush();
        cpu.pc=pc;
        if (hw_frame_done || (cpu.do_nmi | cpu.do_irq | cpu.do_reset)) return true;
        unsigned cycles=hw_native_interval_limit();cycles-=cycles%6;
        if (cycles) {
            if (!wait_spans) atexit(wait_report);
            cpu.a=hw.ram[zp];cpu_nz(cpu.a);
            hw_native_interval.pending=cycles;
            hw_native_interval.addr=(uint16_t)(pc+4);
            hw_native_interval.kind=HW_READ;
            hw_native_interval.value=tail;hw_native_interval.driven=1;
            wait_cycles+=cycles;wait_iterations+=cycles/6;++wait_spans;
            continue;
        }
        if (cpu_fetch_rom(pc,0xa5)) {cpu_interrupt(false);return true;}
        cpu_read_rom((uint16_t)(pc+1),zp,0);
        cpu.a=cpu_read(zp,CYC_POLL|CYC_DONE);cpu_nz(cpu.a);
        cpu.pc=(uint16_t)(pc+2);
        if (hw_frame_done || (cpu.do_nmi | cpu.do_irq | cpu.do_reset)) return true;
        if (cpu_fetch_rom(cpu.pc,0x10)) {cpu_interrupt(false);return true;}
        if (cpu.n) {
            cpu_read_rom((uint16_t)(pc+3),0xfc,CYC_POLL|CYC_DONE);
            cpu.pc=(uint16_t)(pc+4);return true;
        }
        cpu_read_rom((uint16_t)(pc+3),0xfc,CYC_POLL);
        cpu_read_rom((uint16_t)(pc+4),tail,CYC_DONE);
    }
}

/* TAY/LDA-zp/BNE-exit/JMP-back has the same ownership contract. The first
 * iteration retains the incoming accumulator in Y; later quiet iterations
 * retain zero. Every interrupt/frame exit remains an instruction boundary. */
bool cpu_native_wait_copy_poll(uint16_t pc, uint8_t zp)
{
    ++wait_attempts[2];
    if ((pc&0xffu)>0xf7u || pc<0x8000 || !hw_native_interval.enabled ||
        hw_isolated || cyc_trace_enabled || cyc_cpu_ram_read_policy ||
        hw_prg_modified || hw_prg_data_modified ||
        (cpu.do_nmi|cpu.do_irq|cpu.do_reset) || hw.ram[zp]) return false;
    if (hw_cart.mapper==1 && (hw_cart.m.ctrl&0x10) &&
        ((hw_cart.m.chr0^hw_cart.m.chr1)&0x1c)) return false;
    if (cyc_hooks_armed)
        for (uint32_t i=0;i<cyc_native_hook_site_count;++i) {
            uint16_t at=cyc_native_hook_sites[i].addr;
            if (at==pc || at==(uint16_t)(pc+1) ||
                at==(uint16_t)(pc+3) || at==(uint16_t)(pc+5)) return false;
        }
    ++wait_entries[2];
    for (;;) {
        hw_native_flush();cpu.pc=pc;
        if (hw_frame_done || (cpu.do_nmi|cpu.do_irq|cpu.do_reset)) return true;
        unsigned cycles=hw_native_interval_limit();cycles-=cycles%10;
        if (cycles) {
            if (!wait_spans) atexit(wait_report);
            cpu.y=cycles>=20?0:cpu.a;cpu.a=0;cpu_nz(0);
            hw_native_interval.pending=cycles;
            hw_native_interval.addr=(uint16_t)(pc+7);
            hw_native_interval.kind=HW_READ;
            hw_native_interval.value=(uint8_t)(pc>>8);hw_native_interval.driven=1;
            wait_cycles+=cycles;wait_iterations+=cycles/10;++wait_spans;
            continue;
        }
        if (cpu_fetch_rom(pc,0xa8)) {cpu_interrupt(false);return true;}
        cpu_read_rom((uint16_t)(pc+1),0xa5,CYC_POLL|CYC_DONE);
        cpu.y=cpu.a;cpu_nz(cpu.y);cpu.pc=(uint16_t)(pc+1);
        if (hw_frame_done || (cpu.do_nmi|cpu.do_irq|cpu.do_reset)) return true;
        if (cpu_fetch_rom(cpu.pc,0xa5)) {cpu_interrupt(false);return true;}
        cpu_read_rom((uint16_t)(pc+2),zp,0);
        cpu.a=cpu_read(zp,CYC_POLL|CYC_DONE);cpu_nz(cpu.a);cpu.pc=(uint16_t)(pc+3);
        if (hw_frame_done || (cpu.do_nmi|cpu.do_irq|cpu.do_reset)) return true;
        if (cpu_fetch_rom(cpu.pc,0xd0)) {cpu_interrupt(false);return true;}
        cpu_read_rom((uint16_t)(pc+4),3,cpu.z?(CYC_POLL|CYC_DONE):CYC_POLL);
        if (!cpu.z) {
            cpu_read_rom((uint16_t)(pc+5),0x4c,CYC_DONE);
            cpu.pc=(uint16_t)(pc+8);return true;
        }
        cpu.pc=(uint16_t)(pc+5);
        if (hw_frame_done || (cpu.do_nmi|cpu.do_irq|cpu.do_reset)) return true;
        if (cpu_fetch_rom(cpu.pc,0x4c)) {cpu_interrupt(false);return true;}
        cpu_read_rom((uint16_t)(pc+6),(uint8_t)pc,0);
        cpu_read_rom((uint16_t)(pc+7),(uint8_t)(pc>>8),CYC_POLL|CYC_DONE);
    }
}
#endif
