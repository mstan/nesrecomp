/*
 * nestopia_bridge.cpp — Bridge between Nestopia libretro core and NES recomp
 *
 * Uses the standard libretro API: retro_init, retro_load_game, retro_run,
 * retro_get_memory_data. No stubs. No driver dependencies.
 */
#include "nestopia_bridge.h"
#include "libretro.h"

/* Nestopia core internals for PPU/CPU/CHR state extraction (oracle mode). */
#include "source/core/api/NstApiEmulator.hpp"
#include "source/core/NstMachine.hpp"
#include "source/core/NstPpu.hpp"
#include "source/core/NstCartridge.hpp"
#include "source/core/board/NstBoard.hpp"
#include "source/core/board/NstBoardMmc3.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Exposed by libretro.cpp */
namespace Nes { namespace Api { class Emulator; } }
extern Nes::Api::Emulator& nestopia_get_emulator_instance(void);

/* ---- Libretro callbacks ---- */
static uint32_t s_framebuf_xrgb8888[256 * 240];
static unsigned s_frame_width = 256, s_frame_height = 240;
static int16_t  s_audiobuf[48000];
static int      s_audio_frames = 0;
static uint8_t  s_input_state = 0;
static bool     s_loaded = false;

/* Video callback — Nestopia renders here */
static void retro_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (!data) return;
    s_frame_width = width;
    s_frame_height = height;
    for (unsigned y = 0; y < height && y < 240; y++) {
        memcpy(s_framebuf_xrgb8888 + y * 256,
               (const uint8_t *)data + y * pitch,
               (width < 256 ? width : 256) * sizeof(uint32_t));
    }
}

/* Audio callback */
static void retro_audio_sample(int16_t left, int16_t right) {
    (void)left; (void)right;
}

static size_t retro_audio_sample_batch(const int16_t *data, size_t frames) {
    (void)data; (void)frames;
    return frames;
}

/* Input callbacks */
static void retro_input_poll(void) {}

static int16_t retro_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)device; (void)index;
    if (port != 0) return 0;
    /* Map libretro button IDs to our button bitmask */
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_A:      return (s_input_state & 0x80) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_B:      return (s_input_state & 0x40) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: return (s_input_state & 0x20) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_START:  return (s_input_state & 0x10) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_UP:     return (s_input_state & 0x08) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   return (s_input_state & 0x04) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   return (s_input_state & 0x02) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return (s_input_state & 0x01) ? 1 : 0;
        default: return 0;
    }
}

/* Environment callback */
static bool retro_environment(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            *(const char **)data = ".";
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            *(const char **)data = ".";
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            /* Accept any format — Nestopia uses XRGB8888 */
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *var = (struct retro_variable *)data;
            var->value = NULL;
            return false;
        }
        default:
            return false;
    }
}

/* ---- Public API ---- */

extern "C" {

int nestopia_bridge_init(const char *rom_path) {
    /* Set callbacks */
    retro_set_environment(retro_environment);
    retro_set_video_refresh(retro_video_refresh);
    retro_set_audio_sample(retro_audio_sample);
    retro_set_audio_sample_batch(retro_audio_sample_batch);
    retro_set_input_poll(retro_input_poll);
    retro_set_input_state(retro_input_state);

    retro_init();

    /* Load ROM */
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "[nestopia] Cannot open ROM: %s\n", rom_path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom_data = (uint8_t *)malloc(size);
    fread(rom_data, 1, size, f);
    fclose(f);

    struct retro_game_info info = {};
    info.path = rom_path;
    info.data = rom_data;
    info.size = (size_t)size;

    if (!retro_load_game(&info)) {
        fprintf(stderr, "[nestopia] Failed to load ROM: %s\n", rom_path);
        free(rom_data);
        return -2;
    }
    free(rom_data);

    s_loaded = true;
    fprintf(stderr, "[nestopia] Loaded ROM: %s\n", rom_path);
    return 0;
}

void nestopia_bridge_run_frame(uint8_t buttons) {
    if (!s_loaded) return;
    s_input_state = buttons;
    retro_run();
}

void nestopia_bridge_get_ram(uint8_t *out) {
    if (!out) return;
    void *data = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (data && size > 0) {
        memcpy(out, data, size < 0x800 ? size : 0x800);
    }
}

void nestopia_bridge_get_sram(uint8_t *out) {
    if (!out) return;
    void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (data && size > 0) {
        memcpy(out, data, size < 0x2000 ? size : 0x2000);
    }
}

void nestopia_bridge_get_framebuf_argb(uint32_t *out) {
    if (!out) return;
    /* XRGB8888 → ARGB8888 (just set alpha to 0xFF) */
    for (int i = 0; i < 256 * 240; i++) {
        out[i] = s_framebuf_xrgb8888[i] | 0xFF000000;
    }
}

void nestopia_bridge_write_ram(uint16_t addr, uint8_t val) {
    void *data = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (data && addr < size) {
        ((uint8_t *)data)[addr] = val;
    }
}

void nestopia_bridge_get_vram(uint8_t *out, int *out_size) {
    if (!out || !out_size) return;
    void *data = retro_get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_VIDEO_RAM);
    *out_size = (int)size;
    if (data && size > 0) {
        int copy = size < 0x4000 ? (int)size : 0x4000;
        memcpy(out, data, copy);
    }
}

void nestopia_bridge_shutdown(void) {
    if (s_loaded) {
        retro_unload_game();
        s_loaded = false;
    }
    retro_deinit();
}

/* ---- Oracle state extraction (reaches into Nestopia internals) ---- */

uint8_t nestopia_bridge_cpu_read(uint16_t addr) {
    if (!s_loaded) return 0xFF;
    Nes::Api::Emulator &emu = nestopia_get_emulator_instance();
    Nes::Core::Machine &mach = emu.GetMachine();
    return (uint8_t)mach.cpu.Peek(addr);
}

void nestopia_bridge_get_ppu_regs(NestopiaPpuRegs *out) {
    if (!out || !s_loaded) return;
    Nes::Api::Emulator &emu = nestopia_get_emulator_instance();
    Nes::Core::Machine &mach = emu.GetMachine();
    const Nes::Core::Ppu &ppu = mach.ppu;

    out->ctrl = (uint8_t)(ppu.GetCtrl(0) & 0xFF);
    out->mask = (uint8_t)(ppu.GetCtrl(1) & 0xFF);

    /* Reconstruct pixel-level scroll from PPU internals.
     *   bits 0-4 of scroll address: coarse X
     *   bits 5-9: coarse Y
     *   bits 12-14: fine Y
     *   xFine: fine X (0-7) */
    unsigned addr = ppu.GetScrollAddress();
    unsigned xFine = ppu.GetScrollXFine();
    unsigned coarseX = addr & 0x1F;
    unsigned coarseY = (addr >> 5) & 0x1F;
    unsigned fineY = (addr >> 12) & 0x07;

    out->scroll_x = (uint8_t)((coarseX << 3) | (xFine & 7));
    out->scroll_y = (uint8_t)((coarseY << 3) | fineY);
}

void nestopia_bridge_get_ppu_internals(NestopiaPpuInternals *out) {
    if (!out || !s_loaded) return;
    memset(out, 0, sizeof(*out));
    Nes::Api::Emulator &emu = nestopia_get_emulator_instance();
    Nes::Core::Machine &mach = emu.GetMachine();
    const Nes::Core::Ppu &ppu = mach.ppu;

    out->v       = (uint16_t)(ppu.GetScrollAddress() & 0x7FFF);
    out->t       = (uint16_t)(ppu.GetScrollLatch() & 0x7FFF);
    out->w       = (uint8_t)(ppu.GetScrollToggle() & 1);
    out->fine_x  = (uint8_t)(ppu.GetScrollXFine() & 7);
    out->status  = (uint8_t)(ppu.GetStatus() & 0xFF);
    out->oam_addr = (uint8_t)(ppu.GetOamAddr() & 0xFF);
    out->scanline = ppu.GetScanlinePos();

    /* Derive scroll from t (rendering scroll — what frame will actually use) */
    unsigned t = out->t;
    out->scroll_x_from_t = (uint8_t)(((t & 0x1F) << 3) | (out->fine_x & 7));
    out->scroll_y_from_t = (uint8_t)((((t >> 5) & 0x1F) << 3) | ((t >> 12) & 7));

    /* Derive scroll from v (current VRAM address — stale after $2006/$2007) */
    unsigned v = out->v;
    out->scroll_x_from_v = (uint8_t)(((v & 0x1F) << 3) | (out->fine_x & 7));
    out->scroll_y_from_v = (uint8_t)((((v >> 5) & 0x1F) << 3) | ((v >> 12) & 7));
}

void nestopia_bridge_get_chr_ram(uint8_t *out, int len) {
    if (!out || !s_loaded) return;
    if (len > 0x2000) len = 0x2000;
    Nes::Api::Emulator &emu = nestopia_get_emulator_instance();
    Nes::Core::Machine &mach = emu.GetMachine();
    Nes::Core::Ppu::ChrMem &chr = mach.ppu.GetChrMem();
    for (int i = 0; i < len; i++)
        out[i] = chr.Peek(i);
}

void nestopia_bridge_get_nametable(uint8_t *out, int len) {
    if (!out || !s_loaded) return;
    if (len > 0x1000) len = 0x1000;
    Nes::Api::Emulator &emu = nestopia_get_emulator_instance();
    Nes::Core::Machine &mach = emu.GetMachine();
    Nes::Core::Ppu::NmtMem &nmt = mach.ppu.GetNmtMem();
    for (int i = 0; i < len; i++)
        out[i] = nmt.Peek(i);
}

void nestopia_bridge_get_palette(uint8_t *out) {
    if (!out || !s_loaded) return;
    Nes::Api::Emulator &emu = nestopia_get_emulator_instance();
    Nes::Core::Machine &mach = emu.GetMachine();
    mach.ppu.GetPaletteRam(out);
}

void nestopia_bridge_get_oam(uint8_t *out) {
    if (!out || !s_loaded) return;
    Nes::Api::Emulator &emu = nestopia_get_emulator_instance();
    Nes::Core::Machine &mach = emu.GetMachine();
    mach.ppu.GetOamRam(out);
}

void nestopia_bridge_get_cpu_regs(NestopiaCpuRegs *out) {
    if (!out || !s_loaded) return;
    Nes::Api::Emulator &emu = nestopia_get_emulator_instance();
    Nes::Core::Machine &mach = emu.GetMachine();
    out->a  = (uint8_t)mach.cpu.GetA();
    out->x  = (uint8_t)mach.cpu.GetX();
    out->y  = (uint8_t)mach.cpu.GetY();
    out->sp = (uint8_t)mach.cpu.GetSP();
    out->p  = (uint8_t)mach.cpu.GetFlags();
    out->pc = (uint16_t)mach.cpu.GetPC();
}

int nestopia_bridge_get_mirroring(void) {
    if (!s_loaded) return -1;
    Nes::Api::Emulator &emu = nestopia_get_emulator_instance();
    Nes::Core::Machine &mach = emu.GetMachine();
    Nes::Core::Ppu::NmtMem &nmt = mach.ppu.GetNmtMem();
    uint8_t nt[4][16];
    for (int page = 0; page < 4; page++)
        for (int i = 0; i < 16; i++)
            nt[page][i] = nmt.Peek(page * 0x400 + i);
    int nt0_eq_nt1 = (memcmp(nt[0], nt[1], 16) == 0);
    int nt0_eq_nt2 = (memcmp(nt[0], nt[2], 16) == 0);
    if (nt0_eq_nt1 && !nt0_eq_nt2) return 3; /* horizontal */
    if (nt0_eq_nt2 && !nt0_eq_nt1) return 2; /* vertical */
    if (nt0_eq_nt1 && nt0_eq_nt2) return 0;  /* single-screen / all same */
    return -1;
}

int nestopia_bridge_is_loaded(void) {
    return s_loaded ? 1 : 0;
}

/* MMC3 mapper state extraction — returns valid=0 for non-MMC3 boards. */
void nestopia_bridge_get_mapper_state(NestopiaMapperState *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_loaded) return;

    Nes::Api::Emulator &emu = nestopia_get_emulator_instance();
    Nes::Core::Machine &mach = emu.GetMachine();

    /* Access the board through Machine -> Image (Cartridge) -> Board.
     * Returns early if the loaded image isn't a cartridge or isn't MMC3. */
    Nes::Core::Cartridge *cart =
        dynamic_cast<Nes::Core::Cartridge *>(mach.image);
    if (!cart) return;

    Nes::Core::Boards::Mmc3 *mmc3 =
        dynamic_cast<Nes::Core::Boards::Mmc3 *>(cart->GetBoard());
    if (!mmc3) return;

    out->valid = 1;
    out->bank_select = (uint8_t)(mmc3->GetRegsCtrl0() & 0x07);

    /* CHR banks (8 entries) + PRG banks (4 entries) */
    const Nes::byte *chr = mmc3->GetBanksChr();
    const Nes::byte *prg = mmc3->GetBanksPrg();
    for (int i = 0; i < 8; i++) out->regs[i] = chr[i];
    for (int i = 0; i < 4; i++) out->prg[i]  = prg[i];

    /* IRQ state via public accessors */
    out->irq_latch   = (uint8_t)mmc3->GetIrqLatch();
    out->irq_counter  = (uint8_t)mmc3->GetIrqCount();
    out->irq_reload   = (uint8_t)mmc3->GetIrqReload();
    out->irq_enabled  = (uint8_t)mmc3->GetIrqEnabled();
}

} /* extern "C" */
