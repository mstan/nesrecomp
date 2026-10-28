/*
 * cyc_recomp.h - included by NESRecomp --cycle-accurate output
 * (<prefix>_cyc.c and its per-bank <prefix>_cyc_bNN.c files).
 *
 * Generated code performs each instruction's CPU cycles through cpu6502.h;
 * see recompiler/src/cyc_codegen.c.
 */
#pragma once
#include "cpu6502.h"
#include "cyc_hooks.h"

/* One compiled view of the cartridge: the instructions of one 4KB PRG bank as
 * mapped in one of the CPU's eight 4KB slots. A block folds the ROM bytes at
 * its address to constants, so it is only valid while that bank is the one
 * the mapper has there - which is what the generated dispatch checks, using
 * hw_prg_bank4(). NROM has one fixed bank per slot and never leaves it. */
typedef struct {
    const uint8_t *bits;            /* 0x2000 bits: which offsets start an instruction */
    void (*const *chunks)(void);    /* one function per 1KB, 0 where nothing is compiled */
} CycNativeView;

/* Provided by the generated umbrella file. */
extern const char    *cyc_native_program_name;
/* game.toml [game] name: the title players see, or NULL (then the program name). */
extern const char    *cyc_native_display_name;
extern const uint32_t cyc_native_prg_hash;
extern const uint32_t cyc_native_cart_hash;
/* FDS programs (mapper 20): the BIOS CRC32 the compiler verified against
 * bios/disksys.toml, and game.toml's [fds] bios and image, resolved to
 * absolute paths at compile time as the host's defaults. 0 / NULL for
 * cartridges. */
extern const uint32_t cyc_native_fds_bios_crc32;
extern const char    *cyc_native_fds_bios_path;
extern const char    *cyc_native_fds_image_path;
/* game.toml [fds] hle: the HLE axes the program asks for by default (a list
 * common/nes_fds_hle.h parses), or NULL. */
extern const char    *cyc_native_fds_hle;
/* game.toml [game] console: the host's default output stage, a CycConsole
 * (0 = the board's default, 1 = nes, 2 = famicom); --console overrides it. */
extern const uint8_t  cyc_native_console;
bool cyc_native_has(uint16_t addr);
#if NESRECOMP_NATIVE_PRG_GUARDS
bool cyc_native_prg_guard_available(void);
#endif
/* Union of opcode and operand bytes over every compiled ROM bank/slot view. */
bool cyc_native_prg_code_byte(uint32_t offset);
void cyc_native_run(void);

/* One compiled view of code in RAM: instructions compiled from one known
 * image of RAM (a disk file at its load address, or a snapshot a run
 * captured), grouped by static control flow within one 1KB chunk. The block
 * folds its instructions' own bytes to constants and nothing else, so it is
 * valid exactly while RAM holds those bytes: the dependency runs below. The
 * scheduler (cyc_ramview.c) enters it only while that holds, keeping the
 * answer current by watching writes to every byte a view folds; a block that
 * stores to a byte some validated view folds returns (cyc_ram_code_dirty).
 * Addresses are CPU RAM $0000-$07FF (not the stack page) and, on the FDS,
 * PRG RAM $6000-$DFFF. */
typedef struct {
    void (*fn)(void);               /* runs from cpu.pc, one of entries */
    const uint16_t *entries;        /* instruction starts, ascending */
    const uint16_t *runs;           /* dependency: (address, length) pairs */
    const uint8_t  *bytes;          /* what the runs must hold, concatenated */
    uint16_t entry_count, run_count;
    uint32_t hash;                  /* FNV-1a of addresses and bytes: the view's identity */
    uint32_t image;                 /* FNV-1a of the image it was compiled from (base, bytes) */
} CycRamView;

/* Provided by the generated umbrella file (cyc_native_ram_view_count may be 0). */
extern const CycRamView *const cyc_native_ram_views[];
extern const uint32_t cyc_native_ram_view_count;
/* Set by the write watch when a store changes a byte that a validated RAM
 * view folds; cleared by the scheduler before it enters a view. Compiled RAM
 * code tests it after every store that can reach such a byte. */
extern uint8_t cyc_ram_code_dirty;

/* A game.toml [[mod_function_hook]] site (cyc_hooks.h): the plugin id (NULL:
 * plugins register by address), the address, and the content key memory must
 * hold there for the site to fire: the CRC-32 of the len bytes from the
 * address (len 0: none, a fixed ROM slot). */
typedef struct {
    const char *id;
    uint16_t    addr;
    uint8_t     len;
    uint32_t    crc32;
} CycHookSite;

/* Provided by the generated umbrella file (the count may be 0). */
extern const CycHookSite cyc_native_hook_sites[];
extern const uint32_t    cyc_native_hook_site_count;
