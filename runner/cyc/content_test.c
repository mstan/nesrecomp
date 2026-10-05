/* Content tools must honor mapped memory, isolation and generated-code validity. */
#include "hw_internal.h"
#include "hw.h"
#include "cyc_mod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned failures, calls;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static void observe(unsigned reg, uint16_t addr, uint8_t value, unsigned increment) {
    CHECK(addr == 0x0412 && increment == 32);
    CHECK((reg == 6 && value == 0x12) || (reg == 7 && value == 0xA7));
    ++calls;
}
static void write_reg(uint16_t addr, uint8_t value) {
    hw_cycle_start(addr, HW_WRITE); hw_write(addr, value); hw_cycle_finish(false);
}
static void boot(bool chr_rom) {
    static uint8_t image[16 + 16384 + 8192];
    memset(image, 0, sizeof(image)); memcpy(image, "NES\032", 4);
    image[4] = 1; image[5] = chr_rom;
    CHECK(cyc_load_ines(image, 16 + 16384 + (chr_rom ? 8192 : 0)));
    cyc_power_on(0);
}
int main(void) {
    boot(false);
    CHECK(!hw_prg_modified);
    size_t n = 99;
    CHECK(cyc_mod_isolate_begin());
    CHECK(!cyc_mod_prg_data_rw(&n) && n == 0 && !hw_prg_modified);
    CHECK(!cyc_mod_chr_poke(0x0412, 0xA5));
    cyc_mod_isolate_end();
    uint8_t *prg = cyc_mod_prg_data_rw(&n);
    CHECK(prg && n == 16384 && hw_prg_modified);
    if (prg) prg[0] = 0xEA;
    uint8_t value = 0;
    CHECK(cyc_mod_peek_ok(0x8000, &value) && value == 0xEA);
    CHECK(cyc_mod_chr_poke(0x0412, 0xA5));
    CHECK(hw_cart.chr[hw_cart_chr_index(0x0412)] == 0xA5);
    CHECK(!cyc_mod_chr_poke(0x2000, 1));
    cyc_mod_set_ppu_write_hook(observe);
    write_reg(0x2000, 4);
    write_reg(0x2006, 4); write_reg(0x2006, 0x12);
    CHECK(calls == 1);
    for (int i = 0; i < 2; ++i) {
        hw_cycle_start(0, HW_READ); hw_read(0); hw_cycle_finish(false);
    }
    write_reg(0x2007, 0xA7);
    CHECK(calls == 2);
    CHECK(cyc_mod_isolate_begin());
    write_reg(0x2006, 4); write_reg(0x2006, 0x12);
    cyc_mod_isolate_end();
    CHECK(calls == 2);
    cyc_mod_set_ppu_write_hook(NULL);
    boot(true);
    CHECK(!hw_prg_modified && !cyc_mod_chr_poke(0x0412, 0xA5));
    /* Geometry includes ROM bytes, never a mixed board's CHR RAM chip. */
    size_t image_size=16+131072+65536;
    uint8_t *image=(uint8_t *)calloc(1,image_size);
    CHECK(image!=NULL);
    if(image) {
        memcpy(image,"NES\032",4);image[4]=8;image[5]=8;image[6]=0x70;image[7]=0x70; /* TQROM */
        CHECK(cyc_load_ines(image,image_size));cyc_power_on(0);
        CHECK(hw_cart.info.chr_size==65536 && hw_cart.chr_ram_len==8192);
        memset(hw_cart.chr+hw_cart.chr_ram_base,0x5a,hw_cart.chr_ram_len);
        image[16+131072]=0xa7;
        CHECK(!cyc_mod_apply_cart_payload(image+16,image_size-17));
        CHECK(!hw_prg_modified && hw_cart.chr[0]==0);
        CHECK(cyc_mod_apply_cart_payload(image+16,image_size-16));
        CHECK(!hw_prg_modified && hw_cart.chr[0]==0xa7 && hw_cart.chr[hw_cart.chr_ram_base]==0x5a);
        image[16]=0xea;
        CHECK(cyc_mod_isolate_begin());
        CHECK(!cyc_mod_apply_cart_payload(image+16,image_size-16));cyc_mod_isolate_end();
        CHECK(!hw_prg_modified && hw_cart.prg[0]==0);
        CHECK(cyc_mod_apply_cart_payload(image+16,image_size-16));
        CHECK(hw_prg_modified && hw_cart.prg[0]==0xea && hw_cart.chr[hw_cart.chr_ram_base]==0x5a);
        free(image);
    }
    printf("content_test: %u failures\n", failures);
    return failures ? 1 : 0;
}
