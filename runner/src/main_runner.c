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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* ---- Script / record paths (set from CLI) ---- */
static const char *s_script_path = NULL;
static const char *s_record_path = NULL;

/* ---- SDL state (file-level so nes_vblank_callback can access) ---- */
static SDL_Window   *s_window   = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture  *s_texture  = NULL;
static uint32_t      s_framebuf[256 * 240];

/* ---- Screenshot ---- */
#define SHOT_COUNT 10
static int s_shot_index = 1;

static void save_screenshot(void) {
    char path[64];
    snprintf(path, sizeof(path), "C:/temp/nes_shot_%02d.png", s_shot_index);
    static uint8_t rgb[256 * 240 * 3];
    for (int i = 0; i < 256 * 240; i++) {
        uint32_t px = s_framebuf[i];
        rgb[i*3+0] = (px >> 16) & 0xFF;
        rgb[i*3+1] = (px >>  8) & 0xFF;
        rgb[i*3+2] = (px      ) & 0xFF;
    }
    stbi_write_png(path, 256, 240, 3, rgb, 256*3);
    printf("[Shot] Saved %s\n", path);
    s_shot_index++;
    if (s_shot_index > SHOT_COUNT) s_shot_index = 1;
}

/* ---- Debug trace log (C:/temp/debug_trace.txt) ---- */
static FILE *s_debug_log = NULL;

static void debug_log_open(void) {
    s_debug_log = fopen("C:/temp/debug_trace.txt", "w");
    if (s_debug_log) {
        fprintf(s_debug_log, "FRAME,bank,r13,r14,r20,r1F,S\n");
        fflush(s_debug_log);
    }
}

/* Call at NMI time to log one line per frame. Comment out when not debugging. */
static void debug_log_frame(uint64_t frame) {
    if (!s_debug_log) return;
    fprintf(s_debug_log, "%llu,%d,%02X,%02X,%02X,%02X,%02X,%02X\n",
            (unsigned long long)frame,
            g_current_bank,
            g_ram[0x13], g_ram[0x14],
            g_ram[0x20], g_ram[0x1F],
            g_cpu.S,
            g_ram[0x100]);   /* game's bank tracking register ($0100) */
    fflush(s_debug_log);
}

/* ---- VBlank callback (called from ppu_read_reg when $2002 bit7 fires) ---- */
void nes_vblank_callback(void) {
    static uint64_t s_cb_count = 0;
    if (s_cb_count == 0) debug_log_open();
    s_cb_count++;
    if (s_cb_count <= 5 || s_cb_count % 60 == 0)
        printf("[VBlank] callback #%llu frame=%llu\n",
               (unsigned long long)s_cb_count, (unsigned long long)g_frame_count);

    /* Handle SDL events */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) exit(0);
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) exit(0);
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F5)
            g_turbo ^= 1;
    }

    /* Update controller 1 from keyboard state.
     * Mapping: Z=A  X=B  Tab=Select  Enter=Start  Arrows=D-pad */
    {
        const uint8_t *keys = SDL_GetKeyboardState(NULL);
        uint8_t btn = 0;
        if (keys[SDL_SCANCODE_Z])      btn |= 0x80; /* A */
        if (keys[SDL_SCANCODE_X])      btn |= 0x40; /* B */
        if (keys[SDL_SCANCODE_TAB])    btn |= 0x20; /* Select */
        if (keys[SDL_SCANCODE_RETURN]) btn |= 0x10; /* Start */
        if (keys[SDL_SCANCODE_UP])     btn |= 0x08; /* Up */
        if (keys[SDL_SCANCODE_DOWN])   btn |= 0x04; /* Down */
        if (keys[SDL_SCANCODE_LEFT])   btn |= 0x02; /* Left */
        if (keys[SDL_SCANCODE_RIGHT])  btn |= 0x01; /* Right */

        /* Recording: capture keyboard state before script override */
        record_tick(g_frame_count, btn, g_turbo);

        /* Script override: if a script is loaded, use its button state */
        int sp = script_get_buttons();
        if (sp >= 0) btn = (uint8_t)sp;

        g_controller1_buttons = btn;
    }

    /* Per-frame script execution */
    script_tick(g_frame_count, g_ram);
    {
        int ec = script_check_exit();
        if (ec >= 0) exit(ec);
    }

    /* Log per-frame state BEFORE NMI runs */
    debug_log_frame(s_cb_count);

    /* Debug: dump NMI gate variables */
    if (s_cb_count <= 5)
        printf("[NMI_pre] frame=%llu $10=%02X $13=%02X $14=%02X $1A=%02X $0B=%02X\n",
               (unsigned long long)g_frame_count,
               g_ram[0x10], g_ram[0x13], g_ram[0x14], g_ram[0x1A], g_ram[0x0B]);
    /* Clear sprite-0 hit (bit6) and sprite-overflow (bit5) at frame start.
     * Real NES clears all three status bits at pre-render scanline. */
    g_ppustatus &= ~0x60;
    /* Simulate hardware NMI push: RTI inside the handler consumes 2 bytes
     * (P + 1 dummy for PC). On real hardware, NMI pushes PC_hi, PC_lo, P
     * before jumping to the NMI vector. Our static RTI does S+=2.
     * Push 2 bytes here so RTI restores the stack correctly. */
    {
        uint8_t p_save = (uint8_t)((g_cpu.N<<7)|(g_cpu.V<<6)|(1<<5)|
                                   (g_cpu.D<<3)|(g_cpu.I<<2)|(g_cpu.Z<<1)|g_cpu.C);
        g_ram[0x100+g_cpu.S] = 0x00;   g_cpu.S--;  /* PC_lo placeholder */
        g_ram[0x100+g_cpu.S] = p_save; g_cpu.S--;  /* P flags restored by RTI */
    }
    /* Set VBlank for func_NMI's benefit, then call NMI.
     * Do NOT clear bit7 here — let ppu_read_reg($2002) clear it when the
     * game reads $2002 in its spin loop (real NES behavior: VBlank flag is
     * cleared by the first CPU read of $2002 after VBlank starts). */
    g_ppustatus |= 0x80;
    func_NMI();
    if (s_cb_count <= 5) printf("[VBlank] NMI returned ok\n");

    /* Render PPU to framebuffer */
    ppu_render_frame(s_framebuf);

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

    /* Rotating screenshot every 60 frames */
    if (g_frame_count % 60 == 0) {
        save_screenshot();
    }
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

/* ---- ROM Loading ---- */
static uint8_t *s_prg_data = NULL;
static int      s_prg_banks = 0;

static bool load_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return false; }

    uint8_t header[16];
    fread(header, 1, 16, f);

    if (header[0]!='N'||header[1]!='E'||header[2]!='S'||header[3]!=0x1A) {
        fprintf(stderr, "Not an iNES ROM\n");
        fclose(f); return false;
    }

    s_prg_banks = header[4];
    int mapper  = ((header[6]>>4)&0x0F) | (header[7]&0xF0);
    int chr_banks = header[5];

    printf("[Runner] ROM: %d PRG banks x 16KB, %d CHR banks, Mapper %d\n",
           s_prg_banks, chr_banks, mapper);

    if (header[6] & 0x04) fseek(f, 512, SEEK_CUR);

    size_t prg_size = (size_t)s_prg_banks * 0x4000;
    s_prg_data = (uint8_t *)malloc(prg_size);
    if (!s_prg_data) { fclose(f); return false; }
    fread(s_prg_data, 1, prg_size, f);
    fclose(f);

    const uint8_t *fixed = s_prg_data + (size_t)(s_prg_banks-1)*0x4000;
    uint16_t nmi   = fixed[0x3FFA] | ((uint16_t)fixed[0x3FFB]<<8);
    uint16_t reset = fixed[0x3FFC] | ((uint16_t)fixed[0x3FFD]<<8);
    uint16_t irq   = fixed[0x3FFE] | ((uint16_t)fixed[0x3FFF]<<8);
    printf("[Runner] Vectors: NMI=$%04X RESET=$%04X IRQ=$%04X\n", nmi, reset, irq);

    mapper_init(s_prg_data, s_prg_banks);
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: NESRecompGame <baserom.nes> [--script FILE] [--record FILE]\n");
        return 1;
    }

    /* Parse optional flags */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--script") == 0 && i+1 < argc) s_script_path = argv[++i];
        else if (strcmp(argv[i], "--record") == 0 && i+1 < argc) s_record_path = argv[++i];
    }

    if (!load_rom(argv[1])) return 1;

    runtime_init();

    if (s_record_path) record_open(s_record_path);
    if (s_script_path) script_load(s_script_path);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    s_window = SDL_CreateWindow(
        "NESRecomp - Faxanadu",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        768, 720,
        SDL_WINDOW_SHOWN
    );
    if (!s_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }

    s_texture = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        256, 240);
    if (!s_texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(s_renderer, 768, 720);

    memset(s_framebuf, 0, sizeof(s_framebuf));

    printf("[Runner] Starting RESET handler (NMI fires via VBlank callback)...\n");

    /* func_RESET() is the game's main loop — it never returns.
     * NMI is called from nes_vblank_callback() whenever the game
     * reads $2002 (PPUSTATUS) and sees VBlank set. */
    func_RESET();

    /* Unreachable for most games, but clean up anyway */
    SDL_DestroyTexture(s_texture);
    SDL_DestroyRenderer(s_renderer);
    SDL_DestroyWindow(s_window);
    SDL_Quit();
    free(s_prg_data);
    return 0;
}
