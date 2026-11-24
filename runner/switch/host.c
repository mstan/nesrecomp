/* Vanilla cartridge host. The shared cyc_host.c still owns ROM validation,
 * power-on, session setup, battery storage and the application's only main. */
#include <SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#include <dirent.h>
#include <strings.h>
#include <netinet/in.h>
#endif
#include "cyc_core.h"
#include "cyc_host.h"
#include "cyc_host_extras.h"
#include "cyc_ring.h"
#include "cyc_run.h"
#include "cyc_session.h"
#include "cyc_state.h"
#include "ppu_hle_kernel.h"

#ifndef CYC_SWITCH_GAME_DIR
#error CYC_SWITCH_GAME_DIR must name a private per-game data directory
#endif
#define ROM_PATH CYC_SWITCH_GAME_DIR "/rom.nes"
#define AUTO_PATH CYC_SWITCH_GAME_DIR "/saves/auto.cycstate"
#define SLOT_PATH CYC_SWITCH_GAME_DIR "/saves/slot.cycstate"
#ifndef CYC_SWITCH_SAVES
#define CYC_SWITCH_SAVES 0
#endif
#ifndef CYC_SWITCH_VERSION
#define CYC_SWITCH_VERSION "development"
#endif

static SDL_AudioDeviceID audio;
static bool ready, storage_failed, suspended;
static long exit_after = -1;
static bool uncapped;
static bool benchmark;
static long benchmark_warmup = 300;
static const char *replay_path;
static unsigned saves;
static bool network_log;
#ifdef __SWITCH__
static bool sockets_ready;
static int log_socket = -1;
static bool romfs_ready;
#endif

/* Network output is opt-in and infrequent. Keep stderr's SD log intact. */
static void runner_log(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    if (network_log) {
        va_list copy;
        va_copy(copy, ap);
        vfprintf(stdout, format, copy);
        va_end(copy);
        fflush(stdout);
    }
    vfprintf(stderr, format, ap);
    va_end(ap);
}

static void report_platform(void)
{
#ifdef __SWITCH__
    if (!network_log) return;
    u32 cpu_hz = 0;
    Result clock_result = clkrstInitialize();
    if (R_SUCCEEDED(clock_result)) {
        ClkrstSession session;
        clock_result = clkrstOpenSession(&session, PcvModuleId_CpuBus, 3);
        if (R_SUCCEEDED(clock_result)) {
            clock_result = clkrstGetClockRate(&session, &cpu_hz);
            clkrstCloseSession(&session);
        }
        clkrstExit();
    }
    runner_log("platform applet_type=%u operation_mode=%u performance_mode=%u cpu_hz=%u clock_result=0x%08X\n",
        (unsigned)appletGetAppletType(), (unsigned)appletGetOperationMode(),
        (unsigned)appletGetPerformanceMode(), cpu_hz, clock_result);
#endif
}
/* Report the kernel the HLE PPU actually compiled, not the host architecture. */
static const char *sprite_kernel_name(void)
{
#if !NESRECOMP_PPU_HLE
    return "unused";
#elif NESRECOMP_PPU_HLE_SSE2
    return "sse2";
#elif defined(NESRECOMP_PPU_HLE_NEON) && NESRECOMP_PPU_HLE_NEON
    return "neon";
#else
    return "portable";
#endif
}
typedef struct {
    Uint64 frames, emulation, audio, present, sleep;
    Uint64 source_samples, queued_bytes, queue_polls, queue_empty_polls;
    Uint32 queue_min, queue_max;
} FrameTimings;
static void add_totals(FrameTimings *total, const FrameTimings *part)
{
    total->frames += part->frames;
    total->emulation += part->emulation;
    total->audio += part->audio;
    total->present += part->present;
    total->source_samples += part->source_samples;
}
static void report_timings(const FrameTimings *t, Uint64 elapsed, Uint64 frequency,
                           Uint64 bytes_per_second)
{
    if (!t->frames || !elapsed) return;
    double per_frame = 1000.0 / (double)frequency / (double)t->frames;
    runner_log("performance fps=%.2f frames=%llu emulation_ms=%.3f audio_ms=%.3f present_ms=%.3f wait_ms=%.3f\n",
        (double)t->frames * (double)frequency / (double)elapsed, (unsigned long long)t->frames,
        t->emulation * per_frame, t->audio * per_frame, t->present * per_frame, t->sleep * per_frame);
    double wall_ms = 1000.0 * (double)elapsed / (double)frequency;
    double produced_ms = 1000.0 * (double)t->source_samples / 48000.0;
    double byte_ms = 1000.0 / (double)bytes_per_second;
    runner_log("audio_supply produced_ms=%.2f wall_ms=%.2f ratio=%.4f queued_ms=%.2f "
               "queue_empty_polls=%llu queue_polls=%llu queue_min_ms=%.2f queue_max_ms=%.2f\n",
        produced_ms, wall_ms, produced_ms / wall_ms, t->queued_bytes * byte_ms,
        (unsigned long long)t->queue_empty_polls, (unsigned long long)t->queue_polls,
        t->queue_min * byte_ms, t->queue_max * byte_ms);
    report_platform();
}
#if defined(__SWITCH__) && CYC_SWITCH_SAVES
static AppletHookCookie hook;
static bool hooked;
#endif

static bool mkdirs(const char *path)
{
    char copy[1024];
    if (strlen(path) >= sizeof(copy)) return false;
    strcpy(copy, path);
    for (char *p = copy + 1; ; ++p) {
        if (*p != '/' && *p) continue;
        char c = *p; *p = 0;
        /* The mount itself is already supplied by libnx. */
        size_t n = strlen(copy);
        if (n && copy[n - 1] != ':') {
            struct stat st;
            if ((mkdir(copy, 0755) && errno != EEXIST) || stat(copy, &st) || !S_ISDIR(st.st_mode)) return false;
        }
        *p = c;
        if (!c) return true;
    }
}

bool cyc_sdl_error_box(const char *title, const char *text)
{
    fprintf(stderr, "%s: %s\n", title, text);
#ifdef __SWITCH__
    /* SDL message boxes are not implemented by every Switch SDL driver. */
    SDL_Quit();
    consoleInit(NULL);
    printf("%s\n\n%s\n\nPress + to return to the homebrew menu.\n", title, text);
    PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
#endif
    return true;
}

int cyc_switch_init(int argc, char **argv)
{
    bool requested = false;
    network_log = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--nxlink-log")) requested = true;
        if (!strcmp(argv[i], "--window-input") && i + 1 < argc) replay_path = argv[++i];
    }
    if (!mkdirs(CYC_SWITCH_GAME_DIR)) {
        cyc_sdl_error_box("Storage unavailable", "Cannot create the game's directory on the SD card.");
        return 0;
    }
    /* Fixed filenames keep paths independent of hbmenu/forwarder working dirs. */
    if (chdir(CYC_SWITCH_GAME_DIR)) return 0;
    if (!freopen(CYC_SWITCH_GAME_DIR "/runtime.log", "w", stderr)) return 0;
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef __SWITCH__
    if (requested && __nxlink_host.s_addr) {
        sockets_ready = R_SUCCEEDED(socketInitializeDefault());
        if (sockets_ready) {
            log_socket = nxlinkConnectToHost(true, false);
            network_log = log_socket >= 0;
            if (network_log) setvbuf(stdout, NULL, _IONBF, 0);
        }
    }
#endif
    runner_log("Switch runner %s: saves=%s network_log=%s requested=%u\n", CYC_SWITCH_VERSION,
        CYC_SWITCH_SAVES ? "enabled" : "disabled", network_log ? "connected" : "off", requested);
    report_platform();
    return 1;
}

void cyc_switch_shutdown(void)
{
    fflush(stderr);
#ifdef __SWITCH__
    if (romfs_ready) { romfsExit(); romfs_ready = false; }
    if (log_socket >= 0) { fflush(stdout); close(log_socket); log_socket = -1; }
    if (sockets_ready) { socketExit(); sockets_ready = false; }
#endif
    network_log = false;
}


#ifdef __SWITCH__
#define ROM_CONFIG CYC_SWITCH_GAME_DIR "/rom.cfg"
#define SD_ROOT "sdmc:/"
#define PICKER_ROWS 16

typedef struct {
    char *name;
    bool directory;
} RomEntry;

static char selected_rom[768];
static bool selected_in_picker, can_choose_rom;

static int compare_entries(const void *left, const void *right)
{
    const RomEntry *a = left, *b = right;
    if (a->directory != b->directory) return a->directory ? -1 : 1;
    int cmp = strcasecmp(a->name, b->name);
    return cmp ? cmp : strcmp(a->name, b->name);
}

static bool child_path(char *out, size_t size, const char *dir, const char *name)
{
    const char *separator = dir[strlen(dir) - 1] == '/' ? "" : "/";
    int n = snprintf(out, size, "%s%s%s", dir, separator, name);
    return n >= 0 && (size_t)n < size;
}

static bool list_entries(const char *dir, RomEntry **entries, size_t *count)
{
    DIR *opened = opendir(dir);
    if (!opened) return false;
    struct dirent *entry;
    size_t capacity = 0;
    bool ok = true;
    while ((entry = readdir(opened))) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char path[sizeof selected_rom];
        struct stat st;
        if (!child_path(path, sizeof path, dir, name) || stat(path, &st)) continue;
        const char *ext = strrchr(name, '.');
        bool folder = S_ISDIR(st.st_mode);
        if (!folder && (!S_ISREG(st.st_mode) || !ext || strcasecmp(ext, ".nes"))) continue;
        if (*count == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            RomEntry *grown = realloc(*entries, capacity * sizeof **entries);
            if (!grown) { ok = false; break; }
            *entries = grown;
        }
        char *copy = strdup(name);
        if (!copy) { ok = false; break; }
        (*entries)[(*count)++] = (RomEntry){copy, folder};
    }
    closedir(opened);
    if (*count > 1) qsort(*entries, *count, sizeof **entries, compare_entries);
    return ok;
}

static void picker_text(const char *text, size_t columns)
{
    for (; *text && columns; ++text, --columns) {
        unsigned char c = (unsigned char)*text;
        putchar(c < ' ' || c == 127 ? '?' : c);
    }
}

static const char *choose_rom(const char *message)
{
    char dir[sizeof selected_rom] = CYC_SWITCH_GAME_DIR;
    if (message && selected_in_picker) {
        strcpy(dir, selected_rom);
        char *slash = strrchr(dir, '/');
        if (slash == dir + sizeof SD_ROOT - 2) slash[1] = 0;
        else *slash = 0;
    }
    char *const mount_end = dir + sizeof SD_ROOT - 1;
    bool chosen = false;
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    consoleInit(NULL);
    while (appletMainLoop()) {
        RomEntry *entries = NULL;
        size_t count = 0, cursor = 0;
        bool readable = list_entries(dir, &entries, &count);
        bool redraw = true, change_dir = false;
        while (appletMainLoop()) {
            if (redraw) {
                consoleClear();
                puts("Select your original NES cartridge (.nes)\n");
                picker_text(dir, 66);
                puts("\n");
                if (message) puts(message);
                if (!readable) puts("Cannot list this folder.");
                else if (!count) puts("This folder has no NES cartridges or subfolders.");
                size_t start = cursor / PICKER_ROWS * PICKER_ROWS;
                for (size_t i = start; readable && i < count && i < start + PICKER_ROWS; ++i) {
                    printf("%s%s ", i == cursor ? "> " : "  ", entries[i].directory ? "[Folder]" : "        ");
                    picker_text(entries[i].name, 57);
                    putchar('\n');
                }
                puts("\nD-pad: Move   A: Open or select   B: Parent folder   +: Cancel");
                redraw = false;
            }
            consoleUpdate(NULL);
            padUpdate(&pad);
            u64 buttons = padGetButtonsDown(&pad);
            if (buttons & HidNpadButton_Plus) break;
            if (buttons & HidNpadButton_B) {
                if (!*mount_end) break;
                char *slash = strrchr(dir, '/');
                if (slash < mount_end) *mount_end = 0;
                else *slash = 0;
                change_dir = true;
                break;
            }
            if (!readable || !count) continue;
            if ((buttons & HidNpadButton_AnyUp) && cursor) { --cursor; redraw = true; }
            if ((buttons & HidNpadButton_AnyDown) && cursor + 1 < count) { ++cursor; redraw = true; }
            if (!(buttons & HidNpadButton_A)) continue;
            if (!child_path(selected_rom, sizeof selected_rom, dir, entries[cursor].name)) continue;
            if (entries[cursor].directory) {
                strcpy(dir, selected_rom);
                change_dir = true;
            } else {
                chosen = true;
            }
            break;
        }
        for (size_t i = 0; i < count; ++i) free(entries[i].name);
        free(entries);
        if (!change_dir) break;
    }
    consoleExit(NULL);
    selected_in_picker = chosen;
    return chosen ? selected_rom : NULL;
}

static const char *cached_rom(void)
{
    FILE *f = fopen(ROM_CONFIG, "rb");
    if (!f) f = fopen(ROM_CONFIG ".bak", "rb");
    if (!f) return NULL;
    char line[sizeof selected_rom + 2];
    bool complete = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!complete) return NULL;
    char *end = strpbrk(line, "\r\n");
    if (!end || (size_t)(end - line) >= sizeof selected_rom ||
        strncmp(line, SD_ROOT, sizeof SD_ROOT - 1)) return NULL;
    *end = 0;
    strcpy(selected_rom, line);
    return selected_rom;
}

static void remember_selected_rom(void)
{
    if (!selected_in_picker) return;
    const char *temp = ROM_CONFIG ".tmp";
    FILE *f = fopen(temp, "wb");
    if (!f) return;
    bool written = fprintf(f, "%s\n", selected_rom) > 0;
    if (fclose(f) == 0 && written) {
        if (rename(temp, ROM_CONFIG) == 0) return;
        /* libnx can reject replacing a destination. Preserve the old choice
         * until the replacement is in place. */
        const char *backup = ROM_CONFIG ".bak";
        if (errno == EEXIST) {
            remove(backup);
            if (rename(ROM_CONFIG, backup) == 0) {
                if (rename(temp, ROM_CONFIG) == 0) {
                    remove(backup);
                    return;
                }
                rename(backup, ROM_CONFIG);
            }
        }
    }
    remove(temp);
    runner_log("Could not remember the selected cartridge.\n");
}
#endif

bool cyc_switch_retry_rom(const char **rom_path, const char *message)
{
#ifdef __SWITCH__
    if (!can_choose_rom) return false;
    *rom_path = choose_rom(message);
    return true;
#else
    (void)rom_path;
    (void)message;
    return false;
#endif
}

int cyc_sdl_prelaunch(const char **rom_path, const char *bios, NesFdsHleAsk *hle,
                      const char **saved_bios)
{
    (void)bios;
    if (cyc_host_saves_enabled() && !mkdirs(CYC_SWITCH_GAME_DIR "/saves")) {
        cyc_sdl_error_box("Storage unavailable", "Cannot create the save directory on the SD card.");
        return 2;
    }
    if (!*rom_path) {
        *rom_path = ROM_PATH;
#ifdef __SWITCH__
        can_choose_rom = !benchmark && exit_after < 0 && !replay_path;
        if (can_choose_rom) {
            PadState pad;
            padConfigureInput(1, HidNpadStyleSet_NpadStandard);
            padInitializeDefault(&pad);
            padUpdate(&pad);
            const char *cached = cached_rom();
            if (cached) *rom_path = cached;
            struct stat st;
            if ((padGetButtons(&pad) & HidNpadButton_Y) ||
                stat(*rom_path, &st) != 0 || !S_ISREG(st.st_mode))
                *rom_path = choose_rom(NULL);
            if (!*rom_path) return 1;
        }
#endif
    }
    *hle = NES_FDS_HLE_ASK_NONE;
    *saved_bios = NULL;
#ifdef __SWITCH__
    if (replay_path && !strncmp(replay_path, "romfs:/", 7) && !romfs_ready) {
        Result rc = romfsInit();
        if (R_FAILED(rc)) {
            runner_log("Cannot mount input assets: 0x%08X\n", rc);
            return 2;
        }
        romfs_ready = true;
    }
#endif
    return 0;
}
void cyc_sdl_option(const char *name, const char *value)
{
    if (!strcmp(name, "--exit-after")) exit_after = strtol(value, NULL, 10);
    if (!strcmp(name, "--uncapped")) uncapped = true;
    if (!strcmp(name, "--benchmark")) benchmark = true;
    if (!strcmp(name, "--benchmark-warmup")) {
        char *end;
        long frames = strtol(value, &end, 10);
        benchmark_warmup = *value && !*end ? frames : -1;
    }
}
void cyc_sdl_present_out(const char *path, long every) { (void)path; (void)every; }
void cyc_sdl_image_path(const char *path) { (void)path; }

static bool save_to(const char *path)
{
    if (!cyc_host_saves_enabled()) return true;
    if (!ready || storage_failed) return false;
    char err[256];
    if (!cyc_state_save_file(path, err, sizeof(err))) {
        fprintf(stderr, "Save failed: %s\n", err);
        storage_failed = true;
        return false;
    }
    ++saves;
    fprintf(stderr, "Saved frame %llu to %s\n", (unsigned long long)cyc_ring_frame, path);
    return true;
}

#if defined(__SWITCH__) && CYC_SWITCH_SAVES
/* libnx dispatches these hooks while the main thread processes applet
 * messages. appletMainLoop/SDL_PollEvent are only called between NES frames. */
static void applet_event(AppletHookType type, void *unused)
{
    (void)unused;
    if (!ready) return;
    if (type == AppletHookType_OnExitRequest ||
        (type == AppletHookType_OnFocusState && appletGetFocusState() != AppletFocusState_InFocus))
        save_to(AUTO_PATH);
}
#endif

static bool load_from(const char *path, bool optional)
{
    if (!cyc_host_saves_enabled()) return true;
    struct stat st;
    if (stat(path, &st)) {
        if (optional && errno == ENOENT) return true;
        fprintf(stderr, "Cannot access state %s: %s\n", path, strerror(errno));
        return false;
    }
    char err[256];
    if (!cyc_state_load_file(path, err, sizeof(err))) {
        fprintf(stderr, "Cannot load state: %s\n", err);
        return false;
    }
    cyc_session_state_loaded();
    if (audio) SDL_ClearQueuedAudio(audio);
    return true;
}

static SDL_GameController *open_pad(void)
{
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) {
            SDL_GameController *pad = SDL_GameControllerOpen(i);
            if (pad) return pad;
        }
    return NULL;
}

static uint8_t buttons(SDL_GameController *pad)
{
    if (!pad) return 0;
    uint8_t result = 0;
    /* SDL uses physical Xbox positions: east B = Nintendo A, south A = B. */
    const SDL_GameControllerButton map[8] = {
        SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A,
        SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT
    };
    for (unsigned i = 0; i < 8; ++i)
        if (SDL_GameControllerGetButton(pad, map[i])) result |= 0x80u >> i;
    int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (x < -16000) result |= 2;
    if (x > 16000) result |= 1;
    if (y < -16000) result |= 8;
    if (y > 16000) result |= 4;
    return result;
}

int cyc_sdl_main(const char *title, int scale)
{
#ifdef __SWITCH__
    remember_selected_rom();
#endif
    (void)scale;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    SDL_AudioStream *stream = NULL;
    SDL_GameController *pad = NULL;
    const char *fatal = NULL;
    int result = 2;

    audio = 0; ready = storage_failed = suspended = false;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER)) {
        fatal = "Cannot initialize SDL video, audio and controllers."; goto cleanup;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_SHOWN);
    if (window) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (window && !renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) { fatal = "Cannot create the SDL video renderer."; goto cleanup; }
    int vsync_result = SDL_RenderSetVSync(renderer, 0);
    SDL_RendererInfo renderer_info;
    if (!SDL_GetRendererInfo(renderer, &renderer_info))
        runner_log("Renderer=%s flags=%u vsync_off_result=%d PPU=%s\n",
            renderer_info.name, renderer_info.flags, vsync_result, cyc_ppu_implementation());
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 240);
    if (!texture) { fatal = "Cannot create the NES frame texture."; goto cleanup; }

    SDL_AudioSpec want = {0}, have;
    want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (!audio || have.freq <= 0 || !have.channels || !SDL_AUDIO_BITSIZE(have.format)) {
        fatal = "Cannot open the audio device."; goto cleanup;
    }
    stream = SDL_NewAudioStream(AUDIO_S16SYS, 1, 48000, have.format, have.channels, have.freq);
    if (!stream || !cyc_audio_enable(48000)) { fatal = "Cannot configure NES audio conversion."; goto cleanup; }
    runner_log("Audio %d Hz channels=%u format=%u buffer_frames=%u source_rate=48000\n",
        have.freq, have.channels, have.format, have.samples);
    const Uint64 bytes_per_second = (Uint64)have.freq * have.channels * (SDL_AUDIO_BITSIZE(have.format) / 8);
    SDL_PauseAudioDevice(audio, 0);
    pad = open_pad();
    SDL_GameControllerUpdate();
    const bool saves_enabled = cyc_host_saves_enabled();
    /* Hold Nintendo X (SDL north/Y) at launch to deliberately start fresh. */
    bool fresh = pad && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y);
    if (saves_enabled && !fresh && !load_from(AUTO_PATH, true)) {
        fatal = "Cannot read the automatic save. It has been preserved. See runtime.log."; goto cleanup;
    }
    ready = true;
#if defined(__SWITCH__) && CYC_SWITCH_SAVES
    if (saves_enabled) {
        appletHook(&hook, applet_event, NULL); hooked = true;
        if (R_FAILED(appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify))) {
            fatal = "Cannot configure suspend notifications."; goto cleanup;
        }
    }
#endif
    bool running = true, old_save = false, old_load = false;
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    const double period = cyc_host_frame_seconds() * (double)frequency;
    runner_log("Region=%s target_fps=%.3f\n",
        cyc_region() == CYC_REGION_PAL ? "PAL" : "NTSC", 1.0 / cyc_host_frame_seconds());
    runner_log("Sprite_kernel=%s benchmark=%u warmup_frames=%ld input=%s\n", sprite_kernel_name(),
        benchmark, benchmark ? benchmark_warmup : 0, replay_path ? "scripted" : benchmark ? "neutral" : "controller");
    if (benchmark && (CYC_SWITCH_SAVES || benchmark_warmup < 1 || exit_after <= benchmark_warmup)) {
        fatal = "Benchmark requires saves OFF and --exit-after greater than its positive warm-up."; goto cleanup;
    }
    double next = (double)SDL_GetPerformanceCounter();
    Uint64 started = SDL_GetPerformanceCounter(), checkpoint = started, frames = 0, dropped = 0;
    Uint64 report_at = started;
    FrameTimings timings = {0};
    FrameTimings measured = {0};
    Uint64 benchmark_started = 0, benchmark_ended = 0;
    unsigned benchmark_interruptions = 0;
    Uint64 replay_frames = 0;
    while (running) {
#ifdef __SWITCH__
        if (!appletMainLoop()) break;
#endif
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_APP_WILLENTERBACKGROUND ||
                (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
                save_to(AUTO_PATH); suspended = true;
                if (benchmark && frames >= (Uint64)benchmark_warmup) ++benchmark_interruptions;
                SDL_ClearQueuedAudio(audio); SDL_AudioStreamClear(stream);
                SDL_PauseAudioDevice(audio, 1);
            }
            if (event.type == SDL_APP_DIDENTERFOREGROUND ||
                (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)) {
                suspended = false; next = (double)SDL_GetPerformanceCounter();
                SDL_PauseAudioDevice(audio, 0);
            }
        }
        if (!running || storage_failed) break;
        if (pad && !SDL_GameControllerGetAttached(pad)) { SDL_GameControllerClose(pad); pad = NULL; }
        if (!pad) pad = open_pad();
        if (suspended) { SDL_Delay(10); continue; }
        bool save = saves_enabled && pad && SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
        bool load = saves_enabled && pad && SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
        if (save && !old_save) save_to(SLOT_PATH);
        if (load && !old_load) {
            if (!load_from(SLOT_PATH, false)) {
                fatal = "Cannot load the quick-save slot. See runtime.log."; break;
            }
            SDL_AudioStreamClear(stream); next = (double)SDL_GetPerformanceCounter();
        }
        old_save = save; old_load = load;
        if (pad && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) &&
            SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) &&
            SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) break;
        uint8_t input[2] = {benchmark ? 0 : buttons(pad), 0};
        if (cyc_host_window_replay((long)frames, input, 2)) ++replay_frames;
        cyc_session_logical_input(input, 2); cyc_session_input(input);
        cyc_set_controller(0, input[0]); cyc_set_controller(1, input[1]);
        Uint64 stage = SDL_GetPerformanceCounter();
        cyc_session_frame_begin(); cyc_run_frame(); cyc_session_frame_end();
        cyc_host_frame_done((long)cyc_ring_frame);
        Uint64 stage_end = SDL_GetPerformanceCounter();
        timings.emulation += stage_end - stage; stage = stage_end;
        ++timings.frames;
        ++frames;
        int16_t pcm[4096]; size_t count;
        while ((count = cyc_audio_read(pcm, 4096))) {
            timings.source_samples += count;
            if (SDL_AudioStreamPut(stream, pcm, (int)(count * sizeof(*pcm)))) {
                fatal = "Audio conversion failed."; running = false; break;
            }
        }
        Uint32 limit = (Uint32)(have.freq / 10) * have.channels * (SDL_AUDIO_BITSIZE(have.format) / 8);
        unsigned char converted[16384]; int bytes;
        while ((bytes = SDL_AudioStreamGet(stream, converted, sizeof(converted))) > 0) {
            Uint32 queued = SDL_GetQueuedAudioSize(audio);
            if (!timings.queue_polls || queued < timings.queue_min) timings.queue_min = queued;
            if (queued > timings.queue_max) timings.queue_max = queued;
            ++timings.queue_polls;
            if (!queued) ++timings.queue_empty_polls;
            if (queued + (Uint32)bytes > limit) { ++dropped; continue; }
            if (SDL_QueueAudio(audio, converted, (Uint32)bytes)) {
                fatal = "Audio queue failed."; running = false; break;
            }
            timings.queued_bytes += (Uint32)bytes;
        }
        if (bytes < 0) { fatal = "Audio conversion failed."; break; }
        stage_end = SDL_GetPerformanceCounter();
        timings.audio += stage_end - stage; stage = stage_end;
        int width, height;
        SDL_GetRendererOutputSize(renderer, &width, &height);
        /* 4:3 displayed picture; preserve the complete 256x240 framebuffer. */
        int h = height, w = h * 4 / 3;
        if (w > width) { w = width; h = w * 3 / 4; }
        SDL_Rect dest = {(width - w) / 2, (height - h) / 2, w, h};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderClear(renderer);
        if (SDL_UpdateTexture(texture, NULL, cyc_frame_argb(), 256 * sizeof(uint32_t)) ||
            SDL_RenderCopy(renderer, texture, NULL, &dest)) { fatal = "Video presentation failed."; break; }
        SDL_RenderPresent(renderer);
        Uint64 now = SDL_GetPerformanceCounter();
        timings.present += now - stage;
        benchmark_ended = now;
        if (benchmark && frames == (Uint64)benchmark_warmup) {
            /* A fixed emulated warm-up length, independent of host speed. */
            memset(&timings, 0, sizeof(timings));
            report_at = benchmark_started = now;
            runner_log("benchmark_warmup_complete frames=%ld input=%s\n", benchmark_warmup,
                replay_path ? "scripted" : "neutral");
        }
        if (now - report_at >= frequency * 5 && (!benchmark || frames > (Uint64)benchmark_warmup)) {
            if (benchmark) add_totals(&measured, &timings);
            report_timings(&timings, now - report_at, frequency, bytes_per_second);
            memset(&timings, 0, sizeof(timings)); report_at = now;
        }
        if (now - checkpoint >= frequency * 30) { save_to(AUTO_PATH); checkpoint = now; }
        if (exit_after >= 0 && frames >= (Uint64)exit_after) break;
        if (!uncapped) {
            now = SDL_GetPerformanceCounter();
            Uint64 wait_started = now;
            next += period;
            double remaining = (next - (double)now) * 1000.0 / (double)frequency;
            if (remaining > 1) SDL_Delay((Uint32)(remaining - 1));
            /* The last sub-millisecond interval must not become a scheduler
             * yield that overshoots the guest deadline on the Switch. */
            while ((double)SDL_GetPerformanceCounter() < next) {}
            if ((double)now - next > period * 4) next = (double)now;
            timings.sleep += SDL_GetPerformanceCounter() - wait_started;
        }
    }
    if (!fatal && !storage_failed && !save_to(AUTO_PATH)) storage_failed = true;
    if (storage_failed) fatal = "Could not save progress to the SD card. See runtime.log.";
    if (benchmark && benchmark_started && frames > (Uint64)benchmark_warmup) {
        add_totals(&measured, &timings);
        double seconds = (double)(benchmark_ended - benchmark_started) / frequency;
        double per_frame = 1000.0 / (double)frequency / (double)measured.frames;
        runner_log("benchmark kernel=%s frames=%llu warmup_frames=%ld total_frames=%llu "
                   "seconds=%.6f fps=%.3f emulation_ms=%.6f audio_ms=%.6f present_ms=%.6f "
                   "audio_supply=%.6f interruptions=%u complete=%u input=%s replay_frames=%llu\n",
            sprite_kernel_name(), (unsigned long long)measured.frames, benchmark_warmup, (unsigned long long)frames,
            seconds, measured.frames / seconds, measured.emulation * per_frame,
            measured.audio * per_frame, measured.present * per_frame,
            (double)measured.source_samples / 48000.0 / seconds, benchmark_interruptions,
            !fatal && !benchmark_interruptions && frames == (Uint64)exit_after && (!replay_path || replay_frames == frames),
            replay_path ? "scripted" : "neutral", (unsigned long long)replay_frames);
        /* Outside measured time; no per-frame hashing disturbs the benchmark. */
        runner_log("benchmark_state mem=%016llX hw=%016llX\n",
            (unsigned long long)cyc_mem_state_hash(), (unsigned long long)cyc_hw_state_hash());
    }
    report_timings(&timings, SDL_GetPerformanceCounter() - report_at, frequency, bytes_per_second);
    runner_log("frames=%llu seconds=%.3f native_cycles=%llu interp_rom=%llu interp_ram=%llu dropped_audio_chunks=%llu saves=%u\n",
        (unsigned long long)frames, (double)(SDL_GetPerformanceCounter() - started) / frequency,
        (unsigned long long)cyc_run_native_cycles, (unsigned long long)cyc_run_interp_rom_cycles,
        (unsigned long long)cyc_run_interp_ram_cycles, (unsigned long long)dropped, saves);
    result = fatal ? 2 : 0;
cleanup:
    ready = false;
#if defined(__SWITCH__) && CYC_SWITCH_SAVES
    if (hooked) { appletUnhook(&hook); hooked = false; }
#endif
    if (fatal) fprintf(stderr, "%s SDL: %s\n", fatal, SDL_GetError());
    if (pad) SDL_GameControllerClose(pad);
    if (audio) SDL_CloseAudioDevice(audio);
    audio = 0;
    SDL_FreeAudioStream(stream);
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    if (fatal) cyc_sdl_error_box("Cannot continue", fatal);
    return result;
}
