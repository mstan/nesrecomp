/*
 * cyc_core.h - host interface to the cycle-accurate NES machine.
 *
 * Two implementations provide it:
 *   - NESRecomp's machine: its CPU (cpu6502.h: recompiled code plus the
 *     generated interpreter) and hardware (hw_machine.c, hw_ppu.c,
 *     hw_apu.c) behind hw.h;
 *   - the TriCNES oracle (tric_core.cpp), used only to check the first.
 * Hosts use only what is declared here, so the same host code drives both.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- machine control ---- */

/* Load an iNES/NES 2.0 image. Returns false on unsupported input; see MAPPERS.md. */
bool cyc_load_ines(const uint8_t *image, size_t size);
/* ---- Famicom Disk System (hw_fds.c) ----
 *
 * The RAM Adapter is loaded from its BIOS and a disk image instead of an iNES
 * file. cyc_load_fds() accepts any 8 KiB BIOS; hosts check its identity
 * (common/nes_fds.h, bios/disksys.toml) before calling it. The drive profile
 * picks whose model of the drive runs (hw_fds.c lists the differences):
 * MESEN is the nesref oracle's (libretro/Mesen) and the default. stream_crc picks the CRC
 * bytes the side streams carry: the real CRC-16 (default) or Mesen's constant
 * $4D $62; crc_check makes $4030.4 report a mismatch in any profile. */
typedef enum { CYC_FDS_PROFILE_MESEN, CYC_FDS_PROFILE_MESEN2, CYC_FDS_PROFILE_HARDWARE } CycFdsProfile;
typedef enum { CYC_FDS_CRC_COMPUTED, CYC_FDS_CRC_MESEN } CycFdsStreamCrc;
/* Where a written byte lands: under the head (default, every profile), or two
 * bytes behind it as Mesen 0.9.9 / nesref's core store it (hw_fds.c). */
typedef enum { CYC_FDS_WRITE_HEAD, CYC_FDS_WRITE_MESEN } CycFdsWriteAt;
typedef struct {
    CycFdsProfile   profile;
    CycFdsStreamCrc stream_crc;
    CycFdsWriteAt   write_at;
    bool crc_check;
    bool write_protect;    /* the disk's write-protect tab is broken off */
    bool qd;               /* the image is .qd (Mesen2 decides by extension) */
    int  boot_side;        /* side in the drive at power-on; -1 = empty drive */
} CycFdsOptions;
void cyc_fds_default_options(CycFdsOptions *o);
/* image NULL: the RAM Adapter with no disk at all (an empty drive, no sides). */
bool cyc_load_fds(const uint8_t *bios, size_t bios_size, const uint8_t *image, size_t image_size,
                  const CycFdsOptions *options);
bool     cyc_is_fds(void);
unsigned cyc_fds_side_count(void);
/* The side in the drive, or -1 for an empty drive. */
int      cyc_fds_side(void);
/* Eject (false if the drive is already empty) and insert (false unless the
 * drive is empty and the side exists). Take effect on the drive's next cycle;
 * hosts call them between frames, where nesref applies its disk events. */
bool     cyc_fds_eject(void);
bool     cyc_fds_insert(unsigned side);
/* The byte stream the drive clocks for a side, as disk writes left it. */
const uint8_t *cyc_fds_side_stream(unsigned side, uint32_t *len);
/* The stream the loader built from the image, before any write or saved disk. */
const uint8_t *cyc_fds_side_base(unsigned side, uint32_t *len);
/* Put a saved side back (a host's disk sidecar): before power-on, or while the
 * side is out of the drive or its motor is off. False otherwise. */
bool     cyc_fds_set_side_stream(unsigned side, const uint8_t *bytes, uint32_t len);
/* Disk bytes changed by writes since the image was loaded. */
uint32_t cyc_fds_disk_writes(void);
/* Advances on every change to any side (writes and cyc_fds_set_side_stream):
 * hosts compare it with the value at their last save to know a disk is dirty. */
uint64_t cyc_fds_disk_generation(void);
/* A side is in the drive and its motor runs: a disk operation is in progress. */
bool     cyc_fds_motor_on(void);
/* ---- the FDS HLE tier (hw_fds_hle.c; decisions: common/nes_fds_hle.h) ----
 *
 * Hosts decide the plan with nes_fds_hle_plan() and pass its answer here,
 * after cyc_load_fds() and before power-on, and again between frames when a
 * live toggle changes it. id_check / id_pointer are the BIOS's disk-ID check
 * anchor (0: none known, which also turns the always-on disk-ID request
 * observation off). With auto_swap and fast_load off the machine runs exactly
 * as it does without the tier. */
typedef struct {
    bool     auto_swap, fast_load;
    uint16_t id_check;
    uint8_t  id_pointer;
} CycFdsHle;
void cyc_fds_hle_configure(const CycFdsHle *hle);
/* The frame just run was part of a disk load (a load span: data moving, the
 * head rewinding or spinning up, or the BIOS about to start the drive). A
 * host with fast load on runs such frames unpaced. */
bool cyc_fds_hle_loading(void);
typedef struct {
    bool     auto_swap, fast_load, observing, loading;
    int      swap_target;          /* the side an auto swap is putting in, or -1 */
    uint32_t swaps;                /* sides changed by auto swap */
    uint32_t bumps;                /* the same side put back after a wait (no request yet) */
    uint32_t spans, requests;      /* load spans; disk-ID requests seen */
} CycFdsHleStatus;
void cyc_fds_hle_status(CycFdsHleStatus *out);
/* ---- the FDS boot (hw_fds_boot.c, cyc_fds_skip.c) ----
 *
 * boot_jump is the BIOS's jump into the game (JMP ($DFFC), 0: none known).
 * With it known, the machine records where the game starts (ring fds.boot
 * entry) whatever else is planned. boot_skip and auto_insert are the plan's
 * answers (common/nes_fds_hle.h); boot_skip_refused only goes into the ring's
 * plan event. Configure after cyc_load_fds and before power-on; the boot skip
 * also needs cyc_fds_skip_prepare() and cyc_fds_skip_enable() (below). */
typedef struct {
    uint16_t boot_jump;
    bool     boot_skip, boot_skip_refused, auto_insert;
} CycFdsBoot;
void cyc_fds_boot_configure(const CycFdsBoot *boot);
typedef struct {
    bool     entered, skipped;       /* the game has started; the boot skip started it */
    uint16_t entry_pc;
    uint32_t entry_frame;
    uint64_t entry_cycle;
    uint16_t entry_line, entry_dot;  /* where the PPU was */
    bool     insert_armed;           /* auto insert waits for the boot's disk wait */
    bool     auto_inserted;          /* auto insert put side A in, at the end of insert_frame */
    uint32_t insert_frame;
} CycFdsBootStatus;
void cyc_fds_boot_status(CycFdsBootStatus *out);
/* A host's look at the machine after the first run of the instruction at pc
 * (an opcode fetch the RAM Adapter sees: $8000 up): with hw_entry_stop set,
 * the scheduler stops at the next instruction boundary as at the game entry,
 * and cyc_fds_boot_watch_hit() is true from then on. For deriving what the
 * BIOS leaves where; 0 turns it off. */
void cyc_fds_boot_watch(uint16_t pc);
bool cyc_fds_boot_watch_hit(void);
/* The boot skip (cyc_fds_skip.c; model: common/nes_fds_hle.h
 * NesFdsBootModel; analysis: common/nes_fds_boot.h).
 * cyc_fds_skip_prepare runs the BIOS from power-on to its boot LoadFiles call
 * once, headless, keeps the whole machine there (up to that call the boot
 * depends on nothing on the disk), and checks that the model reproduces this
 * image's boot load exactly. It returns NULL when it does, else why not (then
 * the plan refuses the axis). The machine is left in an undefined state: the
 * host powers on as usual afterwards. With the skip enabled, power-on puts
 * that machine back, loads the boot files as LoadFiles would, lets the BIOS
 * run its license check, runs its license screen loop forward and lets it run
 * the last pass and its jump into the game. Call prepare after
 * cyc_fds_boot_configure and before power-on; ppu_alignment as power-on's. */
typedef struct {
    uint16_t load_call, load_entry, jump;
    uint16_t loop_branch, loop_entry, loop_top;
    uint8_t  loop_counter, timer_divider, fast_last, slow_last, divider_reload;
    uint8_t  scroll, scroll_step, scroll_limit;
    uint16_t license, license_vram;
    uint8_t  license_len;
    uint16_t mask_store;
} CycFdsBootModel;
const char *cyc_fds_skip_prepare(const CycFdsBootModel *model, uint8_t ppu_alignment);
void        cyc_fds_skip_enable(bool on);
/* Cycles the skipped boot would have run before the BIOS's LoadFiles call (the
 * pre-run's), so a host can tell the program's own cycles apart; 0 without a
 * skip. */
uint64_t    cyc_fds_skip_prerun_cycles(void);
/* fds.side event sources */
enum { CYC_FDS_SIDE_HOST = 0, CYC_FDS_SIDE_POWER_ON = 1, CYC_FDS_SIDE_HLE = 2, CYC_FDS_SIDE_AUTO = 3 };
/* The sound unit's state in Mesen's field order and widths (FdsAudio,
 * BaseFdsChannel, ModChannel StreamState), so it compares byte for byte with
 * the FdsAudio snapshot in a nesref savestate (tools/cyc/fds_audio_gates.py).
 * Writes CYC_FDS_AUDIO_STATE_BYTES and returns that count. */
#define CYC_FDS_AUDIO_STATE_BYTES 171
size_t   cyc_fds_audio_state(uint8_t *out);
/* PRG RAM ($6000-$DFFF on the FDS) or cartridge work RAM, for host dumps. */
const uint8_t *cyc_cart_ram(size_t *len);
/* The PPU's memories and CHR RAM, for host dumps (NULL / 0 where absent). */
const uint8_t *cyc_ppu_ciram(size_t *len);
const uint8_t *cyc_ppu_palette(void);
const uint8_t *cyc_ppu_oam(void);
const uint8_t *cyc_chr_ram(size_t *len);

/* What CPU RAM holds at power-on. The console leaves no defined state; the
 * default is the pattern AccuracyCoin's power-on page reports from the
 * reference console. Zeros/ones match what other emulators power up with, so
 * a program that reads uninitialized RAM can be compared against them. */
typedef enum { CYC_RAM_PATTERN, CYC_RAM_ZEROS, CYC_RAM_ONES } CycRamInit;
extern CycRamInit cyc_ram_init;

/* Hardware power-on state. ppu_alignment selects the CPU/PPU master clock
 * phase (0-3). The host also powers on the CPU (cyc_run_power_on). */
void cyc_power_on(uint8_t ppu_alignment);
/* FNV-1a 32 of the loaded PRG ROM (matches cyc_native_prg_hash). */
uint32_t cyc_prg_hash(void);
/* Which hardware implementation this is: "nesrecomp" or "tricnes". */
const char *cyc_hw_name(void);

/* Committed nonvolatile bytes, preserved by cyc_power_on. Region 0 is the
 * cartridge; region 1 is Datach's shared 256-byte internal EEPROM.
 * Import only between load/power-on and CPU execution. Length must match. */
/* Datach EAN-8, UPC-A or EAN-13, including a valid check digit. The host
 * chooses swipe speed in CPU cycles per module (1000 is a useful default). */
bool cyc_scan_barcode(const char *digits, unsigned cycles_per_module);
size_t cyc_nvram_size(unsigned region);
bool cyc_nvram_export(unsigned region, void *buffer, size_t size);
bool cyc_nvram_import(unsigned region, const void *buffer, size_t size);

/* ---- host I/O ---- */

/* The 2KB of CPU RAM. */
const uint8_t *cyc_cpu_ram(void);
/* The byte a CPU read of addr would return from RAM or ROM, without the
 * read's side effects. False for addresses that are I/O or open bus. */
bool cyc_debug_peek(uint16_t addr, uint8_t *value);
/* Buttons for controller port 0 or 1 (A B Select Start Up Down Left Right,
 * MSB first), latched by the console when it strobes the port. */
void cyc_set_controller(int port, uint8_t buttons);
/* CPU cycles since power-on, including cycles taken by DMAs. */
uint64_t cyc_cycle_count(void);
/* The picture as 256x240 ARGB8888, and as 9-bit color indices
 * (color | emphasis << 6), as far as the PPU has drawn it. */
const uint32_t *cyc_frame_argb(void);
const uint16_t *cyc_frame_index(void);
/* Audio: signed 16-bit mono at the given rate. cyc_audio_enable returns false
 * if the machine has no audio output. */
bool   cyc_audio_enable(int sample_rate);
size_t cyc_audio_read(int16_t *out, size_t max);
/* Which console's analog output stage shapes the audio (hw_apu.c):
 *   NES      the front-loader's 90 Hz and 440 Hz high-pass and 14 kHz
 *            low-pass (nesdev APU Mixer);
 *   FAMICOM  the Famicom's 37 Hz high-pass (nesdev APU Mixer), the only stage
 *            the Famicom's audio path specifies before its RF modulator.
 * DEFAULT picks by board: FAMICOM for boards that only ever existed for the
 * Famicom and carry expansion audio (the Disk System, Namco 163, VRC6,
 * VRC7), NES for everything else. Takes effect at cyc_audio_enable, or at
 * once while audio is on; cyc_console() is the model in effect (or that
 * cyc_audio_enable would pick for the loaded board). No effect on the CPU. */
typedef enum { CYC_CONSOLE_DEFAULT, CYC_CONSOLE_NES, CYC_CONSOLE_FAMICOM } CycConsole;
void       cyc_set_console(CycConsole console);
CycConsole cyc_console(void);
const char *cyc_console_name(CycConsole console);

/* ---- comparison ---- */

/* Architectural CPU state at an instruction boundary. */
typedef struct {
    uint16_t pc;
    uint8_t  a, x, y, s, p;          /* p without B and bit 5 */
    uint8_t  do_nmi, do_irq;         /* interrupts the next opcode fetch takes */
} CycCpuState;

/* Implemented by the runtime (cyc_run.c) and by the oracle. */
void cyc_cpu_state(CycCpuState *out);

/* Hash of CPU RAM, CIRAM, OAM, palette RAM (6 bits), CHR RAM, the picture's
 * color indices and the cycle count: comparable between any two NES models
 * (cyc_mem_hash in cyc_trace.c). */
uint64_t cyc_mem_state_hash(void);
/* Hash of the hardware internals; comparable only between runs of the same
 * implementation (cyc_hw_name). */
uint64_t cyc_hw_state_hash(void);
/* The hashed hardware fields by name, one per line, to a FILE *. */
void cyc_hw_state_dump(void *file);
/* What cyc_mem_state_hash covers, as hex lines, to a FILE * (cyc_mem_dump in
 * cyc_trace.c gives both machines the same layout). */
void cyc_mem_state_dump(void *file);

/* ---- oracle only ---- */
void cyc_oracle_run_frame(void);

#ifdef __cplusplus
}
#endif
