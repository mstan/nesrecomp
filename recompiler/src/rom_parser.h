/*
 * rom_parser.h — iNES ROM parsing
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PRG_BANK_SIZE 0x4000  /* 16KB */
#define CHR_BANK_SIZE 0x2000  /* 8KB */
#define INES_HEADER_SIZE 16

typedef struct {
    uint8_t *prg_data;      /* All PRG ROM data concatenated */
    int      prg_banks;     /* Number of 16KB PRG banks */
    int      chr_banks;     /* Number of 8KB CHR banks (0 = CHR RAM) */
    int      mapper;        /* Mapper number */
    uint16_t nmi_vector;    /* $FFFA/$FFFB */
    uint16_t reset_vector;  /* $FFFC/$FFFD */
    uint16_t irq_vector;    /* $FFFE/$FFFF */
} NESRom;

/* Read a byte from PRG ROM by NES address + current switchable bank */
uint8_t rom_read(const NESRom *rom, int switchable_bank, uint16_t addr);

/* Get pointer to start of a bank (0-indexed) */
const uint8_t *rom_bank_ptr(const NESRom *rom, int bank);

bool rom_parse(const char *path, NESRom *out);
void rom_free(NESRom *rom);
