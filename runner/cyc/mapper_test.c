/* ROM-free cartridge contract tests. Expected mappings come from the board
 * documentation linked in MAPPERS.md, not from the oracle's bank tables. */
#include "hw_internal.h"
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HwMachine hw;
HwCart hw_cart;
HwPpu ppu;

static uint8_t prg[0x80000], chr[0x80000];
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static uint8_t pattern_read(uint16_t addr)
{
    uint8_t value = hw_cart_chr_read(addr);
    CHECK(hw_cart_chr_read(addr) == value); /* held /RD keeps the old byte */
    hw_cart_ppu_rd(false);
    return value;
}

static void cart(int mapper, unsigned prg_kb, unsigned chr_kb)
{
    memset(&hw_cart, 0, sizeof(hw_cart));
    memset(prg, 0xff, sizeof(prg)); /* writes to ROM avoid bus conflicts by default */
    memset(chr, 0, sizeof(chr));
    hw_cart.mapper = (uint8_t)mapper;
    hw_cart.prg = prg;
    hw_cart.chr = chr;
    hw_cart.prg_slots = prg_kb / 4;
    hw_cart.chr_pages = chr_kb;
    hw_cart.mirroring = HW_MIRROR_HORIZONTAL;
    CHECK(hw_cart_supports(mapper));
    hw_cart_power_on();
}

static void prg_banks(unsigned a, unsigned b, unsigned c, unsigned d)
{
    unsigned expected[4] = {a,b,c,d};
    for (unsigned i=0; i<4; ++i) {
        CHECK(hw_cart.prg_off[2*i] == expected[i]*8192);
        CHECK(hw_cart.prg_off[2*i+1] == expected[i]*8192 + 4096);
    }
}

static void chr_bank(unsigned page, unsigned bank, unsigned pages)
{
    for (unsigned i = 0; i < pages; ++i)
        CHECK(hw_cart.chr_off[page + i] == (bank + i) * 1024);
}

static void no_wram(void)
{
    uint8_t value = 0xa5;
    CHECK(!hw_cart_cpu_read(0x6000, &value));
    CHECK(value == 0xa5);
    CHECK(!hw_cart_irq());
}

/* Per-board tests. */
static void test_mapper232(void)
{
    cart(232, 256, 8);
    prg_banks(0, 1, 6, 7);
    hw_cart_cpu_write(0xbfff, 0x10);
    prg_banks(16, 17, 22, 23);
    hw_cart_cpu_write(0xffff, 1);
    prg_banks(18, 19, 22, 23);
    hw_cart_cpu_write(0x8000, 0x08);
    prg_banks(10, 11, 14, 15);
    hw_cart_cpu_write(0xc000, 0xff);
    prg_banks(14, 15, 14, 15);
    no_wram();
}

static void test_mapper184(void)
{
    cart(184, 32, 32);
    chr_bank(4, 16, 4);
    hw_cart_cpu_write(0x6000, 0x12);
    chr_bank(0, 8, 4); chr_bank(4, 20, 4);
    hw_cart_cpu_write(0x8000, 0xff); hw_cart_cpu_write(0x5fff, 0xff);
    chr_bank(0, 8, 4); chr_bank(4, 20, 4);
    hw_cart_cpu_write(0x7fff, 0);
    chr_bank(0, 0, 4); chr_bank(4, 16, 4);
    no_wram();
}

static void test_mapper180(void)
{
    cart(180, 128, 8);
    hw_cart_cpu_write(0xb000, 3);
    prg_banks(0, 1, 6, 7);
    chr_bank(0, 0, 8);
    prg[hw_cart.prg_off[3]] = 0;
    hw_cart_cpu_write(0xb000, 0xff);
    prg_banks(0, 1, 0, 1);
    no_wram();
}

static void test_mapper140(void)
{
    cart(140, 128, 128);
    hw_cart_cpu_write(0x6000, 0x2a);
    prg_banks(8, 9, 10, 11); chr_bank(0, 80, 8);
    hw_cart_cpu_write(0x5fff, 0); hw_cart_cpu_write(0x8000, 0);
    prg_banks(8, 9, 10, 11);
    hw_cart_cpu_write(0x7fff, 0xff);
    prg_banks(12, 13, 14, 15); chr_bank(0, 120, 8);
    no_wram();
}

static void test_mapper113(void)
{
    cart(113, 256, 128);
    hw_cart_cpu_write(0x4000, 0xff); hw_cart_cpu_write(0x6000, 0xff);
    prg_banks(0, 1, 2, 3);
    hw_cart_cpu_write(0x4100, 235);
    prg_banks(20, 21, 22, 23);
    chr_bank(0, 88, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0x5fff, 0);
    prg_banks(0, 1, 2, 3);
    chr_bank(0, 0, 8);
    no_wram();
}

static void test_mapper94(void)
{
    cart(94, 128, 8);
    hw_cart_cpu_write(0xb000, 12);
    prg_banks(6, 7, 14, 15);
    chr_bank(0, 0, 8);
    prg[hw_cart.prg_off[3]] = 0;
    hw_cart_cpu_write(0xb000, 0xff);
    prg_banks(0, 1, 14, 15);
    no_wram();
}

static void test_mapper87(void)
{
    cart(87, 32, 32);
    hw_cart_cpu_write(0x6000, 1); chr_bank(0, 16, 8);
    hw_cart_cpu_write(0x7fff, 2); chr_bank(0, 8, 8);
    hw_cart_cpu_write(0x5fff, 3); hw_cart_cpu_write(0x8000, 3);
    chr_bank(0, 8, 8);
    prg_banks(0, 1, 2, 3);
    no_wram();
}

static void test_mapper79(void)
{
    cart(79, 64, 64);
    hw_cart_cpu_write(0x4000, 0xff); hw_cart_cpu_write(0x6000, 0xff);
    prg_banks(0, 1, 2, 3);
    hw_cart_cpu_write(0x4100, 11);
    prg_banks(4, 5, 6, 7);
    chr_bank(0, 24, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    hw_cart_cpu_write(0x5fff, 0);
    prg_banks(0, 1, 2, 3);
    chr_bank(0, 0, 8);
    no_wram();
}

static void test_mapper76(void)
{
    cart(76, 128, 128);
    hw_cart_cpu_write(0x8000, 6); hw_cart_cpu_write(0x8001, 3);
    hw_cart_cpu_write(0x8000, 7); hw_cart_cpu_write(0x8001, 4);
    prg_banks(3, 4, 14, 15);
    for (unsigned i = 0; i < 4; ++i) {
        hw_cart_cpu_write(0x9ffe, (uint8_t)(i + 2));
        hw_cart_cpu_write(0x9fff, (uint8_t)(i + 5));
        chr_bank(i * 2, (i + 5) * 2, 2);
    }
    hw_cart_cpu_write(0x8000, 0); hw_cart_cpu_write(0x8001, 0xff);
    chr_bank(0, 10, 2);
    hw_cart_cpu_write(0xa000, 1);
    CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    no_wram();
}

static void test_mapper206(void)
{
    cart(206, 128, 64);
    hw_cart_cpu_write(0x9ffe, 0xc6); hw_cart_cpu_write(0x9fff, 0xf3);
    prg_banks(3, 1, 14, 15);
    hw_cart_cpu_write(0x8000, 0); hw_cart_cpu_write(0x8001, 0xff);
    chr_bank(0, 62, 2);
    hw_cart_cpu_write(0x8000, 2); hw_cart_cpu_write(0x8001, 0xc5);
    chr_bank(4, 5, 1);
    hw_cart_cpu_write(0xa000, 1); hw_cart_cpu_write(0xa001, 0xff);
    hw_cart_cpu_write(0xc001, 0xff); hw_cart_cpu_write(0xe001, 0xff);
    CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    prg_banks(3, 1, 14, 15);
    chr_bank(4, 5, 1);
    no_wram();
}

static void test_mapper75(void)
{
    cart(75, 128, 128);
    hw_cart_cpu_write(0x8123, 3); hw_cart_cpu_write(0xafff, 4); hw_cart_cpu_write(0xcaaa, 5);
    prg_banks(3, 4, 5, 15);
    hw_cart_cpu_write(0xe000, 2); hw_cart_cpu_write(0xffff, 3);
    hw_cart_cpu_write(0x9000, 6);
    chr_bank(0, 72, 4); chr_bank(4, 76, 4);
    CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0x9fff, 1);
    chr_bank(0, 8, 4); chr_bank(4, 12, 4);
    CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    hw_cart_cpu_write(0xbfff, 0xff);
    prg_banks(3, 4, 5, 15);
    no_wram();
}

static void test_mapper71(void)
{
    cart(71, 256, 8);
    prg_banks(0, 1, 30, 31);
    hw_cart_cpu_write(0x8000, 0);
    CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    hw_cart_cpu_write(0x9fff, 0x10);
    CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_B);
    hw_cart_cpu_write(0x9000, 0);
    CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_A);
    hw_cart_cpu_write(0xbfff, 3);
    prg_banks(0, 1, 30, 31);
    prg[0x7c000] = 0; /* no bus conflict */
    hw_cart_cpu_write(0xffff, 0x13);
    prg_banks(6, 7, 30, 31);
    no_wram();
}

static void test_mapper34(void)
{
    cart(34, 64, 64); /* CHR ROM identifies NINA-001. */
    hw_cart_cpu_write(0x7ffc, 1);
    prg_banks(0, 1, 2, 3);
    hw_cart_cpu_write(0x7ffd, 0xff);
    hw_cart_cpu_write(0x7ffe, 2);
    hw_cart_cpu_write(0x7fff, 5);
    prg_banks(4, 5, 6, 7);
    chr_bank(0, 8, 4); chr_bank(4, 20, 4);
    uint8_t v = 0;
    CHECK(hw_cart_cpu_read(0x7ffd, &v) && v == 0xff);
    hw_cart_cpu_write(0x8000, 0);
    prg_banks(4, 5, 6, 7);
    cart(34, 128, 8);
    /* BNROM also permits an unbanked 8 KiB CHR ROM. */
    hw_cart_cpu_write(0x7ffd, 1);
    prg_banks(0, 1, 2, 3);
    hw_cart_cpu_write(0xb000, 6); /* wraps at physical ROM size */
    prg_banks(8, 9, 10, 11);
    prg[hw_cart.prg_off[3]] = 0;
    hw_cart_cpu_write(0xb000, 3);
    prg_banks(0, 1, 2, 3);
    no_wram();
}

static void test_mapper13(void)
{
    cart(13, 32, 16);
    hw_cart_cpu_write(0xb000, 3);
    prg_banks(0, 1, 2, 3);
    chr_bank(0, 0, 4);
    chr_bank(4, 12, 4);
    CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    chr[hw_cart_chr_index(0x1000)] = 0xa5;
    hw_cart_cpu_write(0xb000, 1);
    CHECK(chr[hw_cart_chr_index(0x1000)] == 0);
    hw_cart_cpu_write(0xb000, 3);
    CHECK(chr[hw_cart_chr_index(0x1000)] == 0xa5);
    prg[0x3000] = 0;
    hw_cart_cpu_write(0xb000, 3);
    chr_bank(4, 0, 4);
    no_wram();
}

static void test_mapper11(void)
{
    cart(11, 128, 128);
    hw_cart_cpu_write(0xb000, 0xa2);
    prg_banks(8, 9, 10, 11);
    chr_bank(0, 80, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    prg[hw_cart.prg_off[3]] = 0x11;
    hw_cart_cpu_write(0xb000, 0xff);
    prg_banks(4, 5, 6, 7);
    chr_bank(0, 8, 8);
    no_wram();
}


static void test_mapper31(void)
{
    cart(31, 128, 8);
    for (unsigned i=0; i<32; ++i) memset(prg + i*4096, (int)i, 4096);
    CHECK(hw_cart_prg_read(0xfffc) == 31);
    for (unsigned slot=0; slot<8; ++slot) {
        hw_cart_cpu_write((uint16_t)(0x5000 + slot), (uint8_t)(slot*3));
        CHECK(hw_cart_prg_read((uint16_t)(0x8000 + slot*4096)) == slot*3);
        CHECK(hw_cart_prg_read((uint16_t)(0x8fff + slot*4096)) == slot*3);
    }
    hw_cart_cpu_write(0x5fff, 255); CHECK(hw_cart_prg_read(0xffff) == 31);
    hw_cart_cpu_write(0x6000, 9); CHECK(hw_cart_prg_read(0x8000) == 0);
    hw_cart_cpu_write(0x4fff, 9); CHECK(hw_cart_prg_read(0xffff) == 31);
    no_wram();
}

static void test_mmc2_latches(void)
{
    for (int mapper = 9; mapper <= 10; ++mapper) {
        cart(mapper, 128, 128);
        hw_cart_cpu_write(0xafff, 3);
        if (mapper == 9) { prg_banks(3, 13, 14, 15); no_wram(); }
        else { prg_banks(6, 7, 14, 15); CHECK(hw_cart.has_wram); }
        for (unsigned i = 0; i < 128; ++i) memset(chr + i*1024, (int)i, 1024);
        hw_cart_cpu_write(0xb123, 1); hw_cart_cpu_write(0xc456, 2);
        hw_cart_cpu_write(0xd789, 3); hw_cart_cpu_write(0xeabc, 4);
        for (unsigned addr = 0; addr < 8192; ++addr) {
            pattern_read(0x0fe8); pattern_read(0x1fe8);
            unsigned half = addr >> 12;
            CHECK(pattern_read((uint16_t)addr) == (half ? 16 : 8) + ((addr & 4095) >> 10));
            bool trigger = addr == 0xfd8 || (mapper == 10 && addr >= 0xfd8 && addr <= 0xfdf) ||
                           (addr >= 0x1fd8 && addr <= 0x1fdf);
            CHECK(hw_cart.chr_off[half*4] == (trigger ? (half ? 12 : 4) : (half ? 16 : 8))*1024);
        }
        /* Peeks and CPU writes to unrelated addresses cannot set the latch. */
        pattern_read(0x0fe8);
        (void)hw_cart_chr_index(0xfd8);
        hw_cart_cpu_write(0x8fd8, 0);
        chr_bank(0, 8, 4);
        hw_cart_cpu_write(0xffff, 1); CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
        hw_cart_cpu_write(0xf000, 0); CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    }
}

static void test_variants(void)
{
    /* SEROM connects CPU A14 directly to PRG ROM. */
    cart(1, 32, 32);
    hw_cart.info.submapper = 5;
    hw_cart_power_on();
    for (unsigned i = 0; i < 5; ++i) {
        hw.cycles += 2;
        hw_cart_cpu_write(0xe000, 1);
    }
    prg_banks(0, 1, 2, 3);
    for (int mapper = 2; mapper <= 7; ++mapper) {
        if (mapper != 2 && mapper != 3 && mapper != 7) continue;
        for (unsigned sub = 1; sub <= 2; ++sub) {
            cart(mapper, 128, 32);
            hw_cart.info.submapper = sub;
            prg[0x1000] = 0;
            hw_cart_cpu_write(0x9000, 3);
            CHECK(hw_cart.m.latch == (sub == 1 ? 3 : 0));
        }
    }
}

#include "vrc_test.inc"
#include "vrc6_test.inc"
#include "vrc7_test.inc"
#include "bandai_test.inc"
#include "mmc5_test.inc"
#include "mmc1_board_test.inc"

int main(void)
{
    cart(0, 32, 8);
    prg_banks(0, 1, 2, 3);
    chr_bank(0, 0, 8);
    no_wram();
    /* Run added board contracts. */
    test_mapper232();
    test_mapper184();
    test_mapper180();
    test_mapper140();
    test_mapper113();
    test_mapper94();
    test_mapper87();
    test_mapper79();
    test_mapper76();
    test_mapper206();
    test_mapper75();
    test_mapper71();
    test_mapper34();
    test_mapper13();
    test_mapper11();
    test_variants();
    test_mmc2_latches();
    test_mapper31();
    test_vrc();
    test_vrc6();
    test_vrc7();
    test_bandai();
    test_mmc5();
    test_mmc1_boards();
    printf("mapper contracts: %u checks passed\n", checks);
    return 0;
}
