#pragma once
#include <stdint.h>

/*
 * mapper_init — initialize the mapper for a specific ROM.
 *   mapper_type:        iNES mapper number (0=NROM, 1=MMC1, …)
 *   initial_mirroring:  iNES header mirroring (0=horizontal, 1=vertical)
 */
void mapper_init(const uint8_t *prg_data, int prg_banks,
                 int mapper_type, int initial_mirroring);

void mapper_init_chr(const uint8_t *chr_data, int chr_banks);
void mapper_write(uint16_t addr, uint8_t val);
const uint8_t *mapper_get_switchable_bank(void);
const uint8_t *mapper_get_fixed_bank(void);

/*
 * Returns the current nametable mirroring mode:
 *   0 = one-screen lower bank
 *   1 = one-screen upper bank
 *   2 = vertical mirroring
 *   3 = horizontal mirroring
 */
int mapper_get_mirroring(void);
int mapper_get_type(void);

/* Force-set the switchable PRG bank (used by VBlank save/restore to undo
 * bank switches made by the NMI handler). */
void mapper_set_bank(int bank);

typedef struct {
    int     mapper_type;
    uint8_t shift_reg;
    int     shift_count;
    uint8_t ctrl;
    uint8_t chr0;
    uint8_t chr1;
    uint8_t prg_reg;
    int     current_bank;
    int     mirroring;
    /* MMC3 (Mapper 4) state */
    uint8_t mmc3_bank_select;
    uint8_t mmc3_regs[8];
    uint8_t mmc3_irq_latch;
    uint8_t mmc3_irq_counter;
    int     mmc3_irq_reload;
    int     mmc3_irq_enabled;
} MapperState;

void mapper_get_state(MapperState *out);
void mapper_set_state(const MapperState *in);

/*
 * mapper_clock_scanline — clock the MMC3 scanline counter.
 * Call once per visible scanline (0-239) during rendering.
 * Returns 1 if an IRQ should fire (counter hit zero and IRQs enabled).
 */
int mapper_clock_scanline(void);

/* Debug: get $A000 mirroring write stats */
void mapper_get_a000_debug(int *count, uint8_t *last_val, uint64_t *last_frame);

/* Debug: $A000 trace ring buffer (64 entries) */
typedef struct {
    uint64_t    frame;
    uint8_t     val;
    const char *caller;
} A000TraceEntry;
void mapper_get_a000_trace(int *out_count, int *out_idx);
const void *mapper_get_a000_trace_buf(void);

/* Debug: CHR bank change trace ring buffer (256 entries) */
#define CHR_TRACE_SIZE 256
typedef struct {
    uint64_t    frame;
    uint8_t     regs[6];       /* R0-R5 CHR bank values */
    uint8_t     bank_select;   /* full $8000 value (bit 7 = chr_a12_inv) */
    const char *caller;
} ChrTraceEntry;
void mapper_get_chr_trace(int *out_count, int *out_idx);
const void *mapper_get_chr_trace_buf(void);
void mapper_get_mmc3_chr_regs(uint8_t *regs6_out, uint8_t *bank_select_out);
