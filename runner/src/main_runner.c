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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* ---- Script / record / savestate paths (set from CLI) ---- */
static const char *s_script_path    = NULL;
static const char *s_record_path    = NULL;
static const char *s_loadstate_path = NULL;

/* ---- Password auto-fill ---- */
static const char *s_password          = NULL;
static int         s_password_injected = 0;

/* Returns the mantra table index (0-63) for a character, or -1 if invalid.
 * Bank12 $8764 table order: A-Z (0-25), a-z (26-51), 0-9 (52-61), ',' (62), '?' (63) */
static int password_char_to_index(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return 26 + (ch - 'a');
    if (ch >= '0' && ch <= '9') return 52 + (ch - '0');
    if (ch == ',') return 62;
    if (ch == '?') return 63;
    return -1;
}

/* Inject password into the mantra entry RAM buffer.
 * Detection (Ghidra bank12 analysis):
 *   $0665 == len  — max-length register set to our password length (24 for Faxanadu mantra)
 *   $0666 == 0    — no characters entered yet (fresh screen)
 *   $0600 == 0xFF — first slot is empty sentinel
 * Writes character table indices to $0600+i, sets $0664 = $0666 = len. */
static void maybe_inject_password(void) {
    if (!s_password || s_password_injected) return;

    int len = (int)strlen(s_password);
    if (len == 0 || len > 24) return;

    uint8_t max_len = g_ram[0x665];
    if (max_len == 0)          return;   /* screen not initialized yet */
    if (g_ram[0x666] != 0)     return;   /* something already entered */
    if (g_ram[0x600] != 0xFF)  return;   /* first slot not empty */

    for (int i = 0; i < len; i++) {
        int idx = password_char_to_index(s_password[i]);
        if (idx < 0) {
            fprintf(stderr, "[Password] Unknown character '%c' at position %d — aborted\n",
                    s_password[i], i);
            return;
        }
        g_ram[0x600 + i] = (uint8_t)idx;
    }
    for (int i = len; i < (int)max_len; i++)
        g_ram[0x600 + i] = 0xFF;   /* fill remaining slots with empty sentinel */

    g_ram[0x664] = (uint8_t)len;   /* cursor: positioned after last entered char */
    g_ram[0x666] = (uint8_t)len;   /* characters-entered count */

    s_password_injected = 1;
    printf("[Password] Injected \"%s\" (%d chars)\n", s_password, len);
}

/* ---- SDL state (file-level so nes_vblank_callback can access) ---- */
static SDL_Window        *s_window    = NULL;
static SDL_Renderer      *s_renderer  = NULL;
static SDL_Texture       *s_texture   = NULL;
static uint32_t           s_framebuf[256 * 240];

/* ---- Audio state ---- */
static SDL_AudioDeviceID  s_audio_dev = 0;
#define AUDIO_SAMPLES_PER_FRAME 735
static int16_t            s_audio_frame[AUDIO_SAMPLES_PER_FRAME];

/* ---- Screenshot ---- */
static void save_screenshot(void) {
    char path[80];
    snprintf(path, sizeof(path), "C:/temp/nes_shot_%04llu.png",
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
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F6)
            savestate_save("C:/temp/quicksave.sav");
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F7) {
            record_loadstate(g_frame_count, "C:/temp/quicksave.sav");
            savestate_load("C:/temp/quicksave.sav");
            record_sync_frame(g_frame_count); /* g_frame_count now = restored value */
        }
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
    /* Set VBlank flag unconditionally — game can poll $2002 to detect it. */
    g_ppustatus |= 0x80;
    /* Gate NMI on PPUCTRL bit7 (NMI enable). On real NES, the PPU only
     * generates an NMI at VBlank if bit7 of $2000 is set. The game clears
     * this bit during room transitions (while PPU rendering is disabled) to
     * prevent the NMI handler from running the sprite-0 spin-wait with
     * rendering off, which would loop forever. */
    log_on_change("NMI_enable", (g_ppuctrl >> 7) & 1);
    maybe_inject_password();

    if (g_ppuctrl & 0x80) {
        /* Simulate hardware NMI push so RTI in the handler restores stack. */
        uint8_t p_save = (uint8_t)((g_cpu.N<<7)|(g_cpu.V<<6)|(1<<5)|
                                   (g_cpu.D<<3)|(g_cpu.I<<2)|(g_cpu.Z<<1)|g_cpu.C);
        g_ram[0x100+g_cpu.S] = 0x00;   g_cpu.S--;
        g_ram[0x100+g_cpu.S] = p_save; g_cpu.S--;
        func_NMI();
        if (s_cb_count <= 5) printf("[VBlank] NMI returned ok\n");
    }

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
        fprintf(stderr, "Usage: NESRecompGame <baserom.nes> [--script FILE] [--record FILE] [--loadstate FILE] [--password STRING]\n");
        return 1;
    }

    /* Parse optional flags */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--script") == 0 && i+1 < argc) s_script_path = argv[++i];
        else if (strcmp(argv[i], "--record") == 0 && i+1 < argc) s_record_path = argv[++i];
        else if (strcmp(argv[i], "--loadstate") == 0 && i+1 < argc) s_loadstate_path = argv[++i];
        else if (strcmp(argv[i], "--password") == 0 && i+1 < argc) s_password = argv[++i];
    }

    if (s_password)
        printf("[Password] Will auto-fill mantra: \"%s\"\n", s_password);

    if (!load_rom(argv[1])) return 1;

    runtime_init();

    if (s_loadstate_path) savestate_load(s_loadstate_path);
    if (s_record_path) { record_open(s_record_path); atexit(record_close); }
    if (s_script_path) script_load(s_script_path);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
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
