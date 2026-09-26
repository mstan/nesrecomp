/* ROM-free cartridge contract tests. Expected mappings come from the board
 * documentation linked in MAPPERS.md, not from the oracle's bank tables. */
#include "hw_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HwMachine hw;
HwCart hw_cart;
HwPpu ppu;

static uint8_t prg[0x80000], chr[0x40000];
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static void cart(int mapper, unsigned prg_kb, unsigned chr_kb)
{
    memset(&hw_cart, 0, sizeof(hw_cart));
    memset(prg, 0xff, sizeof(prg)); /* writes to ROM avoid bus conflicts by default */
    memset(chr, 0, sizeof(chr));
    hw_cart.mapper = (uint8_t)mapper;
    hw_cart.prg = prg;
    hw_cart.chr = chr;
    hw_cart.prg_slots = prg_kb / 8;
    hw_cart.chr_pages = chr_kb;
    hw_cart.mirroring = HW_MIRROR_HORIZONTAL;
    CHECK(hw_cart_supports(mapper));
    hw_cart_power_on();
}

static void prg_banks(unsigned a, unsigned b, unsigned c, unsigned d)
{
    CHECK(hw_cart.prg_off[0] == a * 8192);
    CHECK(hw_cart.prg_off[1] == b * 8192);
    CHECK(hw_cart.prg_off[2] == c * 8192);
    CHECK(hw_cart.prg_off[3] == d * 8192);
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
    prg[hw_cart.prg_off[1] + 0x1000] = 0;
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
    prg[hw_cart.prg_off[1] + 0x1000] = 0x11;
    hw_cart_cpu_write(0xb000, 0xff);
    prg_banks(4, 5, 6, 7);
    chr_bank(0, 8, 8);
    no_wram();
}


int main(void)
{
    cart(0, 32, 8);
    prg_banks(0, 1, 2, 3);
    chr_bank(0, 0, 8);
    no_wram();
    /* Run added board contracts. */
    test_mapper75();
    test_mapper71();
    test_mapper34();
    test_mapper13();
    test_mapper11();
    printf("mapper contracts: %u checks passed\n", checks);
    return 0;
}
