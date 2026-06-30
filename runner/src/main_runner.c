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
#include "controller.h"
#include "crc32.h"
#include "color_lut.h"
#include "save_ram.h"
#include "config.h"
#include "hdpack.h"
#include "ppu_dot.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* Always-on audio observability (round-2). Env-gated: zero effect unless
 * RECOMP_AUDIO_DEBUG=<dir> is set. See recomp_audio_debug.h. */
#define RECOMP_AUDIO_DEBUG_IMPL
#include "recomp_audio_debug.h"

/* Shared clock-domain bridge (round-2): a real SDL audio callback pulls from a
 * persistent band-limited resampler + fill controller, decoupling the steady host
 * consumer from the bursty video-paced producer. This is the NES crackle fix:
 * measurement showed the old SDL_QueueAudio push model let the queue hit 0 (SDL
 * fills underruns with silence -> crackle). */
#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"
static rab_bridge   s_bridge;
static int          s_bridge_ready = 0;
static SDL_mutex   *s_audio_mtx    = NULL;

/* ---- Script / record / savestate paths (set from CLI) ---- */
static const char *s_script_path    = NULL;
static const char *s_record_path    = NULL;
static const char *s_loadstate_path = NULL;

/* ---- Smoke test mode (--smoke N) ---- */
static int         s_smoke_frames   = 0;     /* 0 = normal, >0 = headless smoke test */
static int         s_smoke_interval = 100;   /* hash framebuffer every N frames */
static const char *s_smoke_output   = NULL;  /* output file path (NULL = stdout) */
#define SMOKE_MAX_HASHES 10000
static uint32_t    s_smoke_hashes[SMOKE_MAX_HASHES];
static int         s_smoke_hash_frames[SMOKE_MAX_HASHES];
static int         s_smoke_hash_count = 0;

/* ---- SDL state (file-level so nes_vblank_callback can access) ---- */
static SDL_Window        *s_window    = NULL;
static SDL_Renderer      *s_renderer  = NULL;
static SDL_Texture       *s_texture   = NULL;
/* HD texture pack output: when a pack is active the native render still runs at
 * g_render_width x 240, then hdpack_upscale() builds this HD frame (scale x). */
static SDL_Texture       *s_hd_texture = NULL;
static uint32_t          *s_hd_buf     = NULL;
static int                s_hd_scale   = 1;
/* Widescreen globals — default to standard 4:3 NES output.
 * Games override these in game_on_init() before the first frame. */
int g_render_width    = 256;
int g_widescreen_left = 0;
int g_widescreen_right = 0;

/* Per-frame effective margins; -1 = follow the configured margins. */
int g_ws_eff_left  = -1;
int g_ws_eff_right = -1;

/* Widescreen sprite-X sidecar — inert until a game sets g_ws_oam_sidecar.
 * See nes_runtime.h for the population model. */
int     g_ws_oam_sidecar   = 0;
int16_t g_oam_x16[64];
int16_t g_ws_shadow_x16[64];
int16_t g_ws_obj_true_rel  = 0;
uint8_t g_ws_obj_rel8      = 0;
uint8_t g_ws_obj_ctx_valid = 0;

static uint32_t           s_framebuf[512 * 240];  /* sized for max 512px width */
/* Present-time color-LUT scratch (palette swap applied to a COPY only when an
 * alternate palette is opted in via NESRECOMP_PALETTE; default Raw = unused). */
static uint32_t           s_present_buf[512 * 240];

/* On-demand render for Zapper light detection.  When the dot-PPU is active it
 * renders incrementally and only publishes s_framebuf at the frame boundary, so
 * the present buffer is a frame stale — snapshot the CURRENT PPU state into a
 * private buffer (no PPU-status side effects).  With the per-frame renderer
 * (default) we render the current state straight into s_framebuf. */
static uint32_t           s_zapper_snapbuf[512 * 240];
static void zapper_on_demand_render(void) {
    if (g_dot_ppu_on) {
        ppu_dot_render_snapshot(s_zapper_snapbuf);
        runtime_set_zapper_snapshot(s_zapper_snapbuf);
    } else {
        ppu_render_frame(s_framebuf);
        runtime_set_zapper_framebuf(s_framebuf);
    }
}

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

/* SDL audio callback (round-2): runs on the audio thread at the device's steady
 * cadence and pulls mono samples from the bridge. This is the consumer that the
 * old push model lacked; it never stalls on video-frame jitter. */
static void nes_audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int frames = len / (int)sizeof(int16_t);   /* mono S16 */
    if (s_bridge_ready) {
        SDL_LockMutex(s_audio_mtx);
        rab_pull(&s_bridge, (int16_t *)stream, frames);
        SDL_UnlockMutex(s_audio_mtx);
        recomp_audio_debug_push_i16("t3_bridge_out", (int16_t *)stream,
                                    frames, s_bridge.cfg.host_rate, 1);
    } else {
        SDL_memset(stream, 0, (size_t)len);
    }
}

/* ---- Screenshot ---- */
/* Non-static wrapper so ppu_renderer.c can save PNGs */
void save_png(const char *path, int w, int h, const void *rgb, int stride) {
    stbi_write_png(path, w, h, 3, rgb, stride);
}

/* Save an ARGB8888 buffer as PNG. Callable from game code (emulated mode screenshots). */
void runner_save_argb_png(const char *path, const uint32_t *argb, int w, int h) {
    static uint8_t rgb[512 * 240 * 3];  /* max widescreen width */
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

/* Save the current native framebuffer to a given path as PNG.
 * Callable from game hooks (e.g. TCP screenshot command). */
void runner_screenshot(const char *path) {
    runner_save_argb_png(path, s_framebuf, g_render_width, 240);
}

uint32_t *runner_get_framebuffer(void) {
    return s_framebuf;
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

/* ---- Zapper mouse → NES coordinate conversion ---- */
static void zapper_mouse_to_nes(int mouse_x, int mouse_y) {
    if (!s_renderer || !s_window) return;
    /* Get the actual renderer output size (accounts for DPI scaling) */
    int out_w, out_h;
    SDL_GetRendererOutputSize(s_renderer, &out_w, &out_h);
    /* Get window size to compute DPI ratio */
    int win_w, win_h;
    SDL_GetWindowSize(s_window, &win_w, &win_h);
    /* Mouse events use window coordinates — scale to output pixels */
    float dpi_x = (float)out_w / win_w;
    float dpi_y = (float)out_h / win_h;
    float px = mouse_x * dpi_x;
    float py = mouse_y * dpi_y;
    /* Compute letterboxed render area within the output */
    float scale_x = (float)out_w / g_render_width;
    float scale_y = (float)out_h / 240.0f;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;
    float render_w = g_render_width * scale;
    float render_h = 240.0f * scale;
    float offset_x = (out_w - render_w) / 2.0f;
    float offset_y = (out_h - render_h) / 2.0f;
    /* Map to NES coordinates */
    int nes_x = (int)((px - offset_x) * g_render_width / render_w);
    int nes_y = (int)((py - offset_y) * 240.0f / render_h);
    if (nes_x < 0) nes_x = 0;
    if (nes_x > 255) nes_x = 255;
    if (nes_y < 0) nes_y = 0;
    if (nes_y > 239) nes_y = 239;
    g_zapper_x = nes_x;
    g_zapper_y = nes_y;
}

/* ---- Smoke test results ---- */
static void smoke_write_results(void) {
    FILE *f = s_smoke_output ? fopen(s_smoke_output, "w") : stdout;
    if (!f) f = stdout;
    fprintf(f, "{\n");
    fprintf(f, "  \"frames_run\": %llu,\n", (unsigned long long)g_frame_count);
    fprintf(f, "  \"dispatch_miss_count\": %u,\n", g_miss_count_any);
    fprintf(f, "  \"dispatch_miss_unique\": %d,\n", g_miss_unique_count);
    fprintf(f, "  \"dispatch_misses\": [");
    for (int i = 0; i < g_miss_unique_count; i++) {
        fprintf(f, "%s\"$%04X\"", i ? ", " : "", g_miss_unique_addrs[i]);
    }
    fprintf(f, "],\n");
    fprintf(f, "  \"frame_hashes\": {\n");
    for (int i = 0; i < s_smoke_hash_count; i++) {
        fprintf(f, "    \"%d\": \"%08x\"%s\n",
                s_smoke_hash_frames[i], s_smoke_hashes[i],
                (i < s_smoke_hash_count - 1) ? "," : "");
    }
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
    if (s_smoke_output && f != stdout) fclose(f);
}

/* ---- VBlank callback (called from ppu_read_reg when $2002 bit7 fires) ---- */
void nes_vblank_callback(void) {
    static uint64_t s_cb_count = 0;
    if (s_cb_count == 0) { /* debug_log_open(); */ }
    s_cb_count++;

    /* Dot-PPU: complete the just-finished frame and publish it to the
     * presentation framebuffer, then arm the next frame's visible region.
     * Runs before the NMI handler below (so PPU memory still reflects the
     * finished frame) and before the present further down. No-op when off. */
    ppu_dot_frame_boundary();
    if (s_debug && (s_cb_count <= 100 || s_cb_count % 60 == 0))
        printf("[VBlank] callback #%llu frame=%llu\n",
               (unsigned long long)s_cb_count, (unsigned long long)g_frame_count);

    /* In smoke mode, skip all SDL input/event handling */
    if (s_smoke_frames) goto smoke_skip_input;

    /* Handle SDL events */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        controller_handle_event(&ev);  /* gamepad hotplug */
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
        /* Toggle borderless-desktop fullscreen: F11 or Alt+Enter. */
        if (ev.type == SDL_KEYDOWN && s_window &&
            (ev.key.keysym.sym == SDLK_F11 ||
             (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)))) {
            Uint32 is_fs = SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
            SDL_SetWindowFullscreen(s_window, is_fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
    }

    /* Zapper mouse input: poll absolute mouse position each frame. A script
     * TRIGGER takes precedence — the mouse must not clobber a scripted aim/
     * trigger (it would reset g_zapper_x/y/trigger every frame, defeating any
     * headless deterministic Zapper test). */
    if (g_zapper_enabled && keybinds_zapper_mouse() && s_window && !script_has_trigger_override()) {
        int mx, my;
        Uint32 buttons = SDL_GetMouseState(&mx, &my);
        zapper_mouse_to_nes(mx, my);
        g_zapper_trigger = (buttons & SDL_BUTTON_LMASK) ? 1 : 0;
    }

    /* Update controllers from keyboard state via configurable keybinds */
    {
        const uint8_t *keys = SDL_GetKeyboardState(NULL);
        /* Per-player input source from the launcher: 1 keyboard, 2 gamepad, 0
         * none. Each player reads only its selected device. */
        int s1 = g_nes_config.player_src[0], s2 = g_nes_config.player_src[1];
        uint8_t btn = (uint8_t)((s1 == 1 ? keybinds_read_player(keys, 1) : 0) |
                                (s1 == 2 ? controller_read_player(1)      : 0));

        /* Recording: capture combined input before script override */
        record_tick(g_frame_count, btn, g_turbo);

        /* Script override: if a script is loaded, use its button state */
        int sp = script_get_buttons();
        if (sp >= 0) btn = (uint8_t)sp;

        /* TCP debug server override: set_input command */
        int tcp_btn = debug_server_get_input_override();
        if (tcp_btn >= 0) btn = (uint8_t)tcp_btn;

        g_controller1_buttons = btn;
        g_controller2_buttons = (uint8_t)((s2 == 1 ? keybinds_read_player(keys, 2) : 0) |
                                          (s2 == 2 ? controller_read_player(2)      : 0));
    }

smoke_skip_input:

    /* Per-frame script execution */
    if (!s_smoke_frames)
    script_tick(g_frame_count, g_ram);

    /* TCP debug server: poll for commands each frame */
    if (!s_smoke_frames)
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
     * Real NES clears these at the pre-render scanline. The per-frame renderer
     * approximates by clearing here (VBlank start). The dot-PPU clears them at
     * the actual pre-render line instead (ppu_dot.c) — clearing at VBlank start
     * breaks games whose NMI waits for the PREVIOUS frame's sprite-0 hit to
     * clear (SMB's Sprite0Clr) before waiting for the new one. */
    if (!g_dot_ppu_on)
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
    save_ram_tick();   /* dirty-checked SRAM flush (~1 Hz); no-op when inactive */


    /* Frame boundary: always call game_run_nmi so per-frame work (oracle
     * sync in --verify, frame counter advancement, etc.) runs every
     * wall-clock frame regardless of NMI-enable.  game_run_nmi is
     * responsible for gating the game's actual NMI handler on
     * (g_ppuctrl & 0x80) and the nested-depth check internally. */
    if ((g_ppuctrl & 0x80) && runtime_get_vblank_depth() > 1) {
        /* Nested NMI with NMI enabled: skip the handler (would corrupt
         * mid-VRAM transfer state), but still set $1A/$20 to resolve any
         * spin-wait. Do NOT run game_run_nmi here — nested NMIs should
         * not advance oracle/frame cadence. */
        g_ram[0x1A] = 1;
        g_ram[0x20] = 1;
    } else {
        /* Top-level frame boundary (or NMI-disabled frame): push stack
         * frame only if NMI is actually going to run, then delegate to
         * game_run_nmi which decides whether to execute func_NMI. */
        int nmi_will_run = (g_ppuctrl & 0x80) != 0;
        uint8_t s_pre_nmi = g_cpu.S;
        /* Save all CPU registers.  On real 6502, the NMI handler's
         * PHP/PHA/.../PLA/PLP sequence always restores A/X/Y/flags.
         * But the recompiled NMI handler may bail early (stack mismatch),
         * skipping the PLA/PLP epilogue.  Without this save/restore,
         * the interrupted game code resumes with corrupted registers,
         * causing bugs like sprite tile corruption on the title screen. */
        uint8_t a_pre = g_cpu.A, x_pre = g_cpu.X, y_pre = g_cpu.Y;
        uint8_t n_pre = g_cpu.N, v_pre = g_cpu.V, d_pre = g_cpu.D;
        uint8_t i_pre = g_cpu.I, z_pre = g_cpu.Z, c_pre = g_cpu.C;
        if (nmi_will_run) {
            uint8_t p_save = (uint8_t)((g_cpu.N<<7)|(g_cpu.V<<6)|(1<<5)|
                                       (g_cpu.D<<3)|(g_cpu.I<<2)|(g_cpu.Z<<1)|g_cpu.C);
            g_ram[0x100+g_cpu.S] = 0x00;   g_cpu.S--;   /* PCH placeholder */
            g_ram[0x100+g_cpu.S] = 0x00;   g_cpu.S--;   /* PCL placeholder */
            g_ram[0x100+g_cpu.S] = p_save; g_cpu.S--;   /* P (status flags) */
            runtime_set_vblank_firing(1);
        }
        game_run_nmi();
        if (nmi_will_run) {
            runtime_set_vblank_firing(0);
            /* On real 6502, RTI always restores S and all registers.
             * The recompiled NMI handler may return early (bail), so
             * enforce full register restoration. */
            g_cpu.S = s_pre_nmi;
            g_cpu.A = a_pre; g_cpu.X = x_pre; g_cpu.Y = y_pre;
            g_cpu.N = n_pre; g_cpu.V = v_pre; g_cpu.D = d_pre;
            g_cpu.I = i_pre; g_cpu.Z = z_pre; g_cpu.C = c_pre;
        }
    }

    /* Nested VBlanks (depth > 1) exist only to resolve spin-waits ($1A).
     * They must NOT run post-NMI, render, present, or advance frame count.
     * On real NES, NMI never re-enters — running PostNMI (sound engine,
     * etc.) during a nested NMI causes side effects like shadow OAM
     * corruption from sprite management code in the PostNMI chain. */
    if (runtime_get_vblank_depth() > 1)
        return;

    game_post_nmi(g_frame_count);

    /* Record frame state to ring buffer for TCP timeseries queries */
    debug_server_record_frame();

    /* Cross-engine RNG/seed freeze (env-gated, accuracy harness) — applied at
     * the frame boundary so the nesref oracle and recomp share identical RNG. */
    { extern void nes_apply_freeze(void); nes_apply_freeze(); }

    /* Per-frame WRAM delta trace for the nesref state-divergence diff (env-gated). */
    { extern void nes_wram_trace_frame(void); nes_wram_trace_frame(); }

    /* Per-frame PPU-memory (OAM/palette/nametable) delta trace vs Mesen-Lua (env-gated). */
    { extern void nes_ppumem_trace_frame(void); nes_ppumem_trace_frame(); }

    /* (The APU frame-counter IRQ is driven on the CPU-cycle stream in
     * maybe_trigger_vblank → apu_clock_cycles, not per-NMI here, so it also
     * advances for NMI-disabled main-thread code such as the blargg APU tests.) */

    /* Generate one frame of audio after NMI (APU registers now up-to-date).
     * Skip in turbo mode — queued audio would pile up faster than it drains. */
    if (s_audio_dev && !g_turbo && !s_smoke_frames) {
        static int s_synth_mode = -2;            /* -2 = not yet queried */
        static uint64_t s_synth_pos = 0;
        if (s_synth_mode == -2) s_synth_mode = recomp_audio_synth_mode();
        if (s_synth_mode != RAD_SYNTH_OFF)
            recomp_audio_synth_fill(s_synth_mode, s_audio_frame,
                                    AUDIO_SAMPLES_PER_FRAME, 1, 44100.0, &s_synth_pos);
        else
            apu_generate(s_audio_frame, AUDIO_SAMPLES_PER_FRAME);

        /* T1: raw emulator-rate PCM, before volume. */
        recomp_audio_debug_push_i16("t1_apu", s_audio_frame,
                                    AUDIO_SAMPLES_PER_FRAME, 44100.0, 1);

        /* Apply the launcher volume (0..100) as a linear scale. */
        int vol = g_nes_config.volume;
        if (vol < 100) {
            if (vol < 0) vol = 0;
            for (int i = 0; i < AUDIO_SAMPLES_PER_FRAME; i++)
                s_audio_frame[i] = (int16_t)((int)s_audio_frame[i] * vol / 100);
        }
        /* T2: what the bridge receives. */
        recomp_audio_debug_push_i16("t2_bridge_in", s_audio_frame,
                                    AUDIO_SAMPLES_PER_FRAME, 44100.0, 1);

        if (s_bridge_ready) {
            SDL_LockMutex(s_audio_mtx);
            rab_push(&s_bridge, s_audio_frame, AUDIO_SAMPLES_PER_FRAME);
            double fill = rab_fill_ms(&s_bridge);
            rab_stats st; rab_get_stats(&s_bridge, &st);
            SDL_UnlockMutex(s_audio_mtx);
            recomp_audio_debug_eventf("bfill",
                                      "fill_ms=%.1f under=%llu over=%llu stretch_f=%llu stretch_e=%llu",
                                      fill, (unsigned long long)st.underrun_events,
                                      (unsigned long long)st.overflow_drops,
                                      (unsigned long long)st.stretch_frames,
                                      (unsigned long long)st.stretch_events);
        } else {
            /* legacy fallback (bridge failed to init): old push path */
            SDL_QueueAudio(s_audio_dev, s_audio_frame,
                           AUDIO_SAMPLES_PER_FRAME * sizeof(int16_t));
        }
    }

    /* Headless capture helper: RECOMP_AUDIO_DEBUG_DUMP_SECS=N dumps the rings and
     * exits after N seconds of audio frames (for data collection, not ear tests). */
    if (recomp_audio_debug_enabled()) {
        static int s_dump_frames = -2;   /* -2 = unqueried, -1 = disabled */
        static uint64_t s_audio_frames_seen = 0;
        if (s_dump_frames == -2) {
            const char *e = getenv("RECOMP_AUDIO_DEBUG_DUMP_SECS");
            s_dump_frames = (e && *e) ? (int)(atof(e) * 60.0) : -1;
        }
        if (s_dump_frames > 0) {
            s_audio_frames_seen++;
            if ((int)s_audio_frames_seen >= s_dump_frames) {
                recomp_audio_debug_dump(0.0, NULL);
                printf("[audio-debug] dumped after %llu frames; exiting\n",
                       (unsigned long long)s_audio_frames_seen);
                fflush(stdout);
                exit(0);
            }
        }
    }

    /* Predict sprite-0-hit scanline for this frame using the post-NMI OAM
     * and PPU state. Used by ppu_read_reg($2002) to set bit 6 at the right
     * CPU cycle position so games whose hit-detect spin-waits poll $2002
     * mid-frame (Gumshoe-style zappers) see the bit flip when the beam
     * would actually have reached sprite 0. */
    g_predicted_spr0_scanline = ppu_predict_spr0_hit_scanline();

    /* Render PPU to framebuffer.
     * Dot-PPU mode (default): the frame was painted incrementally into the back
     * buffer (during this budget's NMI + main loop) and published to s_framebuf
     * by ppu_dot_frame_boundary at the next frame boundary — including
     * widescreen — so the per-frame compositor is skipped. Only NESRECOMP_DOT_PPU=0
     * uses ppu_render_frame now. */
    if (!g_dot_ppu_on)
        ppu_render_frame(s_framebuf);

    /* Update Zapper light sensor framebuffer for next frame's $4017 reads */
    runtime_set_zapper_framebuf(s_framebuf);

    /* Game-specific post-render (e.g. widescreen margin sprites) */
    game_post_render(s_framebuf);

    /* Smoke test: hash framebuffer at intervals and check exit */
    if (s_smoke_frames) {
        if (g_frame_count % s_smoke_interval == 0 &&
            s_smoke_hash_count < SMOKE_MAX_HASHES) {
            s_smoke_hashes[s_smoke_hash_count] =
                crc32_compute((const uint8_t *)s_framebuf,
                              (size_t)g_render_width * 240 * sizeof(uint32_t));
            s_smoke_hash_frames[s_smoke_hash_count] = (int)g_frame_count;
            s_smoke_hash_count++;
        }
        g_frame_count++;
        if ((int)g_frame_count >= s_smoke_frames) {
            smoke_write_results();
            exit(0);
        }
        return; /* skip all SDL rendering/pacing */
    }

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
            if (s_hd_buf && hdpack_active()) {
                /* Capture the HD output so screenshots match what is displayed. */
                int hw = g_render_width * s_hd_scale, hh = 240 * s_hd_scale;
                hdpack_upscale(s_framebuf, g_render_width, s_hd_buf);
                uint8_t *rgb = (uint8_t *)malloc((size_t)hw * hh * 3);
                if (rgb) {
                    for (int i = 0; i < hw * hh; i++) {
                        uint32_t px = s_hd_buf[i];
                        rgb[i*3+0] = (px >> 16) & 0xFF;
                        rgb[i*3+1] = (px >>  8) & 0xFF;
                        rgb[i*3+2] = (px      ) & 0xFF;
                    }
                    stbi_write_png(shot_path, hw, hh, 3, rgb, hw * 3);
                    free(rgb);
                    printf("[Shot] %s (HD %dx%d)\n", shot_path, hw, hh);
                }
            } else {
                static uint8_t rgb[512 * 240 * 3];  /* max native width */
                int npx = g_render_width * 240;
                for (int i = 0; i < npx; i++) {
                    uint32_t px = s_framebuf[i];
                    rgb[i*3+0] = (px >> 16) & 0xFF;
                    rgb[i*3+1] = (px >>  8) & 0xFF;
                    rgb[i*3+2] = (px      ) & 0xFF;
                }
                stbi_write_png(shot_path, g_render_width, 240, 3, rgb, g_render_width*3);
                printf("[Shot] %s\n", shot_path);
            }
            /* Optional 8KB CHR snapshot dump (authoring HD packs for CHR-RAM
             * games — the active pattern tables aren't in the ROM). */
            const char *chrdump = getenv("NESRECOMP_CHR_DUMP");
            if (chrdump && chrdump[0]) {
                FILE *cf = fopen(chrdump, "wb");
                if (cf) { fwrite(g_chr_ram, 1, 0x2000, cf); fclose(cf);
                          printf("[ChrDump] %s (8KB)\n", chrdump); }
            }
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

    /* Zapper crosshair — always visible when enabled in keybinds.ini */
    if (g_zapper_enabled && keybinds_zapper_mouse() && keybinds_zapper_crosshair()) {
        int cx = g_zapper_x, cy = g_zapper_y;
        uint32_t color = g_zapper_trigger ? 0xFFFF0000 : 0xFFFFFFFF; /* red on fire, white otherwise */
        /* Draw crosshair: horizontal and vertical lines, 11px each */
        for (int d = -5; d <= 5; d++) {
            if (d == 0) continue; /* skip center, drawn separately */
            int hx = cx + d, vy = cy + d;
            if (hx >= 0 && hx < g_render_width && cy >= 0 && cy < 240)
                s_framebuf[cy * g_render_width + hx] = color;
            if (cx >= 0 && cx < g_render_width && vy >= 0 && vy < 240)
                s_framebuf[vy * g_render_width + cx] = color;
        }
        /* Center dot — always bright */
        if (cx >= 0 && cx < g_render_width && cy >= 0 && cy < 240)
            s_framebuf[cy * g_render_width + cx] = 0xFFFFFFFF;
    }

    /* Upload texture and present.
     * In turbo mode, only present every 16th frame to avoid vsync blocking
     * on every SDL_RenderPresent call (~6ms each on a 165Hz monitor). */
    if (!g_turbo || (g_frame_count & 15) == 0) {
        /* Present-time palette swap (opt-in, default Raw = passthrough). Raw
         * presents the raw framebuffer untouched => byte-identical to canon. */
        const uint32_t *present = s_framebuf;
        if (!color_lut_is_passthrough()) {
            color_lut_apply(s_framebuf, s_present_buf, g_render_width, 240);
            present = s_present_buf;
        }
        SDL_RenderClear(s_renderer);
        if (s_hd_texture && hdpack_active()) {
            /* HD pack active: upscale the native frame + per-pixel side channel
             * into the HD buffer, present the HD texture. */
            hdpack_upscale(present, g_render_width, s_hd_buf);
            SDL_UpdateTexture(s_hd_texture, NULL, s_hd_buf, g_render_width * s_hd_scale * 4);
            SDL_RenderCopy(s_renderer, s_hd_texture, NULL, NULL);
        } else {
            SDL_UpdateTexture(s_texture, NULL, present, g_render_width * 4);
            SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
        }
        SDL_RenderPresent(s_renderer);
    }

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
    /* Copy to s_framebuf so TCP screenshot works in emulated mode */
    memcpy(s_framebuf, argb_buf, (size_t)g_render_width * 240 * 4);
    SDL_UpdateTexture(s_texture, NULL, argb_buf, g_render_width * 4);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

/* ---- ROM Loading ---- */
static uint8_t *s_prg_data = NULL;
static int      s_prg_banks = 0;
static uint8_t *s_chr_rom_full = NULL; /* Full CHR ROM for bank switching */
static int      s_rom_has_battery = 0; /* iNES header[6] bit1 — battery-backed SRAM */

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
    s_rom_has_battery = (header[6] & 0x02) ? 1 : 0;

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

/* ---- PRG ROM writable accessor (for runtime text/data overrides) ----
 * Returns a writable pointer to the start of the given 16KB PRG bank,
 * or NULL if bank_num is out of range or ROM not yet loaded.
 * The caller may write override bytes into this buffer; nes_read() will
 * return the patched values on subsequent reads from that address range.
 * Safe to call from game_on_init() after ROM is loaded. */
uint8_t *runner_get_prg_bank_rw(int bank_num) {
    if (!s_prg_data || bank_num < 0 || bank_num >= s_prg_banks) return NULL;
    return s_prg_data + (size_t)bank_num * 0x4000;
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
        else if (strcmp(argv[i], "--smoke")     == 0 && i+1 < argc) s_smoke_frames   = atoi(argv[++i]);
        else if (strcmp(argv[i], "--smoke-interval") == 0 && i+1 < argc) s_smoke_interval = atoi(argv[++i]);
        else if (strcmp(argv[i], "--smoke-output")   == 0 && i+1 < argc) s_smoke_output   = argv[++i];
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

    /* SRAM persistence (saves/<title>.srm <-> g_sram). Auto-enabled for battery
     * games via the iNES bit; synthetic-SRAM games opt in from game_on_init()
     * (which ran just above) via save_ram_request_enable(). NONE backend = no-op. */
    save_ram_init(game_get_name(), s_rom_has_battery);

    if (g_zapper_enabled) {
        runtime_set_zapper_render_callback(zapper_on_demand_render);
        if (keybinds_zapper_mouse())
            printf("[Zapper] Mouse mode enabled — left click to shoot\n");
    }

    if (s_loadstate_path) savestate_load(s_loadstate_path);
    if (s_record_path) { record_open(s_record_path); atexit(record_close); }
    if (s_script_path) script_load(s_script_path);

    /* Dot-accurate PPU (opt-in, EXPERIMENTAL): reads NESRECOMP_DOT_PPU and
     * registers the framebuffer. No-op (per-frame renderer) unless set. */
    ppu_dot_init(s_framebuf);

    /* In smoke mode, skip all SDL initialization — run headless */
    if (s_smoke_frames) {
        printf("[Smoke] Headless mode: running %d frames, hashing every %d\n",
               s_smoke_frames, s_smoke_interval);
        memset(s_framebuf, 0, sizeof(s_framebuf));
        game_run_main();
        /* Unreachable — game_run_main never returns, smoke exits from vblank */
        exit(0);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }

    /* Gamepad support (Xbox/PS/Switch/generic via SDL's mapping DB). */
    controller_init();

    /* Open audio device — use SDL_QueueAudio (callback=NULL) to push samples
     * from the game thread without needing a separate audio thread. */
    {
        SDL_AudioSpec want;
        SDL_memset(&want, 0, sizeof(want));
        want.freq     = 44100;
        want.format   = AUDIO_S16SYS;
        want.channels = 1;
        want.samples  = 512;
        want.callback = nes_audio_cb;   /* round-2: pull from the bridge */
        SDL_AudioSpec got;
        s_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got,
                                          SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (s_audio_dev == 0) {
            fprintf(stderr, "[APU] SDL_OpenAudioDevice: %s (continuing without audio)\n",
                    SDL_GetError());
        } else {
            s_audio_mtx = SDL_CreateMutex();
            rab_config rc; rab_config_defaults(&rc);
            rc.channels    = 1;
            rc.source_rate = 44100.0;
            rc.host_rate   = (double)got.freq;
            /* Tuned to the measured video-paced burst swing (~100 ms): a deep ring
             * with a high target absorbs producer droughts so the device never
             * starves. Latency cost ~90 ms is inaudible for these games. */
            rc.target_ms   = 60.0;
            rc.ring_ms     = 250.0;
            /* Allow up to +/-1.5% ratio correction so the controller can track a
             * real producer/consumer clock mismatch and hold the fill at target
             * instead of drifting toward underrun/overflow. Only the steady-state
             * offset is continuous; for matched clocks it sits near 0. */
            rc.max_correction = 0.015;
            /* Phase-1 boot pre-roll: prime the ring to ~200 ms before playback so
             * the cold-start hitch (JIT warm-up / first audio bursts) is concealed.
             * The added latency is irrelevant pre-gameplay; the servo drains the
             * excess down to target_ms over time. Underrun time-stretch concealment
             * (stretch_enable) is on by default via rab_config_defaults. */
            rc.preroll_ms = 200.0;
            s_bridge_ready = (rab_init(&s_bridge, &rc) == 0);
            SDL_PauseAudioDevice(s_audio_dev, 0); /* start playback */
            printf("[APU] Audio device opened: %d Hz, %d ch  (bridge=%s, target=%.0fms)\n",
                   got.freq, got.channels, s_bridge_ready ? "on" : "FAILED", rc.target_ms);
            if (recomp_audio_debug_init())
                printf("[audio-debug] capture ON (synth=%d)\n", recomp_audio_synth_mode());
        }
    }

    {
        char window_title[64];
        snprintf(window_title, sizeof(window_title), "NESRecomp - %s", game_get_name());
        Uint32 win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
        if (g_nes_config.fullscreen) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        int scale = g_nes_config.window_scale < 1 ? 1 : g_nes_config.window_scale;
        s_window = SDL_CreateWindow(window_title,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            g_render_width * scale, 240 * scale,
            win_flags);
    }
    if (!s_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        exit(1);
    }

    /* Pixel-art scaling: nearest (crisp) by default, linear if the user opted in. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                g_nes_config.linear_filter ? "linear" : "nearest");

    Uint32 render_flags = SDL_RENDERER_PRESENTVSYNC |
        (g_nes_config.renderer == 1 ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED);
    s_renderer = SDL_CreateRenderer(s_window, -1, render_flags);
    if (!s_renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        exit(1);
    }

    /* Preserve the NES aspect ratio (letterbox instead of stretch) and keep
     * scaling to whole-pixel multiples so every NES pixel stays the same size.
     * Applies to both windowed-resize and fullscreen. */
    SDL_RenderSetLogicalSize(s_renderer, g_render_width, 240);
    SDL_RenderSetIntegerScale(s_renderer, g_nes_config.integer_scale ? SDL_TRUE : SDL_FALSE);

    s_texture = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        g_render_width, 240);
    if (!s_texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        exit(1);
    }
    SDL_RenderSetLogicalSize(s_renderer, g_render_width, 240);

    /* HD texture pack (Mesen HD Pack): opt-in via the NESRECOMP_HDPACK env var
     * or config.ini [Display] HdPackEnabled/HdPackDir. When a pack loads, present
     * an HD-resolution texture and set the renderer logical size to match so the
     * upscaled art shows at full detail. Mirrors the SNES MSU-1 opt-in wiring. */
    if (hdpack_load_from_config(mapper_is_chr_ram(), g_render_width) == 0) {
        s_hd_scale = hdpack_scale();
        int hw = g_render_width * s_hd_scale, hh = 240 * s_hd_scale;
        s_hd_buf = (uint32_t *)malloc((size_t)hw * hh * sizeof(uint32_t));
        s_hd_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, hw, hh);
        if (s_hd_buf && s_hd_texture) {
            SDL_RenderSetLogicalSize(s_renderer, hw, hh);
            /* The HD logical size is larger than the native window, so integer
             * scaling would round to 0 and draw nothing. Use fractional fit and
             * size the window to the HD frame (capped to ~90% of the desktop). */
            SDL_RenderSetIntegerScale(s_renderer, SDL_FALSE);
            if (!g_nes_config.fullscreen) {
                int ww = hw, wh = hh;
                SDL_DisplayMode dm;
                if (SDL_GetCurrentDisplayMode(0, &dm) == 0) {
                    double sc = 1.0;
                    int maxw = (int)(dm.w * 0.9), maxh = (int)(dm.h * 0.9);
                    if (ww > maxw) sc = (double)maxw / ww;
                    if (wh * sc > maxh) sc = (double)maxh / wh;
                    ww = (int)(ww * sc); wh = (int)(wh * sc);
                }
                SDL_SetWindowSize(s_window, ww, wh);
                SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            }
            printf("[HDPack] HD output enabled: %dx%d (%dx)\n", hw, hh, s_hd_scale);
        } else {
            fprintf(stderr, "[HDPack] HD texture/buffer alloc failed; using native output\n");
            free(s_hd_buf); s_hd_buf = NULL;
            if (s_hd_texture) { SDL_DestroyTexture(s_hd_texture); s_hd_texture = NULL; }
            hdpack_unload();
            s_hd_scale = 1;
        }
    }

    memset(s_framebuf, 0, sizeof(s_framebuf));

    /* Present-time palette LUT: default Raw (passthrough, byte-identical);
     * opt in via NESRECOMP_PALETTE={raw,2c02,fbx}. */
    color_lut_init_from_env();

    /* Hide OS cursor when the Zapper crosshair replaces it */
    if (g_zapper_enabled && keybinds_zapper_mouse() && keybinds_zapper_crosshair())
        SDL_ShowCursor(SDL_DISABLE);

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
