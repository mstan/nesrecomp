/*
 * rom_parser.h — iNES ROM parsing
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PRG_BANK_SIZE 0x4000  /* 16KB */
#define CHR_BANK_SIZE 0x2000  /* 8KB */
#define INES_HEADER_SIZE 16
#define MAX_32K_WINDOWS 8     /* max 32KB PRG windows (GxROM has up to 4) */

typedef struct {
    uint8_t *prg_data;      /* All PRG ROM data concatenated */
    int      prg_banks;     /* Number of 16KB PRG banks */
    int      chr_banks;     /* Number of 8KB CHR banks (0 = CHR RAM) */
    int      mapper;        /* Mapper number */
    uint16_t nmi_vector;    /* $FFFA/$FFFB (from last/power-on bank) */
    uint16_t reset_vector;  /* $FFFC/$FFFD */
    uint16_t irq_vector;    /* $FFFE/$FFFF */

    /* Per-window vectors for mappers with fully-switchable 32KB banks (e.g. GxROM).
     * num_windows == 0 means vectors are the same for all banks (traditional layout). */
    int      num_windows;
    uint16_t window_nmi[MAX_32K_WINDOWS];
    uint16_t window_irq[MAX_32K_WINDOWS];
} NESRom;

/* True if this mapper switches the entire 32KB ($8000-$FFFF) as one bank */
static inline bool rom_mapper_full_32k_switch(const NESRom *rom) {
    return rom->mapper == 66; /* GxROM; add other mappers here as needed */
}

static inline bool rom_mapper40(const NESRom *rom) {
    return rom->mapper == 40;
}

/* Convert a Mapper-40 CPU PRG window address to the stable 16KB generated
 * identity used by the rest of the recompiler. selected_8k is consulted only
 * for the switchable $C000-$DFFF window; pass -1 when that window is unknown. */
bool rom_mapper40_cpu_to_generated(const NESRom *rom, uint16_t cpu_addr,
                                   int selected_8k, uint16_t *out_addr,
                                   int *out_bank);

/* Read a byte from PRG ROM by NES address + current switchable bank */
uint8_t rom_read(const NESRom *rom, int switchable_bank, uint16_t addr);

/* Get pointer to start of a bank (0-indexed) */
const uint8_t *rom_bank_ptr(const NESRom *rom, int bank);

bool rom_parse(const char *path, NESRom *out);
void rom_free(NESRom *rom);
