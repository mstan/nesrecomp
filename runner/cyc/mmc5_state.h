/* MMC5 state layout, not behavior. See MAPPERS.md for hardware references. */
#pragma once
#include <stdint.h>
typedef struct {
    uint8_t reg[4], length, step, envelope, divider, restart;
    uint16_t timer;
} Mmc5Pulse;
typedef struct {
    uint8_t prg[5];
    uint16_t chr[12];
    uint8_t prg_mode, chr_mode, protect[2], ex_mode, nt, fill_tile, fill_attr, upper, last_b;
    uint8_t ctrl, mask, split, scroll, split_bank, split_y, split_tile, split_active;
    uint8_t multiply[2], irq_line, irq_enable, irq_pending, in_frame, line;
    uint8_t rd, idle, repeats, fetch, ext_attr, background;
    uint16_t last_addr, split_addr;
    uint8_t gpio_mode, gpio_data, timer_active, timer_irq;
    uint16_t timer;
    uint64_t timer_write;
    Mmc5Pulse pulse[2];
    uint16_t audio_divider;
    uint8_t audio_enable, pcm_control, pcm_irq, pcm;
} Mmc5State;

/* Bank-table tags keep writable/open windows out of native ROM dispatch. */
#define MMC5_PRG_RAM UINT32_C(0x80000000)
#define MMC5_PRG_OPEN UINT32_C(0x40000000)
