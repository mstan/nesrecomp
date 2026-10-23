#include "nes_runtime.h"
#include "mapper.h"
#include "interp.h"

#undef NDEBUG
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_test_mapper_type = 0;
static jmp_buf s_discarded_tail;
static int s_tail_test_mode, s_tail_test_calls;
enum { TAIL_COUNT, TAIL_DISCARD, TAIL_CYCLE, TAIL_NESTED_CALL, TAIL_JSR_LAP };
static uint8_t s_lap_before_s;
static uint64_t s_lap_epoch;
static int s_lap_guard_ok, s_lap_inline_data;
uint16_t g_rts_target = 0;
uint16_t g_rti_target = 0;
uint16_t g_rti_source = 0;
int g_rti_bank = -1;
int g_current_bank = 0;
const char *g_last_recomp_func = NULL;
char g_exe_dir[260] = ".";
int g_dot_ppu_on = 0;
int g_render_width = 256;
int g_ws_oam_sidecar = 0;
int g_ws_obj_delta_min = -128;
int g_ws_obj_delta_max = 127;
int16_t g_oam_x16[64];
int16_t g_ws_shadow_x16[64];
int16_t g_ws_obj_true_rel = 0;
uint8_t g_ws_obj_rel8 = 0;
uint8_t g_ws_obj_ctx_valid = 0;
int g_mmc3_win_bank8k[4] = {0, 0, 0, 0};
const char *g_recomp_stack[1];
int g_recomp_stack_top = 0;

void apu_init(void) {}
void apu_clock_cycles(int cycles) { (void)cycles; }
int apu_take_dmc_stall(void) { return 0; }
int apu_irq_asserted(void) { return 0; }
int apu_get_state_blob(uint8_t *buf, int cap) { (void)buf; (void)cap; return 0; }
uint8_t apu_read_status(void) { return 0; }
void apu_write(uint16_t addr, uint8_t val) { (void)addr; (void)val; }
void mapper_clock_cpu(int cycles) { (void)cycles; }
int mapper_irq_asserted(void) { return 0; }
int mapper_get_type(void) { return g_test_mapper_type; }
void mapper_get_state(MapperState *out) { memset(out, 0, sizeof(*out)); }
const uint8_t *mapper_get_switchable_bank(void) { return NULL; }
const uint8_t *mapper_get_fixed_bank(void) { return NULL; }
uint8_t mapper_peek_prg(uint16_t addr) { (void)addr; return 0; }
void mapper_write(uint16_t addr, uint8_t val) { (void)addr; (void)val; }
int mapper_get_mirroring(void) { return 3; }
int mapper_is_chr_ram(void) { return 1; }
void save_ram_mark_dirty(void) {}
void nes_foreign_trace_init_dump(void) {}
void nes_interp_reset_context(void) {}
void nes_interp_frame_boundary(void) {}
void nes_interp_get_stats(NesInterpStats *out) { memset(out, 0, sizeof(*out)); }
int nes_interp_get_hotspots(NesInterpHotspot *out, int cap, int clear_period) {
    (void)out; (void)cap; (void)clear_period; return 0;
}
void nes_vblank_callback(void) {}
void func_IRQ(void) {}
int call_by_address(uint16_t addr) { (void)addr; return 0; }
int call_by_address_cb(uint16_t addr, int caller_bank) {
    if (addr != 0xC531) return 0;
    s_tail_test_calls++;
    if (s_tail_test_mode == TAIL_DISCARD)
        longjmp(s_discarded_tail, 1);
    if (s_tail_test_calls == 1) {
        if (s_tail_test_mode == TAIL_CYCLE)
            call_by_address_tail(addr, caller_bank);
        else if (s_tail_test_mode == TAIL_NESTED_CALL) {
            g_cpu.S -= 2;
            call_by_address_tail(addr, caller_bank);
            g_cpu.S += 2;
        } else if (s_tail_test_mode == TAIL_JSR_LAP) {
            /* Generated JSR site pushed its return; an indexed dispatcher
             * callee pops that operand (net-zero S) and tail-jumps back
             * into the active loop, which defers as a cycle lap. The JSR
             * guard must then reject the equal-S "return" so the pending
             * lap unwinds to the ancestor instead of falling through into
             * inline table bytes. */
            g_ram[0x100 + g_cpu.S--] = 0xC5;
            g_ram[0x100 + g_cpu.S--] = 0x31;
            g_cpu.S += 2;
            call_by_address_tail(addr, caller_bank);
            s_lap_guard_ok = nes_jsr_stack_ok_after_call(s_lap_before_s, s_lap_epoch);
            if (s_lap_guard_ok)
                s_lap_inline_data++;  /* fallthrough would execute table data */
        }
    }
    return 1;
}
int game_dispatch_override(uint16_t addr) { (void)addr; return 0; }
uint8_t game_ram_read_hook(uint16_t pc, uint16_t addr, uint8_t value) {
    (void)pc; (void)addr; return value;
}
void debug_server_request_pause(const char *reason) { (void)reason; }
void ppu_trace_write(uint16_t reg, uint8_t val) { (void)reg; (void)val; }
void ppu_dot_advance(uint32_t ops) { (void)ops; }
int chr_override_active(void) { return 0; }
void chr_override_on_ppuaddr(uint16_t new_addr) { (void)new_addr; }
void chr_override_on_chr_write(uint16_t addr, uint8_t val) { (void)addr; (void)val; }
int ppu_predict_spr0_hit_scanline(void) { return 240; }

static void test_projected_boundary_skips_resume_tick(void) {
    g_nes_cycles = 100;
    g_test_mapper_type = 4;
    g_code_window_base = 0xA000;
    runtime_prepare_guest_resume(0xA123, 1);

    nes_instruction_boundary(0x8123, 7);

    uint16_t pc = 0;
    int tick_charged = 0;
    assert(runtime_get_savestate_resume(&pc, &tick_charged));
    assert(pc == 0xA123);
    assert(tick_charged == 1);
    assert(g_nes_cycles == 100);

    nes_instruction_boundary(0x8123, 7);
    assert(g_nes_cycles == 107);
}

static void test_unprojected_boundary_skips_resume_tick(void) {
    g_nes_cycles = 200;
    g_test_mapper_type = 66;
    g_code_window_base = 0x8000;
    runtime_prepare_guest_resume(0xC123, 1);

    nes_instruction_boundary(0xC123, 5);

    uint16_t pc = 0;
    int tick_charged = 0;
    assert(runtime_get_savestate_resume(&pc, &tick_charged));
    assert(pc == 0xC123);
    assert(tick_charged == 1);
    assert(g_nes_cycles == 200);

    nes_instruction_boundary(0xC123, 5);
    assert(g_nes_cycles == 205);
}

static void test_ppumask_preserves_native_bits(void) {
    for (int enabled = 0; enabled <= 1; enabled++) {
        g_ppumask_translate = (uint8_t)enabled;
        for (int mask = 0; mask <= 255; mask++) {
            ppu_write_reg(0x2001, (uint8_t)mask);
            assert(g_ppumask == (uint8_t)mask);
        }
    }
    g_ppumask_translate = 0;
}

static void test_nested_nmi_flag_is_opt_in(void) {
    const int addresses[] = {-1, 0x33, 0x800};
    for (unsigned i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        memset(g_ram, 0x5A, sizeof(g_ram));
        g_nested_nmi_extra_flag_addr = addresses[i];
        nes_resolve_nested_nmi_spin_flags();
        for (int addr = 0; addr < 0x800; addr++) {
            int written = addr == 0x1A || addr == 0x20 ||
                (addresses[i] == 0x33 && addr == 0x33);
            assert(g_ram[addr] == (written ? 1 : 0x5A));
        }
    }
    g_nested_nmi_extra_flag_addr = -1;
}

static int test_live_tail_cycles_still_iterate(void) {
    runtime_prepare_guest_resume(0xC0CC, 0);
    g_cpu.S = 0xFD;
    s_tail_test_mode = TAIL_CYCLE;
    s_tail_test_calls = 0;
    call_by_address_tail(0xC531, -1);
    if (s_tail_test_calls != 2) {
        fprintf(stderr, "live tail cycle dispatched %d times, expected 2\n", s_tail_test_calls);
        return 0;
    }
    s_tail_test_mode = TAIL_NESTED_CALL;
    s_tail_test_calls = 0;
    call_by_address_tail(0xC531, -1);
    if (s_tail_test_calls != 2 || g_cpu.S != 0xFD) {
        fputs("different guest stack was collapsed into a tail cycle\n", stderr);
        return 0;
    }
    return 1;
}

static int test_resume_discards_abandoned_tail_ancestor(void) {
    g_cpu.S = 0xFD;
    s_tail_test_calls = 0;
    s_tail_test_mode = TAIL_DISCARD;
    if (setjmp(s_discarded_tail) == 0) {
        /* Model the runner longjmp that discards a native C call chain
         * after loading a state. The tail dispatcher cannot run cleanup. */
        call_by_address_tail(0xC531, -1);
        return 0;
    }
    s_tail_test_mode = TAIL_COUNT;
    runtime_prepare_guest_resume(0xC0CC, 1);
    call_by_address_tail(0xC531, -1);
    if (s_tail_test_calls != 2) {
        fprintf(stderr, "resumed transfer dispatched %d times, expected 2; stale tail ancestor\n", s_tail_test_calls);
        return 0;
    }
    return 1;
}

static int test_pending_tail_lap_unwinds_jsr_caller(void) {
    runtime_prepare_guest_resume(0xC0CC, 0);
    g_cpu.S = 0xFF;
    s_tail_test_mode = TAIL_JSR_LAP;
    s_tail_test_calls = 0;
    s_lap_before_s = g_cpu.S;
    s_lap_epoch = runtime_get_interrupt_epoch();
    s_lap_guard_ok = s_lap_inline_data = 0;
    /* Ancestor tail frame; its dispatch defers the lap, then the pending
     * lap must unwind back and be consumed by this same frame. */
    call_by_address_tail(0xC531, -1);
    if (s_lap_guard_ok != 0) {
        fputs("JSR guard accepted a return with a pending tail lap\n", stderr);
        return 0;
    }
    if (s_lap_inline_data != 0) {
        fputs("JSR caller fell through over a pending tail lap\n", stderr);
        return 0;
    }
    if (s_tail_test_calls != 2 || g_cpu.S != 0xFF) {
        fprintf(stderr, "pending lap unwound wrong: calls=%d S=$%02X\n",
                s_tail_test_calls, g_cpu.S);
        return 0;
    }
    /* Ordinary equal-S returns must stay accepted once the lap is consumed. */
    if (!nes_jsr_stack_ok_after_call(g_cpu.S, runtime_get_interrupt_epoch())) {
        fputs("JSR guard rejects ordinary equal-S return after the lap\n", stderr);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--tail-resume") == 0) {
        if (!test_live_tail_cycles_still_iterate() ||
            !test_resume_discards_abandoned_tail_ancestor() ||
            !test_pending_tail_lap_unwinds_jsr_caller())
            return 1;
        puts("runtime tail resume self-test passed");
        return 0;
    }
    test_projected_boundary_skips_resume_tick();
    test_unprojected_boundary_skips_resume_tick();
    if (!test_live_tail_cycles_still_iterate() ||
        !test_resume_discards_abandoned_tail_ancestor() ||
        !test_pending_tail_lap_unwinds_jsr_caller())
        return 1;
    test_ppumask_preserves_native_bits();
    test_nested_nmi_flag_is_opt_in();
    puts("runtime boundary self-test passed");
    return 0;
}
