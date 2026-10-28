/* Compare independent PPU/APU ownership with the original coupled clock.
 * Both modes query identical interval boundaries; only the candidate uses
 * the interval executor. Hashes include devices, bus, RAM and raster output. */
#include "hw_internal.h"
#include "hw.h"
#include "cpu6502.h"
#include "cyc_state.h"
#include "cyc_mod.h"
#include "cyc_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(bool ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

static void access_bus(uint16_t addr, uint8_t value, bool write)
{
    hw_cycle_start(addr, write ? HW_WRITE : HW_READ);
    if (write) hw_write(addr, value); else (void)hw_read(addr);
    hw_cycle_finish(false);
}

static void stimulus(unsigned i)
{
    if (hw_cart.mapper == 4) {
        switch (i % 257) {
        case 11: access_bus(0xc000, (uint8_t)(i >> 8), true); break;
        case 12: access_bus(0xc001, 0, true); break;
        case 13: access_bus(0xe001, 0, true); break;
        case 14: access_bus(0xe000, 0, true); break;
        case 15: access_bus(0x8000, (uint8_t)(i >> 8), true); break;
        case 16: access_bus(0x8001, (uint8_t)(i >> 4), true); break;
        }
    } else if (hw_cart.mapper == 66 && i % 257 == 11) {
        access_bus(0x8000, (uint8_t)((i >> 8) & 0x11), true);
    }
    switch (i % 1031) {
    case 0: access_bus(0x2001, (i & 2048) ? 0x1e : 0, true); break;
    case 1: access_bus(0x2000, (uint8_t)((i >> 5) & 0x7f), true); break;
    case 2: access_bus(0x2005, (uint8_t)i, true); break;
    case 3: access_bus(0x2005, (uint8_t)(i >> 4), true); break;
    case 4: access_bus(0x4014, 2, true); break;
    case 5: access_bus(0x4017, (i & 4096) ? 0 : 0x40, true); break;
    case 6: access_bus(0x4010, 0x8f, true); break;
    case 7: access_bus(0x4013, 1, true); break;
    case 8: access_bus(0x4015, (i & 8192) ? 0x1f : 0x0f, true); break;
    case 9: access_bus(0x4015, 0, false); break;
    case 10: access_bus(0x2002, 0, false); break;
    default: access_bus((uint16_t)(i & 0x7ff), 0, false); break;
    }
}

static uint8_t operand_observer(uint16_t pc, uint16_t addr, uint8_t value)
{
    (void)pc; (void)addr;
    check(!hw_native_interval.pending && !hw_native_interval.credit,
          "operand observer sees committed hardware");
    return value;
}

static void cursor_stimulus(unsigned i)
{
    (void)cpu_fetch_rom(0x8123, 0x63);
    uint16_t addr = (uint16_t)(i & 0x1fff);
    uint8_t value = cpu_read(addr, CYC_POLL);
    if (hw_native_interval.pending) check(hw_dma_stalls == 0, "fast cycle has no DMA stalls");
    value = cpu_ram_read_value(0x8123, addr, value);
    cpu_write(addr, (uint8_t)(value + i), CYC_DONE);
    (void)cpu_read_rom(0x8124, 0x8c, CYC_POLL | CYC_DONE);
    if (i % 17 == 0 && cyc_cpu_rts_observer) cyc_cpu_rts_observer();
    if (i % 37 == 0) stimulus(i);
}

static void return_observer(void)
{
    check(!hw_native_interval.pending && !hw_native_interval.credit,
          "return observer sees committed hardware");
}

static void sprite_output_cases(bool reference)
{
    static const uint8_t positions[] = { 0, 1, 2, 7, 8, 9, 15, 16, 239, 240, 255 };
    static const uint8_t planes[] = { 0, 0xff, 0x80, 0x01, 0xaa, 0x55, 0x81 };
    for (unsigned q = 0; q < 96; ++q) {
        access_bus(0x2001, q % 3 == 0 ? 0x0a : q % 3 == 1 ? 0x14 : 0x1e, true);
        unsigned limit = 0, wait;
        for (wait = 0; wait < 100000; ++wait) {
            limit = hw_native_interval_limit();
            if (limit >= 12) break;
            access_bus(0, 0, false);
        }
        check(wait < 100000, "reach stable sprite fixture");
        for (unsigned i = 0; i < 8; ++i) {
            ppu.spr_x[i] = q % 4 ? positions[(q + i * 3) % sizeof(positions)] : (uint8_t)(q % 9);
            ppu.spr_lo[i] = planes[(q + i) % sizeof(planes)];
            ppu.spr_hi[i] = planes[(q * 3 + i) % sizeof(planes)];
            ppu.spr_attr[i] = (uint8_t)(q * 29u + i * 11u);
        }
        ppu.fine_x = (uint8_t)(q & 7);
#if NESRECOMP_PPU_HLE
        ppu.bg_pixels = 0x924936c9u ^ (q * 0x01020408u);
        ppu.attr_pixels = 0xe4e4e4e4u;
#else
        ppu.bg_lo = 0xa57c; ppu.bg_hi = 0x65a3;
        ppu.attr_lo = 0x5a; ppu.attr_hi = 0xa5;
#endif
        ppu.can_s0hit = ppu.cur_has_s0 = 1;
        ppu.s0hit = ppu.s0hit_late = ppu.s0hit_pending1 = ppu.s0hit_pending2 = 0;
        unsigned n = q % 4 == 0 ? limit : (q % limit) + 1;
        if (reference) {
            for (unsigned j = 0; j < n; ++j) access_bus(0, 0, false);
        } else {
            hw.cpu_addr = 0; hw.cpu_reading = 1;
            hw.data_driven = 1; hw.data_bus = hw.internal_bus = hw.ram[0];
            hw_native_interval_run(n);
        }
        printf("sprite %u %016llx %016llx\n", q,
               (unsigned long long)cyc_hw_state_hash(),
               (unsigned long long)cyc_mem_state_hash());
    }
    if (!reference) check(ppu_native_sprite_spans > 10, "prepared sprite output exercised");
}

int main(int argc, char **argv)
{
    bool reference = argc > 1 && strstr(argv[1], "reference") != NULL;
    bool cursor = argc > 1 && strstr(argv[1], "cpu") != NULL;
    static uint8_t image[16 + 65536 + 16384];
    static const unsigned mappers[] = { 0, 1, 4, 66 };
    memcpy(image, "NES\032", 4);
    for (size_t i = 16; i < sizeof(image); ++i) image[i] = (uint8_t)(i * 37u + (i >> 3));
    for (unsigned board = 0; board < sizeof(mappers) / sizeof(mappers[0]); ++board) {
        unsigned mapper = mappers[board];
        image[4] = mapper >= 4 ? 4 : 2;
        image[5] = mapper >= 4 ? 2 : 1;
        image[6] = (uint8_t)(mapper << 4);
        image[7] = (uint8_t)(mapper & 0xf0);
        for (unsigned align = 0; align < 4; ++align) {
            check(cyc_load_ines(image, 16u + image[4] * 16384u + image[5] * 8192u), "load fixture");
            check(cyc_set_region(CYC_REGION_NTSC), "region");
            cyc_power_on((uint8_t)align);
            check(cyc_audio_enable(44100), "audio");
            check(cyc_zapper_attach(2), "Zapper");
            cyc_set_zapper(120, 100, true);
            access_bus(0x4017, 0x40, true);
            access_bus(0x4015, 0x0f, true);
            for (unsigned i = 0; i < 16; ++i)
                access_bus((uint16_t)(0x4000 + i), (uint8_t)(i * 31u + 7), true);
            for (unsigned i = 0; i < sizeof(hw.ram); ++i) hw.ram[i] = (uint8_t)(i * 13u);
            access_bus(0x2001, 0x1e, true);
            cpu.do_reset = cpu.power_on = 0;
            cyc_cpu_ram_read_policy = cursor ? operand_observer : NULL;
            cyc_mod_set_return_hook(cursor ? return_observer : NULL);
            unsigned spans = 0, cycles = 0;
            printf("case mapper=%u align=%u\n", mapper, align);
            sprite_output_cases(reference);
            if (cursor && !reference) hw_native_begin();
            for (unsigned i = 1; i <= 50000; ++i) {
                if (cursor) {
                    cursor_stimulus(i);
                    if (i % 127 == 0) {
                        hw_native_end();
                        printf("%u %016llx %016llx %u%u%u\n", i,
                               (unsigned long long)cyc_hw_state_hash(),
                               (unsigned long long)cyc_mem_state_hash(),
                               cpu.do_irq, cpu.do_nmi, cpu.nmi_edge);
                        if (!reference) hw_native_begin();
                    }
                    continue;
                }
                /* Normal accesses exercise eligibility invalidation and edges.
                 * Then vary run length, including the last admissible cycle. */
                stimulus(i);
                unsigned n = hw_native_interval_limit();
                if (n) {
                    check(n <= 80, "bounded visible span");
                    uint8_t saved_tick = hw.tick;
                    hw.tick = 0;
                    check(!hw_native_interval_limit(), "partial cycle refusal");
                    hw.tick = saved_tick;
                    cyc_trace_enabled = true;
                    check(!hw_native_interval_limit(), "trace refusal");
                    cyc_trace_enabled = false;
                    cpu.nmi_input = 1;
                    check(!hw_native_interval_limit(), "NMI refusal");
                    cpu.nmi_input = 0;
                    hw.irq_line = 1;
                    check(!hw_native_interval_limit(), "IRQ refusal");
                    hw.irq_line = 0;
                    hw_cart.mapper = 48;
                    check(!hw_native_interval_limit(), "unsupported IRQ board refusal");
                    hw_cart.mapper = (uint16_t)mapper;
                    if (mapper == 4) {
                        uint8_t saved_out = hw_cart.m.irq_out;
                        hw_cart.m.irq_out = 1;
                        check(!hw_native_interval_limit(), "unsampled mapper IRQ refusal");
                        hw_cart.m.irq_out = saved_out;
                        uint8_t saved_table = ppu.bg_table, saved_a12 = hw_cart.m.a12;
                        ppu.bg_table = 1; hw_cart.m.a12 = 0;
                        check(!hw_native_interval_limit(), "possible first A12 rise refusal");
                        ppu.bg_table = saved_table; hw_cart.m.a12 = saved_a12;
                    }
                    hw_extra_timing.extra_scanlines = 1;
                    check(!hw_native_interval_limit(), "extra timing refusal");
                    hw_extra_timing.extra_scanlines = 0;
                    n = (i % n) + 1;
                    if (reference) {
                        for (unsigned j = 0; j < n; ++j) access_bus(0, 0, false);
                    } else {
                        hw.cpu_addr = 0; hw.cpu_reading = 1;
                        hw.data_driven = 1; hw.data_bus = hw.internal_bus = hw.ram[0];
                        hw_native_interval_run(n);
                    }
                    ++spans; cycles += n;
                }
                if (i % 127 == 0)
                    printf("%u %016llx %016llx\n", i,
                           (unsigned long long)cyc_hw_state_hash(),
                           (unsigned long long)cyc_mem_state_hash());
            }
            hw_native_end();
            if (cursor) {
                if (!reference) check(hw_native_interval_cycles > 100, "CPU cursor actually used");
                /* A snapshot may close an unfinished interval. Its derived
                 * credit must be rebuilt, and current-build replay must agree. */
                CycSnapshot *snapshot = cyc_snapshot_new();
                check(snapshot != NULL, "snapshot");
                cyc_snapshot_take(snapshot);
                if (!reference) hw_native_begin();
                for (unsigned j = 1; j <= 257; ++j) cursor_stimulus(j);
                hw_native_end();
                uint64_t after = cyc_hw_state_hash(), picture = cyc_mem_state_hash();
                cyc_snapshot_restore(snapshot);
                if (!reference) hw_native_begin();
                for (unsigned j = 1; j <= 257; ++j) cursor_stimulus(j);
                hw_native_end();
                check(after == cyc_hw_state_hash() && picture == cyc_mem_state_hash(), "cursor replay");
                cyc_snapshot_free(snapshot);
            } else {
                printf("coverage %u %u\n", spans, cycles);
                check(spans > 100, "native intervals actually selected");
            }
            int16_t pcm[8192];
            size_t count = cyc_audio_read(pcm, sizeof(pcm) / sizeof(pcm[0]));
            uint64_t hash = UINT64_C(1469598103934665603);
            for (size_t i = 0; i < count; ++i) {
                hash = (hash ^ (uint8_t)pcm[i]) * UINT64_C(1099511628211);
                hash = (hash ^ (uint8_t)((uint16_t)pcm[i] >> 8)) * UINT64_C(1099511628211);
            }
            printf("pcm %zu %016llx\n", count, (unsigned long long)hash);
            check(cyc_set_region(CYC_REGION_PAL), "set PAL");
            cyc_power_on((uint8_t)align);
            check(hw_pal(), "PAL active");
            check(!hw_native_interval_limit(), "PAL refusal");
        }
    }
    return 0;
}
