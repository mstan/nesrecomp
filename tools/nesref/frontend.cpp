/* nesref — minimal SDL2 libretro frontend: a known-good NES interpreter used
 * as the differential oracle for the NES recompiler. Loads a libretro NES core
 * (e.g. nestopia_libretro.dll / fceumm_libretro.dll), plays a ROM with reliable
 * SDL keyboard input, and — the whole point — exposes the SAME TCP debug-server
 * protocol the recomp runner does, so the exact same probe tooling talks to
 * either side and you diff them 1:1.
 *
 *   nesref.exe <core.dll> <rom.nes>
 *
 * Interface parity with nesrecomp's runner/src/debug_server.c: same loopback
 * port (4370), same line-delimited JSON commands, same response shapes. The
 * memory-level commands are backed by the libretro core's exposed memory:
 *   read_ram/dump_ram/write_ram  -> SYSTEM_RAM (2KB WRAM) + SAVE_RAM ($6000-$7FFF)
 *   read_ppu                     -> VIDEO_RAM (nametable CIRAM) when the core exposes it
 *   read_frame_ram / history     -> per-frame WRAM+SRAM ring (like the runner's ring)
 *   save_state / load_state, ping, frame, help, quit
 * PPU internals (t/v/w, OAM, palette) and CPU registers are NOT exposed by a
 * stock libretro core — those stay the in-process patched-Nestopia oracle's job.
 * Commands that can't be backed return the same {"ok":false,"error":...} shape.
 *
 * Keys (match the recomp NES keybinds): arrows=D-pad, Z=A, X=B, Tab=Select,
 *   Enter=Start.  F1-F9 = load state slot, Shift+F1-F9 = save slot, Esc = quit.
 * Env: NESREF_TRACE=1 writes a per-frame WRAM-diff nes_trace.jsonl; NESREF_WAV
 *   names an audio dump; NESREF_PORT overrides 4370; NESREF_QUIT_FRAMES=N exits.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <vector>
#include "libretro.h"

// ---- core function pointers ----
static HMODULE g_core;
#define LR(sym) static decltype(&sym) p_##sym;
LR(retro_init) LR(retro_deinit) LR(retro_api_version)
LR(retro_get_system_info) LR(retro_get_system_av_info)
LR(retro_set_environment) LR(retro_set_video_refresh)
LR(retro_set_audio_sample) LR(retro_set_audio_sample_batch)
LR(retro_set_input_poll) LR(retro_set_input_state)
LR(retro_set_controller_port_device)
LR(retro_load_game) LR(retro_unload_game) LR(retro_run)
LR(retro_serialize_size) LR(retro_serialize) LR(retro_unserialize)
LR(retro_get_memory_data) LR(retro_get_memory_size)
#undef LR

template<class T> static void bindsym(T& fn, const char* name) {
    fn = (T)GetProcAddress(g_core, name);
    if (!fn) { fprintf(stderr, "missing core symbol: %s\n", name); exit(2); }
}

// ---- video / input state ----
static SDL_Window*   g_win;
static SDL_Renderer* g_ren;
static SDL_Texture*  g_tex;
static int g_tex_w = 0, g_tex_h = 0;
static retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static SDL_GameController* g_pad = nullptr;
static uint32_t g_frame = 0;
static char g_core_name[64] = "libretro";

static void open_first_pad() {
    if (g_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) {
            g_pad = SDL_GameControllerOpen(i);
            if (g_pad) { printf("[controller: %s]\n", SDL_GameControllerName(g_pad)); fflush(stdout); return; }
        }
}

// ---- libretro memory accessors ----
// Mirrors nesrecomp debug_server.c read_byte(): $0000-$1FFF WRAM (+mirrors),
// $6000-$7FFF SRAM, $2000-$2FFF nametable, else 0. WRAM/SRAM/CIRAM come from the
// core; PPU palette/OAM and CPU regs are not exposed by stock libretro.
static uint8_t* mem_ptr(unsigned which, size_t* out_sz) {
    uint8_t* p = (uint8_t*)p_retro_get_memory_data(which);
    size_t sz  = p_retro_get_memory_size(which);
    if (out_sz) *out_sz = p ? sz : 0;
    return p;
}
static uint8_t read_byte(uint32_t addr) {
    size_t sz;
    if (addr < 0x2000) {                                   // WRAM + mirrors
        uint8_t* w = mem_ptr(RETRO_MEMORY_SYSTEM_RAM, &sz);
        if (w && sz) return w[addr & (sz - 1 >= 0x7FF ? 0x7FF : sz - 1)];
        return 0;
    }
    if (addr >= 0x6000 && addr < 0x8000) {                 // battery SRAM
        uint8_t* s = mem_ptr(RETRO_MEMORY_SAVE_RAM, &sz);
        uint32_t off = addr - 0x6000;
        if (s && off < sz) return s[off];
        return 0;
    }
    if (addr >= 0x2000 && addr < 0x3000) {                 // nametable (CIRAM)
        uint8_t* v = mem_ptr(RETRO_MEMORY_VIDEO_RAM, &sz);
        if (v && sz) return v[(addr - 0x2000) & (sz - 1)];
        return 0;
    }
    return 0;
}
static void write_byte(uint32_t addr, uint8_t val) {
    size_t sz;
    if (addr < 0x2000) {
        uint8_t* w = mem_ptr(RETRO_MEMORY_SYSTEM_RAM, &sz);
        if (w && sz) w[addr & (sz - 1 >= 0x7FF ? 0x7FF : sz - 1)] = val;
    } else if (addr >= 0x6000 && addr < 0x8000) {
        uint8_t* s = mem_ptr(RETRO_MEMORY_SAVE_RAM, &sz);
        uint32_t off = addr - 0x6000;
        if (s && off < sz) s[off] = val;
    }
}

// ---- per-frame ring buffer (WRAM + SRAM snapshots) ----
// Same idea as the runner's NESFrameRecord ring: lets read_frame_ram/history
// query historical state so you can diff a captured window against the recomp.
#define RING_CAP 3600                     // ~60s @60fps; 10KB/frame -> ~36MB
struct NesRefRecord { uint32_t frame; uint8_t wram[0x800]; uint8_t sram[0x2000]; };
static NesRefRecord* g_ring = nullptr;    // heap-allocated on demand
static uint64_t g_ring_count = 0;         // total frames ever recorded

static void ring_record() {
    if (!g_ring) { g_ring = (NesRefRecord*)calloc(RING_CAP, sizeof(NesRefRecord)); if (!g_ring) return; }
    NesRefRecord* r = &g_ring[g_frame % RING_CAP];
    r->frame = g_frame;
    size_t sz;
    uint8_t* w = mem_ptr(RETRO_MEMORY_SYSTEM_RAM, &sz);
    memset(r->wram, 0, sizeof(r->wram));
    if (w) memcpy(r->wram, w, sz < sizeof(r->wram) ? sz : sizeof(r->wram));
    uint8_t* s = mem_ptr(RETRO_MEMORY_SAVE_RAM, &sz);
    memset(r->sram, 0, sizeof(r->sram));
    if (s) memcpy(r->sram, s, sz < sizeof(r->sram) ? sz : sizeof(r->sram));
    g_ring_count = (uint64_t)g_frame + 1;
}

// ---- optional per-frame WRAM-diff trace (NESREF_TRACE=1) ----
static FILE*  g_trace;
static int    g_trace_on = 0;
static uint8_t g_trace_prev[0x800];
static int     g_trace_primed = 0;
static void trace_tick() {
    if (!g_trace_on) return;
    size_t sz; uint8_t* w = mem_ptr(RETRO_MEMORY_SYSTEM_RAM, &sz);
    if (!w || !sz) return;
    int n = (int)(sz < 0x800 ? sz : 0x800);
    if (!g_trace_primed) { memcpy(g_trace_prev, w, n); g_trace_primed = 1; return; }
    if (!g_trace) { g_trace = fopen("nes_trace.jsonl", "a"); if (!g_trace) { g_trace_on = 0; return; } }
    for (int a = 0; a < n; a++)
        if (w[a] != g_trace_prev[a]) {
            fprintf(g_trace, "{\"f\":%u,\"addr\":\"0x%04x\",\"region\":\"ram\",\"old\":\"0x%02x\",\"new\":\"0x%02x\"}\n",
                    g_frame, a, g_trace_prev[a], w[a]);
            g_trace_prev[a] = w[a];
        }
    if ((g_frame % 30) == 0) fflush(g_trace);
}

// ---- libretro callbacks ----
static bool cb_environment(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: g_fmt = *(const retro_pixel_format*)data; return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:   *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: if (data) *(bool*)data = false; return true;
        default: return false;
    }
}
static void ensure_texture(unsigned w, unsigned h) {
    if ((int)w == g_tex_w && (int)h == g_tex_h && g_tex) return;
    if (g_tex) SDL_DestroyTexture(g_tex);
    Uint32 sf = (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888) ? SDL_PIXELFORMAT_ARGB8888
              : (g_fmt == RETRO_PIXEL_FORMAT_RGB565)   ? SDL_PIXELFORMAT_RGB565
              :                                          SDL_PIXELFORMAT_ARGB1555;
    g_tex = SDL_CreateTexture(g_ren, sf, SDL_TEXTUREACCESS_STREAMING, w, h);
    g_tex_w = w; g_tex_h = h;
}
static void cb_video(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (data && w && h) { ensure_texture(w, h); SDL_UpdateTexture(g_tex, nullptr, data, (int)pitch); }
    SDL_RenderClear(g_ren);
    if (g_tex) SDL_RenderCopy(g_ren, g_tex, nullptr, nullptr);
    SDL_RenderPresent(g_ren);
}
// ---- audio capture (NESREF_WAV) ----
static FILE*    g_wav;
static uint64_t g_wav_frames;
static uint32_t g_wav_rate = 48000;
static void wav_open(const char* path, double rate) {
    g_wav = fopen(path, "wb"); if (!g_wav) { fprintf(stderr, "cannot open %s\n", path); return; }
    g_wav_rate = (uint32_t)(rate + 0.5); uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, g_wav);
}
static void wav_close() {
    if (!g_wav) return;
    uint32_t data_bytes = (uint32_t)(g_wav_frames * 4);
    uint32_t riff = 36 + data_bytes, fmt32 = 16, brate = g_wav_rate * 4;
    uint16_t pcm = 1, ch = 2, balign = 4, bits = 16;
    fseek(g_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, g_wav); fwrite(&riff, 4, 1, g_wav); fwrite("WAVEfmt ", 1, 8, g_wav);
    fwrite(&fmt32, 4, 1, g_wav); fwrite(&pcm, 2, 1, g_wav); fwrite(&ch, 2, 1, g_wav);
    fwrite(&g_wav_rate, 4, 1, g_wav); fwrite(&brate, 4, 1, g_wav);
    fwrite(&balign, 2, 1, g_wav); fwrite(&bits, 2, 1, g_wav);
    fwrite("data", 1, 4, g_wav); fwrite(&data_bytes, 4, 1, g_wav);
    fclose(g_wav); g_wav = nullptr;
}
static void  cb_audio_sample(int16_t l, int16_t r) { if (g_wav) { int16_t s[2] = {l, r}; fwrite(s, 4, 1, g_wav); g_wav_frames++; } }
static size_t cb_audio_batch(const int16_t* data, size_t frames) { if (g_wav && data && frames) { fwrite(data, 4, frames, g_wav); g_wav_frames += frames; } return frames; }
static void  cb_input_poll(void) {}
static int16_t cb_input_state(unsigned port, unsigned device, unsigned, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    SDL_Scancode sc; SDL_GameControllerButton gb;
    switch (id) {                                          // NES recomp keybinds
        case RETRO_DEVICE_ID_JOYPAD_A:      sc = SDL_SCANCODE_Z;      gb = SDL_CONTROLLER_BUTTON_A; break;
        case RETRO_DEVICE_ID_JOYPAD_B:      sc = SDL_SCANCODE_X;      gb = SDL_CONTROLLER_BUTTON_B; break;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: sc = SDL_SCANCODE_TAB;    gb = SDL_CONTROLLER_BUTTON_BACK; break;
        case RETRO_DEVICE_ID_JOYPAD_START:  sc = SDL_SCANCODE_RETURN; gb = SDL_CONTROLLER_BUTTON_START; break;
        case RETRO_DEVICE_ID_JOYPAD_UP:     sc = SDL_SCANCODE_UP;     gb = SDL_CONTROLLER_BUTTON_DPAD_UP; break;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   sc = SDL_SCANCODE_DOWN;   gb = SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   sc = SDL_SCANCODE_LEFT;   gb = SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  sc = SDL_SCANCODE_RIGHT;  gb = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
        default: return 0;
    }
    if (ks[sc]) return 1;
    if (g_pad && SDL_GameControllerGetButton(g_pad, gb)) return 1;
    if (g_pad) {
        const int DZ = 16000;
        if (id == RETRO_DEVICE_ID_JOYPAD_LEFT  && SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX) < -DZ) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT && SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX) >  DZ) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_UP    && SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY) < -DZ) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_DOWN  && SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY) >  DZ) return 1;
    }
    return 0;
}

// ---- save states (Fn=load slot, Shift+Fn=save slot) ----
static void slot_path(int slot, char* out, size_t n) { snprintf(out, n, "nes_state_%d.bin", slot); }
static void save_state_file(const char* path) {
    size_t n = p_retro_serialize_size(); if (!n) return;
    std::vector<uint8_t> buf(n);
    if (p_retro_serialize(buf.data(), n)) { FILE* f = fopen(path, "wb"); if (f) { fwrite(buf.data(), 1, n, f); fclose(f); } }
}
static bool load_state_file(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long fn = ftell(f); fseek(f, 0, SEEK_SET);
    if (fn <= 0) { fclose(f); return false; }
    size_t need = p_retro_serialize_size();
    size_t bn = ((size_t)fn > need) ? (size_t)fn : need;
    std::vector<uint8_t> buf(bn, 0);
    fread(buf.data(), 1, (size_t)fn, f); fclose(f);
    return p_retro_unserialize(buf.data(), need);
}

// ====================================================================
// TCP debug server — nesrecomp protocol parity (loopback 4370, line JSON)
// ====================================================================
static SOCKET g_listen = INVALID_SOCKET, g_client = INVALID_SOCKET;
static char   g_rxbuf[8192]; static int g_rxlen = 0;

static void srv_send(const char* s) {
    if (g_client == INVALID_SOCKET) return;
    int len = (int)strlen(s);
    send(g_client, s, len, 0); send(g_client, "\n", 1, 0);
}
static void srv_fmt(const char* fmt, ...) {
    char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    srv_send(buf);
}
static void srv_ok(int id)               { srv_fmt("{\"id\":%d,\"ok\":true}", id); }
static void srv_err(int id, const char* m){ srv_fmt("{\"id\":%d,\"ok\":false,\"error\":\"%s\"}", id, m); }

// minimal JSON field readers (same flavour as debug_server.c)
static bool j_str(const char* j, const char* key, char* out, int cap) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(j, pat); if (!p) return false;
    p = strchr(p + strlen(pat), ':'); if (!p) return false; p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') { p++; int i = 0; while (*p && *p != '"' && i < cap - 1) out[i++] = *p++; out[i] = 0; return true; }
    int i = 0; while (*p && *p != ',' && *p != '}' && *p != ' ' && i < cap - 1) out[i++] = *p++; out[i] = 0; return i > 0;
}
static int j_int(const char* j, const char* key, int def) {
    char v[32]; if (!j_str(j, key, v, sizeof(v))) return def; return atoi(v);
}
static uint32_t hexu32(const char* s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return (uint32_t)strtoul(s, nullptr, 16);
}

static void srv_dispatch(const char* line) {
    char cmd[64]; if (!j_str(line, "cmd", cmd, sizeof(cmd))) { strncpy(cmd, line, 63); cmd[63] = 0;
        int n = (int)strlen(cmd); while (n > 0 && (cmd[n-1]=='\r'||cmd[n-1]=='\n'||cmd[n-1]==' ')) cmd[--n]=0; }
    int id = j_int(line, "id", 0);

    if (!strcmp(cmd, "ping")) { srv_fmt("{\"id\":%d,\"ok\":true,\"frame\":%u}", id, g_frame); return; }
    if (!strcmp(cmd, "frame")) { srv_fmt("{\"id\":%d,\"ok\":true,\"frame\":%u,\"last_func\":\"%s\"}", id, g_frame, g_core_name); return; }
    if (!strcmp(cmd, "quit")) { srv_ok(id); closesocket(g_client); g_client = INVALID_SOCKET; exit(0); }
    if (!strcmp(cmd, "help")) {
        srv_fmt("{\"id\":%d,\"ok\":true,\"commands\":[\"ping\",\"frame\",\"read_ram\",\"dump_ram\",\"write_ram\",\"read_ppu\",\"read_frame_ram\",\"history\",\"save_state\",\"load_state\",\"quit\"],"
                "\"note\":\"nesref: libretro differential oracle, nesrecomp debug_server protocol parity. PPU palette/OAM + CPU regs need the in-process patched oracle, not a stock libretro core.\"}", id);
        return;
    }
    if (!strcmp(cmd, "read_ram") || !strcmp(cmd, "read_ppu")) {
        char a[32]; if (!j_str(line, "addr", a, sizeof(a))) { srv_err(id, "missing addr"); return; }
        uint32_t addr = hexu32(a); int len = j_int(line, "len", 1); if (len < 1) len = 1; if (len > 256) len = 256;
        char hex[513]; for (int i = 0; i < len; i++) snprintf(hex + i*2, 3, "%02x", read_byte(addr + i));
        srv_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"len\":%d,\"hex\":\"%s\"}", id, addr, len, hex); return;
    }
    if (!strcmp(cmd, "dump_ram")) {
        char a[32]; if (!j_str(line, "addr", a, sizeof(a))) { srv_err(id, "missing addr"); return; }
        uint32_t addr = hexu32(a); int len = j_int(line, "len", 256); if (len < 1) len = 1; if (len > 8192) len = 8192;
        for (int off = 0; off < len; ) { int chunk = len - off; if (chunk > 256) chunk = 256;
            char hex[513]; for (int i = 0; i < chunk; i++) snprintf(hex + i*2, 3, "%02x", read_byte(addr + off + i));
            srv_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"offset\":%d,\"len\":%d,\"hex\":\"%s\"}", id, addr+off, off, chunk, hex); off += chunk; }
        return;
    }
    if (!strcmp(cmd, "write_ram")) {
        char a[32], v[32]; if (!j_str(line, "addr", a, sizeof(a))) { srv_err(id, "missing addr"); return; }
        if (!j_str(line, "val", v, sizeof(v))) { srv_err(id, "missing val"); return; }
        write_byte(hexu32(a), (uint8_t)hexu32(v)); srv_ok(id); return;
    }
    if (!strcmp(cmd, "history")) {
        uint64_t oldest = (g_ring_count > RING_CAP) ? g_ring_count - RING_CAP : 0;
        uint64_t newest = g_ring_count ? g_ring_count - 1 : 0;
        srv_fmt("{\"id\":%d,\"ok\":true,\"count\":%llu,\"oldest\":%llu,\"newest\":%llu,\"capacity\":%d}",
                id, (unsigned long long)g_ring_count, (unsigned long long)oldest, (unsigned long long)newest, RING_CAP); return;
    }
    if (!strcmp(cmd, "read_frame_ram")) {
        int f = j_int(line, "frame", -1); if (f < 0) { srv_err(id, "missing frame"); return; }
        uint64_t oldest = (g_ring_count > RING_CAP) ? g_ring_count - RING_CAP : 0;
        if (!g_ring || (uint64_t)f < oldest || (uint64_t)f >= g_ring_count) { srv_err(id, "frame not in buffer"); return; }
        NesRefRecord* r = &g_ring[(uint32_t)f % RING_CAP];
        if (r->frame != (uint32_t)f) { srv_err(id, "frame record mismatch"); return; }
        char a[32]; const char* region = j_str(line, "region", a, sizeof(a)) ? a : "ram";
        char ad[32]; uint32_t addr = j_str(line, "addr", ad, sizeof(ad)) ? hexu32(ad) : 0;
        int len = j_int(line, "len", 16); if (len < 1) len = 1; if (len > 256) len = 256;
        char hex[513];
        for (int i = 0; i < len; i++) {
            uint8_t b = 0; uint32_t x = addr + i;
            if (!strcmp(region, "sram")) { if (x < 0x2000) b = r->sram[x]; }
            else                         { if (x < 0x800)  b = r->wram[x]; }
            snprintf(hex + i*2, 3, "%02x", b);
        }
        srv_fmt("{\"id\":%d,\"ok\":true,\"frame\":%u,\"region\":\"%s\",\"addr\":\"0x%04X\",\"len\":%d,\"hex\":\"%s\"}", id, r->frame, region, addr, len, hex); return;
    }
    if (!strcmp(cmd, "save_state")) {
        char p[256]; const char* path = j_str(line, "path", p, sizeof(p)) ? p : "nes_state.bin";
        save_state_file(path); srv_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\"}", id, path); return;
    }
    if (!strcmp(cmd, "load_state")) {
        char p[256]; const char* path = j_str(line, "path", p, sizeof(p)) ? p : "nes_state.bin";
        bool ok = load_state_file(path); if (ok) srv_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\"}", id, path); else srv_err(id, "load failed"); return;
    }
    srv_err(id, "unknown command (try 'help')");
}

static void srv_init(int port) {
    WSADATA wsa; if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { fprintf(stderr, "[debug] WSAStartup failed\n"); return; }
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen == INVALID_SOCKET) { fprintf(stderr, "[debug] socket failed\n"); return; }
    int yes = 1; setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((u_short)port);
    if (bind(g_listen, (sockaddr*)&a, sizeof(a)) != 0) { fprintf(stderr, "[debug] bind %d failed\n", port); closesocket(g_listen); g_listen = INVALID_SOCKET; return; }
    listen(g_listen, 1);
    u_long nb = 1; ioctlsocket(g_listen, FIONBIO, &nb);
    printf("[debug] TCP server listening on 127.0.0.1:%d (nesrecomp protocol parity)\n", port); fflush(stdout);
}
static void srv_poll() {
    if (g_listen == INVALID_SOCKET) return;
    if (g_client == INVALID_SOCKET) {
        SOCKET c = accept(g_listen, nullptr, nullptr);
        if (c != INVALID_SOCKET) { u_long nb = 1; ioctlsocket(c, FIONBIO, &nb); g_client = c; g_rxlen = 0; }
        else return;
    }
    for (;;) {
        char tmp[2048]; int n = recv(g_client, tmp, sizeof(tmp), 0);
        if (n == 0) { closesocket(g_client); g_client = INVALID_SOCKET; return; }
        if (n < 0) { if (WSAGetLastError() == WSAEWOULDBLOCK) break; closesocket(g_client); g_client = INVALID_SOCKET; return; }
        for (int i = 0; i < n; i++) {
            char ch = tmp[i];
            if (ch == '\n') { g_rxbuf[g_rxlen] = 0; if (g_rxlen) srv_dispatch(g_rxbuf); g_rxlen = 0; }
            else if (g_rxlen < (int)sizeof(g_rxbuf) - 1) g_rxbuf[g_rxlen++] = ch;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: nesref <core.dll> <rom.nes>\n"); return 1; }
    const char* corePath = argv[1]; const char* romPath = argv[2];

    g_core = LoadLibraryA(corePath);
    if (!g_core) { fprintf(stderr, "LoadLibrary failed: %s (err %lu)\n", corePath, GetLastError()); return 2; }
    bindsym(p_retro_init, "retro_init"); bindsym(p_retro_deinit, "retro_deinit");
    bindsym(p_retro_api_version, "retro_api_version");
    bindsym(p_retro_get_system_info, "retro_get_system_info");
    bindsym(p_retro_get_system_av_info, "retro_get_system_av_info");
    bindsym(p_retro_set_environment, "retro_set_environment");
    bindsym(p_retro_set_video_refresh, "retro_set_video_refresh");
    bindsym(p_retro_set_audio_sample, "retro_set_audio_sample");
    bindsym(p_retro_set_audio_sample_batch, "retro_set_audio_sample_batch");
    bindsym(p_retro_set_input_poll, "retro_set_input_poll");
    bindsym(p_retro_set_input_state, "retro_set_input_state");
    bindsym(p_retro_set_controller_port_device, "retro_set_controller_port_device");
    bindsym(p_retro_load_game, "retro_load_game"); bindsym(p_retro_unload_game, "retro_unload_game");
    bindsym(p_retro_run, "retro_run");
    bindsym(p_retro_serialize_size, "retro_serialize_size");
    bindsym(p_retro_serialize, "retro_serialize"); bindsym(p_retro_unserialize, "retro_unserialize");
    bindsym(p_retro_get_memory_data, "retro_get_memory_data");
    bindsym(p_retro_get_memory_size, "retro_get_memory_size");

    p_retro_set_environment(cb_environment);
    p_retro_init();

    retro_system_info si; memset(&si, 0, sizeof si); p_retro_get_system_info(&si);
    if (si.library_name) { strncpy(g_core_name, si.library_name, sizeof(g_core_name)-1); g_core_name[sizeof(g_core_name)-1]=0; }
    printf("core: %s %s  need_fullpath=%d\n", si.library_name ? si.library_name : "?",
           si.library_version ? si.library_version : "?", si.need_fullpath);

    retro_game_info gi; memset(&gi, 0, sizeof gi); gi.path = romPath;
    std::vector<uint8_t> rom;
    if (!si.need_fullpath) {
        FILE* f = fopen(romPath, "rb"); if (!f) { fprintf(stderr, "cannot open rom %s\n", romPath); return 3; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        rom.resize(n); fread(rom.data(), 1, n, f); fclose(f); gi.data = rom.data(); gi.size = rom.size();
    }
    p_retro_set_video_refresh(cb_video);
    p_retro_set_audio_sample(cb_audio_sample);
    p_retro_set_audio_sample_batch(cb_audio_batch);
    p_retro_set_input_poll(cb_input_poll);
    p_retro_set_input_state(cb_input_state);
    if (!p_retro_load_game(&gi)) { fprintf(stderr, "retro_load_game failed\n"); return 4; }
    p_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    retro_system_av_info av; memset(&av, 0, sizeof av); p_retro_get_system_av_info(&av);
    int vw = (int)av.geometry.base_width, vh = (int)av.geometry.base_height;
    if (vw <= 0) vw = 256; if (vh <= 0) vh = 240;
    printf("core timing: fps=%.4f sample_rate=%.2f\n", av.timing.fps, av.timing.sample_rate);

    { const char* t = getenv("NESREF_TRACE"); g_trace_on = (t && t[0] && t[0] != '0') ? 1 : 0; }
    { const char* wp = getenv("NESREF_WAV"); if (wp && wp[0]) wav_open(wp, av.timing.sample_rate > 0 ? av.timing.sample_rate : 48000.0); }
    long quit_frames = 0; { const char* qf = getenv("NESREF_QUIT_FRAMES"); if (qf && qf[0]) quit_frames = atol(qf); }
    int port = 4370; { const char* pp = getenv("NESREF_PORT"); if (pp && pp[0]) port = atoi(pp); }
    srv_init(port);

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 5; }
    open_first_pad();
    g_win = SDL_CreateWindow("nesref (libretro) — nesrecomp oracle | Fn load / Shift+Fn save / Esc quit",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, vw * 2, vh * 2, SDL_WINDOW_RESIZABLE);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(g_ren, vw, vh);

    printf("RUN. KB: arrows=DPad Z=A X=B Tab=Select Enter=Start | Fn=load slot, Shift+Fn=save | Esc=quit\n"); fflush(stdout);

    bool running = true;
    Uint64 freq = SDL_GetPerformanceFrequency(), prev = SDL_GetPerformanceCounter();
    const double target = (double)freq / 60.098;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_CONTROLLERDEVICEADDED) open_first_pad();
            else if (e.type == SDL_CONTROLLERDEVICEREMOVED) { if (g_pad) { SDL_GameControllerClose(g_pad); g_pad = nullptr; } open_first_pad(); }
            else if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
                SDL_Scancode s = e.key.keysym.scancode;
                if (s == SDL_SCANCODE_ESCAPE) running = false;
                else if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F9) {
                    int slot = (int)(s - SDL_SCANCODE_F1) + 1; char path[64]; slot_path(slot, path, sizeof path);
                    if (e.key.keysym.mod & KMOD_SHIFT) { save_state_file(path); printf("[slot %d saved]\n", slot); }
                    else { printf("[slot %d %s]\n", slot, load_state_file(path) ? "loaded" : "empty/failed"); }
                    fflush(stdout);
                }
            }
        }
        srv_poll();
        p_retro_run();
        g_frame++;
        ring_record();
        trace_tick();
        srv_poll();
        if (quit_frames > 0 && g_frame >= (uint32_t)quit_frames) running = false;
        for (;;) {
            Uint64 now = SDL_GetPerformanceCounter(); double el = (double)(now - prev);
            if (el >= target) { prev = now; break; }
            double rem_ms = (target - el) * 1000.0 / (double)freq;
            if (rem_ms > 1.5) SDL_Delay((Uint32)(rem_ms - 1.0));
        }
    }
    if (g_trace) fflush(g_trace);
    wav_close();
    p_retro_unload_game(); p_retro_deinit();
    if (g_client != INVALID_SOCKET) closesocket(g_client);
    if (g_listen != INVALID_SOCKET) closesocket(g_listen);
    WSACleanup();
    SDL_Quit(); FreeLibrary(g_core);
    free(g_ring);
    return 0;
}
