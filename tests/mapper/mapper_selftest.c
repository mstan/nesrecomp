#include "mapper.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

uint64_t g_frame_count = 0;
const char *g_last_recomp_func = NULL;
uint8_t g_chr_ram[0x2000];

static void test_uxrom_prg_banking(void) {
    uint8_t prg[8 * 0x4000] = {0};

    for (int bank = 0; bank < 8; bank++) {
        prg[bank * 0x4000 + 0x1000] = (uint8_t)bank;
    }

    mapper_init(prg, 8, 2, 0);
    assert(mapper_peek_prg(0x9000) == 0);
    assert(mapper_peek_prg(0xD000) == 7);

    mapper_write(0xC005, 5);
    assert(mapper_peek_prg(0x9000) == 5);
    assert(mapper_peek_prg(0xD000) == 7);

    mapper_write(0xFFFF, 0xFF);
    assert(mapper_peek_prg(0x9000) == 7);
    assert(mapper_peek_prg(0xD000) == 7);

    mapper_write(0x7FFF, 3);
    assert(mapper_peek_prg(0x9000) == 7);
}

static void test_mapper40_prg_and_irq(void) {
    uint8_t prg[4 * 0x4000] = {0};
    for (int bank8 = 0; bank8 < 8; bank8++) {
        prg[bank8 * 0x2000 + 0x0123] = (uint8_t)(0xA0 + bank8);
    }

    mapper_init(prg, 4, 40, 0);
    assert(mapper_peek_prg(0x6123) == 0xA6);
    assert(mapper_peek_prg(0x8123) == 0xA4);
    assert(mapper_peek_prg(0xA123) == 0xA5);
    assert(mapper_peek_prg(0xC123) == 0xA0);
    assert(mapper_peek_prg(0xE123) == 0xA7);

    mapper_write(0xE000, 3);
    assert(mapper_peek_prg(0xC123) == 0xA3);

    mapper_write(0xA000, 0);
    mapper_clock_cpu(4095);
    assert(!mapper_irq_asserted());
    mapper_clock_cpu(1);
    assert(mapper_irq_asserted());
    mapper_clock_cpu(4096);
    assert(mapper_irq_asserted());

    MapperState saved;
    mapper_get_state(&saved);
    mapper_write(0x8000, 0);
    assert(!mapper_irq_asserted());
    mapper_write(0xE000, 1);
    assert(mapper_peek_prg(0xC123) == 0xA1);
    mapper_set_state(&saved);
    assert(mapper_irq_asserted());
    assert(mapper_peek_prg(0xC123) == 0xA3);

    mapper_write(0x8000, 0);
    assert(!mapper_irq_asserted());
}

int main(void) {
    test_uxrom_prg_banking();
    test_mapper40_prg_and_irq();
    puts("mapper self-test passed");
    return 0;
}
