/* Synthetic payloads exercise exact legacy-board recognition without ROM data. */
#include "../../common/nes_cart.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t image[16 + 131072];
int main(void) {
    memcpy(image, "NES\x1a", 4);
    image[4] = 8;
    image[6] = 0x12; /* MMC1, battery, unspecified legacy RAM size. */
    /* CRC-forged all-zero synthetic PRG; this is only a database-key fixture. */
    const uint8_t suffix[] = { 0xf6,0x61,0x11,0x61 };
    memcpy(image + sizeof(image) - 4, suffix, 4);
    assert(nes_crc32(0, image + 16, sizeof(image) - 16) == 0xFD9C577Fu);
    NesCartInfo c;
    assert(nes_cart_image(image, sizeof(image), &c));
    assert(c.mapper == 1 && !c.nes2 && !c.prg_ram && c.prg_nvram == 8192);
    assert(c.chr_ram == 8192 && !c.vertical && c.battery);
    /* A changed payload cannot inherit the recognized derivative's board. */
    image[16] = 1;
    assert(nes_cart_image(image, sizeof(image), &c));
    assert(c.prg_nvram == 32768);
    image[16] = 0;
    /* Explicit legacy metadata remains authoritative. */
    image[8] = 2;
    assert(nes_cart_image(image, sizeof(image), &c));
    assert(c.prg_nvram == 16384);
    image[8] = 0;
    /* The same payload on a different declared mapper is not an alias. */
    image[6] = 2;
    assert(nes_cart_image(image, sizeof(image), &c));
    assert(c.mapper == 0);
    puts("cartridge header self-test passed");
    return 0;
}
