/*
 * mapper.h — Mapper 1 (MMC1) interface
 */
#pragma once
#include <stdint.h>

void mapper_init(const uint8_t *prg_data, int prg_banks);
void mapper_write(uint16_t addr, uint8_t val);

/* Get pointer to start of switchable bank (current bank) */
const uint8_t *mapper_get_switchable_bank(void);
/* Get pointer to start of fixed bank (always last bank) */
const uint8_t *mapper_get_fixed_bank(void);

/* Current switchable bank index */
extern int g_current_bank;

/* MMC1 mirroring mode from control register bits 1:0
 *   0: one-screen lower  1: one-screen upper
 *   2: vertical          3: horizontal */
int mapper_get_mirroring(void);
