/* Held frame IRQ: compare compiled-cycle helpers against the coupled clock.
 * Exercise I changes at their real CLI/SEI/PLP/RTI bus-cycle positions. */
#include "hw_internal.h"
#include "cpu6502.h"
#include "cyc_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check(bool ok, const char *why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); exit(1); }
}
static void bus(uint16_t addr, uint8_t value, bool write) {
    hw_cycle_start(addr, write ? HW_WRITE : HW_READ);
    if (write) hw_write(addr, value); else (void)hw_read(addr);
    hw_cycle_finish(false);
}
static void record(unsigned n) {
    hw_native_end();
    printf("%u %016llx %016llx I=%u irq=%u nmi=%u\n", n,
        (unsigned long long)cyc_hw_state_hash(), (unsigned long long)cyc_mem_state_hash(),
        cpu.i, cpu.do_irq, cpu.do_nmi);
}
static void nop_then_irq(void) {
    check(!cpu_fetch_rom(0x8101, 0xea), "CLI/PLP delays IRQ one instruction");
    (void)cpu_read_rom(0x8102, 0xea, CYC_POLL | CYC_DONE);
    check(cpu_fetch_rom(0x8102, 0xea), "IRQ recognized after delayed instruction");
}
int main(int argc, char **argv) {
    bool candidate = argc > 1 && !strcmp(argv[1], "candidate");
    static uint8_t image[16 + 32768 + 8192];
    memcpy(image, "NES\032", 4); image[4] = 2; image[5] = 1;
    for (size_t i = 16; i < sizeof(image); ++i) image[i] = (uint8_t)(i * 37u + (i >> 3));
    for (unsigned align = 0; align < 4; ++align) {
        check(cyc_load_ines(image, sizeof(image)), "fixture");
        cyc_power_on((uint8_t)align);
        check(cyc_audio_enable(48000), "audio");
        cpu.i = 1; cpu.do_reset = cpu.power_on = 0;
        for (unsigned i = 0; i < 60000; ++i) bus(0, 0, false);
        bus(0x2001, 0x1e, true);
        bus(0x4017, 0x00, true);
        hw_native_set_masked_irq(true);
        unsigned wait;
        for (wait = 0; wait < 100000; ++wait) {
            if (hw.irq_line && apu_native_masked_irq_ready() && hw_native_interval_limit() >= 20) break;
            bus(0, 0, false);
        }
        check(wait < 100000, "reach held IRQ and stable render span");
        check(hw.irq_line && cpu.i && !cpu.do_irq, "masked pending level");
        CycSnapshot *snapshot = cyc_snapshot_new(); check(snapshot != NULL, "snapshot");
        cyc_snapshot_take(snapshot);
        printf("align %u\n", align);
        for (unsigned kind = 0; kind < 8; ++kind) {
            cyc_snapshot_restore(snapshot);
            hw_native_set_masked_irq(candidate);
            if (kind == 2) cpu.i = 0; /* SEI begins unmasked. */
            hw_native_begin();
            check(!cpu_fetch_rom(0x8100, 0xea), "initial opcode not interrupted");
            switch (kind) {
            case 0: /* Stay masked across multiple complete frame sequences. */
                for (unsigned i = 0; i < 80000; ++i) {
                    (void)cpu_read_rom(0x8101, 0xea, CYC_POLL | CYC_DONE);
                    check(!cpu_fetch_rom(0x8100, 0xea), "masked IRQ not taken");
                }
                break;
            case 1: /* CLI: poll before clearing I. */
                (void)cpu_read_rom(0x8101, 0xea, CYC_POLL | CYC_DONE); cpu.i = 0;
                nop_then_irq(); break;
            case 2: /* SEI: a sampled IRQ survives setting I. */
                (void)cpu_read_rom(0x8101, 0xea, CYC_POLL | CYC_DONE); cpu.i = 1;
                check(cpu_fetch_rom(0x8101, 0xea), "SEI retains sampled IRQ"); break;
            case 3: /* PLP: final-cycle poll sees old I. */
                (void)cpu_read_rom(0x8101, 0xea, 0);
                (void)cpu_read(0x1fd, 0); hw.ram[0x1fe] = 0x20;
                cpu_set_p(cpu_read(0x1fe, CYC_POLL | CYC_DONE));
                nop_then_irq(); break;
            case 4: /* RTI: restored I precedes the final poll. */
                (void)cpu_read_rom(0x8101, 0xea, 0);
                (void)cpu_read(0x1fc, 0); hw.ram[0x1fd] = 0x20;
                cpu_set_p(cpu_read(0x1fd, 0));
                (void)cpu_read(0x1fe, 0);
                (void)cpu_read(0x1ff, CYC_POLL | CYC_DONE);
                check(cpu_fetch_rom(0x8101, 0xea), "RTI polls restored I"); break;
            case 5: /* Reading status schedules IRQ clear. */
                (void)cpu_read(0x4015, CYC_POLL | CYC_DONE);
                for (unsigned i = 0; i < 40; ++i) (void)cpu_read(0, CYC_POLL | CYC_DONE);
                check(!hw.irq_line, "status read clears IRQ"); break;
            case 6: /* Register write resets sequencer and inhibits IRQ. */
                cpu_write(0x4017, 0xc0, CYC_POLL | CYC_DONE);
                for (unsigned i = 0; i < 40; ++i) (void)cpu_read(0, CYC_POLL | CYC_DONE);
                check(!hw.irq_line, "inhibit clears IRQ"); break;
            case 7: /* DMA invalidates credit and retains original stalls. */
                cpu_write(0x4014, 2, CYC_POLL | CYC_DONE);
                (void)cpu_read_rom(0x8101, 0xea, CYC_POLL | CYC_DONE);
                break;
            }
            record(kind);
        }
        check(!candidate || hw_native_interval_cycles > 1000, "held IRQ path actually used");
        cyc_snapshot_free(snapshot);
    }
    return 0;
}
