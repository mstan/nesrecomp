/*
 * mapper.c — NES mapper implementations
 *
 * Supported mappers:
 *   0 — NROM (Super Mario Bros., etc.)   No bank switching; PRG fixed at $8000-$FFFF.
 *   1 — MMC1 (Faxanadu, etc.)            5-bit serial shift register, 4 sub-registers.
 *
 * Add new mappers: extend mapper_init() and mapper_write() with a new case.
 */
#include "mapper.h"
#include "nes_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t *s_prg_data  = NULL;
static int            s_prg_banks = 0;
int                   g_current_bank = 0;
static int            s_mapper_type  = 0;
static int            s_mirroring    = 3; /* default: horizontal */

/* ── Mapper 1 (MMC1) state ─────────────────────────────────────────────────── */
static uint8_t s_shift_reg   = 0x10; /* Reset state: bit 4 set */
static int     s_shift_count  = 0;
static uint8_t s_ctrl    = 0x1C;     /* Default: PRG mode 3, CHR mode 0 */
static uint8_t s_chr0    = 0;
static uint8_t s_chr1    = 0;
static uint8_t s_prg_reg = 0;

/* Trace file */
static FILE *s_mapper_trace = NULL;
extern uint64_t g_frame_count; /* defined in runtime.c */

/* ── MMC1 helper ───────────────────────────────────────────────────────────── */
static void mmc1_apply_prg(void) {
    int mode = (s_ctrl >> 2) & 3;
    int bank = s_prg_reg & 0x0F;
    int new_bank = 0;

    switch (mode) {
        case 0: case 1:
            new_bank = bank & 0x0E;  /* 32KB mode: both $8000 and $C000 together */
            break;
        case 2:
            new_bank = 0;            /* Fix $8000 to bank 0, switch $C000 */
            break;
        case 3:
            new_bank = bank;         /* Fix $C000 to last bank, switch $8000 */
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

    /* Update mirroring from MMC1 ctrl bits 0-1 */
    s_mirroring = s_ctrl & 0x03;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

void mapper_init(const uint8_t *prg_data, int prg_banks,
                 int mapper_type, int initial_mirroring) {
    s_prg_data    = prg_data;
    s_prg_banks   = prg_banks;
    s_mapper_type = mapper_type;
    g_current_bank = 0;

    /* Convert iNES mirroring flag (0=horizontal, 1=vertical) to renderer values
     * (2=vertical, 3=horizontal) for Mapper 0. MMC1 overrides at runtime. */
    s_mirroring = (initial_mirroring == 1) ? 2 : 3;

    /* MMC1 reset state */
    s_shift_reg   = 0x10;
    s_shift_count = 0;
    s_ctrl        = 0x1C; /* PRG mode 3 */
    s_chr0        = 0;
    s_chr1        = 0;
    s_prg_reg     = 0;

    s_mapper_trace = fopen("C:/temp/mapper_trace.csv", "w");
    if (s_mapper_trace) {
        fprintf(s_mapper_trace, "EVENT,bank,PC,FRAME\n");
        fflush(s_mapper_trace);
    }

    printf("[Mapper] Init: %d PRG banks, Mapper %d\n", prg_banks, mapper_type);
}

void mapper_write(uint16_t addr, uint8_t val) {
    switch (s_mapper_type) {
        case 0:
            /* NROM: no bank switching; writes to $8000+ are ignored */
            return;

        case 1:
            /* MMC1: 5-bit serial shift register */
            if (val & 0x80) {
                /* Reset */
                s_shift_reg   = 0x10;
                s_shift_count = 0;
                s_ctrl |= 0x0C;
                return;
            }
            s_shift_reg = (s_shift_reg >> 1) | ((val & 1) << 4);
            s_shift_count++;
            if (s_shift_count == 5) {
                uint8_t data = s_shift_reg & 0x1F;
                if      (addr <= 0x9FFF) { s_ctrl    = data; }
                else if (addr <= 0xBFFF) { s_chr0    = data; }
                else if (addr <= 0xDFFF) { s_chr1    = data; }
                else                     { s_prg_reg = data; mmc1_apply_prg(); }
                s_shift_reg   = 0x10;
                s_shift_count = 0;
            }
            return;

        default:
            /* Unknown mapper: ignore writes */
            return;
    }
}

const uint8_t *mapper_get_switchable_bank(void) {
    if (!s_prg_data) return NULL;
    switch (s_mapper_type) {
        case 0:
            /* NROM: $8000-$BFFF = first 16KB of PRG (bank 0) */
            return s_prg_data;
        default:
            return s_prg_data + (size_t)g_current_bank * 0x4000;
    }
}

const uint8_t *mapper_get_fixed_bank(void) {
    if (!s_prg_data) return NULL;
    /* Both Mapper 0 and Mapper 1: fixed bank = last 16KB of PRG */
    return s_prg_data + (size_t)(s_prg_banks - 1) * 0x4000;
}

int mapper_get_mirroring(void) {
    return s_mirroring;
}

void mapper_get_state(MapperState *out) {
    out->mapper_type  = s_mapper_type;
    out->shift_reg    = s_shift_reg;
    out->shift_count  = s_shift_count;
    out->ctrl         = s_ctrl;
    out->chr0         = s_chr0;
    out->chr1         = s_chr1;
    out->prg_reg      = s_prg_reg;
    out->current_bank = g_current_bank;
    out->mirroring    = s_mirroring;
}

void mapper_set_state(const MapperState *in) {
    s_mapper_type  = in->mapper_type;
    s_shift_reg    = in->shift_reg;
    s_shift_count  = in->shift_count;
    s_ctrl         = in->ctrl;
    s_chr0         = in->chr0;
    s_chr1         = in->chr1;
    s_prg_reg      = in->prg_reg;
    g_current_bank = in->current_bank;
    s_mirroring    = in->mirroring;
}
