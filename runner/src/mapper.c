/*
 * mapper.c — Mapper 1 (MMC1) implementation
 *
 * MMC1 uses a 5-bit serial shift register written one bit at a time.
 * Writing a value with bit 7 set resets the shift register.
 * After 5 writes, the accumulated 5-bit value is applied to a register
 * determined by address bits 14-13:
 *   $8000-$9FFF: Control  (mirroring, PRG/CHR bank mode)
 *   $A000-$BFFF: CHR bank 0
 *   $C000-$DFFF: CHR bank 1
 *   $E000-$FFFF: PRG bank select
 *
 * Faxanadu uses PRG bank mode 3 by default:
 *   - $8000-$BFFF: switchable 16KB bank (g_current_bank)
 *   - $C000-$FFFF: fixed to last bank (bank 15)
 */
#include "mapper.h"
#include "nes_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t *s_prg_data  = NULL;
static int            s_prg_banks = 0;
int                   g_current_bank = 0;

/* MMC1 shift register state */
static uint8_t s_shift_reg   = 0x10; /* Reset state: bit 4 set */
static int     s_shift_count  = 0;

/* MMC1 internal registers */
static uint8_t s_ctrl    = 0x1C; /* Default: PRG mode 3, CHR mode 0 */
static uint8_t s_chr0    = 0;
static uint8_t s_chr1    = 0;
static uint8_t s_prg_reg = 0;

/* Trace file */
static FILE *s_mapper_trace = NULL;
extern uint64_t g_frame_count; /* defined in runtime.c */

void mapper_init(const uint8_t *prg_data, int prg_banks) {
    s_prg_data  = prg_data;
    s_prg_banks = prg_banks;
    g_current_bank = 0;
    s_shift_reg   = 0x10;
    s_shift_count  = 0;
    s_ctrl    = 0x1C; /* PRG mode 3: fix last bank at $C000, switch $8000 */
    s_chr0    = 0;
    s_chr1    = 0;
    s_prg_reg = 0;

    s_mapper_trace = fopen("C:/temp/mapper_trace.csv", "w");
    if (s_mapper_trace) {
        fprintf(s_mapper_trace, "EVENT,bank,PC,FRAME\n");
        fflush(s_mapper_trace);
    }
    printf("[Mapper] Init: %d banks, Mapper 1 (MMC1)\n", prg_banks);
}

static void mmc1_apply_prg(void) {
    int mode = (s_ctrl >> 2) & 3;
    int bank = s_prg_reg & 0x0F;
    int new_bank = 0;

    switch (mode) {
        case 0: case 1:
            /* 32KB mode: switch both $8000 and $C000 together */
            new_bank = bank & 0x0E;
            break;
        case 2:
            /* Fix $8000 to bank 0, switch $C000 */
            new_bank = 0;
            break;
        case 3:
            /* Fix $C000 to last bank, switch $8000 — most common */
            new_bank = bank;
            break;
    }

    if (new_bank != g_current_bank) {
        g_current_bank = new_bank;
        if (s_mapper_trace) {
            fprintf(s_mapper_trace, "BANK_SWITCH,bank=%d,PC=?,F=%llu\n",
                    new_bank, (unsigned long long)g_frame_count);
            fflush(s_mapper_trace);
        }
    }
}

void mapper_write(uint16_t addr, uint8_t val) {
    /* Reset shift register if bit 7 set */
    if (val & 0x80) {
        s_shift_reg   = 0x10;
        s_shift_count  = 0;
        s_ctrl |= 0x0C; /* Set PRG bank mode to 3 */
        return;
    }

    /* Shift in bit 0 */
    s_shift_reg = (s_shift_reg >> 1) | ((val & 1) << 4);
    s_shift_count++;

    if (s_shift_count == 5) {
        uint8_t data = s_shift_reg & 0x1F;
        /* Apply to the appropriate register */
        if (addr <= 0x9FFF)      { s_ctrl    = data; }
        else if (addr <= 0xBFFF) { s_chr0    = data; }
        else if (addr <= 0xDFFF) { s_chr1    = data; }
        else                     { s_prg_reg = data; mmc1_apply_prg(); }

        /* Reset shift register */
        s_shift_reg   = 0x10;
        s_shift_count  = 0;
    }
}

const uint8_t *mapper_get_switchable_bank(void) {
    if (!s_prg_data) return NULL;
    return s_prg_data + (size_t)g_current_bank * 0x4000;
}

const uint8_t *mapper_get_fixed_bank(void) {
    if (!s_prg_data) return NULL;
    return s_prg_data + (size_t)(s_prg_banks - 1) * 0x4000;
}
