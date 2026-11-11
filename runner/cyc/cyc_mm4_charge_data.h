/* Immutable MM4 charge art/constants; title adapters own actors and behavior. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint8_t count, tile[8], attr[8]; int8_t x[2][8], y[2][8]; } CycMm4ChargeSprite;
typedef struct { uint8_t last, duration, sprite[8]; } CycMm4ChargeAnimation;
typedef struct {
    CycMm4ChargeSprite sprites[7];
    CycMm4ChargeAnimation animations[3]; /* forming, flying, half */
    uint8_t speed[6], tiles[6][16], flash[3][16];
} CycMm4ChargeData;
extern const uint8_t cyc_mm4_charge_tile_ids[6];
/* Requires a verified resident cyc_mm4_donor. Publishes only a complete result. */
bool cyc_mm4_charge_data_load(CycMm4ChargeData *out);
