#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "nes_runtime.h"
#include "hdpack.h"
CPU6502State g_cpu;
uint8_t g_ram[0x0800], g_sram[0x2000], g_chr_ram[0x2000], g_ppu_oam[0x100], g_ppu_pal[0x20], g_ppu_nt[0x1000];
int g_chr_is_rom;
uint8_t g_ppuctrl, g_ppumask, g_ppustatus, g_ppuscroll_x, g_ppuscroll_y;
uint8_t g_ppuscroll_x_hud, g_ppuscroll_y_hud, g_ppuctrl_hud;
int g_spr0_split_active, g_spr0_reads_ctr_legacy, g_spr0_predict_disable, g_predicted_spr0_scanline, g_spr0_split_write_scanline;
int g_render_width = 256, g_widescreen_left = 0, g_widescreen_right = 0;
int g_ws_eff_left = -1, g_ws_eff_right = -1, g_ws_oam_sidecar;
int16_t g_oam_x16[64];
uint64_t g_frame_count;
uint16_t g_ppuaddr;
int g_zapper_enabled;
static int s_mirroring;
void ppu_render_frame(uint32_t *framebuf);
int ppu_renderer_background_opaque(int x, int y);
uint8_t nes_read(uint16_t addr) { (void)addr; return 0; }
void nes_write(uint16_t addr, uint8_t val) { (void)addr; (void)val; }
uint8_t game_ram_read_hook(uint16_t pc, uint16_t addr, uint8_t val) { (void)pc; (void)addr; return val; }
void runtime_call_irq_handler(void) {}
void runtime_sync_scroll_from_v(void) {}
void runtime_sync_scroll_from_t(void) {}
void runtime_set_ppuaddr(uint16_t addr) { g_ppuaddr = addr; }
uint16_t runtime_get_ppu_t(void) { return 0; }
uint64_t runtime_get_ppu_v_write_epoch(void) { return 0; }
int runtime_scroll_from_t_valid(void) { return 0; }
int runtime_get_visible_frame_start(uint8_t *ctrl, uint8_t *sx, uint8_t *sy, uint16_t *t, uint64_t *frame) { (void)ctrl; (void)sx; (void)sy; (void)t; (void)frame; return 0; }
void runtime_record_frame_start_scroll(void) {}
int runtime_get_mapper_irq_split(int *scanline, uint8_t *ctrl_before, uint8_t *mask_before, uint8_t *sx_before, uint8_t *sy_before, uint8_t *ctrl_after, uint8_t *mask_after, uint8_t *sx_after, uint8_t *sy_after, uint64_t *frame) { (void)scanline; (void)ctrl_before; (void)mask_before; (void)sx_before; (void)sy_before; (void)ctrl_after; (void)mask_after; (void)sx_after; (void)sy_after; (void)frame; return 0; }
int mapper_get_mirroring(void) { return s_mirroring; }
int mapper_clock_scanline(void) { return 0; }
void save_png(const char *path, int w, int h, const void *rgb, int stride) { (void)path; (void)w; (void)h; (void)rgb; (void)stride; }
int hdpack_active(void) { return 0; }
int hdpack_scale(void) { return 1; }
HdPixel *hdpack_pixels(void) { return 0; }
int hdpack_recording(void) { return 0; }
void hdpack_frame_begin(void) {}
static double now_ms(void) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart; }
static uint32_t fnv1a(const void *data, size_t len) { const uint8_t *p = (const uint8_t *)data; uint32_t h = 2166136261u; for (size_t i=0;i<len;i++) h=(h^p[i])*16777619u; return h; }
static void init_state(void) {
    memset(g_chr_ram,0,sizeof(g_chr_ram)); memset(g_ppu_oam,0xF8,sizeof(g_ppu_oam)); memset(g_ppu_nt,0,sizeof(g_ppu_nt)); memset(g_ppu_pal,0,sizeof(g_ppu_pal)); memset(g_oam_x16,0,sizeof(g_oam_x16));
    for (int i=0;i<(int)sizeof(g_chr_ram);i++) g_chr_ram[i]=(uint8_t)((i*37+(i>>4))&0xFF);
    for (int i=0;i<(int)sizeof(g_ppu_nt);i++) g_ppu_nt[i]=(uint8_t)((i*11+(i>>2))&0xFF);
    for (int i=0;i<32;i++) g_ppu_pal[i]=(uint8_t)((i*3+1)&0x3F);
    g_ppuctrl=0x10; g_ppumask=0x1E; g_ppustatus=0; g_ppuscroll_x=5; g_ppuscroll_y=7; g_ppuctrl_hud=0; g_ppuscroll_x_hud=0; g_ppuscroll_y_hud=0; g_spr0_split_active=0; g_zapper_enabled=0; g_frame_count=10; g_ppuaddr=0; s_mirroring=2; g_render_width=256; g_widescreen_left=0; g_widescreen_right=0; g_ws_eff_left=-1; g_ws_eff_right=-1;
    g_ppu_oam[0]=20; g_ppu_oam[1]=4; g_ppu_oam[2]=0; g_ppu_oam[3]=30; g_ppu_oam[4]=31; g_ppu_oam[5]=6; g_ppu_oam[6]=0x21; g_ppu_oam[7]=44;
}
int main(void) {
    static uint32_t fb[256*240];
    const int frames = 7000;
    init_state(); for (int i=0;i<256*240;i++) fb[i]=0x12345678u;
    for (int i=0;i<40;i++) ppu_render_frame(fb);
    double t0=now_ms();
    for (int i=0;i<frames;i++) ppu_render_frame(fb);
    double elapsed=now_ms()-t0;
    printf("classic_only ms=%.3f hash=%08x opaque=%d\n", elapsed, fnv1a(fb,sizeof(fb)), ppu_renderer_background_opaque(30,20));
    return 0;
}
