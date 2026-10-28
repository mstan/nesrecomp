/* Atomic PRG edits, external folded data, pointer escapes and image epochs. */
#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_mod.h"
#include "cyc_recomp.h"
#include "cyc_run.h"
#include "cyc_state.h"
#include "cyc_trace.h"
#include "hw_internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(bool ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

static void serial(uint16_t addr, unsigned value)
{
    for (unsigned i = 0; i < 5; ++i) {
        cpu_write(addr, (uint8_t)((value >> i) & 1), CYC_DONE);
        (void)cpu_read(0, CYC_DONE);
    }
}

int main(int argc, char **argv)
{
    check(argc == 6, "path native alignment trace data-only");
    bool native = atoi(argv[2]) != 0, trace = atoi(argv[4]) != 0;
    bool data_only = atoi(argv[5]) != 0;
    unsigned align = (unsigned)atoi(argv[3]);
    uint8_t image[16 + 32768], payload[32768];
    FILE *f = fopen(argv[1], "rb"); check(f != NULL, "open fixture");
    check(fread(image, 1, sizeof(image), f) == sizeof(image), "read fixture"); fclose(f);
    check(cyc_load_ines(image, sizeof(image)), "load fixture");
    cyc_run_native = native; cyc_trace_enabled = trace;
    cyc_power_on((uint8_t)align); cyc_run_power_on();
    check(cyc_audio_enable(48000), "audio enable");
    check(cyc_mod_copy_cart_payload(payload, sizeof(payload)) == sizeof(payload), "copy payload");
    check(cyc_native_has(0x8F00) && cyc_native_has(0xC000) && cyc_native_has(0xC400), "explicit seeds compiled");
    uint64_t pcm = UINT64_C(1469598103934665603), samples = 0;
    uint64_t modified_native = 0, modified_intervals = 0;
    int16_t audio[2048];
    uint64_t epoch_before_reload = 0;
    for (unsigned frame = 0; frame < 33; ++frame) {
        if (frame == 5) {
            payload[0x0E01] = 0xC4;
            check(data_only ? cyc_mod_apply_cart_data_payload(payload, sizeof(payload)) :
                              cyc_mod_apply_cart_payload(payload, sizeof(payload)), "patch indirect pointer data");
            check(cyc_native_has(0x8F00), "unchanged indirect JMP still admitted");
        }
        if (frame == 8) {
            payload[0x4401] = 3;
            check(!cyc_mod_apply_cart_data_payload(payload, sizeof(payload)), "data-only code edit refused");
            check(cyc_mod_peek(0xC401) == 2, "refused edit atomic");
            payload[0x4401] = 2;
        }
        if (frame == 9 || frame == 13) {
            payload[0x4401] = (frame == 9) ? 3 : 2;
            check(cyc_mod_apply_cart_payload(payload, sizeof(payload)), "atomic code edit/restore");
            /* The predecessor's conservative operand tail may also reject
             * C000; the independent bank containing 8F00 must stay native. */
            check(cyc_native_has(0x8F00), "independent unchanged chunk retained");
            check(cyc_native_has(0xC400) == (frame == 13), "changed chunk fallback then readmission");
        }
        if (frame == 17) {
            size_t size; uint8_t *rw = cyc_mod_prg_data_rw(&size);
            check(rw != NULL && size == sizeof(payload), "borrow writable PRG");
            rw[0x4401] = 4;
            check(!cyc_native_prg_guard_available() && !cyc_native_has(0x8F00), "pointer escape disables ROM admission");
        }
        if (frame == 21) {
            payload[0x4401] = 2;
            check(cyc_mod_apply_cart_payload(payload, sizeof(payload)), "atomic replacement after pointer escape");
            check(!cyc_native_prg_guard_available(), "escaped pointer remains untracked until load");
        }
        if (frame == 25) {
            epoch_before_reload = hw_prg_guard_epoch;
            check(cyc_load_ines(image, sizeof(image)), "reload original image");
            cyc_power_on((uint8_t)align); cyc_run_power_on();
            check(cyc_audio_enable(48000), "reload audio enable");
            check(hw_prg_guard_epoch > epoch_before_reload, "epoch monotonic across reload");
            check(cyc_mod_copy_cart_payload(payload, sizeof(payload)) == sizeof(payload), "reload payload");
            payload[0x0E01] = 0xC4; payload[0x4401] = 5;
            check(cyc_mod_apply_cart_payload(payload, sizeof(payload)), "new image code edit");
            check(cyc_native_has(0x8F00) && !cyc_native_has(0xC400), "old valid cache not reused");
        }
        uint64_t before = cyc_run_native_cycles, interval_before = hw_native_interval_cycles;
        cyc_run_frame();
        uint64_t used = cyc_run_native_cycles - before;
        if (frame >= 6 && frame < 17) {
            modified_native += used;
            modified_intervals += hw_native_interval_cycles - interval_before;
        }
        if (frame >= 17 && frame < 25) check(!used, "no ROM native cycles after pointer escape");
        size_t count;
        while ((count = cyc_audio_read(audio, sizeof(audio) / sizeof(audio[0]))) != 0) {
            for (size_t i = 0; i < count; ++i) {
                uint16_t value = (uint16_t)audio[i];
                pcm = (pcm ^ (value & 255)) * UINT64_C(1099511628211);
                pcm = (pcm ^ (value >> 8)) * UINT64_C(1099511628211);
            }
            samples += count;
        }
        CycCpuState regs; cyc_cpu_state(&regs);
        printf("%u %04X %02X %02X %02X %02X %02X %016" PRIx64 " %016" PRIx64
               " %016" PRIx64 " %016" PRIx64 " %" PRIu64 "\n", frame, regs.pc,
               regs.a, regs.x, regs.y, regs.s, regs.p, cyc_hw_state_hash(),
               cyc_mem_state_hash(), trace ? cyc_trace_hash : 0, pcm, samples);
    }
    check(samples > 20000, "full PCM captured");
    check(cyc_cpu_ram()[5] != 0, "new image executes changed operand");
    if (native) {
        check(modified_native > 0, "modified tracked image actually uses native ROM");
        if (!trace) check(modified_intervals > 0, "modified native ROM uses deadline cursor");
    }
    /* Restore derived mapping pointers on the next native entry, for both
     * in-process snapshots and the current build's serialized state. */
    CycSnapshot *snapshot = cyc_snapshot_new(); check(snapshot != NULL, "snapshot allocation");
    cyc_snapshot_take(snapshot);
    uint8_t *state = NULL; size_t state_size = 0; char error[256];
    check(cyc_state_save(&state, &state_size, error, sizeof(error)), "save current patched build");
    cyc_run_frame();
    uint64_t replay_hw = cyc_hw_state_hash(), replay_mem = cyc_mem_state_hash();
    CycCpuState replay_regs; cyc_cpu_state(&replay_regs);
    for (unsigned kind = 0; kind < 2; ++kind) {
        if (kind) check(cyc_state_load(state, state_size, error, sizeof(error)), "load current patched build");
        else cyc_snapshot_restore(snapshot);
        cyc_run_frame(); CycCpuState regs; cyc_cpu_state(&regs);
        check(cyc_hw_state_hash() == replay_hw && cyc_mem_state_hash() == replay_mem &&
              regs.pc == replay_regs.pc && regs.a == replay_regs.a && regs.x == replay_regs.x &&
              regs.y == replay_regs.y && regs.s == replay_regs.s && regs.p == replay_regs.p,
              "patched native snapshot/state replay");
    }
    free(state); cyc_snapshot_restore(snapshot);
    if (!trace) {
        /* A mapper store can change a folded data source while the generated
         * umbrella is still running. Its cached pointers must follow it. */
        uint8_t value[2]; uint64_t machine[2], memory[2];
        for (unsigned fast = 0; fast < 2; ++fast) {
            cyc_snapshot_restore(snapshot);
            if (fast) hw_native_begin_checked_rom();
            serial(0xE000, 1);
            unsigned wait;
            for (wait = 0; wait < 100000; ++wait) {
                hw_native_flush();
                if (hw_native_interval_limit()) break;
                (void)cpu_read(0, CYC_DONE);
            }
            check(wait < 100000, "mapper probe reaches stable device window");
            uint64_t before = hw_native_interval_cycles;
            value[fast] = fast ? cpu_read_checked_rom(0x8E01, 0xC4, CYC_DONE) : cpu_read(0x8E01, CYC_DONE);
            hw_native_end();
            if (fast) check(hw_native_interval_cycles > before, "mapper probe uses cached native data read");
            machine[fast] = cyc_hw_state_hash(); memory[fast] = cyc_mem_state_hash();
        }
        check(value[0] == 0xEA && value[1] == value[0] && machine[0] == machine[1] && memory[0] == memory[1],
              "mapper change refreshes native data mapping with exact device behavior");
        cyc_snapshot_restore(snapshot);
    }
    unsigned mapper = hw_cart.mapper;
    hw_cart.mapper = 2;
    check(!cyc_native_prg_guard_available(), "unsupported board stays interpreted");
    hw_cart.mapper = 5;
    hw_cart.prg_off[0] |= MMC5_PRG_RAM;
    const uint8_t *previous = hw_native_interval.prg[0];
    hw_native_begin_checked_rom();
    check(!hw_native_interval.enabled && hw_native_interval.prg[0] == previous,
          "unsupported RAM/open-bus mapping never forms cached ROM pointers");
    hw_native_end();
    cyc_snapshot_restore(snapshot);
    hw_cart.mapper = mapper;
    cyc_snapshot_free(snapshot);
    /* Exhaustion is sticky even after a new image, rather than reusing a cache
     * generation after wrap. It is tested last, outside the compared route. */
    hw_prg_guard_epoch = UINT64_MAX;
    payload[0x100] ^= 1;
    check(cyc_mod_apply_cart_payload(payload, sizeof(payload)), "epoch exhaustion edit");
    check(!cyc_native_prg_guard_available(), "exhausted epoch fails closed");
    check(cyc_load_ines(image, sizeof(image)) && !cyc_native_prg_guard_available(), "reload cannot reuse exhausted epoch");
    fprintf(stderr, "PASS: native modified cycles=%" PRIu64 ", deadline cycles=%" PRIu64 "\n",
            modified_native, modified_intervals);
    return 0;
}
