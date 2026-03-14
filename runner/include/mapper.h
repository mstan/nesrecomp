#pragma once
#include <stdint.h>

void mapper_init(const uint8_t *prg_data, int prg_banks);
void mapper_write(uint16_t addr, uint8_t val);
const uint8_t *mapper_get_switchable_bank(void);
const uint8_t *mapper_get_fixed_bank(void);

typedef struct {
    uint8_t shift_reg;
    int     shift_count;
    uint8_t ctrl;
    uint8_t chr0;
    uint8_t chr1;
    uint8_t prg_reg;
    int     current_bank;
} MapperState;

void mapper_get_state(MapperState *out);
void mapper_set_state(const MapperState *in);
