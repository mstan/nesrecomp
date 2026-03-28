/*
 * mapper.c — NES mapper implementations
 *
 * Supported mappers:
 *   0 — NROM (Super Mario Bros., etc.)   No bank switching; PRG fixed at $8000-$FFFF.
 *   1 — MMC1 (Faxanadu, etc.)            5-bit serial shift register, 4 sub-registers.
 *   4 — MMC3 (Mega Man 3, SMB3, etc.)    Bank select/data registers, scanline IRQ.
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

/* ── CHR ROM data (for bank switching) ─────────────────────────────────────── */
static const uint8_t *s_chr_rom_data = NULL;
static int            s_chr_rom_banks = 0; /* number of 8KB CHR ROM banks */

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

/* ── MMC1 CHR bank switching ───────────────────────────────────────────────── */
extern uint8_t g_chr_ram[0x2000];

static void mmc1_apply_chr(void) {
    if (!s_chr_rom_data || s_chr_rom_banks == 0) return;

    int chr_mode = (s_ctrl >> 4) & 1;
    int total_4k = s_chr_rom_banks * 2; /* number of 4KB banks */

    if (chr_mode == 0) {
        /* 8KB mode: s_chr0 selects an 8KB bank (bit 0 ignored) */
        int bank_8k = (s_chr0 >> 1) % s_chr_rom_banks;
        memcpy(g_chr_ram, s_chr_rom_data + (size_t)bank_8k * 0x2000, 0x2000);
    } else {
        /* 4KB mode: s_chr0 selects $0000-$0FFF, s_chr1 selects $1000-$1FFF */
        int bank0 = s_chr0 % total_4k;
        int bank1 = s_chr1 % total_4k;
        memcpy(g_chr_ram,          s_chr_rom_data + (size_t)bank0 * 0x1000, 0x1000);
        memcpy(g_chr_ram + 0x1000, s_chr_rom_data + (size_t)bank1 * 0x1000, 0x1000);
    }
}

/* ── Mapper 4 (MMC3) state ─────────────────────────────────────────────────── */
static uint8_t s_mmc3_bank_select = 0;   /* $8000: register select + mode bits */
static uint8_t s_mmc3_regs[8]     = {0}; /* R0-R7: bank register values */
static uint8_t s_mmc3_irq_latch   = 0;   /* $C000: scanline counter reload */
static uint8_t s_mmc3_irq_counter = 0;   /* current scanline counter */
static int     s_mmc3_irq_reload  = 0;   /* flag: reload counter on next clock */
static int     s_mmc3_irq_enabled = 0;   /* $E001 enables, $E000 disables */
static int     s_mmc3_prg_banks_8k = 0;  /* total 8KB PRG banks */

/* MMC3 uses 8KB PRG granularity mapped into 4 windows.
 * We build contiguous 16KB buffers for the existing API. */
static uint8_t s_mmc3_prg_low[0x4000];   /* $8000-$BFFF */
static uint8_t s_mmc3_prg_high[0x4000];  /* $C000-$FFFF */

static void mmc3_apply_prg(void) {
    if (!s_prg_data) return;
    int n = s_mmc3_prg_banks_8k;
    if (n == 0) return;

    int prg_mode = (s_mmc3_bank_select >> 6) & 1;
    int r6 = s_mmc3_regs[6] % n;
    int r7 = s_mmc3_regs[7] % n;
    int second_last = (n >= 2) ? n - 2 : 0;
    int last = n - 1;

    const uint8_t *bank_8000, *bank_a000, *bank_c000, *bank_e000;

    if (prg_mode == 0) {
        /* Mode 0: $8000=R6, $A000=R7, $C000=(-2), $E000=(-1) */
        bank_8000 = s_prg_data + (size_t)r6 * 0x2000;
        bank_a000 = s_prg_data + (size_t)r7 * 0x2000;
        bank_c000 = s_prg_data + (size_t)second_last * 0x2000;
        bank_e000 = s_prg_data + (size_t)last * 0x2000;
    } else {
        /* Mode 1: $8000=(-2), $A000=R7, $C000=R6, $E000=(-1) */
        bank_8000 = s_prg_data + (size_t)second_last * 0x2000;
        bank_a000 = s_prg_data + (size_t)r7 * 0x2000;
        bank_c000 = s_prg_data + (size_t)r6 * 0x2000;
        bank_e000 = s_prg_data + (size_t)last * 0x2000;
    }

    memcpy(s_mmc3_prg_low,          bank_8000, 0x2000);
    memcpy(s_mmc3_prg_low + 0x2000, bank_a000, 0x2000);
    memcpy(s_mmc3_prg_high,          bank_c000, 0x2000);
    memcpy(s_mmc3_prg_high + 0x2000, bank_e000, 0x2000);

    /* Expose the 16KB bank index for recompiler dispatch.
     * Recompiler uses 16KB banks (0-15); MMC3 R6 is an 8KB index (0-31). */
    g_current_bank = r6 / 2;

    if (s_mapper_trace) {
        fprintf(s_mapper_trace, "MMC3_PRG,R6=%d,R7=%d,mode=%d,F=%llu\n",
                r6, r7, prg_mode, (unsigned long long)g_frame_count);
        fflush(s_mapper_trace);
    }
}

static void mmc3_apply_chr(void) {
    if (!s_chr_rom_data || s_chr_rom_banks == 0) return;

    int chr_a12_inv = (s_mmc3_bank_select >> 7) & 1;
    int total_1k = s_chr_rom_banks * 8; /* number of 1KB CHR banks */

    /* R0/R1 select 2KB banks, R2-R5 select 1KB banks.
     * chr_a12_inv swaps which half gets the 2KB vs 1KB banks. */
    int r0 = (s_mmc3_regs[0] & 0xFE) % total_1k; /* 2KB aligned */
    int r1 = (s_mmc3_regs[1] & 0xFE) % total_1k;
    int r2 = s_mmc3_regs[2] % total_1k;
    int r3 = s_mmc3_regs[3] % total_1k;
    int r4 = s_mmc3_regs[4] % total_1k;
    int r5 = s_mmc3_regs[5] % total_1k;

    if (chr_a12_inv == 0) {
        /* $0000-$07FF = R0 (2KB), $0800-$0FFF = R1 (2KB)
         * $1000-$13FF = R2 (1KB), $1400-$17FF = R3, $1800-$1BFF = R4, $1C00-$1FFF = R5 */
        memcpy(g_chr_ram + 0x0000, s_chr_rom_data + (size_t)r0 * 0x0400, 0x0800);
        memcpy(g_chr_ram + 0x0800, s_chr_rom_data + (size_t)r1 * 0x0400, 0x0800);
        memcpy(g_chr_ram + 0x1000, s_chr_rom_data + (size_t)r2 * 0x0400, 0x0400);
        memcpy(g_chr_ram + 0x1400, s_chr_rom_data + (size_t)r3 * 0x0400, 0x0400);
        memcpy(g_chr_ram + 0x1800, s_chr_rom_data + (size_t)r4 * 0x0400, 0x0400);
        memcpy(g_chr_ram + 0x1C00, s_chr_rom_data + (size_t)r5 * 0x0400, 0x0400);
    } else {
        /* Inverted: $0000 gets the 1KB banks, $1000 gets the 2KB banks */
        memcpy(g_chr_ram + 0x0000, s_chr_rom_data + (size_t)r2 * 0x0400, 0x0400);
        memcpy(g_chr_ram + 0x0400, s_chr_rom_data + (size_t)r3 * 0x0400, 0x0400);
        memcpy(g_chr_ram + 0x0800, s_chr_rom_data + (size_t)r4 * 0x0400, 0x0400);
        memcpy(g_chr_ram + 0x0C00, s_chr_rom_data + (size_t)r5 * 0x0400, 0x0400);
        memcpy(g_chr_ram + 0x1000, s_chr_rom_data + (size_t)r0 * 0x0400, 0x0800);
        memcpy(g_chr_ram + 0x1800, s_chr_rom_data + (size_t)r1 * 0x0400, 0x0800);
    }
}

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

    /* MMC3 reset state */
    s_mmc3_bank_select = 0;
    memset(s_mmc3_regs, 0, sizeof(s_mmc3_regs));
    s_mmc3_irq_latch   = 0;
    s_mmc3_irq_counter = 0;
    s_mmc3_irq_reload  = 0;
    s_mmc3_irq_enabled = 0;
    s_mmc3_prg_banks_8k = prg_banks * 2; /* iNES gives 16KB banks; MMC3 uses 8KB */
    if (mapper_type == 4) {
        mmc3_apply_prg(); /* Set up initial PRG windows */
    }

    s_mapper_trace = fopen("C:/temp/mapper_trace.csv", "w");
    if (s_mapper_trace) {
        fprintf(s_mapper_trace, "EVENT,bank,PC,FRAME\n");
        fflush(s_mapper_trace);
    }

    printf("[Mapper] Init: %d PRG banks, Mapper %d\n", prg_banks, mapper_type);
}

void mapper_init_chr(const uint8_t *chr_data, int chr_banks) {
    s_chr_rom_data  = chr_data;
    s_chr_rom_banks = chr_banks;
    if (chr_banks > 0 && s_mapper_type == 1) {
        mmc1_apply_chr(); /* Load initial CHR bank into g_chr_ram */
        printf("[Mapper] CHR ROM: %d x 8KB banks, initial bank switching applied\n", chr_banks);
    }
    if (chr_banks > 0 && s_mapper_type == 4) {
        mmc3_apply_chr(); /* Load initial CHR banks into g_chr_ram */
        printf("[Mapper] CHR ROM: %d x 8KB banks (MMC3), initial bank switching applied\n", chr_banks);
    }
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
                if      (addr <= 0x9FFF) { s_ctrl    = data; mmc1_apply_chr(); }
                else if (addr <= 0xBFFF) { s_chr0    = data; mmc1_apply_chr(); }
                else if (addr <= 0xDFFF) { s_chr1    = data; mmc1_apply_chr(); }
                else                     { s_prg_reg = data; mmc1_apply_prg(); }
                s_shift_reg   = 0x10;
                s_shift_count = 0;
            }
            return;

        case 4:
            /* MMC3: bank select/data + mirroring + IRQ */
            if (addr < 0x8000) return;

            if (addr <= 0x9FFF) {
                if (addr & 1) {
                    /* $8001: Bank data — write to selected register */
                    int reg = s_mmc3_bank_select & 0x07;
                    s_mmc3_regs[reg] = val;
                    if (reg <= 5)
                        mmc3_apply_chr();
                    else
                        mmc3_apply_prg();
                } else {
                    /* $8000: Bank select — which register + PRG/CHR mode */
                    int old_prg_mode = (s_mmc3_bank_select >> 6) & 1;
                    int old_chr_mode = (s_mmc3_bank_select >> 7) & 1;
                    s_mmc3_bank_select = val;
                    if (((val >> 6) & 1) != old_prg_mode)
                        mmc3_apply_prg();
                    if (((val >> 7) & 1) != old_chr_mode)
                        mmc3_apply_chr();
                }
            } else if (addr <= 0xBFFF) {
                if (addr & 1) {
                    /* $A001: PRG RAM protect (ignored for recomp) */
                } else {
                    /* $A000: Mirroring */
                    s_mirroring = (val & 1) ? 3 : 2; /* 0=vertical, 1=horizontal */
                }
            } else if (addr <= 0xDFFF) {
                if (addr & 1) {
                    /* $C001: IRQ reload — set flag to reload counter */
                    s_mmc3_irq_reload = 1;
                    s_mmc3_irq_counter = 0;
                } else {
                    /* $C000: IRQ latch — value to reload counter with */
                    s_mmc3_irq_latch = val;
                }
            } else {
                if (addr & 1) {
                    /* $E001: IRQ enable */
                    s_mmc3_irq_enabled = 1;
                } else {
                    /* $E000: IRQ disable + acknowledge */
                    s_mmc3_irq_enabled = 0;
                }
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
        case 4:
            /* MMC3: pre-built 16KB buffer from 2x 8KB banks */
            return s_mmc3_prg_low;
        default:
            return s_prg_data + (size_t)g_current_bank * 0x4000;
    }
}

const uint8_t *mapper_get_fixed_bank(void) {
    if (!s_prg_data) return NULL;
    switch (s_mapper_type) {
        case 4:
            /* MMC3: pre-built 16KB buffer from 2x 8KB banks */
            return s_mmc3_prg_high;
        default:
            /* Mapper 0 and Mapper 1: fixed bank = last 16KB of PRG */
            return s_prg_data + (size_t)(s_prg_banks - 1) * 0x4000;
    }
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
    /* MMC3 */
    out->mmc3_bank_select = s_mmc3_bank_select;
    memcpy(out->mmc3_regs, s_mmc3_regs, sizeof(s_mmc3_regs));
    out->mmc3_irq_latch   = s_mmc3_irq_latch;
    out->mmc3_irq_counter = s_mmc3_irq_counter;
    out->mmc3_irq_reload  = s_mmc3_irq_reload;
    out->mmc3_irq_enabled = s_mmc3_irq_enabled;
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
    /* MMC3 */
    s_mmc3_bank_select = in->mmc3_bank_select;
    memcpy(s_mmc3_regs, in->mmc3_regs, sizeof(s_mmc3_regs));
    s_mmc3_irq_latch   = in->mmc3_irq_latch;
    s_mmc3_irq_counter = in->mmc3_irq_counter;
    s_mmc3_irq_reload  = in->mmc3_irq_reload;
    s_mmc3_irq_enabled = in->mmc3_irq_enabled;
    /* Re-apply bank mappings */
    if (s_mapper_type == 4) {
        mmc3_apply_prg();
        mmc3_apply_chr();
    }
}
