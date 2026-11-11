/* Final MM2 charge loader, independent of title RAM and CHR allocation. */
#include "cyc_mm4_charge_data.h"
#include "cyc_mm4_donor.h"
#include <string.h>
const uint8_t cyc_mm4_charge_tile_ids[6] = {0x65, 0x64, 0x63, 0x66, 0x39, 0x49};
static uint8_t byte(unsigned a) { return cyc_mm4_byte(a < 0xA000 ? 0x0C : 0x0D, a); }
static unsigned pointer(unsigned lo, unsigned hi, unsigned i) { return cyc_mm4_byte(0x0D, lo + i) | (unsigned)cyc_mm4_byte(0x0D, hi + i) << 8; }
bool cyc_mm4_charge_data_load(CycMm4ChargeData *out)
{
    if (!out || !cyc_mm4_donor.prg) return false;
    CycMm4ChargeData next; memset(&next, 0, sizeof next);
    for (int k = 0; k < 3; k++) for (int i = 0; i < 16; i++) next.flash[k][i] = cyc_mm4_donor.prg[0x7926D + k * 16 + i];
    for (int i = 0; i < 7; i++) {
        CycMm4ChargeSprite *s = &next.sprites[i];
        unsigned p = pointer(0xB800, 0xBA00, 0x31 + i);
        unsigned count = byte(p) + 1u;
        if (count > 8) return false;
        s->count = (uint8_t)count;
        for (unsigned j = 0; j < count; j++) { s->tile[j] = byte(p + 2 + j * 2); s->attr[j] = byte(p + 3 + j * 2); }
        for (int side = 0; side < 2; side++) {
            unsigned o = pointer(0xBC00, 0xBD00, byte(p + 1) + side);
            for (unsigned j = 0; j < count; j++) { s->y[side][j] = (int8_t)byte(o + j * 2); s->x[side][j] = (int8_t)byte(o + j * 2 + 1); }
        }
    }
    const uint8_t ids[] = {0x20, 0x23, 0x21};
    for (int a = 0; a < 3; a++) {
        unsigned p = pointer(0xBE00, 0xBF00, ids[a]);
        CycMm4ChargeAnimation *r = &next.animations[a];
        r->last = byte(p) & 0x7F; r->duration = byte(p + 1);
        if (r->last >= 8) return false;
        for (int i = 0; i <= r->last; i++) { r->sprite[i] = byte(p + 2 + i); if (r->sprite[i] < 0x31 || r->sprite[i] > 0x37) return false; }
    }
    for (int i = 0; i < 6; i++) {
        next.speed[i] = cyc_mm4_byte(0x3A, 0x88C3 + i);
        for (int j = 0; j < 16; j++) next.tiles[i][j] = cyc_mm4_byte(0, 0x8000 + cyc_mm4_charge_tile_ids[i] * 16 + j);
    }
    /* Native Buster/face-shot sprite palettes, initialization 3E:C853. */
    for (int i = 0; i < 8; i++) next.palette[i / 4][i % 4] = cyc_mm4_byte(0x3E, 0xC853 + i);
    if (!(next.flash[0][0] == 0x0F && next.flash[1][0] == 0x2C && next.flash[2][0] == 0x11 &&
        next.animations[0].last == 5 && next.animations[0].duration == 1 && next.animations[0].sprite[0] == 0x31 &&
        next.animations[1].last == 2 && next.animations[2].sprite[0] == 0x32 && next.speed[3] == 12 && next.speed[5] == 5 &&
        next.sprites[1].count == 4 && next.sprites[4].count == 6 && next.sprites[5].count == 7 && next.sprites[6].count == 7)) return false;
    *out = next; return true;
}
