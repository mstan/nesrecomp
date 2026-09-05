#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hdpack.h"
#include "nes_runtime.h"

CPU6502State g_cpu;
uint8_t g_ram[0x0800];
uint8_t g_sram[0x2000];
uint8_t g_chr_ram[0x2000];
uint8_t g_ppu_oam[0x100];
uint8_t g_ppu_pal[0x20];
uint8_t g_ppu_nt[0x1000];
int g_chr_is_rom;

uint8_t g_ppuctrl;
uint8_t g_ppumask;
uint8_t g_ppustatus;
uint8_t g_ppuscroll_x;
uint8_t g_ppuscroll_y;
uint8_t g_ppuscroll_x_hud;
uint8_t g_ppuscroll_y_hud;
uint8_t g_ppuctrl_hud;

int g_spr0_split_active;
int g_spr0_reads_ctr_legacy;
int g_spr0_predict_disable;
int g_predicted_spr0_scanline;
int g_spr0_split_write_scanline;
int g_render_width = 256;
int g_widescreen_left;
int g_widescreen_right;
int g_ws_eff_left = -1;
int g_ws_eff_right = -1;
int g_ws_oam_sidecar;
int16_t g_oam_x16[64];
uint64_t g_frame_count;
uint16_t g_ppuaddr;
int g_zapper_enabled;

static int s_mirroring;
static int s_irq_after;
static int s_irq_fire_line;
static int s_irq_line;
static int s_hd_record;
static HdPixel s_pixels[512 * 240];

void ppu_render_frame(uint32_t *framebuf);
int ppu_renderer_background_opaque(int x, int y);

uint8_t nes_read(uint16_t addr) {
    (void)addr;
    return 0;
}

void nes_write(uint16_t addr, uint8_t val) {
    (void)addr;
    (void)val;
}

uint8_t game_ram_read_hook(uint16_t pc, uint16_t addr, uint8_t val) {
    (void)pc;
    (void)addr;
    return val;
}

void runtime_call_irq_handler(void) {
    g_ppuctrl ^= 0x11;
    g_ppuscroll_x = (uint8_t)(g_ppuscroll_x + 3);
    g_ppuscroll_y = (uint8_t)(g_ppuscroll_y + 5);
    g_chr_ram[0x1000] ^= 0x55;
    g_ppu_pal[3] ^= 0x03;
}

void runtime_sync_scroll_from_v(void) {
}

void runtime_sync_scroll_from_t(void) {
}

void runtime_set_ppuaddr(uint16_t addr) {
    g_ppuaddr = addr;
}

uint16_t runtime_get_ppu_t(void) {
    return 0;
}

uint64_t runtime_get_ppu_v_write_epoch(void) {
    return 0;
}

int runtime_scroll_from_t_valid(void) {
    return 0;
}

int runtime_get_visible_frame_start(uint8_t *ctrl, uint8_t *sx, uint8_t *sy,
                                    uint16_t *t, uint64_t *frame) {
    (void)ctrl;
    (void)sx;
    (void)sy;
    (void)t;
    (void)frame;
    return 0;
}

void runtime_record_frame_start_scroll(void) {
}

int runtime_get_mapper_irq_split(int *scanline, uint8_t *ctrl_before,
                                 uint8_t *mask_before, uint8_t *sx_before,
                                 uint8_t *sy_before, uint8_t *ctrl_after,
                                 uint8_t *mask_after, uint8_t *sx_after,
                                 uint8_t *sy_after, uint64_t *frame) {
    (void)scanline;
    (void)ctrl_before;
    (void)mask_before;
    (void)sx_before;
    (void)sy_before;
    (void)ctrl_after;
    (void)mask_after;
    (void)sx_after;
    (void)sy_after;
    (void)frame;
    return 0;
}

int mapper_get_mirroring(void) {
    return s_mirroring;
}

int mapper_clock_scanline(void) {
    return s_irq_after && s_irq_line++ == s_irq_fire_line;
}

void save_png(const char *path, int w, int h, const void *rgb, int stride) {
    (void)path;
    (void)w;
    (void)h;
    (void)rgb;
    (void)stride;
}

int hdpack_active(void) {
    return s_hd_record;
}

int hdpack_scale(void) {
    return 1;
}

HdPixel *hdpack_pixels(void) {
    return s_hd_record ? s_pixels : NULL;
}

int hdpack_recording(void) {
    return s_hd_record;
}

void hdpack_frame_begin(void) {
    memset(s_pixels, 0, sizeof(s_pixels));
}

static uint32_t fnv1a_update(uint32_t h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t fnv1a(const void *data, size_t len) {
    return fnv1a_update(2166136261u, data, len);
}

static uint32_t hash_u8(uint32_t h, uint8_t v) {
    return fnv1a_update(h, &v, sizeof(v));
}

static uint32_t hash_u32le(uint32_t h, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v & 0xFFu),
        (uint8_t)((v >> 8) & 0xFFu),
        (uint8_t)((v >> 16) & 0xFFu),
        (uint8_t)((v >> 24) & 0xFFu)
    };
    return fnv1a_update(h, b, sizeof(b));
}

static uint32_t hash_i32le(uint32_t h, int32_t v) {
    return hash_u32le(h, (uint32_t)v);
}

static uint32_t hash_chr16(uint32_t h, const uint8_t *p) {
    uint8_t present = p ? 1u : 0u;
    h = hash_u8(h, present);
    if (p)
        h = fnv1a_update(h, p, 16);
    return h;
}

static uint32_t hp_sem_hash(int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        HdPixel *p = &s_pixels[i];
        h = hash_i32le(h, p->bg_index);
        h = hash_chr16(h, p->bg_t16);
        h = hash_u8(h, p->bg_p0);
        h = hash_u8(h, p->bg_p1);
        h = hash_u8(h, p->bg_p2);
        h = hash_u8(h, p->bg_p3);
        h = hash_u8(h, p->bg_ox);
        h = hash_u8(h, p->bg_oy);
        h = hash_u8(h, p->bg_has);
        h = hash_u32le(h, p->bg_argb);
        h = hash_u32le(h, p->backdrop);
        h = hash_i32le(h, p->sp_index);
        h = hash_chr16(h, p->sp_t16);
        h = hash_u8(h, p->sp_p1);
        h = hash_u8(h, p->sp_p2);
        h = hash_u8(h, p->sp_p3);
        h = hash_u8(h, p->sp_ox);
        h = hash_u8(h, p->sp_oy);
        h = hash_u8(h, p->sp_hm);
        h = hash_u8(h, p->sp_vm);
        h = hash_u8(h, p->sp_has);
        h = hash_u32le(h, p->sp_argb);
    }
    return h;
}

static void init_state(void) {
    memset(g_ram, 0, sizeof(g_ram));
    memset(g_sram, 0, sizeof(g_sram));
    memset(g_chr_ram, 0, sizeof(g_chr_ram));
    memset(g_ppu_oam, 0xF8, sizeof(g_ppu_oam));
    memset(g_ppu_nt, 0, sizeof(g_ppu_nt));
    memset(g_ppu_pal, 0, sizeof(g_ppu_pal));
    memset(g_oam_x16, 0, sizeof(g_oam_x16));
    memset(s_pixels, 0, sizeof(s_pixels));
    for (int i = 0; i < (int)sizeof(g_chr_ram); i++)
        g_chr_ram[i] = (uint8_t)((i * 37 + (i >> 4)) & 0xFF);
    for (int i = 0; i < (int)sizeof(g_ppu_nt); i++)
        g_ppu_nt[i] = (uint8_t)((i * 11 + (i >> 2)) & 0xFF);
    for (int i = 0; i < 32; i++)
        g_ppu_pal[i] = (uint8_t)((i * 3 + 1) & 0x3F);
    g_ppuctrl = 0x10;
    g_ppumask = 0x1E;
    g_ppustatus = 0;
    g_ppuscroll_x = 5;
    g_ppuscroll_y = 7;
    g_ppuctrl_hud = 0;
    g_ppuscroll_x_hud = 0;
    g_ppuscroll_y_hud = 0;
    g_spr0_split_active = 0;
    g_zapper_enabled = 0;
    g_ws_oam_sidecar = 0;
    g_frame_count = 10;
    g_ppuaddr = 0;
    s_mirroring = 2;
    s_irq_after = 0;
    s_irq_line = 0;
    s_irq_fire_line = 0;
    s_hd_record = 0;
    g_render_width = 256;
    g_widescreen_left = 0;
    g_widescreen_right = 0;
    g_ws_eff_left = -1;
    g_ws_eff_right = -1;
    g_ppu_oam[0] = 20;
    g_ppu_oam[1] = 4;
    g_ppu_oam[2] = 0;
    g_ppu_oam[3] = 30;
    g_ppu_oam[4] = 31;
    g_ppu_oam[5] = 6;
    g_ppu_oam[6] = 0x21;
    g_ppu_oam[7] = 44;
}

typedef struct {
    const char *name;
    uint32_t fb;
    uint32_t opaque;
    uint32_t hd;
    uint8_t status;
} CaseExpect;

static int check_case(const CaseExpect *expect, uint32_t *fb) {
    uint8_t opaque[512 * 240];
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < g_render_width; x++)
            opaque[y * g_render_width + x] =
                (uint8_t)ppu_renderer_background_opaque(x, y);
    }

    uint32_t fb_hash = fnv1a(fb, (size_t)g_render_width * 240u * 4u);
    uint32_t op_hash = fnv1a(opaque, (size_t)g_render_width * 240u);
    uint32_t hd_hash = hp_sem_hash(g_render_width * 240);

    if (fb_hash != expect->fb || op_hash != expect->opaque ||
        hd_hash != expect->hd || g_ppustatus != expect->status) {
        fprintf(stderr,
                "%s mismatch: fb=%08x/%08x opaque=%08x/%08x hd=%08x/%08x status=%02x/%02x\n",
                expect->name, fb_hash, expect->fb, op_hash, expect->opaque,
                hd_hash, expect->hd, g_ppustatus, expect->status);
        return 1;
    }
    return 0;
}

int main(void) {
    uint32_t fb[512 * 240];
    for (int i = 0; i < 512 * 240; i++)
        fb[i] = 0x12345678u;

    const CaseExpect vanilla = {
        "vanilla", 0xc63bd627u, 0xd48cd6f3u, 0xa2185dc5u, 0x40u
    };
    init_state();
    ppu_render_frame(fb);
    if (check_case(&vanilla, fb))
        return 1;

    const CaseExpect sprites_only = {
        "sprites_only_after_bg", 0xc471d87fu, 0xb87d5dc5u, 0xa2185dc5u, 0x40u
    };
    g_ppumask = 0x10;
    ppu_render_frame(fb);
    if (check_case(&sprites_only, fb))
        return 1;

    const CaseExpect retained_off = {
        "retained_off", 0xc471d87fu, 0xb87d5dc5u, 0xa2185dc5u, 0x40u
    };
    g_ppumask = 0x00;
    ppu_render_frame(fb);
    if (check_case(&retained_off, fb))
        return 1;

    const CaseExpect pillarbox = {
        "widescreen_pillarbox", 0x37cf5627u, 0x4eac2ef3u, 0x93141dc5u, 0x40u
    };
    init_state();
    g_render_width = 512;
    g_widescreen_left = 128;
    g_widescreen_right = 128;
    g_ws_eff_left = 0;
    g_ws_eff_right = 0;
    ppu_render_frame(fb);
    if (check_case(&pillarbox, fb))
        return 1;

    const CaseExpect widescreen_full = {
        "widescreen_full", 0xbb51dff3u, 0x28357ee9u, 0x93141dc5u, 0x40u
    };
    init_state();
    g_render_width = 512;
    g_widescreen_left = 128;
    g_widescreen_right = 128;
    g_ws_eff_left = 128;
    g_ws_eff_right = 128;
    ppu_render_frame(fb);
    if (check_case(&widescreen_full, fb))
        return 1;

    const CaseExpect irq_split = {
        "irq_split", 0x2c6d6f41u, 0xccc048bcu, 0xa2185dc5u, 0x40u
    };
    init_state();
    s_irq_after = 1;
    s_irq_fire_line = 23;
    ppu_render_frame(fb);
    if (check_case(&irq_split, fb))
        return 1;

    const CaseExpect hdpack_meta = {
        "hdpack_meta", 0xc63bd627u, 0xd48cd6f3u, 0xdc1b1addu, 0x40u
    };
    init_state();
    s_hd_record = 1;
    ppu_render_frame(fb);
    if (check_case(&hdpack_meta, fb))
        return 1;

    return 0;
}
