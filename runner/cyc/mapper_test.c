/* ROM-free cartridge contract tests. Expected mappings come from the board
 * documentation linked in MAPPERS.md, not from the oracle's bank tables. */
#include "hw_internal.h"
#include "hw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HwMachine hw;
HwCart hw_cart;
HwPpu ppu;
/* hw_machine.c pieces the FDS HLE tier (hw_fds_hle.c) links against. */
int hw_dma_stalls;
bool cyc_debug_peek(uint16_t addr, uint8_t *value) { (void)addr; *value = 0; return false; }
/* ...and the boot observation (hw_fds_boot.c). */
bool hw_frame_done, hw_entry_stop, hw_entry_hit;
/* The code watch (hw_machine.c): no compiled RAM views here. */
uint8_t hw_code_watch[HW_CODE_BYTES];
static void no_code_write(unsigned phys, uint8_t value) { (void)phys; (void)value; }
void (*hw_code_write)(unsigned phys, uint8_t value) = no_code_write;

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
    hw_cart.mapper = (uint16_t)mapper;
    hw_cart.prg = prg;
    hw_cart.chr = chr;
    hw_cart.prg_slots = prg_kb / 4;
    hw_cart.chr_pages = chr_kb;
    hw_cart.mirroring = HW_MIRROR_HORIZONTAL;
    CHECK(hw_cart_supports(mapper));
    hw_cart_power_on();
}

/* Reports the calling line, since many board tests share this helper. */
#define prg_banks(a, b, c, d) prg_banks_at(__LINE__, a, b, c, d)
static void prg_banks_at(int line, unsigned a, unsigned b, unsigned c, unsigned d)
{
    unsigned expected[4] = {a,b,c,d};
    for (unsigned i=0; i<4; ++i) {
        if (hw_cart.prg_off[2*i] != expected[i]*8192 || hw_cart.prg_off[2*i+1] != expected[i]*8192 + 4096)
            fprintf(stderr, "prg_banks from line %d: slot %u is %u, expected %u\n", line, i,
                    hw_cart.prg_off[2*i] / 8192, expected[i]);
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
static void cpu_cycles(unsigned n);   /* sunsoft_test.inc */

static void mmc3_reg(unsigned index, uint8_t value, uint8_t mode)
{
    hw_cart_cpu_write(0x8000, (uint8_t)(mode | index));
    hw_cart_cpu_write(0x8001, value);
}

static unsigned txsrom_nt(uint16_t addr) { return hw_cart_ciram_a10(addr) ? 1 : 0; }

/* nesdev wiki, INES Mapper 080 / 207 / 082 and NES 2.0 552: Taito X1. */
static void test_taito_x1(void)
{
    uint8_t value;
    cart(80, 256, 256); hw_cart.has_wram = 1; hw_cart.wram_len = 128;
    prg_banks(0, 0, 0, 31);
    hw_cart_cpu_write(0x7efa, 3); hw_cart_cpu_write(0x7efd, 0x45); hw_cart_cpu_write(0x7eff, 0x3e);
    prg_banks(3, 5, 30, 31);
    hw_cart_cpu_write(0x7ef0, 0x13); chr_bank(0, 0x12, 2);            /* 2 KiB: bit 0 ignored */
    hw_cart_cpu_write(0x7ef1, 0x20); chr_bank(2, 0x20, 2);
    for (unsigned i = 0; i < 4; ++i) hw_cart_cpu_write((uint16_t)(0x7ef2 + i), (uint8_t)(0x41 + i));
    for (unsigned i = 0; i < 4; ++i) chr_bank(4 + i, 0x41 + i, 1);
    hw_cart_cpu_write(0x7ef6, 1); CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0x7ef7, 0); CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);   /* not mirrored */
    hw_cart_cpu_write(0x7ef6, 0); CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    value = 0xa5; CHECK(!hw_cart_cpu_read(0x7f00, &value) && value == 0xa5);      /* locked */
    hw_cart_cpu_write(0x7f00, 0x11);
    hw_cart_cpu_write(0x7ef9, 0xa3);
    CHECK(hw_cart_cpu_read(0x7f00, &value) && value == 0);
    hw_cart_cpu_write(0x7f05, 0x5a); CHECK(hw_cart_cpu_read(0x7f85, &value) && value == 0x5a);
    CHECK(!hw_cart_cpu_read(0x6000, &value) && !hw_cart_cpu_read(0x7eef, &value));
    hw_cart_cpu_write(0x7ef8, 0xa2); CHECK(!hw_cart_cpu_read(0x7f05, &value));
    /* 207: bit 7 of $7EF0/$7EF1 drives CIRAM A10 for the upper/lower nametables. */
    cart(207, 256, 256); hw_cart.has_wram = 1; hw_cart.wram_len = 128;
    hw_cart_cpu_write(0x7ef0, 0x80); hw_cart_cpu_write(0x7ef1, 0x00);
    CHECK(hw_cart_ciram_a10(0x2000) && hw_cart_ciram_a10(0x2400) && !hw_cart_ciram_a10(0x2800) && !hw_cart_ciram_a10(0x2c00));
    hw_cart_cpu_write(0x7ef6, 1);
    hw_cart_cpu_write(0x7ef0, 0x00); hw_cart_cpu_write(0x7ef1, 0x81);
    CHECK(!hw_cart_ciram_a10(0x2000) && hw_cart_ciram_a10(0x2800) && hw_cart_ciram_a10(0x3c00));
    chr_bank(2, 0x80, 2);
    /* 82: CHR halves swap with $7EF6 bit 1; three keyed RAM windows; PRG bits 2-5. */
    cart(82, 128, 256); hw_cart.has_wram = 1; hw_cart.wram_len = 5120;
    hw_cart_cpu_write(0x7efa, 0x0c); hw_cart_cpu_write(0x7efb, 0x3c); hw_cart_cpu_write(0x7efc, 0xff);
    prg_banks(3, 15, 15, 15);
    hw_cart_cpu_write(0x7ef0, 0x13); hw_cart_cpu_write(0x7ef1, 0x21); hw_cart_cpu_write(0x7ef2, 0x40);
    chr_bank(0, 0x12, 2); chr_bank(2, 0x20, 2); chr_bank(4, 0x40, 1);
    hw_cart_cpu_write(0x7ef6, 2); chr_bank(4, 0x12, 2); chr_bank(6, 0x20, 2); chr_bank(0, 0x40, 1);
    CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    hw_cart_cpu_write(0x7ef7, 0xca); hw_cart_cpu_write(0x7ef8, 0x69);
    hw_cart_cpu_write(0x6000, 1); hw_cart_cpu_write(0x6fff, 2); hw_cart_cpu_write(0x7000, 3);
    CHECK(hw_cart_cpu_read(0x6000, &value) && value == 1);
    CHECK(hw_cart_cpu_read(0x6fff, &value) && value == 2);
    CHECK(!hw_cart_cpu_read(0x7000, &value));
    hw_cart_cpu_write(0x7ef9, 0x84); hw_cart_cpu_write(0x7000, 3);
    CHECK(hw_cart_cpu_read(0x73ff, &value) && hw_cart_cpu_read(0x7000, &value) && value == 3);
    CHECK(!hw_cart_cpu_read(0x7400, &value));
    hw_cart_cpu_write(0x7ef7, 0x00); CHECK(!hw_cart_cpu_read(0x6000, &value) && hw_cart_cpu_read(0x6800, &value));
    /* 552: bits 0-5 are A18..A13. */
    cart(552, 512, 256); hw_cart.has_wram = 1; hw_cart.wram_len = 5120;
    hw_cart_cpu_write(0x7efa, 0x01); hw_cart_cpu_write(0x7efb, 0x20); hw_cart_cpu_write(0x7efc, 0x06);
    prg_banks(32, 1, 24, 63);
}

/* nesdev wiki, INES Mapper 032: Irem G-101. */
static void test_mapper32(void)
{
    cart(32, 256, 256);
    prg_banks(0, 0, 30, 31);
    hw_cart_cpu_write(0x8fff, 0xe5); hw_cart_cpu_write(0xa123, 0x27);   /* 5 bits */
    prg_banks(5, 7, 30, 31);
    hw_cart_cpu_write(0x9000, 2); prg_banks(30, 7, 5, 31);              /* swap */
    CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0x9000, 1); prg_banks(5, 7, 30, 31); CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    for (unsigned i = 0; i < 8; ++i) hw_cart_cpu_write((uint16_t)(0xb000 + i), (uint8_t)(0xf0 + i));
    for (unsigned i = 0; i < 8; ++i) chr_bank(i, 0xf0 + i, 1);
    hw_cart_cpu_write(0xbff9, 0x11); chr_bank(1, 0x11, 1);              /* A2-A0 decode */
    no_wram();
    /* Submapper 1 (Major League): one-screen high, PRG mode fixed. */
    cart(32, 256, 256); hw_cart.info.submapper = 1; hw_cart_power_on();
    CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_B);
    hw_cart_cpu_write(0x8000, 3); hw_cart_cpu_write(0x9000, 3);
    prg_banks(3, 0, 30, 31); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_B);
}

/* nesdev wiki, INES Mapper 033 / 048: Taito TC0190 / TC0690. */
static void test_taito(void)
{
    cart(33, 256, 256);
    prg_banks(0, 0, 30, 31);
    hw_cart_cpu_write(0x8000, 0x45); CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    hw_cart_cpu_write(0xc001, 0xc7);                    /* A14 ignored: $8001 */
    prg_banks(5, 7, 30, 31);
    hw_cart_cpu_write(0x8002, 0x13); chr_bank(0, 0x26, 2);
    hw_cart_cpu_write(0x8003, 0x7f); chr_bank(2, 0xfe, 2);
    for (unsigned i = 0; i < 4; ++i) hw_cart_cpu_write((uint16_t)(0xa000 + i), (uint8_t)(0x30 + i));
    for (unsigned i = 0; i < 4; ++i) chr_bank(4 + i, 0x30 + i, 1);
    hw_cart_cpu_write(0xe002, 0x77); chr_bank(6, 0x77, 1); /* $E002 = $A002 */
    hw_cart_cpu_write(0x8000, 0x05); CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    no_wram();
    /* TC0690: mirroring at $E000, inverted latch, IRQ ~4 CPU cycles late. */
    cart(48, 256, 256);
    CHECK(hw_cart.watch_ppu_addr && hw_cart.watch_cpu);
    hw_cart_cpu_write(0x8000, 0x45); prg_banks(5, 0, 30, 31);
    CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0xe000, 0x40); CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    hw_cart_cpu_write(0xc000, 0xfe); CHECK(hw_cart.m.irq_latch == 1);
    hw_cart_cpu_write(0xc001, 0); hw_cart_cpu_write(0xc002, 0);
    hw.cycles = 100;
    hw_cart_ppu_addr(0x0000); hw.cycles += 4; hw_cart_ppu_addr(0x1000);   /* reload to 1 */
    hw_cart_ppu_addr(0x0000); hw.cycles += 4; hw_cart_ppu_addr(0x1000);   /* 0: delay starts */
    CHECK(!hw_cart_irq());
    cpu_cycles(21); CHECK(!hw_cart_irq());
    cpu_cycles(1); CHECK(hw_cart_irq());
    hw_cart_cpu_write(0xc003, 0); CHECK(!hw_cart_irq());
    /* $C003 also cancels a pending delayed IRQ. */
    hw_cart_cpu_write(0xc002, 0);
    hw_cart_ppu_addr(0x0000); hw.cycles += 4; hw_cart_ppu_addr(0x1000);   /* reload 1 */
    hw_cart_ppu_addr(0x0000); hw.cycles += 4; hw_cart_ppu_addr(0x1000);   /* 0 */
    cpu_cycles(2); hw_cart_cpu_write(0xc003, 0); cpu_cycles(10); CHECK(!hw_cart_irq());
}

/* Namco 118 boards (nesdev wiki, INES Mapper 088, 095, 154): Namco 108
 * banking; 88/154 wire PPU A12 to CHR A16, 154 adds one-screen control in bit
 * 6 of any $8000-$FFFF write, 95 wires CHR A15 to CIRAM A10. */
static void test_mapper_namco118(void)
{
    cart(88, 128, 128);
    hw_cart_cpu_write(0x8000, 0); hw_cart_cpu_write(0x8001, 0x45);    /* R0 -> $04, left half */
    hw_cart_cpu_write(0x8000, 2); hw_cart_cpu_write(0x8001, 0x03);    /* R2 -> $43, right half */
    hw_cart_cpu_write(0x8000, 6); hw_cart_cpu_write(0x8001, 5);
    chr_bank(0, 0x04, 2); chr_bank(4, 0x43, 1); prg_banks(5, 1, 14, 15);
    CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);                 /* soldered */
    hw_cart_cpu_write(0xc000, 0x40); CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    no_wram();
    cart(154, 128, 128);
    CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_A);
    hw_cart_cpu_write(0xe000, 0x40); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_B);
    prg_banks(0, 1, 14, 15);                                           /* $E000 writes no bank */
    hw_cart_cpu_write(0x8000, 0x02); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_A);
    hw_cart_cpu_write(0x8001, 0x47); chr_bank(4, 0x47, 1); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_B);
    cart(95, 128, 32);
    hw_cart_cpu_write(0x8000, 0); hw_cart_cpu_write(0x8001, 0x20);
    hw_cart_cpu_write(0x8000, 1); hw_cart_cpu_write(0x8001, 0x02);
    CHECK(hw_cart_nt_a10(0x2000) == 0x400 && hw_cart_nt_a10(0x2400) == 0x400);
    CHECK(hw_cart_nt_a10(0x2800) == 0 && hw_cart_nt_a10(0x2c00) == 0);
    hw_cart_cpu_write(0x8000, 1); hw_cart_cpu_write(0x8001, 0x22);
    CHECK(hw_cart_nt_a10(0x2800) == 0x400);
    no_wram();
}

/* Discrete latches (nesdev wiki, INES Mapper 078, 089, 093, 097, 072, 092,
 * 086, 101). Most have AND bus conflicts; 97, 86 and 101 do not. */
static void test_discrete_batch3(void)
{
    /* 78: [CCCC MPPP]; submapper 3 (Holy Diver) M = H/V, else one-screen. */
    cart(78, 128, 128);
    hw_cart_cpu_write(0x8000, 0x5b); prg_banks(6, 7, 14, 15); chr_bank(0, 40, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_B);
    prg[hw_cart.prg_off[0]] = 0x0f; hw_cart_cpu_write(0x8000, 0xf0);     /* AND bus conflict */
    prg_banks(0, 1, 14, 15); chr_bank(0, 0, 8); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_A);
    cart(78, 128, 128); hw_cart.info.submapper = 3;
    hw_cart_cpu_write(0x8000, 0x08); CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0x8000, 0x00); CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    no_wram();
    /* 89: [CPPP MCCC]. */
    cart(89, 128, 128);
    hw_cart_cpu_write(0x8000, 0xd5); prg_banks(10, 11, 14, 15); chr_bank(0, 13 * 8, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_A);
    hw_cart_cpu_write(0x8000, 0x0b); chr_bank(0, 24, 8); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_B);
    no_wram();
    /* 93: [.PPP ...E]; E = CHR RAM enable (disabled: open bus, writes ignored). */
    cart(93, 128, 8); hw_cart.chr_ram = 1;
    hw_cart_cpu_write(0x8000, 0x30); prg_banks(6, 7, 14, 15);
    CHECK(hw_cart_chr_read(0x0123) == 0x23 && !hw_cart.chr_write[0]);
    hw_cart_cpu_write(0x8000, 0x31); CHECK(hw_cart.chr_write[0] && hw_cart.chr_write[7]);
    chr[0x0123] = 0x99; CHECK(hw_cart_chr_read(0x0123) == 0x99);
    no_wram();
    /* 97: $8000-$BFFF [M..P PPPP]; $8000 fixed last, $C000 switchable; M 0 H / 1 V. */
    cart(97, 256, 8);
    prg_banks(30, 31, 0, 1);
    hw_cart_cpu_write(0x8000, 0x85); prg_banks(30, 31, 10, 11); CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0xc000, 0x03); prg_banks(30, 31, 10, 11);         /* not decoded */
    hw_cart_cpu_write(0xbfff, 0x03); prg_banks(30, 31, 6, 7); CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    /* 72 / 92: banks load on a 0 -> 1 edge of bit 7 (PRG) and bit 6 (CHR). */
    cart(72, 128, 128);
    hw_cart_cpu_write(0x8000, 0x83); prg_banks(6, 7, 14, 15);
    hw_cart_cpu_write(0x8000, 0x85); prg_banks(6, 7, 14, 15);           /* no edge */
    hw_cart_cpu_write(0x8000, 0x05); hw_cart_cpu_write(0x8000, 0x85); prg_banks(10, 11, 14, 15);
    hw_cart_cpu_write(0x8000, 0x43); chr_bank(0, 24, 8);
    hw_cart_cpu_write(0x8000, 0x44); chr_bank(0, 24, 8);
    cart(92, 256, 128);
    prg_banks(0, 1, 30, 31);
    hw_cart_cpu_write(0x8000, 0x83); prg_banks(0, 1, 6, 7);
    /* 86: $6000-$6FFF [.CPP ..CC]; $7000 (speech) and $8000+ change nothing. */
    cart(86, 128, 64);
    hw_cart_cpu_write(0x6000, 0x65); prg_banks(8, 9, 10, 11); chr_bank(0, 40, 8);
    hw_cart_cpu_write(0x7000, 0x00); hw_cart_cpu_write(0x8000, 0x00); prg_banks(8, 9, 10, 11); chr_bank(0, 40, 8);
    no_wram();
    /* 101: $6000-$7FFF selects 8 KiB CHR, bits in order. */
    cart(101, 32, 64);
    hw_cart_cpu_write(0x7fff, 0x06); chr_bank(0, 48, 8);
    no_wram();
}

/* nesdev wiki, INES Mapper 077 (Irem LROG017) and 096 (Oeka Kids). */
static void test_chr_tricks_batch3(void)
{
    cart(77, 128, 16);
    hw_cart_cpu_write(0x8000, 0x32); prg_banks(8, 9, 10, 11);
    CHECK(hw_cart.chr_off[0] == 6 * 1024 && hw_cart.chr_off[1] == 7 * 1024);
    CHECK(!hw_cart.chr_write[0] && !hw_cart.chr_write[1] && hw_cart.chr_write[2] && hw_cart.chr_write[7]);
    CHECK(hw_cart.chr_off[2] == hw_cart.chr_ram_base && hw_cart.chr_off[7] == hw_cart.chr_ram_base + 5 * 1024);
    prg[hw_cart.prg_off[0]] = 0x10; hw_cart_cpu_write(0x8000, 0xf3);   /* AND bus conflict */
    prg_banks(0, 1, 2, 3); CHECK(hw_cart.chr_off[0] == 2 * 1024);
    no_wram();
    cart(96, 128, 32);
    CHECK(hw_cart.watch_ppu_addr);
    hw_cart_cpu_write(0x8000, 0x07); prg_banks(12, 13, 14, 15);
    CHECK(hw_cart.chr_off[0] == 16 * 1024 && hw_cart.chr_off[4] == 28 * 1024);
    hw_cart_ppu_addr(0x2300); CHECK(hw_cart.chr_off[0] == 28 * 1024);   /* inner = A9-A8 */
    hw_cart_ppu_addr(0x2100); CHECK(hw_cart.chr_off[0] == 28 * 1024);   /* no move into $2xxx */
    hw_cart_ppu_addr(0x3f00); hw_cart_ppu_addr(0x2100); CHECK(hw_cart.chr_off[0] == 20 * 1024);
    hw_cart_ppu_addr(0x0000); hw_cart_ppu_addr(0x2e00); CHECK(hw_cart.chr_off[0] == 24 * 1024);
    no_wram();
}

/* Unlicensed variants (nesdev wiki, INES Mapper 144, 146, 148). */
static void test_unlicensed_batch3(void)
{
    cart(144, 128, 128);
    prg[hw_cart.prg_off[0]] = 0xf3;
    hw_cart_cpu_write(0x8000, 0x22); prg_banks(12, 13, 14, 15); chr_bank(0, 16, 8);  /* ROM D0 wins */
    prg[hw_cart.prg_off[0]] = 0xf0; hw_cart_cpu_write(0x8000, 0x33); prg_banks(0, 1, 2, 3); chr_bank(0, 24, 8);
    cart(146, 64, 64);
    hw_cart_cpu_write(0x4100, 0x0b); prg_banks(4, 5, 6, 7); chr_bank(0, 24, 8);
    hw_cart_cpu_write(0x4000, 0x00); hw_cart_cpu_write(0x8000, 0x00); prg_banks(4, 5, 6, 7);
    cart(148, 64, 64);
    hw_cart_cpu_write(0x8000, 0x0d); prg_banks(4, 5, 6, 7); chr_bank(0, 40, 8);
    prg[hw_cart.prg_off[0]] = 0x07; hw_cart_cpu_write(0x8000, 0x0e); prg_banks(0, 1, 2, 3); chr_bank(0, 48, 8);
    no_wram();
}

/* nesdev wiki, INES Mapper 067: Sunsoft-3. */
static void test_mapper67(void)
{
    cart(67, 256, 256);
    CHECK(hw_cart.watch_cpu);
    prg_banks(0, 1, 30, 31);
    hw_cart_cpu_write(0xf800, 5); prg_banks(10, 11, 30, 31);
    hw_cart_cpu_write(0xffff, 2); prg_banks(4, 5, 30, 31);
    hw_cart_cpu_write(0x8800, 3); chr_bank(0, 6, 2);
    hw_cart_cpu_write(0xb800, 0x40); chr_bank(6, 0x80, 2);
    hw_cart_cpu_write(0x9000, 9); chr_bank(2, 2, 2);                    /* not a register */
    hw_cart_cpu_write(0xe800, 1); CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    hw_cart_cpu_write(0xe800, 3); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_B);
    no_wram();
    /* Counter loaded high then low; runs with $D800 bit 4; wrap disables and asserts. */
    hw_cart_cpu_write(0xc800, 0x00); hw_cart_cpu_write(0xc800, 0x03);
    CHECK(hw_cart.m.irq_counter16 == 3);
    cpu_cycles(5); CHECK(hw_cart.m.irq_counter16 == 3);              /* paused */
    hw_cart_cpu_write(0xd800, 0x10);
    cpu_cycles(3); CHECK(hw_cart.m.irq_counter16 == 0 && !hw_cart_irq());
    cpu_cycles(1); CHECK(hw_cart.m.irq_counter16 == 0xffff && hw_cart_irq() && !hw_cart.m.irq_enable);
    cpu_cycles(4); CHECK(hw_cart.m.irq_counter16 == 0xffff);
    hw_cart_cpu_write(0xd800, 0x10); CHECK(hw_cart_irq());          /* only $8000 acknowledges */
    hw_cart_cpu_write(0x8000, 0); CHECK(!hw_cart_irq());
    /* $D800 resets the byte toggle. */
    hw_cart_cpu_write(0xd800, 0); hw_cart_cpu_write(0xc800, 0x12); hw_cart_cpu_write(0xd800, 0);
    hw_cart_cpu_write(0xc800, 0x34); hw_cart_cpu_write(0xc800, 0x56); CHECK(hw_cart.m.irq_counter16 == 0x3456);
}

/* nesdev wiki, INES Mapper 065: Irem H3001. */
static void test_mapper65(void)
{
    cart(65, 256, 256);
    CHECK(hw_cart.watch_cpu);
    prg_banks(0, 1, 30, 31);                                        /* power-on $00/$01 */
    hw_cart_cpu_write(0x8000, 5); hw_cart_cpu_write(0xa007, 7); prg_banks(5, 7, 30, 31);
    hw_cart_cpu_write(0x9000, 0x80); prg_banks(30, 7, 5, 31);       /* no $C000 register */
    hw_cart_cpu_write(0xc000, 9); prg_banks(30, 7, 5, 31);
    hw_cart_cpu_write(0xb003, 0x44); chr_bank(3, 0x44, 1);
    hw_cart_cpu_write(0x9001, 0x00); CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0x9001, 0x80); CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    hw_cart_cpu_write(0x9001, 0x40); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_A);
    hw_cart_cpu_write(0x9001, 0xc0); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_A);
    no_wram();
    /* 16-bit counter: $9004 loads, $9003 bit 7 enables, IRQ on reaching 0, stops there. */
    hw_cart_cpu_write(0x9005, 0x00); hw_cart_cpu_write(0x9006, 0x05); hw_cart_cpu_write(0x9004, 0);
    cpu_cycles(10); CHECK(hw_cart.m.irq_counter16 == 5);            /* disabled: holds */
    hw_cart_cpu_write(0x9003, 0x80);
    cpu_cycles(4); CHECK(hw_cart.m.irq_counter16 == 1 && !hw_cart_irq());
    cpu_cycles(1); CHECK(hw_cart.m.irq_counter16 == 0 && hw_cart_irq());
    cpu_cycles(5); CHECK(hw_cart.m.irq_counter16 == 0 && hw_cart_irq());
    hw_cart_cpu_write(0x9003, 0x80); CHECK(!hw_cart_irq());         /* acknowledge */
    cpu_cycles(20); CHECK(!hw_cart_irq());                          /* stopped at 0 */
    hw_cart_cpu_write(0x9004, 0); CHECK(!hw_cart_irq());            /* reload also acknowledges */
    cpu_cycles(5); CHECK(hw_cart_irq());                            /* still enabled */
    hw_cart_cpu_write(0x9003, 0); hw_cart_cpu_write(0x9004, 0); cpu_cycles(10); CHECK(!hw_cart_irq());
}

/* nesdev wiki, RAMBO-1 (mapper 64) and INES Mapper 158. */
static void rambo_rise(void) { hw_cart_ppu_addr(0x0000); hw.cycles += 4; hw_cart_ppu_addr(0x1000); }

static void test_mapper64(void)
{
    cart(64, 256, 256);
    CHECK(hw_cart.watch_ppu_addr && hw_cart.watch_cpu);
    prg_banks(0, 0, 0, 31);
    hw_cart_cpu_write(0x8000, 6); hw_cart_cpu_write(0x8001, 3);
    hw_cart_cpu_write(0x8000, 7); hw_cart_cpu_write(0x8001, 4);
    hw_cart_cpu_write(0x8000, 15); hw_cart_cpu_write(0x8001, 5);
    prg_banks(3, 4, 5, 31);
    hw_cart_cpu_write(0x8000, 0x4f); prg_banks(5, 4, 3, 31);          /* P swaps $8000/$C000 */
    /* 2 KiB mode ignores the low bit; K uses R8/R9; C inverts A12. */
    hw_cart_cpu_write(0x8000, 0); hw_cart_cpu_write(0x8001, 0x0b);
    hw_cart_cpu_write(0x8000, 1); hw_cart_cpu_write(0x8001, 0x14);
    hw_cart_cpu_write(0x8000, 2); hw_cart_cpu_write(0x8001, 0x30);
    hw_cart_cpu_write(0x8000, 8); hw_cart_cpu_write(0x8001, 0x21);
    hw_cart_cpu_write(0x8000, 9); hw_cart_cpu_write(0x8001, 0x22);
    CHECK(hw_cart.chr_off[0] == 10*1024 && hw_cart.chr_off[1] == 11*1024 && hw_cart.chr_off[2] == 20*1024 &&
          hw_cart.chr_off[3] == 21*1024 && hw_cart.chr_off[4] == 0x30*1024);
    hw_cart_cpu_write(0x8000, 0x20);
    CHECK(hw_cart.chr_off[0] == 11*1024 && hw_cart.chr_off[1] == 0x21*1024 && hw_cart.chr_off[2] == 20*1024 &&
          hw_cart.chr_off[3] == 0x22*1024);
    hw_cart_cpu_write(0x8000, 0xa0);
    CHECK(hw_cart.chr_off[4] == 11*1024 && hw_cart.chr_off[5] == 0x21*1024 && hw_cart.chr_off[0] == 0x30*1024);
    CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0xa000, 1); CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    no_wram();
    /* Scanline mode: a $C001 reload uses latch|1; /IRQ two CPU cycles after the rise. */
    hw_cart_cpu_write(0xc000, 2); hw_cart_cpu_write(0xc001, 0); hw_cart_cpu_write(0xe001, 0);
    hw.cycles = 100;
    rambo_rise(); CHECK(hw_cart.m.irq_counter == 3);
    rambo_rise(); rambo_rise(); CHECK(hw_cart.m.irq_counter == 1 && !hw_cart_irq());
    rambo_rise(); CHECK(hw_cart.m.irq_counter == 0 && !hw_cart_irq());
    cpu_cycles(1); CHECK(!hw_cart_irq());
    cpu_cycles(1); CHECK(hw_cart_irq());
    rambo_rise(); CHECK(hw_cart.m.irq_counter == 2);                  /* zero reloads the latch */
    hw_cart_cpu_write(0xe000, 0); CHECK(!hw_cart_irq());
    /* Cycle mode: one clock per 4 CPU cycles, restarted by $C001; /IRQ one cycle late. */
    hw_cart_cpu_write(0xc000, 1); hw_cart_cpu_write(0xc001, 1); hw_cart_cpu_write(0xe001, 0);
    rambo_rise(); CHECK(hw_cart.m.irq_counter == 2);                  /* A12 ignored in cycle mode */
    cpu_cycles(3); CHECK(hw_cart.m.irq_reload);
    cpu_cycles(1); CHECK(hw_cart.m.irq_counter == 1 && !hw_cart.m.irq_reload);
    cpu_cycles(4); CHECK(hw_cart.m.irq_counter == 0 && !hw_cart_irq());
    cpu_cycles(1); CHECK(hw_cart_irq());
    hw_cart_cpu_write(0xe000, 0); CHECK(!hw_cart_irq());
    /* $E000 also cancels a pending /IRQ: latch 0 triggers on the next clock. */
    hw_cart_cpu_write(0xc000, 0); hw_cart_cpu_write(0xc001, 1); hw_cart_cpu_write(0xe001, 0);
    cpu_cycles(4); CHECK(hw_cart.m.irq_counter == 0 && !hw_cart_irq());
    hw_cart_cpu_write(0xe000, 0); cpu_cycles(8); CHECK(!hw_cart_irq());
}

static void test_mapper158(void)
{
    cart(158, 128, 256);
    /* CHR A17 (bank bit 7) drives CIRAM A10; $A000 does nothing. */
    hw_cart_cpu_write(0x8000, 0); hw_cart_cpu_write(0x8001, 0x80);
    hw_cart_cpu_write(0x8000, 1); hw_cart_cpu_write(0x8001, 0x02);
    CHECK(hw_cart_nt_a10(0x2000) == 0x400 && hw_cart_nt_a10(0x2400) == 0x400);
    CHECK(hw_cart_nt_a10(0x2800) == 0 && hw_cart_nt_a10(0x2c00) == 0);
    hw_cart_cpu_write(0x8000, 0x22); hw_cart_cpu_write(0x8001, 0x84);   /* K; R2 bit 7 */
    hw_cart_cpu_write(0x8000, 0xa8); hw_cart_cpu_write(0x8001, 0x00);   /* C; R8 */
    CHECK(hw_cart_nt_a10(0x2000) == 0x400 && hw_cart_nt_a10(0x2400) == 0);  /* C: pages 0-3 = R2-R5 */
    hw_cart_cpu_write(0xa000, 1); CHECK(hw_cart_nt_a10(0x2000) == 0x400);
    no_wram();
}

/* nesdev wiki, INES Mapper 018: Jaleco SS88006. */
static void test_mapper18(void)
{
    cart(18, 512, 256);
    CHECK(hw_cart.watch_cpu);
    prg_banks(0, 0, 0, 63);
    hw_cart_cpu_write(0x8000, 0xf5); hw_cart_cpu_write(0x8001, 0x02);    /* $25 */
    hw_cart_cpu_write(0x8002, 0x09); hw_cart_cpu_write(0x8003, 0x03);    /* $39 */
    hw_cart_cpu_write(0x9000, 0x0e); hw_cart_cpu_write(0x9001, 0x0f);    /* $FE -> 6 bits */
    prg_banks(0x25, 0x39, 0x3e, 63);
    for (unsigned p = 0; p < 8; ++p) {
        uint16_t base = (uint16_t)(0xa000 + (p >> 1) * 0x1000 + (p & 1) * 2);
        hw_cart_cpu_write(base, (uint8_t)(p + 1)); hw_cart_cpu_write((uint16_t)(base + 1), (uint8_t)(15 - p));
    }
    for (unsigned p = 0; p < 8; ++p) chr_bank(p, (15 - p) << 4 | (p + 1), 1);
    /* Other address bits are ignored: $AFFD = $A001 (page 0 high nibble). */
    hw_cart_cpu_write(0xaffd, 0x0a); chr_bank(0, 0xa1, 1);
    static const uint8_t mirror[4] = { HW_MIRROR_HORIZONTAL, HW_MIRROR_VERTICAL, HW_MIRROR_SCREEN_A, HW_MIRROR_SCREEN_B };
    for (unsigned m = 0; m < 4; ++m) { hw_cart_cpu_write(0xf002, (uint8_t)m); CHECK(hw_cart.mirroring == mirror[m]); }
    /* $9002: bit 0 enables the RAM chip, bit 1 allows writes. */
    hw_cart.has_wram = 1; hw_cart.wram_len = 8192;
    uint8_t value = 0xa5;
    hw_cart_cpu_write(0x9002, 0); CHECK(!hw_cart_cpu_read(0x6000, &value) && value == 0xa5);
    hw_cart_cpu_write(0x9002, 3); hw_cart_cpu_write(0x6000, 0x5a); CHECK(hw_cart_cpu_read(0x6000, &value) && value == 0x5a);
    hw_cart_cpu_write(0x9002, 1); hw_cart_cpu_write(0x6000, 0x11); CHECK(hw_cart_cpu_read(0x6000, &value) && value == 0x5a);
    hw_cart_cpu_write(0x9003, 0); CHECK(hw_cart_cpu_read(0x6000, &value));   /* $9003 is not the RAM register */
    hw_cart_cpu_write(0x9002, 2); value = 0xa5; CHECK(!hw_cart_cpu_read(0x6000, &value));
    /* IRQ: 16-bit reload $1234, underflow interrupts, counting continues. */
    hw_cart_cpu_write(0xe000, 4); hw_cart_cpu_write(0xe001, 3); hw_cart_cpu_write(0xe002, 2); hw_cart_cpu_write(0xe003, 1);
    cpu_cycles(100); CHECK(!hw_cart_irq());
    hw_cart_cpu_write(0xf000, 0); CHECK(hw_cart.m.jaleco.counter == 0x1234);
    cpu_cycles(100); CHECK(hw_cart.m.jaleco.counter == 0x1234);          /* disabled */
    hw_cart_cpu_write(0xf001, 1);
    cpu_cycles(0x1234); CHECK(!hw_cart_irq() && hw_cart.m.jaleco.counter == 0);
    cpu_cycles(1); CHECK(hw_cart_irq() && hw_cart.m.jaleco.counter == 0xffff);
    cpu_cycles(2); CHECK(hw_cart_irq() && hw_cart.m.jaleco.counter == 0xfffd);
    hw_cart_cpu_write(0xf001, 1); CHECK(!hw_cart_irq());                   /* acknowledge */
    /* 4-, 8- and 12-bit widths leave the upper bits alone. */
    static const uint8_t ctrl[3] = { 9, 5, 3 };
    static const uint16_t mask[3] = { 0x000f, 0x00ff, 0x0fff };
    for (unsigned w = 0; w < 3; ++w) {
        hw_cart_cpu_write(0xf000, 0); hw_cart_cpu_write(0xf001, ctrl[w]);
        unsigned low = 0x1234 & mask[w];
        cpu_cycles(low); CHECK(!hw_cart_irq() && hw_cart.m.jaleco.counter == (0x1234 & ~mask[w]));
        cpu_cycles(1); CHECK(hw_cart_irq() && hw_cart.m.jaleco.counter == ((0x1234 & ~mask[w]) | mask[w]));
        hw_cart_cpu_write(0xf000, 0); CHECK(!hw_cart_irq());                /* $F000 also acknowledges */
    }
    hw_cart_cpu_write(0xf001, 0);
}

/* nesdev wiki, INES Mapper 185: chip-select values gate CHR ROM. */
static void test_mapper185(void)
{
    for (unsigned sub = 4; sub <= 7; ++sub) {
        cart(185, 32, 8);
        for (unsigned i = 0; i < 8192; ++i) chr[i] = (uint8_t)(0x80 + (i >> 10));
        hw_cart.info.submapper = (uint8_t)sub; hw_cart_power_on();
        prg_banks(0, 1, 2, 3); chr_bank(0, 0, 8);
        for (unsigned cs = 0; cs < 8; ++cs) {
            hw_cart_cpu_write(0xb000, (uint8_t)(0x30 | cs));   /* ROM $FF: no conflict */
            bool on = (cs & 3) == sub - 4;
            CHECK(pattern_read(0x0456) == (on ? 0x81 : 0x57));
            CHECK(pattern_read(0x1c02) == (on ? 0x87 : 0x03));
        }
        prg[0x1000] = 0xfc;                                   /* AND bus conflict */
        hw_cart_cpu_write(0x9000, (uint8_t)(sub - 4 + 4));
        CHECK(hw_cart.m.latch == ((sub - 4 + 4) & 0xfc));
        prg_banks(0, 1, 2, 3); no_wram();
    }
    /* Submapper 0: disabled for the fetches of the first two $2007 reads. */
    cart(185, 32, 8);
    for (unsigned i = 0; i < 8192; ++i) chr[i] = 0x80;
    hw_cart_cpu_write(0xb000, 0x13);
    CHECK(pattern_read(0x0010) == 0x11); hw_cart_ppu_data_read();
    CHECK(pattern_read(0x0010) == 0x11); hw_cart_ppu_data_read();
    CHECK(pattern_read(0x0010) == 0x11); hw_cart_ppu_data_read();
    CHECK(pattern_read(0x0010) == 0x80);
    hw_cart_cpu_write(0xb000, 0x00); CHECK(pattern_read(0x0010) == 0x80);
}

/* nesdev wiki, INES Mapper 228: 1.MHHPPP PPS.CCCC plus D0-D1. */
static void test_mapper228(void)
{
    cart(228, 256, 512);
    prg_banks(0, 1, 2, 3); chr_bank(0, 0, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0x8000 | 0x2000 | 5 << 6 | 0x20 | 3, 2);   /* 16 KiB page 5 twice */
    prg_banks(10, 11, 10, 11); chr_bank(0, 14 * 8, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    hw_cart_cpu_write(0x8000 | 5 << 6 | 0xf, 0xff);               /* even/odd pair 4-5 */
    prg_banks(8, 9, 10, 11); chr_bank(0, 63 * 8, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0xffff, 0);                                 /* chip 3 wraps in 256 KiB */
    prg_banks(30, 31, 30, 31); chr_bank(0, 60 * 8, 8);
    hw_cart_cpu_write(0x7fff, 1); prg_banks(30, 31, 30, 31);      /* below $8000: no latch */
    no_wram();
    /* Action 52: chips 0, 1, 3 stored consecutively; chip 2 is open bus. */
    cart(228, 512, 512);
    hw_cart.prg_len = 0x180000; hw_cart.prg_slots = 512;
    hw_cart_cpu_write(0x8000 | 3 << 11 | 1 << 6 | 0x20, 0);       /* chip 3 page 1 */
    for (unsigned s = 0; s < 8; ++s) CHECK(hw_cart.prg_off[s] == 65u * 16384 + (s % 4) * 4096);
    CHECK(hw_prg_is_rom(0x8000));
    hw_cart_cpu_write(0x8000 | 2 << 11, 0);                       /* chip 2 */
    for (unsigned s = 0; s < 8; ++s) CHECK(hw_cart.prg_off[s] & MMC5_PRG_OPEN);
    CHECK(!hw_prg_is_rom(0x8000) && !hw_prg_is_rom(0xffff));
    hw_cart_cpu_write(0x8000 | 1 << 11 | 31 << 6, 0);             /* chip 1, pair 30-31 */
    prg_banks(124, 125, 126, 127);
    hw_cart_cpu_write(0x8000, 0); prg_banks(0, 1, 2, 3);
}

/* nesdev wiki, INES Mapper 041: address-latched outer bank, gated inner CHR. */
static void test_mapper41(void)
{
    cart(41, 256, 128);
    prg_banks(0, 1, 2, 3); chr_bank(0, 0, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0x603d, 0x00);            /* PRG 5, CHR outer 3, horizontal */
    prg_banks(20, 21, 22, 23); chr_bank(0, 96, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    hw_cart_cpu_write(0x9000, 2); chr_bank(0, 112, 8);   /* inner 2: bank 14 */
    prg[hw_cart.prg_off[1]] = 0x01;             /* ROM drives $9000 = $01 */
    hw_cart_cpu_write(0x9000, 3); chr_bank(0, 104, 8);   /* 3 AND 1 */
    hw_cart_cpu_write(0x6803, 0xff);            /* outside $6000-$67FF */
    prg_banks(20, 21, 22, 23);
    hw_cart_cpu_write(0x6003, 0xff);            /* PRG 3: inner writes ignored */
    prg_banks(12, 13, 14, 15); chr_bank(0, 8, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_VERTICAL);
    hw_cart_cpu_write(0xb000, 2); chr_bank(0, 8, 8);
    hw_cart_cpu_write(0x67ff, 0); prg_banks(28, 29, 30, 31); chr_bank(0, 104, 8);
    hw_cart_cpu_write(0xb000, 0); chr_bank(0, 96, 8);
    no_wram();
}

/* nesdev wiki, TQROM: bank bit 6 selects the 8 KiB CHR RAM chip. */
static void test_mapper119(void)
{
    cart(119, 128, 64);
    hw_cart.chr_ram_base = 64 * 1024; hw_cart.chr_ram_len = 8192;
    hw_cart_power_on();
    CHECK(hw_cart.watch_ppu_addr);
    prg_banks(0, 1, 14, 15);
    mmc3_reg(0, 0x40, 0); /* 2 KiB: RAM pages 0-1 */
    mmc3_reg(1, 0x3f, 0); /* 2 KiB: ROM pages 62-63 */
    mmc3_reg(2, 0x47, 0); mmc3_reg(3, 0x7f, 0); /* RAM page 7 twice: A10-A12 only */
    mmc3_reg(4, 0x80, 0); mmc3_reg(5, 0xbf, 0); /* bit 6 clear: ROM, bits 0-5 */
    static const uint32_t ram = 64 * 1024;
    const uint32_t off[8] = { ram, ram + 1024, 62 * 1024, 63 * 1024,
                              ram + 7 * 1024, ram + 7 * 1024, 0, 63 * 1024 };
    const uint8_t writable[8] = { 1, 1, 0, 0, 1, 1, 0, 0 };
    for (unsigned p = 0; p < 8; ++p) {
        CHECK(hw_cart.chr_off[p] == off[p]);
        CHECK(hw_cart.chr_write[p] == writable[p]);
        CHECK(hw_cart_chr_index((uint16_t)(p * 1024 + 0x155)) == off[p] + 0x155);
    }
    hw_cart_cpu_write(0x8000, 0x80); /* swap the halves */
    for (unsigned p = 0; p < 8; ++p) {
        CHECK(hw_cart.chr_off[p ^ 4] == off[p]);
        CHECK(hw_cart.chr_write[p ^ 4] == writable[p]);
    }
    chr[ram + 7 * 1024 + 3] = 0x99;
    CHECK(pattern_read(0x0003) == 0x99 && pattern_read(0x0403) == 0x99);
    /* No work RAM on the board, whatever $A001 says. */
    hw_cart_cpu_write(0xa001, 0x80);
    no_wram();
    hw.cycles = 100;
    hw_cart_cpu_write(0xc000, 0); hw_cart_cpu_write(0xc001, 0); hw_cart_cpu_write(0xe001, 0);
    hw_cart_ppu_addr(0x0000); hw.cycles += 4; hw_cart_ppu_addr(0x1000);
    CHECK(hw_cart_irq());
    /* CHR-RAM boards stay writable through the MMC3 path. */
    cart(4, 128, 8);
    hw_cart.chr_ram = 1; hw_cart_power_on();
    mmc3_reg(2, 3, 0);
    for (unsigned p = 0; p < 8; ++p) CHECK(hw_cart.chr_write[p]);
}

/* nesdev wiki, TxSROM: CHR A17 drives CIRAM A10. */
static void test_mapper118(void)
{
    cart(118, 128, 128);
    CHECK(hw_cart.watch_ppu_addr && hw_cart.has_wram);
    prg_banks(0, 1, 14, 15);
    mmc3_reg(6, 5, 0); mmc3_reg(7, 9, 0);
    prg_banks(5, 9, 14, 15);
    /* Mode 0: R0 covers $2000-$27FF, R1 $2800-$2FFF; bit 7 is not a
     * nametable-only bit, it is the CHR A17 the 128 KiB ROM ignores. */
    mmc3_reg(0, 0x84, 0); mmc3_reg(1, 0x06, 0);
    chr_bank(0, 4, 2); chr_bank(2, 6, 2);
    CHECK(txsrom_nt(0x2000) && txsrom_nt(0x23ff) && txsrom_nt(0x2400) && txsrom_nt(0x27ff));
    CHECK(!txsrom_nt(0x2800) && !txsrom_nt(0x2fff));
    /* R2-R5 map the other pattern half; their A17 is ignored for $2000-$2FFF
     * but drives $3000-$3EFF, which the MMC3 decodes as pattern pages 4-7. */
    mmc3_reg(2, 0x80, 0); mmc3_reg(3, 0, 0); mmc3_reg(4, 0x80, 0); mmc3_reg(5, 0, 0);
    CHECK(txsrom_nt(0x2000) && !txsrom_nt(0x2800));
    CHECK(txsrom_nt(0x3000) && !txsrom_nt(0x3400) && txsrom_nt(0x3800) && !txsrom_nt(0x3c00));
    /* $A000 is disconnected. */
    for (unsigned v = 0; v < 2; ++v) {
        hw_cart_cpu_write(0xa000, (uint8_t)v);
        CHECK(txsrom_nt(0x2000) && txsrom_nt(0x2400) && !txsrom_nt(0x2800) && !txsrom_nt(0x2c00));
    }
    /* Mode 1 swaps the halves: each 1 KiB register selects one nametable. */
    hw_cart_cpu_write(0x8000, 0x80);
    CHECK(txsrom_nt(0x2000) && !txsrom_nt(0x2400) && txsrom_nt(0x2800) && !txsrom_nt(0x2c00));
    mmc3_reg(3, 0x81, 0x80);
    CHECK(txsrom_nt(0x2400));
    mmc3_reg(0, 0, 0x80); /* R0 now maps $1000-$17FF and $3000-$37FF */
    CHECK(!txsrom_nt(0x3000) && !txsrom_nt(0x3400) && txsrom_nt(0x2000));
    chr_bank(0, 0, 1); chr_bank(1, 1, 1); chr_bank(4, 0, 2);
    /* The MMC3 IRQ counter runs from A12 exactly as on mapper 4. */
    hw.cycles = 100;
    hw_cart_cpu_write(0xc000, 1); hw_cart_cpu_write(0xc001, 0); hw_cart_cpu_write(0xe001, 0);
    for (unsigned edge = 0; edge < 2; ++edge) {
        hw_cart_ppu_addr(0x0000); hw.cycles += 4; hw_cart_ppu_addr(0x1000);
    }
    CHECK(hw_cart_irq());
    hw_cart_cpu_write(0xe000, 0); CHECK(!hw_cart_irq());
    /* Work RAM follows the MMC3's $A001 protect bits. */
    uint8_t value = 0;
    hw_cart_cpu_write(0xa001, 0x80); hw_cart_cpu_write(0x6000, 0x5a);
    CHECK(hw_cart_cpu_read(0x6000, &value) && value == 0x5a);
    hw_cart_cpu_write(0xa001, 0xc0); hw_cart_cpu_write(0x6000, 0x11);
    CHECK(hw_cart_cpu_read(0x6000, &value) && value == 0x5a);
}

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

/* Bandai 74161/7432 (nesdev INES Mapper 070, 152): one latch, 16 KiB PRG at
 * $8000 in the high nibble, 8 KiB CHR in the low one, last 16 KiB fixed, AND
 * bus conflicts. 70's mirroring is soldered; 152 selects one screen in bit 7
 * and has three PRG bits. */
static void test_mapper70(void)
{
    cart(70, 256, 128);
    prg_banks(0, 1, 30, 31); chr_bank(0, 0, 8);
    hw_cart_cpu_write(0x8000, 0xa5);
    prg_banks(20, 21, 30, 31); chr_bank(0, 40, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    prg[hw_cart.prg_off[0]] = 0x0f;
    hw_cart_cpu_write(0x8000, 0xff);                     /* AND bus conflict */
    prg_banks(0, 1, 30, 31); chr_bank(0, 120, 8);
    CHECK(hw_cart.mirroring == HW_MIRROR_HORIZONTAL);
    no_wram();
}

static void test_mapper152(void)
{
    cart(152, 256, 128);
    prg_banks(0, 1, 30, 31); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_A);
    hw_cart_cpu_write(0xc000, 0xd3);
    prg_banks(10, 11, 30, 31); chr_bank(0, 24, 8); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_B);
    hw_cart_cpu_write(0x8000, 0x7f);                     /* three PRG bits */
    prg_banks(14, 15, 30, 31); chr_bank(0, 120, 8); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_A);
    prg[hw_cart.prg_off[0]] = 0x25;
    hw_cart_cpu_write(0x8000, 0xff);                     /* AND bus conflict */
    prg_banks(4, 5, 30, 31); chr_bank(0, 40, 8); CHECK(hw_cart.mirroring == HW_MIRROR_SCREEN_A);
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
#include "mapper40_test.inc"
#include "sunsoft_test.inc"
#include "namco_test.inc"

int main(void)
{
    cart(0, 32, 8);
    prg_banks(0, 1, 2, 3);
    chr_bank(0, 0, 8);
    no_wram();
    /* Run added board contracts. */
    test_mapper41();
    test_mapper228();
    test_mapper185();
    test_mapper18();
    test_taito();
    test_mapper32();
    test_taito_x1();
    test_mapper118();
    test_mapper119();
    test_mapper232();
    test_mapper184();
    test_mapper180();
    test_mapper70();
    test_mapper152();
    test_mapper64();
    test_mapper158();
    test_mapper65();
    test_mapper67();
    test_discrete_batch3();
    test_chr_tricks_batch3();
    test_unlicensed_batch3();
    test_mapper_namco118();
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
    test_mmc1a();
    test_mapper40();
    test_sunsoft();
    test_namco();
    printf("mapper contracts: %u checks passed\n", checks);
    return 0;
}
