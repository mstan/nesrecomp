/* nesref — minimal SDL2 libretro frontend hosting a cycle-accurate NES core
 * (Mesen / fceumm / nestopia libretro DLL) as the differential ORACLE for the
 * NES static recompiler. Mirrors snesrecomp/tools/snesref: loads the core, runs
 * a ROM, and logs per-frame CPU-RAM changes (same JSON shape as the recomp's
 * NESRECOMP_WRAM_TRACE) for first-divergence diffing via wram_diff.py.
 *
 *   nesref.exe <core.dll> <rom.nes>
 *
 * Env:
 *   NESREF_TRACE_FILE=<path>   per-frame RAM-delta JSONL   (default nesref_trace.jsonl)
 *   NESREF_FRAMES=<N>          headless: run N frames (no input), flush, quit
 *   NESREF_DUMP="a,b,c"        also print RAM $00..$0F at these frame numbers (oracle readback)
 *
 * Keys: arrows=D-pad, Z=B, X=A, Enter=Start, RShift=Select, Esc=quit.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "libretro.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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

template<class T> static void bind(T& fn, const char* name) {
    fn = (T)GetProcAddress(g_core, name);
    if (!fn) { fprintf(stderr, "missing core symbol: %s\n", name); exit(2); }
}

// ---- video state ----
static SDL_Window*   g_win;
static SDL_Renderer* g_ren;
static SDL_Texture*  g_tex;
static int g_tex_w = 0, g_tex_h = 0;
static retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;

// ---- last frame as RGB24 (for screenshot / framebuf diff) ----
static std::vector<uint8_t> g_rgb;  // w*h*3
static int g_rgb_w=0, g_rgb_h=0;
static void store_rgb(const void* data, unsigned w, unsigned h, size_t pitch) {
    g_rgb.resize((size_t)w*h*3); g_rgb_w=w; g_rgb_h=h;
    for (unsigned y=0;y<h;y++) {
        const uint8_t* row=(const uint8_t*)data + y*pitch;
        for (unsigned x=0;x<w;x++) {
            uint8_t r,g,b;
            if (g_fmt==RETRO_PIXEL_FORMAT_XRGB8888) {
                const uint32_t p=((const uint32_t*)row)[x]; r=(p>>16)&0xFF; g=(p>>8)&0xFF; b=p&0xFF;
            } else if (g_fmt==RETRO_PIXEL_FORMAT_RGB565) {
                const uint16_t p=((const uint16_t*)row)[x]; r=((p>>11)&0x1F)<<3; g=((p>>5)&0x3F)<<2; b=(p&0x1F)<<3;
            } else { // 0RGB1555
                const uint16_t p=((const uint16_t*)row)[x]; r=((p>>10)&0x1F)<<3; g=((p>>5)&0x1F)<<3; b=(p&0x1F)<<3;
            }
            uint8_t* o=&g_rgb[((size_t)y*w+x)*3]; o[0]=r; o[1]=g; o[2]=b;
        }
    }
}
static void write_shot(const char* path) {
    if (g_rgb_w<=0) { fprintf(stderr,"[nesref shot] no frame yet\n"); return; }
    if (stbi_write_png(path, g_rgb_w, g_rgb_h, 3, g_rgb.data(), g_rgb_w*3))
        printf("[nesref shot] %s (%dx%d)\n", path, g_rgb_w, g_rgb_h);
    fflush(stdout);
}

// ---- CPU-RAM trace (NES work RAM = 2KB, $0000-$07FF) ----
#define WRAM_LO 0x0000
#define WRAM_HI 0x07ff
static FILE* g_log;
static uint8_t g_prev[WRAM_HI - WRAM_LO + 1];
static bool    g_primed = false;
static uint32_t g_frame = 0;

static const char* trace_path() {
    const char* p = getenv("NESREF_TRACE_FILE");
    return (p && p[0]) ? p : "nesref_trace.jsonl";
}
static void emit(int addr, uint8_t o, uint8_t n) {
    if (!g_log) { g_log = fopen(trace_path(), "a"); if (!g_log) return; }
    fprintf(g_log, "{\"f\":%u,\"adr\":\"0x%04x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
            g_frame, addr, o, n);
}
static void trace_tick() {
    uint8_t* ram = (uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t sz = p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || sz <= WRAM_HI) return;
    if (!g_primed) {
        for (int a=WRAM_LO;a<=WRAM_HI;a++){ g_prev[a-WRAM_LO]=ram[a]; emit(a,0,ram[a]); }
        g_primed=true; return;
    }
    for (int a=WRAM_LO;a<=WRAM_HI;a++){ uint8_t v=ram[a]; if(v!=g_prev[a-WRAM_LO]){ emit(a,g_prev[a-WRAM_LO],v); g_prev[a-WRAM_LO]=v; } }
    if (g_log && (g_frame % 30)==0) fflush(g_log);
}

// Cross-engine RNG/seed freeze: force fixed RAM bytes each frame so the oracle
// and recomp share identical RNG (Random $18 seeded by FrameCounter desyncs
// free-running engines). NESREF_FREEZE="0x18=0x00,0x..=0x..". Honors the same
// spec as the recomp's NESRECOMP_FREEZE. Applied after retro_run (frame end).
static void apply_freeze() {
    static int st=-1; static int addrs[16]; static int vals[16]; static int n=0;
    if (st<0) { st=0; const char* e=getenv("NESREF_FREEZE");
        if (e && e[0]) { char buf[256]; strncpy(buf,e,sizeof buf-1); buf[sizeof buf-1]=0;
            for (char* t=strtok(buf,","); t && n<16; t=strtok(nullptr,",")) {
                unsigned a,v; if (sscanf(t," 0x%x = 0x%x",&a,&v)==2){addrs[n]=a;vals[n]=v;n++;} }
            st=n?1:0; } }
    if (st!=1) return;
    uint8_t* ram=(uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t sz=p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram) return;
    for (int i=0;i<n;i++) if ((size_t)addrs[i]<sz) ram[addrs[i]]=(uint8_t)vals[i];
}

// Optional oracle readback: print RAM $00..$0F at requested frames.
static void dump_check() {
    static std::vector<long> frames; static bool init=false;
    if (!init) { init=true; const char* v=getenv("NESREF_DUMP");
        if (v && v[0]) { char buf[256]; strncpy(buf,v,sizeof buf-1); buf[sizeof buf-1]=0;
            for (char* t=strtok(buf,","); t; t=strtok(nullptr,",")) frames.push_back(atol(t)); } }
    for (long f : frames) if ((long)g_frame==f) {
        uint8_t* ram=(uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        if (!ram) return;
        printf("[nesref dump f=%u] $00..$0F:", g_frame);
        for (int a=0;a<16;a++) printf(" %02x", ram[a]);
        printf("\n"); fflush(stdout);
    }
}

// ---- libretro callbacks ----
static bool cb_environment(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: g_fmt = *(const retro_pixel_format*)data; return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:   *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: if(data) *(bool*)data=false; return true;
        default: return false;
    }
}
static void ensure_texture(unsigned w, unsigned h) {
    if ((int)w==g_tex_w && (int)h==g_tex_h && g_tex) return;
    if (g_tex) SDL_DestroyTexture(g_tex);
    Uint32 sf = (g_fmt==RETRO_PIXEL_FORMAT_XRGB8888) ? SDL_PIXELFORMAT_ARGB8888
              : (g_fmt==RETRO_PIXEL_FORMAT_RGB565)   ? SDL_PIXELFORMAT_RGB565
              :                                        SDL_PIXELFORMAT_ARGB1555;
    g_tex = SDL_CreateTexture(g_ren, sf, SDL_TEXTUREACCESS_STREAMING, w, h);
    g_tex_w=w; g_tex_h=h;
}
static void cb_video(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (data && w && h) store_rgb(data, w, h, pitch);   // capture every frame (works headless)
    if (!g_ren) return;
    if (data && w && h) { ensure_texture(w,h); SDL_UpdateTexture(g_tex, nullptr, data, (int)pitch); }
    SDL_RenderClear(g_ren);
    if (g_tex) SDL_RenderCopy(g_ren, g_tex, nullptr, nullptr);
    SDL_RenderPresent(g_ren);
}
// ---- audio capture (NESREF_WAV) ----
// Always-on ground-truth PCM tap: NESREF_WAV=<path> streams the core's audio
// callbacks to a stereo s16 WAV at the core's reported sample rate. The header
// is finalized on clean exit; a force-killed capture is still recoverable (the
// analyzer's loader tolerates an unfinalized header). Works headless — the core
// renders audio every retro_run regardless of pacing, so a NESREF_FRAMES run
// yields a deterministic faster-than-realtime oracle capture.
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

// ---- Input script (NESREF_SCRIPT) with STATE-ANCHORING ----
// Runtime interpreter matching the recomp runner's input_script.c so the co-sim
// drives BOTH engines through the SAME inputs into gameplay. Commands: HOLD/
// RELEASE (instant), WAIT n (hold current mask n frames), WAIT_RAM8 <hex_addr>
// <hex_val> (block until WRAM[addr]==val, 30s timeout). WAIT_RAM8 is the key
// addition: it anchors input on GAME STATE (e.g. Update_Select $0100==$C0 = in a
// horizontal level) so both engines apply the next input at the SAME state,
// immune to load-timing frame drift. TURBO/SCREENSHOT/LOG/EXIT ignored (oracle
// runs to NESREF_FRAMES). NES mask bits: A 0x80 B 0x40 SEL 0x20 ST 0x10 U 0x08
// D 0x04 L 0x02 R 0x01.
struct SCmd { int type; int iarg; uint8_t barg; uint16_t addr; uint8_t val; };
// type: 0=WAIT, 1=HOLD, 2=RELEASE, 3=WAIT_RAM8
static std::vector<SCmd> g_cmds;
static bool g_have_script = false;
static int  g_pc = 0, g_wait_left = 0;
static uint8_t g_held = 0;
static long g_waitram_start = -1;
static bool g_script_done = false;   // set when all commands consumed (= recomp EXIT)
static uint8_t script_btn_mask(const char* n){
    if(!strcmp(n,"A"))return 0x80;      if(!strcmp(n,"B"))return 0x40;
    if(!strcmp(n,"SELECT"))return 0x20; if(!strcmp(n,"START"))return 0x10;
    if(!strcmp(n,"UP"))return 0x08;     if(!strcmp(n,"DOWN"))return 0x04;
    if(!strcmp(n,"LEFT"))return 0x02;   if(!strcmp(n,"RIGHT"))return 0x01;
    return 0;
}
static void load_script(){
    const char* path=getenv("NESREF_SCRIPT");
    if(!path||!path[0]) return;
    FILE* f=fopen(path,"r");
    if(!f){ fprintf(stderr,"[nesref script] cannot open %s\n",path); return; }
    char line[256];
    while(fgets(line,sizeof line,f)){
        char cmd[32]={0}, a1[32]={0}, a2[32]={0}; int nn=sscanf(line,"%31s %31s %31s",cmd,a1,a2);
        if(nn<1) continue;
        SCmd c={0,0,0,0,0};
        if(!strcmp(cmd,"HOLD")&&nn>=2){ c.type=1; c.barg=script_btn_mask(a1); g_cmds.push_back(c); }
        else if(!strcmp(cmd,"RELEASE")&&nn>=2){ c.type=2; c.barg=script_btn_mask(a1); g_cmds.push_back(c); }
        else if(!strcmp(cmd,"WAIT")&&nn>=2){ c.type=0; c.iarg=atoi(a1); g_cmds.push_back(c); }
        else if(!strcmp(cmd,"WAIT_RAM8")&&nn>=3){ c.type=3;
            c.addr=(uint16_t)strtol(a1,0,16); c.val=(uint8_t)strtol(a2,0,16); g_cmds.push_back(c); }
        /* TURBO/SCREENSHOT/LOG/EXIT/ASSERT_RAM8 ignored */
    }
    fclose(f);
    g_have_script=!g_cmds.empty();
    fprintf(stderr,"[nesref script] loaded %s: %zu commands\n",path,g_cmds.size());
}
// Advance the script for the frame about to be computed; sets g_held. Reads live
// WRAM for WAIT_RAM8. Called once per frame BEFORE retro_run.
static void script_tick(const uint8_t* ram){
    if(!g_have_script) return;
    for(int guard=0; guard<100000; guard++){
        if(g_pc>=(int)g_cmds.size()){ g_held=0; g_script_done=true; return; }  // done (= recomp EXIT)
        SCmd& c=g_cmds[g_pc];
        if(c.type==1){ g_held|=c.barg; g_pc++; continue; }
        if(c.type==2){ g_held&=(uint8_t)~c.barg; g_pc++; continue; }
        if(c.type==0){                                      // WAIT n
            if(g_wait_left==0) g_wait_left=c.iarg;
            g_wait_left--;
            if(g_wait_left>0) return;
            g_wait_left=0; g_pc++; continue;
        }
        if(c.type==3){                                      // WAIT_RAM8 addr val
            if(g_waitram_start<0) g_waitram_start=(long)g_frame;
            if(ram && ram[c.addr & 0x7FF]==c.val){ g_waitram_start=-1; g_pc++; continue; }
            if((long)g_frame - g_waitram_start > 30*60){     // 30s timeout (match recomp)
                fprintf(stderr,"[nesref script] WAIT_RAM8 $%04X==%02X TIMEOUT (got %02X) f=%u\n",
                        c.addr, c.val, ram?ram[c.addr&0x7FF]:0, g_frame);
                g_waitram_start=-1; g_pc++; continue;
            }
            return;                                          // keep waiting; hold current mask
        }
        g_pc++;
    }
}
static int16_t cb_input_state(unsigned port, unsigned device, unsigned, unsigned id) {
    if (port!=0 || device!=RETRO_DEVICE_JOYPAD) return 0;
    if (g_have_script) {
        uint8_t m = g_held;   // set by script_tick() before this frame's retro_run
        switch (id) {
            case RETRO_DEVICE_ID_JOYPAD_A:      return (m&0x80)?1:0;
            case RETRO_DEVICE_ID_JOYPAD_B:      return (m&0x40)?1:0;
            case RETRO_DEVICE_ID_JOYPAD_SELECT: return (m&0x20)?1:0;
            case RETRO_DEVICE_ID_JOYPAD_START:  return (m&0x10)?1:0;
            case RETRO_DEVICE_ID_JOYPAD_UP:     return (m&0x08)?1:0;
            case RETRO_DEVICE_ID_JOYPAD_DOWN:   return (m&0x04)?1:0;
            case RETRO_DEVICE_ID_JOYPAD_LEFT:   return (m&0x02)?1:0;
            case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return (m&0x01)?1:0;
            default: return 0;
        }
    }
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    SDL_Scancode sc;
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_B:      sc=SDL_SCANCODE_Z;      break;
        case RETRO_DEVICE_ID_JOYPAD_A:      sc=SDL_SCANCODE_X;      break;
        case RETRO_DEVICE_ID_JOYPAD_START:  sc=SDL_SCANCODE_RETURN; break;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: sc=SDL_SCANCODE_RSHIFT; break;
        case RETRO_DEVICE_ID_JOYPAD_UP:     sc=SDL_SCANCODE_UP;     break;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   sc=SDL_SCANCODE_DOWN;   break;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   sc=SDL_SCANCODE_LEFT;   break;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  sc=SDL_SCANCODE_RIGHT;  break;
        default: return 0;
    }
    return ks[sc] ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr,"usage: nesref <core.dll> <rom.nes>\n"); return 1; }
    const char* corePath = argv[1];
    const char* romPath  = argv[2];

    g_core = LoadLibraryA(corePath);
    if (!g_core) { fprintf(stderr,"LoadLibrary failed: %s (err %lu)\n", corePath, GetLastError()); return 2; }
    bind(p_retro_init,"retro_init"); bind(p_retro_deinit,"retro_deinit");
    bind(p_retro_api_version,"retro_api_version");
    bind(p_retro_get_system_info,"retro_get_system_info");
    bind(p_retro_get_system_av_info,"retro_get_system_av_info");
    bind(p_retro_set_environment,"retro_set_environment");
    bind(p_retro_set_video_refresh,"retro_set_video_refresh");
    bind(p_retro_set_audio_sample,"retro_set_audio_sample");
    bind(p_retro_set_audio_sample_batch,"retro_set_audio_sample_batch");
    bind(p_retro_set_input_poll,"retro_set_input_poll");
    bind(p_retro_set_input_state,"retro_set_input_state");
    bind(p_retro_set_controller_port_device,"retro_set_controller_port_device");
    bind(p_retro_load_game,"retro_load_game"); bind(p_retro_unload_game,"retro_unload_game");
    bind(p_retro_run,"retro_run");
    bind(p_retro_serialize_size,"retro_serialize_size");
    bind(p_retro_serialize,"retro_serialize"); bind(p_retro_unserialize,"retro_unserialize");
    bind(p_retro_get_memory_data,"retro_get_memory_data");
    bind(p_retro_get_memory_size,"retro_get_memory_size");

    p_retro_set_environment(cb_environment);
    p_retro_init();

    retro_system_info si; memset(&si,0,sizeof si); p_retro_get_system_info(&si);
    printf("core: %s %s  need_fullpath=%d\n", si.library_name?si.library_name:"?",
           si.library_version?si.library_version:"?", si.need_fullpath);

    retro_game_info gi; memset(&gi,0,sizeof gi); gi.path=romPath;
    std::vector<uint8_t> rom;
    if (!si.need_fullpath) {
        FILE* f=fopen(romPath,"rb"); if(!f){ fprintf(stderr,"cannot open rom %s\n",romPath); return 3; }
        fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
        rom.resize(n); fread(rom.data(),1,n,f); fclose(f);
        gi.data=rom.data(); gi.size=rom.size();
    }
    p_retro_set_video_refresh(cb_video);
    p_retro_set_audio_sample(cb_audio_sample);
    p_retro_set_audio_sample_batch(cb_audio_batch);
    p_retro_set_input_poll(cb_input_poll);
    p_retro_set_input_state(cb_input_state);
    if (!p_retro_load_game(&gi)) { fprintf(stderr,"retro_load_game failed\n"); return 4; }
    p_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    // Probe which memory regions this core exposes (NESREF_MEMPROBE=1). Finding
    // (2026-07-01): Mesen, FCEUmm, and Nestopia libretro cores ALL expose only
    // SYSTEM_RAM (2KB) for NES — VIDEO_RAM/SAVE_RAM/RTC are null/0. So the
    // libretro path cannot supply PPU-internal memory or a cycle counter; the
    // co-sim PPU-mem and cycle cross-checks require the external MesenCE Lua
    // (emu.read(nesNametableRam/nesSpriteRam/nesPaletteRam) + cpu.cycleCount).
    if (getenv("NESREF_MEMPROBE")) {
        struct { int id; const char* n; } mem[] = {
            {RETRO_MEMORY_SAVE_RAM,"SAVE_RAM"}, {RETRO_MEMORY_RTC,"RTC"},
            {RETRO_MEMORY_SYSTEM_RAM,"SYSTEM_RAM"}, {RETRO_MEMORY_VIDEO_RAM,"VIDEO_RAM"} };
        for (auto& m : mem) {
            void* d = p_retro_get_memory_data(m.id);
            size_t s = p_retro_get_memory_size(m.id);
            printf("[nesref mem] %-10s data=%p size=%zu\n", m.n, d, s);
        }
        fflush(stdout);
    }

    retro_system_av_info av; memset(&av,0,sizeof av); p_retro_get_system_av_info(&av);
    int vw=(int)av.geometry.base_width, vh=(int)av.geometry.base_height;
    if(vw<=0)vw=256; if(vh<=0)vh=240;
    { const char* wp = getenv("NESREF_WAV");
      if (wp && wp[0]) {
          double sr = av.timing.sample_rate > 0 ? av.timing.sample_rate : 48000.0;
          wav_open(wp, sr);
          printf("WAV capture -> %s @ %.1f Hz\n", wp, sr); fflush(stdout);
      } }

    // Headless when NESREF_FRAMES is set: no SDL window (deterministic capture).
    const char* framesEnv = getenv("NESREF_FRAMES");
    long run_frames = (framesEnv && framesEnv[0]) ? atol(framesEnv) : -1;
    bool headless = (run_frames > 0);

    if (!headless) {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 5; }
        g_win = SDL_CreateWindow("nesref (libretro oracle) — Esc quit",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, vw*2, vh*2, SDL_WINDOW_RESIZABLE);
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        SDL_RenderSetLogicalSize(g_ren, vw, vh);
        printf("RUN. KB: arrows=DPad Z=B X=A Enter=Start RShift=Select Esc=quit\n"); fflush(stdout);
    } else {
        printf("RUN headless: %ld frames\n", run_frames); fflush(stdout);
    }

    load_script();   // NESREF_SCRIPT: per-frame input for co-sim drive-through

    bool running=true;
    Uint64 freq = headless?0:SDL_GetPerformanceFrequency();
    Uint64 prev = headless?0:SDL_GetPerformanceCounter();
    const double target = headless?0:(double)freq / 60.098;
    while (running) {
        if (!headless) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type==SDL_QUIT) running=false;
                else if (e.type==SDL_KEYDOWN && e.key.repeat==0 && e.key.keysym.scancode==SDL_SCANCODE_ESCAPE) running=false;
            }
        }
        // State-anchored input: advance the script (reading live WRAM for
        // WAIT_RAM8) to set this frame's held buttons before the core polls.
        script_tick((const uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
        p_retro_run();
        g_frame++;
        apply_freeze();   // force shared RNG/seed before snapshotting (matches recomp order)
        trace_tick();
        dump_check();
        { static long sf=-2; static const char* sfile=nullptr;
          if (sf==-2){ const char* v=getenv("NESREF_SHOT"); sf=(v&&v[0])?atol(v):-1;
                       sfile=getenv("NESREF_SHOT_FILE"); if(!sfile||!sfile[0]) sfile="nesref_shot.png"; }
          if (sf>0 && (long)g_frame==sf) write_shot(sfile); }
        // Gated savestate dump (NESREF_STATEDUMP=<frame>:<path>): probe whether
        // Mesen's serialize blob is parseable for PPU/APU/cycle state in-process.
        { static long df=-2; static const char* dpath=nullptr;
          if (df==-2){ const char* v=getenv("NESREF_STATEDUMP");
                       if(v&&v[0]){ static char pb[512]; long f; char p[480];
                                    if(sscanf(v,"%ld:%479s",&f,p)==2){df=f;strcpy(pb,p);dpath=pb;} else df=-1; }
                       else df=-1; }
          if (df>0 && (long)g_frame==df) {
              size_t sz=p_retro_serialize_size();
              std::vector<uint8_t> blob(sz);
              if (p_retro_serialize(blob.data(), sz)) {
                  FILE* bf=fopen(dpath,"wb"); if(bf){ fwrite(blob.data(),1,sz,bf); fclose(bf);
                      printf("[nesref statedump] frame %ld -> %s (%zu bytes)\n", df, dpath, sz); fflush(stdout); }
              } else printf("[nesref statedump] retro_serialize failed (sz=%zu)\n", sz);
          } }

        // In-process CYCLE oracle (NESREF_CYCLE_FILE=<path>): Mesen exposes no
        // cycle counter over libretro's memory API, but its full state -- incl.
        // the monotonic CPU cycle count -- is in the retro_serialize blob. We
        // pull it in-process (no external Mesen.exe). The offset is discovered
        // self-calibrating: at a calibration frame, scan the blob for the unique
        // 8-byte LE value ~= frame * 29780.5 and lock it (robust across ROM/core
        // version -- no magic constant). Then emit {f,cyc} per frame.
        { static int st=-1; static FILE* cf=nullptr; static long coff=-1;
          if (st<0){ const char* v=getenv("NESREF_CYCLE_FILE"); cf=(v&&v[0])?fopen(v,"w"):nullptr; st=cf?1:0; }
          if (st==1) {
              size_t sz=p_retro_serialize_size();
              static std::vector<uint8_t> blob; blob.resize(sz);
              if (p_retro_serialize(blob.data(), sz)) {
                  if (coff<0 && g_frame>=30) {
                      // Lock the cycle-counter offset: unique 8-byte LE within 1% of frame*29780.5.
                      double expect=(double)g_frame*29780.5; long lo=(long)(expect*0.99), hi=(long)(expect*1.01);
                      long found=-1; int n=0;
                      for (size_t o=0;o+8<=sz;o++){ uint64_t val; memcpy(&val,&blob[o],8);
                          if ((long long)val>=lo && (long long)val<=hi){ found=(long)o; n++; } }
                      if (n==1){ coff=found; printf("[nesref cycle] locked offset 0x%lx (f=%u cyc=%llu)\n",
                                    coff, g_frame, (unsigned long long)*(uint64_t*)&blob[coff]); fflush(stdout); }
                      else printf("[nesref cycle] calib f=%u: %d candidates (need 1), retrying\n", g_frame, n);
                  }
                  if (coff>=0){ uint64_t cyc; memcpy(&cyc,&blob[coff],8);
                      fprintf(cf, "{\"f\":%u,\"cyc\":%llu}\n", g_frame, (unsigned long long)cyc);
                      if ((g_frame%30)==0) fflush(cf); }
              }
          } }

        // In-process PPU-mem oracle (NESREF_PPU_FILE=<path>): OAM + palette + the
        // 2KB nametable RAM from the serialize blob. libretro exposes no PPU memory
        // (VIDEO_RAM is null on Mesen/FCEUmm/Nestopia), but the savestate has it.
        // Anchor on the 2KB WRAM found by CONTENT match (memmem of the live
        // SYSTEM_RAM -- robust, ROM-independent); the PPU regions sit at fixed deltas
        // relative to it in the Mesen-core layout (OAM = wram-0x16c, palette =
        // wram-0x190, nametables = wram+0xab4; NROM/SMB-measured, override via
        // NESREF_PPU_OAM_D / _PAL_D / _NT_D for other mappers). Emits a per-frame
        // delta-JSONL image matching the recomp's NESRECOMP_PPUMEM_TRACE:
        // [0x000-0x0FF]=OAM, [0x100-0x11F]=palette, [0x200-0x9FF]=nametable 2KB.
        { static int st=-1; static FILE* pf=nullptr; static uint8_t prev[0xA00]; static bool primed=false;
          static long oamd=0x16c, pald=0x190, ntd=-0xab4;  /* wram-relative (ntd negative => after wram) */
          if (st<0){ const char* v=getenv("NESREF_PPU_FILE"); pf=(v&&v[0])?fopen(v,"w"):nullptr; st=pf?1:0;
                     const char* a=getenv("NESREF_PPU_OAM_D"); if(a)oamd=strtol(a,0,0);
                     const char* p=getenv("NESREF_PPU_PAL_D"); if(p)pald=strtol(p,0,0);
                     const char* n=getenv("NESREF_PPU_NT_D");  if(n)ntd=strtol(n,0,0); }
          if (st==1) {
              size_t sz=p_retro_serialize_size();
              static std::vector<uint8_t> blob; blob.resize(sz);
              uint8_t* wram=(uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
              if (wram && p_retro_serialize(blob.data(), sz)) {
                  // Locate the 2KB WRAM in the blob (first exact 2048-byte match).
                  long wo=-1;
                  for (size_t o=0;o+2048<=sz;o++){ if(!memcmp(&blob[o],wram,2048)){wo=(long)o;break;} }
                  if (wo>=0) {
                      uint8_t img[0xA00]; memset(img,0,sizeof img);
                      long oo=wo-oamd, po=wo-pald, no=wo-ntd;
                      if (oo>=0 && po>=0 && no>=0 &&
                          oo+256<=(long)sz && po+32<=(long)sz && no+0x800<=(long)sz) {
                          memcpy(img,       &blob[oo], 256);    // OAM        -> [0x000-0x0FF]
                          memcpy(img+0x100, &blob[po], 32);     // palette    -> [0x100-0x11F]
                          memcpy(img+0x200, &blob[no], 0x800);  // nametables -> [0x200-0x9FF]
                          if (!primed){ for(int a=0;a<0xA00;a++){ prev[a]=img[a];
                                fprintf(pf,"{\"f\":%u,\"adr\":\"0x%04x\",\"old\":\"0x00\",\"val\":\"0x%02x\"}\n",g_frame,a,img[a]); }
                                primed=true; }
                          else { for(int a=0;a<0xA00;a++) if(img[a]!=prev[a]){
                                fprintf(pf,"{\"f\":%u,\"adr\":\"0x%04x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",g_frame,a,prev[a],img[a]);
                                prev[a]=img[a]; } }
                          if ((g_frame%30)==0) fflush(pf);
                      }
                  }
              }
          } }

        if (run_frames>0 && (long)g_frame>=run_frames) { if(g_log){fflush(g_log);} running=false; }
        // Script finished (all commands incl. the post-anchor settle consumed) =
        // the recomp's EXIT: stop here so both engines' FINAL state is the same
        // script point, not NESREF_FRAMES (which would overshoot past the anchor).
        if (g_have_script && g_script_done) { if(g_log){fflush(g_log);} running=false; }
        if (!headless) {
            for (;;) {
                Uint64 now=SDL_GetPerformanceCounter();
                double el=(double)(now-prev);
                if (el>=target) { prev=now; break; }
                double rem_ms=(target-el)*1000.0/(double)freq;
                if (rem_ms>1.5) SDL_Delay((Uint32)(rem_ms-1.0));
            }
        }
    }
    if (g_log) fflush(g_log);
    wav_close();
    p_retro_unload_game(); p_retro_deinit();
    if (!headless) SDL_Quit();
    FreeLibrary(g_core);
    return 0;
}
