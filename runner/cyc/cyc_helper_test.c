/*
 * cyc_helper_test.c - exhaustive check of the CPU operation helpers in
 * cpu6502.h (cpu_adc, cpu_sbc, cpu_cmp, cpu_bit, the shifts and the status
 * register) against plain reference formulas, for every input.
 *
 * Recompiled code and the interpreter inline these helpers. The per-cycle
 * comparison with the oracle catches a miscompiled copy wherever it runs;
 * this test catches a wrong formula directly and names it. (An earlier build
 * found MSVC 19.44 /O2 compiling one shift helper wrong this way.)
 *
 *   cyc_helper_test        exit status 0 = all helpers agree
 */
#include "cpu6502.h"

#include <stdio.h>

/* The helpers under test do not touch the bus. */
int  hw_dma_stalls;
bool hw_frame_done;
void hw_cycle_start(uint16_t addr, HwCycleKind kind) { (void)addr; (void)kind; }
uint8_t hw_read(uint16_t addr) { (void)addr; return 0; }
uint8_t hw_read_rom(uint16_t addr, uint8_t value) { (void)addr; return value; }
void hw_write(uint16_t addr, uint8_t value) { (void)addr; (void)value; }
void hw_cycle_finish(bool done) { (void)done; }
bool hw_irq_line(void) { return false; }
bool cyc_trace_enabled;
void cyc_trace_instruction(uint16_t pc, uint8_t a, uint8_t x, uint8_t y, uint8_t s, uint8_t p) {
    (void)pc; (void)a; (void)x; (void)y; (void)s; (void)p;
}
Cpu6502 cpu;

typedef struct {
    int result, c, z, n, v;   /* v < 0: not checked; result < 0: not checked */
} Expect;

static int failures;

static void load(uint8_t a, int c) {
    cpu.a = a;
    cpu.c = (uint8_t)c;
    cpu.v = 1;
    cpu.z = (uint8_t)!c;  /* arbitrary, distinct from typical results */
    cpu.n = (uint8_t)c;
}

static Expect nzc(int result, int flags_from, int c, int v) {
    Expect e = {result, c, (flags_from & 0xFF) == 0, (flags_from & 0x80) != 0, v};
    return e;
}

static void check(const char *name, int input, int carry_in, int got, Expect e) {
    bool bad = (e.result >= 0 && got != e.result) || cpu.c != e.c || cpu.z != e.z || cpu.n != e.n ||
               (e.v >= 0 && cpu.v != e.v);
    if (bad && failures++ < 20) {
        fprintf(stderr, "%s input=%04X C_in=%d: got %02X C=%d Z=%d N=%d V=%d, expected %02X C=%d Z=%d N=%d V=%d\n",
                name, input, carry_in, got & 0xFF, cpu.c, cpu.z, cpu.n, cpu.v, e.result & 0xFF, e.c, e.z, e.n, e.v);
    }
}

int main(void) {
    for (int a = 0; a < 256; a++) {
        for (int c = 0; c < 2; c++) {
            int got;
            load((uint8_t)a, c);
            got = cpu_asl((uint8_t)a);
            check("asl", a, c, got, nzc((a << 1) & 0xFF, a << 1, (a & 0x80) != 0, 1));
            load((uint8_t)a, c);
            got = cpu_lsr((uint8_t)a);
            check("lsr", a, c, got, nzc(a >> 1, a >> 1, a & 1, 1));
            load((uint8_t)a, c);
            got = cpu_rol((uint8_t)a);
            check("rol", a, c, got, nzc(((a << 1) | c) & 0xFF, (a << 1) | c, (a & 0x80) != 0, 1));
            load((uint8_t)a, c);
            got = cpu_ror((uint8_t)a);
            check("ror", a, c, got, nzc((a >> 1) | (c << 7), (a >> 1) | (c << 7), a & 1, 1));

            for (int m = 0; m < 256; m++) {
                int in = a << 8 | m;
                load((uint8_t)a, c);
                cpu_adc((uint8_t)m);
                {
                    int sum = a + m + c;
                    check("adc", in, c, cpu.a, nzc(sum & 0xFF, sum, sum > 0xFF, ((a ^ sum) & (m ^ sum) & 0x80) != 0));
                }
                load((uint8_t)a, c);
                cpu_sbc((uint8_t)m);
                {
                    int diff = a - m - (1 - c);
                    check("sbc", in, c, cpu.a, nzc(diff & 0xFF, diff, diff >= 0, ((a ^ m) & (a ^ diff) & 0x80) != 0));
                }
                load((uint8_t)a, c);
                cpu_cmp((uint8_t)a, (uint8_t)m);
                check("cmp", in, c, cpu.a, nzc(a, a - m, a >= m, 1));
                load((uint8_t)a, c);
                cpu_bit((uint8_t)m);
                {
                    Expect e = {a, c, (a & m) == 0, (m & 0x80) != 0, (m & 0x40) != 0};
                    check("bit", in, c, cpu.a, e);
                }
            }
        }
    }
    for (int p = 0; p < 256; p++) {
        cpu_set_p((uint8_t)p);
        int got = cpu_get_p(0), want = (p & 0xCF) | 0x20;
        if (got != want && failures++ < 20)
            fprintf(stderr, "cpu_set_p/cpu_get_p(0) %02X: got %02X, expected %02X\n", p, got, want);
        got = cpu_get_p(1);
        if (got != (want | 0x10) && failures++ < 20)
            fprintf(stderr, "cpu_get_p(1) %02X: got %02X, expected %02X\n", p, got, want | 0x10);
    }

    if (failures) {
        fprintf(stderr, "cyc_helper_test: %d mismatches\n", failures);
        return 1;
    }
    printf("cyc_helper_test: all CPU helpers match the reference\n");
    return 0;
}
