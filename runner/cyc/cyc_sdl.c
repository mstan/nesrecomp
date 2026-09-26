/*
 * cyc_sdl.c - SDL2 window and audio for NESRecomp --cycle-accurate builds.
 *
 * Keys: arrows = D-pad, X = A, Z = B, Enter = Start, Right Shift = Select,
 *       Tab (hold) = fast forward, F2 = switch between recompiled code and
 *       the interpreter (live; both produce the same machine, cycle for
 *       cycle), F12 = screenshot (cyc_shot_NNNN.png), Esc = quit.
 * The first connected game controller also works.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "cyc_core.h"
#include "cyc_png.h"
#include "cyc_run.h"

#include <stdio.h>

#define NES_A      0x80
#define NES_B      0x40
#define NES_SELECT 0x20
#define NES_START  0x10
#define NES_UP     0x08
#define NES_DOWN   0x04
#define NES_LEFT   0x02
#define NES_RIGHT  0x01

#define AUDIO_RATE 48000

static uint8_t read_input(SDL_GameController *pad) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint8_t b = 0;
    if (k[SDL_SCANCODE_X]) b |= NES_A;
    if (k[SDL_SCANCODE_Z]) b |= NES_B;
    if (k[SDL_SCANCODE_RSHIFT]) b |= NES_SELECT;
    if (k[SDL_SCANCODE_RETURN]) b |= NES_START;
    if (k[SDL_SCANCODE_UP]) b |= NES_UP;
    if (k[SDL_SCANCODE_DOWN]) b |= NES_DOWN;
    if (k[SDL_SCANCODE_LEFT]) b |= NES_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) b |= NES_RIGHT;
    if (pad) {
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) b |= NES_A;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) b |= NES_B;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) b |= NES_SELECT;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) b |= NES_START;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= NES_UP;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= NES_DOWN;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= NES_LEFT;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= NES_RIGHT;
    }
    /* The NES pad cannot press opposite directions at once. */
    if ((b & (NES_UP | NES_DOWN)) == (NES_UP | NES_DOWN)) b &= (uint8_t)~(NES_UP | NES_DOWN);
    if ((b & (NES_LEFT | NES_RIGHT)) == (NES_LEFT | NES_RIGHT)) b &= (uint8_t)~(NES_LEFT | NES_RIGHT);
    return b;
}

int cyc_sdl_main(const char *title, int scale) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (scale < 1) scale = 1;
    SDL_Window *win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 256 * scale,
                                       240 * scale, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 240)
                           : NULL;
    if (!tex) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_RenderSetLogicalSize(ren, 256, 240);

    /* Audio is queued as frames produce it; fast forward drops it. */
    SDL_AudioSpec want = {0}, have;
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev && cyc_audio_enable(have.freq)) SDL_PauseAudioDevice(dev, 0);
    else if (!dev) fprintf(stderr, "SDL audio: %s (continuing without sound)\n", SDL_GetError());

    SDL_GameController *pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks() && !pad; i++)
        if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);

    const double frame_seconds = 1.0 / 60.0988;
    const Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 next = SDL_GetPerformanceCounter();
    Uint64 fps_mark = next;
    uint64_t native_mark = cyc_run_native_cycles;
    uint64_t cycles_mark = cyc_cycle_count();
    int frames = 0, shot = 0;
    bool running = true;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                switch (ev.key.keysym.scancode) {
                case SDL_SCANCODE_ESCAPE: running = false; break;
                case SDL_SCANCODE_F2: cyc_run_native = !cyc_run_native; break;
                case SDL_SCANCODE_F12: {
                    char name[64];
                    snprintf(name, sizeof(name), "cyc_shot_%04d.png", shot++);
                    if (cyc_write_png(name, cyc_frame_argb(), 256, 240)) printf("saved %s\n", name);
                    break;
                }
                default: break;
                }
            }
            if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(ev.cdevice.which);
        }

        cyc_set_controller(0, read_input(pad));
        cyc_run_frame();
        frames++;

        const bool fast = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB] != 0;
        int16_t pcm[4096];
        size_t n;
        while ((n = cyc_audio_read(pcm, 4096)) > 0) {
            /* Keep latency bounded: skip a frame's audio if ~100 ms are queued. */
            if (dev && !fast && SDL_GetQueuedAudioSize(dev) < (Uint32)(have.freq / 10) * 2)
                SDL_QueueAudio(dev, pcm, (Uint32)(n * sizeof(int16_t)));
        }

        SDL_UpdateTexture(tex, NULL, cyc_frame_argb(), 256 * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        Uint64 now = SDL_GetPerformanceCounter();
        if (now - fps_mark >= freq) {
            double secs = (double)(now - fps_mark) / (double)freq;
            uint64_t cycles = cyc_cycle_count() - cycles_mark;
            double native_pct = cycles ? 100.0 * (double)(cyc_run_native_cycles - native_mark) / (double)cycles : 0.0;
            char t[256];
            snprintf(t, sizeof(t), "%s - %.1f%% of CPU cycles recompiled%s - %.0f fps", title, native_pct,
                     cyc_run_native ? "" : " [interpreter only, F2]", frames / secs);
            SDL_SetWindowTitle(win, t);
            fps_mark = now;
            native_mark = cyc_run_native_cycles;
            cycles_mark = cyc_cycle_count();
            frames = 0;
        }

        if (fast) {
            next = now;
            continue;
        }
        next += (Uint64)(frame_seconds * (double)freq);
        if (next > now) {
            Uint32 ms = (Uint32)((next - now) * 1000 / freq);
            if (ms > 1) SDL_Delay(ms - 1);
            while (SDL_GetPerformanceCounter() < next) {}
        } else if (now - next > freq / 4) {
            next = now; /* fell behind: don't try to catch up */
        }
    }

    if (dev) SDL_CloseAudioDevice(dev);
    if (pad) SDL_GameControllerClose(pad);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
