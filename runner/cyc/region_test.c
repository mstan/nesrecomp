/* Region clocks and state phase are observable contracts, independent of a
 * game's presentation rate. PAL constants: NESdev Cycle reference chart and
 * APU Frame Counter; Mesen2's 2A07 tables provide a second implementation. */
#include "hw_internal.h"
#include "cyc_state.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void boot(CycRegion region, unsigned align)
{
    static uint8_t image[16 + 16384];
    memcpy(image, "NES\032", 4); image[4] = 1;
    CHECK(cyc_load_ines(image, sizeof(image)));
    CHECK(cyc_set_region(region));
    cyc_power_on((uint8_t)align);
}

static void cycles(unsigned count)
{
    while (count--) {
        hw_cycle_start(0, HW_READ);
        hw_read(0);
        hw_cycle_finish(false);
    }
}

static unsigned apu_field(const char *name)
{
    FILE *file = tmpfile();
    CHECK(file != NULL);
    if (!file) return 0;
    apu_state_dump(file); rewind(file);
    char line[1024], field[80];
    snprintf(field, sizeof(field), "apu.%s ", name);
    unsigned result = 0;
    bool found = false;
    while (fgets(line, sizeof(line), file)) if (!strncmp(line, field, strlen(field))) {
        const char *bytes = line + strlen(field);
        /* Dump bytes are in the machine's little-endian order. */
        for (unsigned i = 0; i < 4 && bytes[2*i] && bytes[2*i] != '\n'; ++i) {
            unsigned b = 0; sscanf(bytes + 2*i, "%2x", &b); result |= b << (8*i);
        }
        found = true; break;
    }
    CHECK(found); fclose(file); return result;
}

static size_t chunk(uint8_t *data, size_t length, const char *tag)
{
    for (size_t at = 32; at + 8 <= length;) {
        uint32_t size; memcpy(&size, data + at + 4, 4);
        if (!memcmp(data + at, tag, 4)) return at;
        at += 8 + size;
    }
    return 0;
}

int main(void)
{
    CHECK(!cyc_set_region((CycRegion)2));
    for (unsigned align = 0; align < 5; ++align) {
        boot(CYC_REGION_PAL, align);
        CHECK(cyc_region() == CYC_REGION_PAL);
        CHECK(fabs(cyc_cpu_hz() - 1662607.03125) < 0.001);
        CHECK(fabs(1.0 / cyc_frame_seconds() - 50.0069779683) < 0.000001);
        CHECK(cyc_zapper_attach(2)); /* its counter sees actual PPU dot edges */
        uint64_t dots = hw_zapper.dots;
        cycles(100);
        CHECK(hw_zapper.dots - dots == 320); /* 16 PPU dots / 5 CPU cycles */
        CHECK(hw_region_timing.phase == align);
        dots = hw_zapper.dots;
        unsigned position = ppu.scanline * 341 + ppu.dot;
        cycles(66495); /* Exactly two PAL frames, no odd-frame skipped dot. */
        CHECK(hw_zapper.dots - dots == 341 * 312 * 2);
        CHECK((unsigned)(ppu.scanline * 341 + ppu.dot) == position);
        CHECK(!ppu.skipped_dot);
    }
    boot(CYC_REGION_PAL, 0);
    CHECK(cyc_audio_enable(48000));
    cycles(66495);
    int16_t pcm[2048];
    size_t samples = cyc_audio_read(pcm, 2048);
    CHECK(samples >= 1919 && samples <= 1920);
    cyc_audio_enable(0);

    boot(CYC_REGION_PAL, 0);
    ppu.show_bg = ppu.eval_bg = ppu.instant_bg = 1;
    CHECK(cyc_zapper_attach(2));
    uint64_t rendered_dots = hw_zapper.dots;
    cycles(66495);
    CHECK(hw_zapper.dots - rendered_dots == 341 * 312 * 2);
    CHECK(!ppu.skipped_dot);
    /* A sprite at Y=9 must not wrap into VBlank line 265 during refresh. */
    ppu.scanline = 265; ppu.dot = 0; ppu.oam[0] = 9;
    ppu.oam_addr = 0; ppu.oam2_addr = 0; ppu.oam2_full = 0;
    for (unsigned i = 0; i < 68; ++i) { ppu_dot(); ppu_half_dot(); }
    CHECK(ppu.oam2[1] == 0xFF); /* no tile byte selected from that sprite */

    boot(CYC_REGION_PAL, 0);
    for (unsigned i = 1; i < 33252; ++i) apu_cycle();
    CHECK(apu_field("counter") == 33252);
    CHECK(apu_field("frame_irq") == 1);
    apu_cycle(); CHECK(apu_field("counter") == 33253);
    apu_cycle(); CHECK(apu_field("counter") == 0);
    boot(CYC_REGION_PAL, 0);
    apu_write(0x4017, 0x80);
    for (unsigned i = 0; i < 5; ++i) apu_cycle();
    unsigned initial = apu_field("counter");
    for (unsigned i = initial; i < 41565; ++i) apu_cycle();
    CHECK(apu_field("frame_irq") == 0);
    apu_cycle(); CHECK(apu_field("counter") == 0);
    static const unsigned dmc_pal[16] = {398,354,316,298,276,236,210,198,176,148,132,118,98,78,66,50};
    for (unsigned i = 0; i < 16; ++i) {
        apu_write(0x4010, (uint8_t)i); CHECK(apu_field("dmc_rate") == dmc_pal[i]);
    }

    boot(CYC_REGION_PAL, 0);
    hw_write(0x4014, 2);
    cycles(20); CHECK(hw_dma_stalls == 0); /* operand reads do not halt 2A07 */
    hw_cycle_start(0x8000, HW_FETCH);
    CHECK(hw_dma_stalls >= 513 && hw_dma_stalls <= 514);
    hw_read(0x8000); hw_cycle_finish(false);

    boot(CYC_REGION_PAL, 3);
    CHECK(cyc_zapper_attach(2));
    cycles(7); /* phase is neither the reset phase nor zero */
    char error[160]; uint8_t *state = NULL; size_t length = 0;
    CHECK(cyc_state_save(&state, &length, error, sizeof(error)));
    CHECK(state && state[8] == 4);
    uint64_t before = cyc_hw_state_hash();
    CycSnapshot *snapshot = cyc_snapshot_new(); CHECK(snapshot != NULL);
    cyc_snapshot_take(snapshot);
    cycles(111); uint64_t after = cyc_hw_state_hash();
    CHECK(cyc_state_load(state, length, error, sizeof(error)));
    CHECK(cyc_hw_state_hash() == before);
    cycles(111); CHECK(cyc_hw_state_hash() == after);
    cyc_snapshot_restore(snapshot); CHECK(cyc_hw_state_hash() == before);
    cyc_snapshot_free(snapshot);
    size_t at = chunk(state, length, "REGN"); CHECK(at != 0);
    if (at) {
        state[at + 9] = 5; /* damaged /5 divider phase */
        CHECK(!cyc_state_load(state, length, error, sizeof(error)));
        CHECK(cyc_hw_state_hash() == before);
        state[at + 9] = hw_region_timing.phase;
    }
    boot(CYC_REGION_NTSC, 0);
    before = cyc_hw_state_hash();
    CHECK(!cyc_state_load(state, length, error, sizeof(error)));
    CHECK(cyc_hw_state_hash() == before);
    free(state);
    CHECK(cyc_state_save(&state, &length, error, sizeof(error)));
    CHECK(state && state[8] == 2 && !chunk(state, length, "REGN"));
    boot(CYC_REGION_PAL, 0); before = cyc_hw_state_hash();
    CHECK(!cyc_state_load(state, length, error, sizeof(error)));
    CHECK(cyc_hw_state_hash() == before);
    free(state);
    printf("cyc_region_test: %u failures\n", failures);
    return failures != 0;
}
