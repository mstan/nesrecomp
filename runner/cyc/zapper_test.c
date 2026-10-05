/* Exercise the PPU's actual pixel path, rather than a completed host picture. */
#include "hw_internal.h"
#include "cyc_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void boot(void)
{
    static uint8_t image[16 + 16384];
    memcpy(image, "NES\032", 4);
    image[4] = 1;
    CHECK(cyc_load_ines(image, sizeof(image)));
    cyc_power_on(0);
    CHECK(cyc_zapper_attach(2));
}

static bool light(void)
{
    CycZapperState state;
    cyc_zapper_state(&state);
    return state.light;
}

static size_t chunk(uint8_t *data, size_t len, const char *tag)
{
    for (size_t at = 32; at + 8 <= len;) {
        uint32_t size;
        memcpy(&size, data + at + 4, 4);
        if (!memcmp(data + at, tag, 4)) return at;
        at += 8 + size;
    }
    return 0;
}

int main(void)
{
    boot();
    CHECK(!cyc_zapper_attach(3));
    cyc_set_zapper(100, 100, false);
    hw.cpu_addr = 0x4017;
    CHECK((hw_read(0x4017) & 0x1F) == 8);          /* dark, released */
    cyc_set_zapper(100, 100, true);
    CHECK((hw_read(0x4017) & 0x1F) == 0x18);      /* trigger active high */
    cyc_set_controller(0, 0x80);
    hw_write(0x4016, 1);
    apu_cycle();
    hw.cpu_addr = 0x4016;
    CHECK((hw_read(0x4016) & 1) == 1);            /* port 1 remains a pad */

    /* With rendering disabled the beam outputs the universal background
     * color. A white full-screen flash must charge the gun, but only once
     * the beam reaches the aperture, not from future rows of a framebuffer. */
    ppu.scanline = 90; ppu.dot = 0; ppu.palette[0] = 0x30;
    for (unsigned i = 0; i < 341 * 6; ++i) ppu_dot();
    CHECK(!light());
    while (ppu.scanline == 96 && ppu.dot < 99) ppu_dot();
    CHECK(!light());
    ppu_dot();
    CHECK(light());
    hw.cpu_addr = 0x4017;
    CHECK((hw_read(0x4017) & 0x1F) == 0x10);      /* light active low */
    /* A black flash cannot refresh the sensor. Decay is measured in dots. */
    ppu.palette[0] = 0x0F;
    while (hw_zapper.dots + 1 < hw_zapper.light_until) ppu_dot();
    CHECK(light());
    ppu_dot();
    CHECK(!light());
    for (unsigned i = 0; i < 341 * 30; ++i) ppu_dot();
    CHECK(!light());

    /* White pixels away from the aim, or an offscreen aim, cannot hit. */
    ppu.palette[0] = 0x30;
    cyc_set_zapper(250, 230, false);
    ppu.scanline = 100; ppu.dot = 0;
    for (unsigned i = 0; i < 341; ++i) ppu_dot();
    CHECK(!light());
    cyc_set_zapper(-1, 100, true);
    for (unsigned i = 0; i < 341 * 240; ++i) ppu_dot();
    CHECK(!light() && hw_zapper.x == -1 && hw_zapper.y == -1);

    /* A state taken with a charged sensor retains its remaining lifetime. */
    cyc_set_zapper(100, 100, true);
    ppu.scanline = 100; ppu.dot = 104;
    ppu_dot();
    CHECK(light());
    HwZapper charged = hw_zapper;
    char error[160];
    uint8_t *data = NULL; size_t len = 0;
    CHECK(cyc_state_save(&data, &len, error, sizeof(error)));
    CHECK(data && data[8] == 3);
    CycSnapshot *snapshot = cyc_snapshot_new();
    CHECK(snapshot != NULL);
    cyc_snapshot_take(snapshot);
    cyc_zapper_attach(0);
    cyc_snapshot_restore(snapshot);
    CHECK(!memcmp(&charged, &hw_zapper, sizeof(charged)));
    cyc_snapshot_free(snapshot);
    cyc_zapper_attach(1);
    CHECK(cyc_state_load(data, len, error, sizeof(error)));
    CHECK(!memcmp(&charged, &hw_zapper, sizeof(charged)));

    size_t at = chunk(data, len, "ZAPP");
    CHECK(at != 0);
    HwZapper bad = charged;
    bad.port = 3;
    memcpy(data + at + 8, &bad, sizeof(bad));
    CHECK(!cyc_state_load(data, len, error, sizeof(error)));
    CHECK(!memcmp(&charged, &hw_zapper, sizeof(charged)));
    bad = charged; bad.light_until = bad.dots + 20 * 341 + 1;
    memcpy(data + at + 8, &bad, sizeof(bad));
    CHECK(!cyc_state_load(data, len, error, sizeof(error)));
    CHECK(!memcmp(&charged, &hw_zapper, sizeof(charged)));

    /* Same-layout v2 states are still valid and explicitly have no gun. */
    uint32_t version = 2;
    memcpy(data + 8, &version, 4);
    memmove(data + at, data + at + 8 + sizeof(charged), len - at - 8 - sizeof(charged));
    len -= 8 + sizeof(charged);
    CHECK(cyc_state_load(data, len, error, sizeof(error)));
    CHECK(hw_zapper.port == 0 && hw_zapper.x == -1 && hw_zapper.y == -1);
    free(data);
    printf("cyc_zapper_test: %u failures\n", failures);
    return failures != 0;
}
