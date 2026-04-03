/*
 * main_runner.c — SDL2 window, VBlank-callback NMI, PNG screenshot save
 *
 * NES architecture: RESET never returns (it IS the main game loop).
 * NMI fires asynchronously every VBlank. We simulate this by hooking
 * ppu_read_reg($2002): when the game reads the VBlank flag, we call
 * nes_vblank_callback() which runs func_NMI() + renders the frame.
 *
 * Usage: NESRecompGame.exe <baserom.nes>
 * - 768x720 window (256x240 NES, 3x scale)
 * - PNG screenshot every 60 frames (rotating 01..10)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <SDL.h>
#include "nes_runtime.h"
#include "input_script.h"
#include "savestate.h"
#include "logger.h"
#include "apu.h"
#include "mapper.h"
#include "game_extras.h"
#include "debug_server.h"
#include "keybinds.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* ---- Script / record / savestate paths (set from CLI) ---- */
static const char *s_script_path    = NULL;
static const char *s_record_path    = NULL;
static const char *s_loadstate_path = NULL;

/* ---- SDL state (file-level so nes_vblank_callback can access) ---- */
static SDL_Window        *s_window    = NULL;
static SDL_Renderer      *s_renderer  = NULL;
static SDL_Texture       *s_texture   = NULL;
static uint32_t           s_framebuf[256 * 240];

/* ---- OAM debug window (--debug flag) ---- */
static int                s_debug         = 0;
static SDL_Window        *s_dbg_window    = NULL;
static SDL_Renderer      *s_dbg_renderer  = NULL;
static SDL_Texture       *s_dbg_texture   = NULL;
static uint32_t           s_dbg_buf[256 * 256];

/* ---- RAM Watch Window ----
 * Shows tracked RAM addresses as large human-readable cards.
 * Uses direct SDL_RenderFillRect — no pixel buffer, no coordinate flip risk.
 * Add entries with watch_add() before the game loop starts. */
#define WATCH_W          560
#define WATCH_ENTRY_H     90
#define MAX_WATCH         8

typedef struct {
    uint16_t   addr;
    const char *label;
    uint8_t    last_val;
    int        flash;     /* countdown in frames; >0 = recently changed */
    int        dirty;     /* needs redraw */
} WatchEntry;

static WatchEntry    s_watch[MAX_WATCH];
static int           s_watch_count     = 0;
static SDL_Window   *s_watch_window    = NULL;
static SDL_Renderer *s_watch_renderer  = NULL;

static void watch_add(uint16_t addr, const char *label) {
    if (s_watch_count >= MAX_WATCH) return;
    s_watch[s_watch_count++] = (WatchEntry){ addr, label, 0xFF, 0, 1 };
}

/* ---- 8x8 Bitmap Font (ASCII 0x20–0x7F) ---- */
static const uint8_t s_font8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 20   */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 21 ! */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* 22 " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* 23 # */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* 24 $ */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* 25 % */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* 26 & */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* 27 ' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* 28 ( */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* 29 ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 2A * */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* 2B + */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* 2C , */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* 2D - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* 2E . */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* 2F / */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 30 0 */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 31 1 */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 32 2 */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 33 3 */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 34 4 */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 35 5 */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 36 6 */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 37 7 */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 38 8 */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 39 9 */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* 3A : */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* 3B ; */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* 3C < */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* 3D = */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 3E > */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* 3F ? */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* 40 @ */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* 41 A */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* 42 B */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* 43 C */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* 44 D */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* 45 E */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* 46 F */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* 47 G */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* 48 H */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 49 I */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* 4A J */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* 4B K */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* 4C L */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* 4D M */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* 4E N */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* 4F O */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* 50 P */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* 51 Q */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* 52 R */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* 53 S */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 54 T */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* 55 U */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 56 V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 57 W */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* 58 X */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* 59 Y */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* 5A Z */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* 5B [ */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* 5C \ */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* 5D ] */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* 5E ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 5F _ */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* 60 ` */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* 61 a */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* 62 b */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* 63 c */
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, /* 64 d */
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, /* 65 e */
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, /* 66 f */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* 67 g */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* 68 h */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* 69 i */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* 6A j */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* 6B k */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 6C l */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* 6D m */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* 6E n */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* 6F o */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* 70 p */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* 71 q */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* 72 r */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* 73 s */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* 74 t */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* 75 u */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 76 v */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* 77 w */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* 78 x */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* 79 y */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* 7A z */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* 7B { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 7C | */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* 7D } */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* 7E ~ */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* 7F   */
};

/* Draw one character directly via SDL_RenderFillRect. font8x8_basic: bit0 = leftmost pixel. */
static void watch_draw_char(int x, int y, char c, int sc, uint8_t r, uint8_t g, uint8_t b) {
    if (c < 0x20 || c > 0x7F) c = '?';
    const uint8_t *glyph = s_font8[(uint8_t)c - 0x20];
    SDL_SetRenderDrawColor(s_watch_renderer, r, g, b, 255);
    for (int gy = 0; gy < 8; gy++)
        for (int gx = 0; gx < 8; gx++)
            if (glyph[gy] & (1 << gx)) {
                SDL_Rect px = { x + gx*sc, y + gy*sc, sc, sc };
                SDL_RenderFillRect(s_watch_renderer, &px);
            }
}

static void watch_draw_str(int x, int y, const char *str, int sc, uint8_t r, uint8_t g, uint8_t b) {
    for (; *str; str++, x += 8 * sc)
        watch_draw_char(x, y, *str, sc, r, g, b);
}

static void watch_render_frame(void) {
    if (!s_watch_window || !s_watch_renderer || s_watch_count == 0) return;

    /* Check for changes */
    static uint32_t s_last_miss_count = 0xFFFFFFFF;
    int any_dirty = (g_miss_count_any != s_last_miss_count);
    s_last_miss_count = g_miss_count_any;
    for (int i = 0; i < s_watch_count; i++) {
        WatchEntry *e = &s_watch[i];
        uint8_t cur = g_ram[e->addr];
        if (cur != e->last_val) { e->last_val = cur; e->flash = 45; e->dirty = 1; }
        else if (e->flash > 0)  { e->flash--;                        e->dirty = 1; }
        if (e->dirty) any_dirty = 1;
    }
    if (!any_dirty) return;

    SDL_SetRenderDrawColor(s_watch_renderer, 10, 10, 20, 255);
    SDL_RenderClear(s_watch_renderer);

    for (int i = 0; i < s_watch_count; i++) {
        WatchEntry *e = &s_watch[i];
        e->dirty = 0;
        int oy = 6 + i * WATCH_ENTRY_H;
        int flashing = (e->flash > 0);

        /* Card background */
        SDL_SetRenderDrawColor(s_watch_renderer,
            flashing ? 20 : 15, flashing ? 30 : 15, flashing ? 16 : 26, 255);
        SDL_Rect card = { 8, oy, WATCH_W - 16, WATCH_ENTRY_H - 4 };
        SDL_RenderFillRect(s_watch_renderer, &card);

        /* Left accent bar */
        SDL_SetRenderDrawColor(s_watch_renderer,
            flashing ? 0x44 : 0x22, flashing ? 0xFF : 0x33, flashing ? 0x44 : 0x66, 255);
        SDL_Rect bar = { 8, oy, 6, WATCH_ENTRY_H - 4 };
        SDL_RenderFillRect(s_watch_renderer, &bar);

        /* Value: 2-digit hex, scale 4 */
        char val_str[8];
        snprintf(val_str, sizeof(val_str), "%02X", e->last_val);
        watch_draw_str(20, oy + 6, val_str, 4,
            flashing ? 0x88 : 0x00, 0xFF, flashing ? 0x88 : 0x99);

        /* Address, scale 2 */
        char addr_str[8];
        snprintf(addr_str, sizeof(addr_str), "$%04X", e->addr);
        watch_draw_str(110, oy + 14, addr_str, 2, 0x44, 0x55, 0x77);

        /* Description, scale 2 */
        watch_draw_str(20, oy + 52, e->label, 2, 0x99, 0xAA, 0xCC);

        /* Separator */
        SDL_SetRenderDrawColor(s_watch_renderer, 0x1A, 0x1A, 0x2A, 255);
        SDL_Rect sep = { 8, oy + WATCH_ENTRY_H - 5, WATCH_W - 16, 1 };
        SDL_RenderFillRect(s_watch_renderer, &sep);
    }

    /* Dispatch miss monitor — unique address list */
    {
        int oy = 6 + s_watch_count * WATCH_ENTRY_H;

        /* Header */
        SDL_SetRenderDrawColor(s_watch_renderer, 0x22, 0x08, 0x08, 255);
        SDL_Rect hdr = { 8, oy, WATCH_W - 16, 18 };
        SDL_RenderFillRect(s_watch_renderer, &hdr);
        char hdr_str[40];
        snprintf(hdr_str, sizeof(hdr_str), "DISPATCH MISSES  total:%u", g_miss_count_any);
        watch_draw_str(14, oy + 4, hdr_str, 1, 0x88, 0x44, 0x44);

        /* One row per unique missed address */
        int row_h = 18;
        int n = g_miss_unique_count;
        SDL_SetRenderDrawColor(s_watch_renderer, 15, 10, 10, 255);
        SDL_Rect body = { 8, oy + 20, WATCH_W - 16, (n > 0 ? n : 1) * row_h + 4 };
        SDL_RenderFillRect(s_watch_renderer, &body);

        if (n == 0) {
            watch_draw_str(20, oy + 24, "none", 1, 0x44, 0x44, 0x44);
        } else {
            for (int i = 0; i < n; i++) {
                int is_last = (g_miss_unique_addrs[i] == g_miss_last_addr);
                char row[24];
                snprintf(row, sizeof(row), "$%04X", g_miss_unique_addrs[i]);
                int rx = 20 + (i % 3) * 120;
                int ry = oy + 24 + (i / 3) * row_h;
                watch_draw_str(rx, ry, row, 2,
                    is_last ? 0xFF : 0xAA,
                    is_last ? 0x66 : 0x55,
                    is_last ? 0x22 : 0x55);
            }
        }
    }

    SDL_RenderPresent(s_watch_renderer);
}

/* ---- Audio state ---- */
static SDL_AudioDeviceID  s_audio_dev = 0;
#define AUDIO_SAMPLES_PER_FRAME 735
static int16_t            s_audio_frame[AUDIO_SAMPLES_PER_FRAME];

/* ---- Screenshot ---- */
/* Non-static wrapper so ppu_renderer.c can save PNGs */
void save_png(const char *path, int w, int h, const void *rgb, int stride) {
    stbi_write_png(path, w, h, 3, rgb, stride);
}

/* Save an ARGB8888 buffer as PNG. Callable from game code (emulated mode screenshots). */
void runner_save_argb_png(const char *path, const uint32_t *argb, int w, int h) {
    static uint8_t rgb[256 * 240 * 3];
    int pixels = w * h;
    for (int i = 0; i < pixels; i++) {
        uint32_t px = argb[i];
        rgb[i*3+0] = (px >> 16) & 0xFF;
        rgb[i*3+1] = (px >>  8) & 0xFF;
        rgb[i*3+2] = (px      ) & 0xFF;
    }
    stbi_write_png(path, w, h, 3, rgb, w * 3);
}

static void save_screenshot(void) {
    char path[128];
    snprintf(path, sizeof(path), "C:/temp/mm3_shot_%04llu.png",
             (unsigned long long)g_frame_count);
    static uint8_t rgb[256 * 240 * 3];
    for (int i = 0; i < 256 * 240; i++) {
        uint32_t px = s_framebuf[i];
        rgb[i*3+0] = (px >> 16) & 0xFF;
        rgb[i*3+1] = (px >>  8) & 0xFF;
        rgb[i*3+2] = (px      ) & 0xFF;
    }
    stbi_write_png(path, 256, 240, 3, rgb, 256*3);
    printf("[Shot] Saved %s\n", path);
}

/* ---- Debug trace log (C:/temp/debug_trace.txt) ---- */
static FILE *s_debug_log = NULL;

static void debug_log_open(void) {
    s_debug_log = fopen("C:/temp/debug_trace.txt", "w");
    if (s_debug_log) {
        fprintf(s_debug_log, "FRAME,slot,Y,tile,attr,X\n");
        fflush(s_debug_log);
    }
}

/* One-time dump of CHR tiles A0-C0 + all 32 palette bytes when item sprites appear */
static int s_chr_dumped = 0;
static void chr_dump_once(uint64_t frame) {
    if (s_chr_dumped) return;
    s_chr_dumped = 1;
    FILE *f = fopen("C:/temp/chr_dump.txt", "w");
    if (!f) return;
    /* Palette dump */
    fprintf(f, "PALETTE:");
    for (int i = 0; i < 32; i++) fprintf(f, " %02X", g_ppu_pal[i]);
    fprintf(f, "  (at frame %llu)\n", (unsigned long long)frame);
    /* CHR tile dump */
    fprintf(f, "CHR_ADDR,B0,B1,B2,B3,B4,B5,B6,B7,B8,B9,BA,BB,BC,BD,BE,BF\n");
    for (int tile = 0xA0; tile <= 0xC0; tile++) {
        int addr = tile * 16;
        fprintf(f, "%04X", addr);
        for (int b = 0; b < 16; b++) fprintf(f, ",%02X", g_chr_ram[addr+b]);
        fprintf(f, "\n");
    }
    fclose(f);
    printf("[CHR] Dumped CHR+PAL at frame %llu\n", (unsigned long long)frame);
}

/* Call at NMI time to log one line per frame. Comment out when not debugging. */
static void debug_log_frame(uint64_t frame) {
    if (!s_debug_log) return;
    /* Log all visible OAM entries every frame: FRAME,slot,Y,tile,attr,X */
    for (int s = 0; s < 64; s++) {
        uint8_t sy = g_ppu_oam[s*4+0];
        if (sy < 240) {
            fprintf(s_debug_log, "%llu,%d,%02X,%02X,%02X,%02X\n",
                    (unsigned long long)frame, s,
                    sy, g_ppu_oam[s*4+1],
                    g_ppu_oam[s*4+2], g_ppu_oam[s*4+3]);
        }
    }
    fflush(s_debug_log);
}

/* ---- VBlank callback (called from ppu_read_reg when $2002 bit7 fires) ---- */
void nes_vblank_callback(void) {
    static uint64_t s_cb_count = 0;
    if (s_cb_count == 0) { /* debug_log_open(); */ }
    s_cb_count++;
    if (s_debug && (s_cb_count <= 100 || s_cb_count % 60 == 0))
        printf("[VBlank] callback #%llu frame=%llu\n",
               (unsigned long long)s_cb_count, (unsigned long long)g_frame_count);

    /* Handle SDL events */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) exit(0);
        if (ev.type == SDL_WINDOWEVENT &&
            ev.window.event == SDL_WINDOWEVENT_CLOSE) exit(0);
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) exit(0);
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F5)
            g_turbo ^= 1;
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F6)
            savestate_save("C:/temp/quicksave.sav");
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F7) {
            record_loadstate(g_frame_count, "C:/temp/quicksave.sav");
            savestate_load("C:/temp/quicksave.sav");
            record_sync_frame(g_frame_count); /* g_frame_count now = restored value */
        }
    }

    /* Update controllers from keyboard state via configurable keybinds */
    {
        const uint8_t *keys = SDL_GetKeyboardState(NULL);
        uint8_t btn = keybinds_read_player(keys, 1);

        /* Recording: capture keyboard state before script override */
        record_tick(g_frame_count, btn, g_turbo);

        /* Script override: if a script is loaded, use its button state */
        int sp = script_get_buttons();
        if (sp >= 0) btn = (uint8_t)sp;

        /* TCP debug server override: set_input command */
        int tcp_btn = debug_server_get_input_override();
        if (tcp_btn >= 0) btn = (uint8_t)tcp_btn;

        g_controller1_buttons = btn;
        g_controller2_buttons = keybinds_read_player(keys, 2);
    }

    /* Per-frame script execution */
    script_tick(g_frame_count, g_ram);

    /* TCP debug server: poll for commands each frame */
    debug_server_poll();

    /* Log per-frame state BEFORE NMI runs */
    debug_log_frame(s_cb_count);

    if (s_debug) {
        log_on_change("$46_mode", g_ram[0x46]);
        log_on_change("$68_palChg", g_ram[0x68]);
        log_on_change("$5E_state", g_ram[0x5E]);
        log_on_change("$66_ppuUpd", g_ram[0x66]);
    }
    /* Clear sprite-0 hit (bit6) and sprite-overflow (bit5) at frame start.
     * Real NES clears all three status bits at pre-render scanline. */
    g_ppustatus &= ~0x60;
    /* Set VBlank flag unconditionally — game can poll $2002 to detect it. */
    g_ppustatus |= 0x80;
    /* Gate NMI on PPUCTRL bit7 (NMI enable). On real NES, the PPU only
     * generates an NMI at VBlank if bit7 of $2000 is set. The game clears
     * this bit during room transitions (while PPU rendering is disabled) to
     * prevent the NMI handler from running the sprite-0 spin-wait with
     * rendering off, which would loop forever. */
    log_on_change("NMI_enable", (g_ppuctrl >> 7) & 1);
    game_on_frame(g_frame_count);


    if (g_ppuctrl & 0x80) {
        /* Check if this is a NESTED NMI (already inside game_run_nmi).
         * On real NES, NMI can nest — but the nested NMI runs a full frame
         * later, when game state is consistent.  In the recompiler, nested
         * NMI fires mid-VRAM-transfer when game state is inconsistent,
         * causing the NMI handler's column loader to compute wrong PPU
         * addresses (e.g. ppuaddr=$FFC0 → writes to palette instead of NT).
         *
         * Fix: for nested NMIs, just set $1A=1 to resolve the spin-wait
         * that triggered the nesting.  Skip the full NMI handler to avoid
         * VRAM transfers with corrupted state. */
        if (runtime_get_vblank_depth() > 1) {
            g_ram[0x1A] = 1;  /* signal the $1A spin-wait to exit */
        } else {
            /* Normal (non-nested) NMI: push stack frame and run handler.
             * Real 6502 pushes 3 bytes: PCH, PCL, P (status flags).
             * PCH/PCL are placeholders — recompiled code never uses the
             * return address from RTI, but the NMI handler does stack-relative
             * reads at $106,X and $107,X that DEPEND on all 3 bytes being
             * present. */
            uint8_t p_save = (uint8_t)((g_cpu.N<<7)|(g_cpu.V<<6)|(1<<5)|
                                       (g_cpu.D<<3)|(g_cpu.I<<2)|(g_cpu.Z<<1)|g_cpu.C);
            g_ram[0x100+g_cpu.S] = 0x00;   g_cpu.S--;   /* PCH placeholder */
            g_ram[0x100+g_cpu.S] = 0x00;   g_cpu.S--;   /* PCL placeholder */
            g_ram[0x100+g_cpu.S] = p_save; g_cpu.S--;   /* P (status flags) */
            runtime_set_vblank_firing(1);
            game_run_nmi();
            runtime_set_vblank_firing(0);
        }
    }

    game_post_nmi(g_frame_count);

    /* Generate one frame of audio after NMI (APU registers now up-to-date).
     * Skip in turbo mode — queued audio would pile up faster than it drains. */
    if (s_audio_dev && !g_turbo) {
        /* Don't over-buffer: skip if more than 6 frames already queued */
        if (SDL_GetQueuedAudioSize(s_audio_dev) < AUDIO_SAMPLES_PER_FRAME * sizeof(int16_t) * 6) {
            apu_generate(s_audio_frame, AUDIO_SAMPLES_PER_FRAME);
            SDL_QueueAudio(s_audio_dev, s_audio_frame, AUDIO_SAMPLES_PER_FRAME * sizeof(int16_t));
        }
    }

    /* Render PPU to framebuffer */
    ppu_render_frame(s_framebuf);

    /* OAM debug window — updated every 6 frames to avoid flicker/lag */
    if (s_debug && s_dbg_texture && g_frame_count % 6 == 0) {
        ppu_render_oam_debug(s_dbg_buf);
        SDL_UpdateTexture(s_dbg_texture, NULL, s_dbg_buf, 256 * 4);
        SDL_RenderClear(s_dbg_renderer);
        SDL_RenderCopy(s_dbg_renderer, s_dbg_texture, NULL, NULL);
        SDL_RenderPresent(s_dbg_renderer);
        /* Update debug window title with frame + visible sprite count */
        {
            int vis = 0;
            for (int _s = 0; _s < 64; _s++)
                if (g_ppu_oam[_s*4] < 0xEF) vis++;
            char title[80];
            snprintf(title, sizeof(title),
                     "OAM Debug  frame=%llu  visible=%d  sprCHR=$%04X",
                     (unsigned long long)g_frame_count, vis,
                     (g_ppuctrl & 0x08) ? 0x1000 : 0x0000);
            SDL_SetWindowTitle(s_dbg_window, title);
        }
    }

    /* RAM watch window */
    watch_render_frame();

    /* Script-triggered named screenshot */
    {
        char shot_path[256];
        if (script_wants_screenshot(shot_path, sizeof(shot_path))) {
            static uint8_t rgb[256 * 240 * 3];
            for (int i = 0; i < 256 * 240; i++) {
                uint32_t px = s_framebuf[i];
                rgb[i*3+0] = (px >> 16) & 0xFF;
                rgb[i*3+1] = (px >>  8) & 0xFF;
                rgb[i*3+2] = (px      ) & 0xFF;
            }
            stbi_write_png(shot_path, 256, 240, 3, rgb, 256*3);
            printf("[Shot] %s\n", shot_path);
        }
    }

    /* Exit check after screenshot is saved */
    {
        int ec = script_check_exit();
        if (ec >= 0) exit(ec);
    }

    /* Auto-screenshot disabled — use F8 or input scripts for screenshots */
    /* if (g_frame_count % 120 == 0) { save_screenshot(); } */
    g_frame_count++;

    /* Upload texture and present */
    SDL_UpdateTexture(s_texture, NULL, s_framebuf, 256 * 4);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);

    /* 60Hz pacing — skipped in turbo mode */
    if (!g_turbo) {
        static uint32_t s_last_tick = 0;
        uint32_t now = SDL_GetTicks();
        if (s_last_tick == 0) s_last_tick = now;
        uint32_t elapsed = now - s_last_tick;
        if (elapsed < 16) SDL_Delay(16 - elapsed);
        s_last_tick = SDL_GetTicks();
    }
}

/* ---- Public render function for emulated mode ---- */
/* Accepts a 256x240 ARGB8888 buffer and presents it to the SDL window.
 * Used by the emulated mode frame loop (Nestopia drives rendering). */
void runner_present_framebuf(const uint32_t *argb_buf) {
    if (!s_texture || !s_renderer || !argb_buf) return;
    SDL_UpdateTexture(s_texture, NULL, argb_buf, 256 * 4);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

/* ---- ROM Loading ---- */
static uint8_t *s_prg_data = NULL;
static int      s_prg_banks = 0;
static uint8_t *s_chr_rom_full = NULL; /* Full CHR ROM for bank switching */

static bool load_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return false; }

    uint8_t header[16];
    fread(header, 1, 16, f);

    if (header[0]!='N'||header[1]!='E'||header[2]!='S'||header[3]!=0x1A) {
        fprintf(stderr, "Not an iNES ROM\n");
        fclose(f); return false;
    }

    s_prg_banks   = header[4];
    int chr_banks = header[5];
    int mapper    = ((header[6] >> 4) & 0x0F) | (header[7] & 0xF0);
    int mirroring = (header[6] & 0x01);  /* 0=horizontal, 1=vertical */
    int has_trainer = (header[6] & 0x04) ? 1 : 0;

    printf("[Runner] ROM: %d PRG banks x 16KB, %d CHR banks x 8KB, Mapper %d\n",
           s_prg_banks, chr_banks, mapper);

    if (has_trainer) fseek(f, 512, SEEK_CUR);

    /* Load PRG ROM */
    size_t prg_size = (size_t)s_prg_banks * 0x4000;
    s_prg_data = (uint8_t *)malloc(prg_size);
    if (!s_prg_data) { fclose(f); return false; }
    fread(s_prg_data, 1, prg_size, f);

    /* Load CHR ROM, if present.
     * For multi-bank CHR ROM, store the full data for mapper bank switching.
     * The mapper will copy the correct bank(s) into g_chr_ram (8KB PPU view).
     * For CHR RAM games (chr_banks==0), g_chr_ram starts zeroed and
     * is populated dynamically via PPU $2007 DMA writes. */
    if (chr_banks > 0) {
        size_t chr_size = (size_t)chr_banks * 0x2000;
        s_chr_rom_full = (uint8_t *)malloc(chr_size);
        if (s_chr_rom_full) {
            size_t n = fread(s_chr_rom_full, 1, chr_size, f);
            printf("[Runner] CHR ROM: loaded %zu bytes (%d banks)\n", n, chr_banks);
            /* Copy first 8KB into g_chr_ram as initial state */
            size_t init_size = (chr_size < sizeof(g_chr_ram)) ? chr_size : sizeof(g_chr_ram);
            memcpy(g_chr_ram, s_chr_rom_full, init_size);
        }
        g_chr_is_rom = 1; /* protect CHR ROM from PPU $2007 writes */
    }

    fclose(f);

    const uint8_t *fixed = s_prg_data + (size_t)(s_prg_banks - 1) * 0x4000;
    uint16_t nmi   = fixed[0x3FFA] | ((uint16_t)fixed[0x3FFB] << 8);
    uint16_t reset = fixed[0x3FFC] | ((uint16_t)fixed[0x3FFD] << 8);
    uint16_t irq   = fixed[0x3FFE] | ((uint16_t)fixed[0x3FFF] << 8);
    printf("[Runner] Vectors: NMI=$%04X RESET=$%04X IRQ=$%04X\n", nmi, reset, irq);

    mapper_init(s_prg_data, s_prg_banks, mapper, mirroring);
    if (s_chr_rom_full && chr_banks > 0)
        mapper_init_chr(s_chr_rom_full, chr_banks);
    return true;
}

/*
 * nesrecomp_runner_run — main runner entry point, called by launcher.c after
 * ROM discovery and CRC verification. argv[1] is guaranteed to be the ROM path.
 */
void nesrecomp_runner_run(int argc, char *argv[]) {

    /* Parse optional flags */
    for (int i = 2; i < argc; i++) {
        if      (strcmp(argv[i], "--script")    == 0 && i+1 < argc) s_script_path    = argv[++i];
        else if (strcmp(argv[i], "--record")    == 0 && i+1 < argc) s_record_path    = argv[++i];
        else if (strcmp(argv[i], "--loadstate") == 0 && i+1 < argc) s_loadstate_path = argv[++i];
        else if (strcmp(argv[i], "--debug")     == 0) s_debug = 1;
        else {
            /* Offer remaining args to the game-specific handler */
            const char *val = (i+1 < argc && argv[i+1][0] != '-') ? argv[i+1] : NULL;
            if (game_handle_arg(argv[i], val)) {
                if (val) i++; /* game consumed the value */
            }
        }
    }

    if (s_debug) printf("[Debug] OAM debug window enabled\n");

    if (!load_rom(argv[1])) exit(1);

    /* Expose ROM path to game extras (used by verify mode to init FCEUX) */
    extern const char *g_rom_path_for_extras;
    g_rom_path_for_extras = argv[1];

    runtime_init();
    keybinds_init(argv[0]);
    game_on_init();

    if (s_loadstate_path) savestate_load(s_loadstate_path);
    if (s_record_path) { record_open(s_record_path); atexit(record_close); }
    if (s_script_path) script_load(s_script_path);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }

    /* Open audio device — use SDL_QueueAudio (callback=NULL) to push samples
     * from the game thread without needing a separate audio thread. */
    {
        SDL_AudioSpec want;
        SDL_memset(&want, 0, sizeof(want));
        want.freq     = 44100;
        want.format   = AUDIO_S16SYS;
        want.channels = 1;
        want.samples  = 512;
        want.callback = NULL;
        SDL_AudioSpec got;
        s_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
        if (s_audio_dev == 0) {
            fprintf(stderr, "[APU] SDL_OpenAudioDevice: %s (continuing without audio)\n",
                    SDL_GetError());
        } else {
            SDL_PauseAudioDevice(s_audio_dev, 0); /* start playback */
            printf("[APU] Audio device opened: %d Hz, %d ch\n", got.freq, got.channels);
        }
    }

    {
        char window_title[64];
        snprintf(window_title, sizeof(window_title), "NESRecomp - %s", game_get_name());
        Uint32 win_flags = SDL_WINDOW_SHOWN;
        win_flags |= SDL_WINDOW_RESIZABLE;
        s_window = SDL_CreateWindow(window_title,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            768, 720,
            win_flags);
    }
    if (!s_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        exit(1);
    }
    /* Game window always on top; debug windows stay beneath */
    SDL_SetWindowAlwaysOnTop(s_window, SDL_TRUE);

    s_renderer = SDL_CreateRenderer(s_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        exit(1);
    }

    s_texture = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        256, 240);
    if (!s_texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        exit(1);
    }
    SDL_RenderSetLogicalSize(s_renderer, 256, 240);

    memset(s_framebuf, 0, sizeof(s_framebuf));

    /* OAM debug window — created only when --debug is passed */
    if (s_debug) {
        s_dbg_window = SDL_CreateWindow(
            "OAM Debug",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            512, 512,   /* 256x256 content at 2x display scale */
            SDL_WINDOW_SHOWN
        );
        if (s_dbg_window) {
            s_dbg_renderer = SDL_CreateRenderer(s_dbg_window, -1,
                SDL_RENDERER_ACCELERATED);
            if (s_dbg_renderer) {
                s_dbg_texture = SDL_CreateTexture(s_dbg_renderer,
                    SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING,
                    256, 256);
                if (!s_dbg_texture)
                    fprintf(stderr, "[Debug] SDL_CreateTexture: %s\n", SDL_GetError());
                else
                    printf("[Debug] OAM window created (256x256 content, 512x512 display)\n");
            } else {
                fprintf(stderr, "[Debug] SDL_CreateRenderer: %s\n", SDL_GetError());
            }
        } else {
            fprintf(stderr, "[Debug] SDL_CreateWindow: %s\n", SDL_GetError());
        }
    }

    /* RAM Watch window — created alongside OAM debug window */
    if (s_debug) {
        /* ---- Watched variables ---- */

        int wh = s_watch_count * WATCH_ENTRY_H + 20 + MAX_MISS_UNIQUE * 6 + 60; /* watch + miss list */
        s_watch_window = SDL_CreateWindow(
            "RAM Watch",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            WATCH_W, wh,
            SDL_WINDOW_SHOWN
        );
        if (s_watch_window) {
            s_watch_renderer = SDL_CreateRenderer(s_watch_window, -1,
                SDL_RENDERER_ACCELERATED);
            if (s_watch_renderer)
                printf("[Debug] Watch window created (%d entries)\n", s_watch_count);
        }
    }

    /* Don't force window on top — let the user manage window stacking */

    printf("[Runner] Starting main game loop...\n");

    /* game_run_main() defaults to func_RESET() (native recompiled main loop,
     * never returns). In emulated mode, it runs FCEUX frames in a loop. */
    game_run_main();

    /* Unreachable for most games, but clean up anyway */
    SDL_DestroyTexture(s_texture);
    SDL_DestroyRenderer(s_renderer);
    SDL_DestroyWindow(s_window);
    SDL_Quit();
    free(s_prg_data);
}
