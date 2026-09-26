/* cpu6502.c - the CPU sequences that are not instruction templates. See
 * cpu6502.h. */
#include "cpu6502.h"

#include <string.h>

Cpu6502 cpu;

void cpu_power_on(void) {
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
