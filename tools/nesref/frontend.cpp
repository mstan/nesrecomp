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
 *   NESREF_STATUS_FILE=<path> JSONL {type:"frame",f} after each flushed snapshot,
 *                            then {type:"complete",f,requested_frames,reason}.
 *                            Require exit 0 AND completion; deltas alone cannot
 *                            prove that a frame with no memory changes ran.
 * Cycle/PPU extraction supports Mesen 0.9.9 MST v13 (see tools/build-oracle.sh
 * in the parent project for the tested source pin), not arbitrary libretro cores.
 *
 * Keys: arrows=D-pad, Z=B, X=A, Enter=Start, RShift=Select, Esc=quit.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <stdexcept>
#include <string>
#include <vector>
#include "libretro.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// ---- core function pointers ----
static void* g_core;
static bool g_headless = false;
static bool g_initialized = false, g_loaded = false;
static FILE *g_cycle_log, *g_ppu_log, *g_status;
static void fail(const char* message) { throw std::runtime_error(message); }
static FILE* open_output(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) throw std::runtime_error(std::string("cannot open output ") + path + ": " + strerror(errno));
    return f;
}
static FILE* optional_output(const char* name) {
    const char* path = getenv(name);
    return path && path[0] ? open_output(path) : nullptr;
}
static void flush_output(FILE* f) {
    if (f && (fflush(f) != 0 || ferror(f))) fail("trace/capture write failed");
}
static void close_output(FILE*& f) {
    if (!f) return;
    FILE* closing = f;
    f = nullptr;
    if (fclose(closing) != 0) fail("output close failed");
}
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
    fn = (T)SDL_LoadFunction(g_core, name);
    if (!fn) throw std::runtime_error(std::string("missing core symbol ") + name + ": " + SDL_GetError());
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
    if (g_rgb_w<=0) fail("screenshot requested but no frame received");
    if (stbi_write_png(path, g_rgb_w, g_rgb_h, 3, g_rgb.data(), g_rgb_w*3))
        printf("[nesref shot] %s (%dx%d)\n", path, g_rgb_w, g_rgb_h);
    else fail("screenshot write failed");
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
    fprintf(g_log, "{\"f\":%u,\"adr\":\"0x%04x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
            g_frame, addr, o, n);
}
static void trace_tick() {
    uint8_t* ram = (uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t sz = p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || sz <= WRAM_HI) fail("core does not expose 2048 bytes of SYSTEM_RAM");
    if (!g_primed) {
        for (int a=WRAM_LO;a<=WRAM_HI;a++){ g_prev[a-WRAM_LO]=ram[a]; emit(a,0,ram[a]); }
        g_primed=true; return;
    }
    for (int a=WRAM_LO;a<=WRAM_HI;a++){ uint8_t v=ram[a]; if(v!=g_prev[a-WRAM_LO]){ emit(a,g_prev[a-WRAM_LO],v); g_prev[a-WRAM_LO]=v; } }
}

// Mesen 0.9.9 / MST v13, libretro/Mesen 0102910c39ad1a62bc3f784466f3f67ca9eae335.
// Console::SaveState writes CPU, PPU, RAM, APU, controllers, mapper snapshots.
// Snapshotable wraps each Stream block in two equal u32 lengths; arrays have
// one u32 element count. Parse lengths rather than WRAM-relative offsets:
// PPU sprite blocks and mapper CHR/work/save RAM change subsequent positions.
static void trace_state(const std::vector<uint8_t>& blob) {
    auto range = [&](size_t pos, size_t count, size_t end) {
        if (end > blob.size() || pos > end || count > end-pos)
            fail("truncated or unsupported Mesen savestate layout");
    };
    auto u32 = [&](size_t pos) -> uint32_t {
        range(pos, 4, blob.size());
        return uint32_t(blob[pos]) | (uint32_t(blob[pos+1]) << 8) |
               (uint32_t(blob[pos+2]) << 16) | (uint32_t(blob[pos+3]) << 24);
    };
    auto block = [&](size_t& pos, size_t end, size_t& length) -> size_t {
        range(pos, 8, end);
        length = u32(pos);
        if (length != u32(pos+4)) fail("unsupported Mesen Stream block");
        size_t data = pos+8;
        range(data, length, end);
        pos = data+length;
        return data;
    };
    range(0, 58, blob.size());
    if (memcmp(blob.data(), "MST", 3) || u32(3) != 0x909 || u32(7) != 13)
        fail("cycle/PPU traces require Mesen 0.9.9 MST format 13");
    range(58, u32(54), blob.size()); // ROM filename is variable length, including spaces.
    size_t pos = 58+u32(54), starts[6], ends[6];
    for (int i=0; i<6; ++i) {
        uint32_t n = u32(pos);
        starts[i] = pos+4;
        range(starts[i], n, blob.size());
        pos = ends[i] = starts[i]+n;
    }
    size_t n, cursor = starts[0];
    size_t cpu = block(cursor, ends[0], n);
    if (n != 50) fail("unsupported Mesen CPU state size");
    uint64_t cycles = 0;
    for (unsigned i=0; i<8; ++i) cycles |= uint64_t(blob[cpu+7+i]) << (i*8);
    static uint64_t previous_cycles = 0;
    if (cycles <= previous_cycles) fail("Mesen CPU cycle counter is not monotonic");
    previous_cycles = cycles;
    if (g_cycle_log) fprintf(g_cycle_log, "{\"f\":%u,\"cyc\":%llu}\n", g_frame, (unsigned long long)cycles);
    if (!g_ppu_log) return;

    cursor = starts[1];
    size_t ppu = block(cursor, ends[1], n);
    if (n != 489 || u32(ppu+101) != 32 || u32(ppu+137) != 256 || u32(ppu+397) != 32)
        fail("unsupported Mesen PPU state layout");
    size_t pal = ppu+105, oam = ppu+141;
    cursor = starts[2];
    size_t ram = block(cursor, ends[2], n);
    if (n != 2052 || u32(ram) != 2048 ||
        memcmp(&blob[ram+4], p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM), 2048))
        fail("Mesen serialized RAM does not match live SYSTEM_RAM");
    ram += 4;
    cursor = starts[5];
    size_t count = block(cursor, ends[5], n);
    if (n != 1 || blob[count] != 2) fail("PPU trace requires two physical nametables");
    size_t mapper = block(cursor, ends[5], n), mapper_end = mapper+n;
    pos = mapper+4; // MirroringType (32-bit enum).
    for (int i=0; i<3; ++i) { // Skip CHR RAM, mapper work RAM, battery save RAM.
        range(pos, 4, mapper_end);
        uint32_t bytes = u32(pos);
        range(pos+4, bytes, mapper_end);
        pos += 4+bytes;
    }
    range(pos, 4+2048, mapper_end);
    if (u32(pos) != 2048) fail("unsupported Mesen nametable array size");
    size_t nt = pos+4;
    // Retain explicit upstream offset overrides for existing diagnostic runs.
    const char* vars[] = {"NESREF_PPU_OAM_D", "NESREF_PPU_PAL_D", "NESREF_PPU_NT_D"};
    size_t* offsets[] = {&oam, &pal, &nt};
    const size_t lengths[] = {256, 32, 2048};
    for (int i=0; i<3; ++i) if (const char* value = getenv(vars[i])) {
        char* end; errno = 0;
        long delta = strtol(value, &end, 0);
        if (errno || end == value || *end || delta < -long(blob.size()) || delta > long(ram))
            fail("invalid PPU offset override");
        *offsets[i] = size_t(long(ram)-delta);
        range(*offsets[i], lengths[i], blob.size());
    }
    if (g_frame == 1) fprintf(stderr,
        "[nesref state] MST v13 cycle=0x%zx RAM=0x%zx OAM=0x%zx palette=0x%zx NT=0x%zx (mapper=%u)\n",
        cpu+7, ram, oam, pal, nt, unsigned(blob[11]) | (unsigned(blob[12]) << 8));
    uint8_t img[0xA00] = {};
    memcpy(img, &blob[oam], 256);
    memcpy(img+0x100, &blob[pal], 32);
    memcpy(img+0x200, &blob[nt], 2048);
    static uint8_t prev[0xA00];
    for (int a=0; a<0xA00; ++a) if (g_frame == 1 || img[a] != prev[a]) {
        fprintf(g_ppu_log, "{\"f\":%u,\"adr\":\"0x%04x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
                g_frame, a, prev[a], img[a]);
        prev[a] = img[a];
    }
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
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            if (*(const retro_pixel_format*)data > RETRO_PIXEL_FORMAT_RGB565) return false;
            g_fmt = *(const retro_pixel_format*)data; return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:   *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: if(data) *(bool*)data=false; return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            retro_variable* var = (retro_variable*)data;
            var->value = nullptr;
            if (!strcmp(var->key, "mesen_ramstate")) var->value = "All 0s (Default)";
            else if (!strcmp(var->key, "mesen_region")) var->value = "Auto";
            return var->value != nullptr;
        }
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
    if (!g_tex) fail(SDL_GetError());
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
    g_wav = open_output(path);
    g_wav_rate = (uint32_t)(rate + 0.5); uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, g_wav);
}
static void wav_close() {
    if (!g_wav) return;
    uint32_t data_bytes = (uint32_t)(g_wav_frames * 4);
    uint32_t riff = 36 + data_bytes, fmt32 = 16, brate = g_wav_rate * 4;
    uint16_t pcm = 1, ch = 2, balign = 4, bits = 16;
    flush_output(g_wav);
    if (fseek(g_wav, 0, SEEK_SET) != 0) fail("cannot seek WAV header");
    fwrite("RIFF", 1, 4, g_wav); fwrite(&riff, 4, 1, g_wav); fwrite("WAVEfmt ", 1, 8, g_wav);
    fwrite(&fmt32, 4, 1, g_wav); fwrite(&pcm, 2, 1, g_wav); fwrite(&ch, 2, 1, g_wav);
    fwrite(&g_wav_rate, 4, 1, g_wav); fwrite(&brate, 4, 1, g_wav);
    fwrite(&balign, 2, 1, g_wav); fwrite(&bits, 2, 1, g_wav);
    fwrite("data", 1, 4, g_wav); fwrite(&data_bytes, 4, 1, g_wav);
    flush_output(g_wav);
    close_output(g_wav);
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
    if(!f) throw std::runtime_error(std::string("cannot open input script ") + path);
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
    bool read_error = ferror(f) != 0;
    fclose(f);
    if (read_error) fail("input script read failed");
    if (g_cmds.empty()) fail("input script contains no supported commands");
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
                fail("input script WAIT_RAM8 timed out");
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
    if (g_headless) return 0;  // Never consult host input in a neutral headless run.
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

static int run_frontend(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr,"usage: nesref <core.dylib|core.so|core.dll> <rom.nes>\n"); return 1; }
    const char* corePath = argv[1];
    const char* romPath  = argv[2];

    const char* framesEnv = getenv("NESREF_FRAMES");
    long run_frames = -1;
    if (framesEnv) {
        char* end; errno = 0;
        run_frames = strtol(framesEnv, &end, 10);
        if (errno || end == framesEnv || *end || run_frames <= 0 || run_frames > INT_MAX)
            fail("NESREF_FRAMES must be an integer in 1..2147483647");
    }
    const bool headless = g_headless = run_frames > 0;
    load_script();

    g_core = SDL_LoadObject(corePath);
    if (!g_core) { fprintf(stderr,"cannot load core %s: %s\n", corePath, SDL_GetError()); return 2; }
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

    if (p_retro_api_version() != RETRO_API_VERSION) fail("unsupported libretro API version");

    p_retro_set_environment(cb_environment);
    p_retro_init();
    g_initialized = true;

    retro_system_info si; memset(&si,0,sizeof si); p_retro_get_system_info(&si);
    printf("core: %s %s  need_fullpath=%d\n", si.library_name?si.library_name:"?",
           si.library_version?si.library_version:"?", si.need_fullpath);

    retro_game_info gi; memset(&gi,0,sizeof gi); gi.path=romPath;
    static std::vector<uint8_t> rom; // Keep in-memory content alive through core teardown.
    {
        FILE* f=fopen(romPath,"rb"); if(!f) throw std::runtime_error(std::string("cannot open ROM ") + romPath);
        if (fseek(f,0,SEEK_END) != 0) { fclose(f); fail("cannot seek ROM"); }
        long n=ftell(f);
        if (n <= 0 || n > 64*1024*1024 || fseek(f,0,SEEK_SET) != 0) { fclose(f); fail("invalid ROM size"); }
        if (!si.need_fullpath) {
            rom.resize(size_t(n));
            size_t got = fread(rom.data(),1,rom.size(),f);
            if (got != rom.size()) { fclose(f); fail("short ROM read"); }
        }
        fclose(f);
    }
    if (!si.need_fullpath) {
        gi.data=rom.data(); gi.size=rom.size();
    }
    p_retro_set_video_refresh(cb_video);
    p_retro_set_audio_sample(cb_audio_sample);
    p_retro_set_audio_sample_batch(cb_audio_batch);
    p_retro_set_input_poll(cb_input_poll);
    p_retro_set_input_state(cb_input_state);
    if (!p_retro_load_game(&gi)) { fprintf(stderr,"retro_load_game failed\n"); return 4; }
    g_loaded = true;
    p_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    g_log = open_output(trace_path());
    g_cycle_log = optional_output("NESREF_CYCLE_FILE");
    g_ppu_log = optional_output("NESREF_PPU_FILE");
    g_status = optional_output("NESREF_STATUS_FILE");

    // Probe which memory regions this core exposes (NESREF_MEMPROBE=1).
    // Mesen exposes SYSTEM_RAM but no PPU memory/counter via this API. The
    // optional cycle/PPU streams below decode the pinned core's savestate.
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

    if (!headless) {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 5; }
        g_win = SDL_CreateWindow("nesref (libretro oracle) — Esc quit",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, vw*2, vh*2, SDL_WINDOW_RESIZABLE);
        if (!g_win) fail(SDL_GetError());
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!g_ren) fail(SDL_GetError());
        if (SDL_RenderSetLogicalSize(g_ren, vw, vh) != 0) fail(SDL_GetError());
        printf("RUN. KB: arrows=DPad Z=B X=A Enter=Start RShift=Select Esc=quit\n"); fflush(stdout);
    } else {
        printf("RUN headless: %ld frames\n", run_frames); fflush(stdout);
    }

    long dump_frame = -1;
    const char* dump_path = nullptr;
    if (const char* value = getenv("NESREF_STATEDUMP")) {
        char* end; errno = 0;
        dump_frame = strtol(value, &end, 10);
        if (errno || end == value || dump_frame <= 0 || *end != ':' || !end[1])
            fail("NESREF_STATEDUMP must be frame:path");
        dump_path = end+1;
    }
    long shot_frame = -1;
    const char* shot_path = getenv("NESREF_SHOT_FILE");
    if (!shot_path || !shot_path[0]) shot_path = "nesref_shot.png";
    if (const char* value = getenv("NESREF_SHOT")) {
        char* end; errno = 0;
        shot_frame = strtol(value, &end, 10);
        if (errno || end == value || *end || shot_frame <= 0)
            fail("NESREF_SHOT must be a positive frame number");
    }
    bool running=true;
    const char* reason = "quit";
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
        if (!running) break;
        // State-anchored input: advance the script (reading live WRAM for
        // WAIT_RAM8) to set this frame's held buttons before the core polls.
        script_tick((const uint8_t*)p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
        p_retro_run();
        g_frame++;
        apply_freeze();   // force shared RNG/seed before snapshotting (matches recomp order)
        trace_tick();
        dump_check();
        if ((long)g_frame == shot_frame) write_shot(shot_path);
        if (g_cycle_log || g_ppu_log || (long)g_frame == dump_frame) {
            size_t sz = p_retro_serialize_size();
            if (!sz || sz > 64*1024*1024) fail("invalid core serialization size");
            static std::vector<uint8_t> blob;
            blob.assign(sz, 0); // Mesen pads its maximum-size buffer; avoid stale tail bytes.
            if (!p_retro_serialize(blob.data(), sz)) fail("retro_serialize failed");
            if ((long)g_frame == dump_frame) {
                FILE* f = open_output(dump_path);
                bool ok = fwrite(blob.data(), 1, sz, f) == sz;
                int closed = fclose(f);
                if (!ok || closed != 0) fail("savestate dump write failed");
                printf("[nesref statedump] frame %ld -> %s (%zu bytes)\n", dump_frame, dump_path, sz);
            }
            if (g_cycle_log || g_ppu_log) trace_state(blob);
        }
        // Flush all requested data before publishing the heartbeat. RAM/PPU
        // delta streams deliberately omit unchanged frames; status does not.
        flush_output(g_log); flush_output(g_cycle_log); flush_output(g_ppu_log); flush_output(g_wav);
        if (g_status) {
            fprintf(g_status, "{\"type\":\"frame\",\"f\":%u}\n", g_frame);
            flush_output(g_status);
        }
        if (run_frames>0 && (long)g_frame>=run_frames) { reason="frame_limit"; running=false; }
        // Script finished (all commands incl. the post-anchor settle consumed) =
        // the recomp's EXIT: stop here so both engines' FINAL state is the same
        // script point, not NESREF_FRAMES (which would overshoot past the anchor).
        if (g_have_script && g_script_done) { reason="script_done"; running=false; }
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
    if (dump_frame > (long)g_frame || shot_frame > (long)g_frame)
        fail("run ended before the requested screenshot/savestate frame");
    wav_close();
    close_output(g_log); close_output(g_cycle_log); close_output(g_ppu_log);
    if (g_status) {
        fprintf(g_status, "{\"type\":\"complete\",\"f\":%u,\"requested_frames\":%ld,\"reason\":\"%s\"}\n",
                g_frame, run_frames, reason);
        flush_output(g_status);
        close_output(g_status);
    }
    return 0;
}

int main(int argc, char** argv) {
    int result = 2;
    try { result = run_frontend(argc, argv); }
    catch (const std::exception& error) { fprintf(stderr, "nesref: %s\n", error.what()); }
    // Also clean up partial initialization and failed output runs. Such runs
    // have no completion record and a nonzero exit status, even if traces exist.
    if (g_loaded) p_retro_unload_game();
    if (g_initialized) p_retro_deinit();
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
    if (g_core) SDL_UnloadObject(g_core);
    for (FILE* f : {g_log, g_cycle_log, g_ppu_log, g_status, g_wav}) if (f) fclose(f);
    return result;
}
