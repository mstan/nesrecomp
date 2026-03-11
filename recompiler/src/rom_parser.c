/*
 * rom_parser.c — iNES header parsing, bank extraction, vector reads
 */
#include "rom_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool rom_parse(const char *path, NESRom *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "rom_parser: cannot open %s\n", path);
        return false;
    }

    /* Read iNES header */
    uint8_t header[INES_HEADER_SIZE];
    if (fread(header, 1, INES_HEADER_SIZE, f) != INES_HEADER_SIZE) {
        fclose(f);
        return false;
    }

    /* Validate magic */
    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A) {
        fprintf(stderr, "rom_parser: not an iNES file\n");
        fclose(f);
        return false;
    }

    out->prg_banks = header[4];
    out->chr_banks = header[5];
    out->mapper    = ((header[6] >> 4) & 0x0F) | (header[7] & 0xF0);

    /* Skip trainer if present */
    if (header[6] & 0x04) {
        fseek(f, 512, SEEK_CUR);
    }

    /* Read all PRG ROM */
    size_t prg_size = (size_t)out->prg_banks * PRG_BANK_SIZE;
    out->prg_data = (uint8_t *)malloc(prg_size);
    if (!out->prg_data) {
        fclose(f);
        return false;
    }
    if (fread(out->prg_data, 1, prg_size, f) != prg_size) {
        fprintf(stderr, "rom_parser: PRG read failed\n");
        free(out->prg_data);
        fclose(f);
        return false;
    }
    fclose(f);

    /* Fixed bank is always the last bank */
    int fixed_bank = out->prg_banks - 1;
    const uint8_t *fixed = out->prg_data + (size_t)fixed_bank * PRG_BANK_SIZE;

    /* Read vectors from fixed bank — NES $FFFA-$FFFF = offset $3FFA in bank */
    out->nmi_vector   = fixed[0x3FFA] | ((uint16_t)fixed[0x3FFB] << 8);
    out->reset_vector = fixed[0x3FFC] | ((uint16_t)fixed[0x3FFD] << 8);
    out->irq_vector   = fixed[0x3FFE] | ((uint16_t)fixed[0x3FFF] << 8);

    return true;
}

void rom_free(NESRom *rom) {
    free(rom->prg_data);
    rom->prg_data = NULL;
}

const uint8_t *rom_bank_ptr(const NESRom *rom, int bank) {
    if (bank < 0 || bank >= rom->prg_banks) return NULL;
    return rom->prg_data + (size_t)bank * PRG_BANK_SIZE;
}

uint8_t rom_read(const NESRom *rom, int switchable_bank, uint16_t addr) {
    int fixed_bank = rom->prg_banks - 1;
    if (addr >= 0xC000) {
        /* Fixed bank */
        return rom->prg_data[(size_t)fixed_bank * PRG_BANK_SIZE + (addr - 0xC000)];
    } else if (addr >= 0x8000) {
        /* Switchable bank */
        if (switchable_bank < 0 || switchable_bank >= rom->prg_banks)
            return 0xFF;
        return rom->prg_data[(size_t)switchable_bank * PRG_BANK_SIZE + (addr - 0x8000)];
    }
    return 0xFF; /* Not ROM space */
}
