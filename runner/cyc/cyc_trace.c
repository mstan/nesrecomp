/* cyc_trace.c - see cyc_trace.h.
 *
 * Halted DMA cycles record neither address nor data bus. While a DMA holds the
 * CPU, the address bus keeps the address of the read the CPU is waiting to
 * perform (6502 RDY behavior); TriCNES instead re-reads whatever its
 * addressBus variable last held, which on some cycles is the previous
 * address. The value read that way only matters if something reads open bus
 * before the next access drives the bus, and those reads are recorded. */
#include "cyc_trace.h"

#include <stdio.h>

bool     cyc_trace_enabled;
uint64_t cyc_trace_hash;
void    *cyc_trace_file;

static bool     have_access, access_write, is_dma, is_halt;
static uint16_t access_addr;
static uint8_t  access_value;
uint32_t cyc_trace_cycle;

uint64_t cyc_trace_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t cyc_mem_hash(uint64_t cycles, const uint8_t *ram, const uint8_t *ciram, size_t ciram_len, const uint8_t *oam,
                      const uint8_t *palette, const uint8_t *chr_ram, size_t chr_ram_len,
                      const uint8_t *wram, size_t wram_len, const uint16_t *frame_index) {
    uint64_t h = cycles, acc = 0;
    for (int i = 0; i < 0x800; i++) acc = acc * 131 + ram[i];
    h = cyc_trace_mix(h, acc);
    acc = 0;
    for (size_t i = 0; i < ciram_len; i++) acc = acc * 131 + ciram[i];
    h = cyc_trace_mix(h, acc);
    acc = 0;
    for (int i = 0; i < 0x100; i++) acc = acc * 131 + oam[i];
    for (int i = 0; i < 0x20; i++) acc = acc * 131 + (palette[i] & 0x3F);
    h = cyc_trace_mix(h, acc);
    if (chr_ram) {
        acc = 0;
        for (size_t i = 0; i < chr_ram_len; i++) acc = acc * 131 + chr_ram[i];
        h = cyc_trace_mix(h, acc);
    }
    if (wram) {
        acc = 0;
        for (size_t i = 0; i < wram_len; i++) acc = acc * 131 + wram[i];
        h = cyc_trace_mix(h, acc);
    }
    acc = 0;
    for (int i = 0; i < 256 * 240; i++) acc = acc * 131 + frame_index[i];
    return cyc_trace_mix(h, acc);
}

static void dump_bytes(FILE *f, const char *name, const uint8_t *p, size_t n, uint8_t mask) {
    for (size_t i = 0; i < n; i += 32) {
        fprintf(f, "%s %04X:", name, (unsigned)i);
        for (size_t j = i; j < i + 32 && j < n; j++) fprintf(f, " %02X", p[j] & mask);
        fputc('\n', f);
    }
}

void cyc_mem_dump(void *file, uint64_t cycles, const uint8_t *ram, const uint8_t *ciram, size_t ciram_len, const uint8_t *oam,
                  const uint8_t *palette, const uint8_t *chr_ram, size_t chr_ram_len, const uint8_t *wram,
                  size_t wram_len, const uint16_t *frame_index) {
    FILE *f = (FILE *)file;
    fprintf(f, "cycles %llu\n", (unsigned long long)cycles);
    dump_bytes(f, "ram", ram, 0x800, 0xFF);
    dump_bytes(f, "ciram", ciram, ciram_len, 0xFF);
    dump_bytes(f, "oam", oam, 0x100, 0xFF);
    dump_bytes(f, "palette", palette, 0x20, 0x3F);
    if (chr_ram) dump_bytes(f, "chr_ram", chr_ram, chr_ram_len, 0xFF);
    if (wram) dump_bytes(f, "wram", wram, wram_len, 0xFF);
    for (int y = 0; y < 240; y++) {
        fprintf(f, "frame %3d:", y);
        for (int x = 0; x < 256; x++) fprintf(f, " %03X", frame_index[y * 256 + x]);
        fputc('\n', f);
    }
}

void cyc_trace_instruction(uint16_t pc, uint8_t a, uint8_t x, uint8_t y, uint8_t s, uint8_t p) {
    cyc_trace_hash = cyc_trace_mix(cyc_trace_hash, 0x1000000000000ull | (uint64_t)pc << 32 |
                                                       (uint64_t)a << 24 | (uint64_t)x << 16 |
                                                       (uint64_t)y << 8 | s);
    cyc_trace_hash = cyc_trace_mix(cyc_trace_hash, p);
    if (cyc_trace_file)
        fprintf((FILE *)cyc_trace_file, "%u INSN PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X\n", cyc_trace_cycle, pc, a,
                x, y, s, p);
}

void cyc_trace_access(uint16_t addr, uint8_t value, bool write) {
    if (have_access) return;
    have_access = true;
    access_addr = addr;
    access_value = value;
    access_write = write;
}

void cyc_trace_dma(bool halt) {
    is_dma = true;
    is_halt = halt;
}

void (*cyc_trace_cycle_hook)(const CycTraceCycle *cycle);

void cyc_trace_cycle_end(bool instruction_done, uint8_t data_bus) {
    char kind;
    uint64_t rec;
    if (cyc_trace_cycle_hook) {
        CycTraceCycle c = {cyc_trace_cycle, access_addr, access_value, data_bus,
                           !have_access ? '-' : access_write ? 'W' : 'R', is_dma, is_halt, instruction_done};
        cyc_trace_cycle_hook(&c);
    }
    if (is_dma && is_halt) {
        kind = 'H';
        rec = 0x2000000000000ull;
    } else {
        kind = !have_access ? '-' : access_write ? 'W' : 'R';
        if (is_dma) kind = (char)(kind + ('a' - 'A'));
        rec = (uint64_t)(unsigned char)kind << 40 | (uint64_t)access_addr << 16 | (uint64_t)access_value << 8 |
              data_bus;
        if (!is_dma) rec |= (uint64_t)instruction_done << 48;
    }
    cyc_trace_hash = cyc_trace_mix(cyc_trace_hash, rec);
    if (cyc_trace_file) {
        if (kind == 'H') /* the address is printed for diagnosis but not hashed */
            fprintf((FILE *)cyc_trace_file, "%u H (%04X)\n", cyc_trace_cycle, access_addr);
        else
            fprintf((FILE *)cyc_trace_file, "%u %c %04X %02X db=%02X%s\n", cyc_trace_cycle, kind, access_addr,
                    access_value, data_bus, !is_dma && instruction_done ? " done" : "");
    }
    cyc_trace_cycle++;
    have_access = is_dma = is_halt = false;
    access_addr = 0;
    access_value = 0;
}
