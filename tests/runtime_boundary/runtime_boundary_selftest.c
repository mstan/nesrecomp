#include "nes_runtime.h"
#include "mapper.h"
#include "interp.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_test_mapper_type = 0;
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
    (void)addr; (void)caller_bank; return 0;
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

int main(void) {
    test_projected_boundary_skips_resume_tick();
    test_unprojected_boundary_skips_resume_tick();
    puts("runtime boundary self-test passed");
    return 0;
}
