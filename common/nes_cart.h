/* Cartridge file metadata, shared by the compiler and both machines.
 * https://www.nesdev.org/wiki/NES_2.0
 * This is file decoding only; mapper behavior remains independently modeled. */
#ifndef NES_CART_H
#define NES_CART_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t prg_size, chr_size, prg_ram, prg_nvram, chr_ram, chr_nvram;
    uint16_t mapper;
    uint8_t submapper, nes2, four_screen, vertical, battery, trainer;
    uint8_t timing, console;
    size_t data_offset;
} NesCartInfo;

static inline bool nes_rom_size(uint8_t lo, uint8_t hi, uint32_t unit, uint32_t *out)
{
    uint64_t n;
    if (hi == 15) {
        unsigned exponent = lo >> 2;
        if (exponent > 26) return false;
        n = (UINT64_C(1) << exponent) * ((lo & 3) * 2 + 1);
    } else n = ((uint32_t)hi * 256 + lo) * (uint64_t)unit;
    /* Bound allocations and bank arithmetic; no supported board needs >64 MiB. */
    if (n > 0x4000000) return false;
    *out = (uint32_t)n;
    return true;
}

static inline uint32_t nes_ram_size(unsigned shift)
{
    return shift ? 64u << shift : 0;
}

static inline bool nes_cart_header(const uint8_t *h, size_t size, NesCartInfo *c)
{
    if (!h || size < 16 || memcmp(h, "NES\x1a", 4)) return false;
    memset(c, 0, sizeof(*c));
    c->nes2 = (h[7] & 12) == 8;
    c->mapper = (h[6] >> 4) | (h[7] & 240);
    c->four_screen = (h[6] >> 3) & 1;
    c->trainer = (h[6] >> 2) & 1;
    c->battery = (h[6] >> 1) & 1;
    c->vertical = h[6] & 1;
    c->console = h[7] & 3;
    c->data_offset = 16 + (c->trainer ? 512 : 0);
    if (c->nes2) {
        c->mapper |= (h[8] & 15) << 8;
        c->submapper = h[8] >> 4;
        if (!nes_rom_size(h[4], h[9] & 15, 16384, &c->prg_size) ||
            !nes_rom_size(h[5], h[9] >> 4, 8192, &c->chr_size)) return false;
        c->prg_ram = nes_ram_size(h[10] & 15);
        c->prg_nvram = nes_ram_size(h[10] >> 4);
        c->chr_ram = nes_ram_size(h[11] & 15);
        c->chr_nvram = nes_ram_size(h[11] >> 4);
        c->timing = h[12] & 3;
    } else {
        c->prg_size = (uint32_t)h[4] * 16384;
        c->chr_size = (uint32_t)h[5] * 8192;
        /* iNES leaves RAM ambiguous. Retain established per-board defaults. */
        bool wram = c->mapper == 1 || c->mapper == 4 || c->mapper == 5 ||
                    c->mapper == 10 || c->mapper == 21 || c->mapper == 23 || c->mapper == 25 || c->mapper == 73 ||
                    c->mapper == 24 || c->mapper == 26 || c->mapper == 85 ||
                    (c->mapper == 34 && c->chr_size > 8192);
        uint32_t ram = h[8] ? (uint32_t)h[8] * 8192 : wram ? 8192 : 0;
        /* Legacy MMC5 headers cannot identify EKROM/ETROM/EWROM. A 64K
         * compatibility allocation covers both chip selects; NES 2.0 gives
         * the actual geometry and consequently the actual open-bus holes. */
        if (c->mapper==5 && !h[8]) ram=65536;
        /* Old headers do not distinguish SNROM/SOROM/SXROM. Reserve all
         * four banks for compatibility; NES 2.0 supplies actual wiring. */
        if (c->mapper==1 && !h[8]) ram=32768;
        if (c->battery) c->prg_nvram = ram; else c->prg_ram = ram;
        if (c->mapper == 157) { c->prg_ram=0; c->prg_nvram=c->battery?128:0; }
        if (c->mapper == 153) { c->prg_ram=0; c->prg_nvram=8192; }
        if (c->mapper == 159) { c->prg_ram=0; c->prg_nvram=128; }
        if (c->mapper == 16) { c->prg_ram=0; c->prg_nvram=c->battery?256:0; }
        if (!c->chr_size) c->chr_ram = c->mapper == 13 ? 16384 : 8192;
    }
    return c->prg_size >= 4096;
}

static inline bool nes_cart_image(const uint8_t *image, size_t size, NesCartInfo *c)
{
    if (!nes_cart_header(image, size, c) || c->data_offset > size) return false;
    size -= c->data_offset;
    if (c->prg_size > size) return false;
    return c->chr_size <= size - c->prg_size;
}

/* Metadata combinations implemented by the cycle runtime. Unknown variants
 * must not silently acquire the behavior of submapper zero. */
static inline bool nes_cart_variant_supported(const NesCartInfo *c)
{
    if (c->console || (c->nes2 && c->timing != 0 && c->timing != 2)) return false;
    if (c->prg_ram + c->prg_nvram > 0x20000 || c->chr_ram + c->chr_nvram > 0x100000)
        return false;
    if (c->chr_size && (c->chr_ram || c->chr_nvram)) return false;
    if (!c->chr_size && !c->chr_ram && !c->chr_nvram) return false;
    switch (c->mapper) {
    case 5: {
        uint32_t ram=c->prg_ram+c->prg_nvram;
        bool dual=c->prg_ram && c->prg_nvram;
        bool chips=dual ? (c->prg_ram==8192 || c->prg_ram==32768) &&
                         (c->prg_nvram==8192 || c->prg_nvram==32768) :
            (!ram || ram==8192 || ram==16384 || ram==32768 || ram==65536 || ram==131072);
        return !c->submapper && !c->four_screen && c->prg_size<=1048576 && c->chr_size<=1048576 && chips;
    }
    case 157: return !c->submapper && !c->prg_ram && (c->prg_nvram==0 || c->prg_nvram==128) &&
        !c->chr_size && c->chr_ram==8192 && !c->chr_nvram && c->prg_size<=262144;
    case 153: return !c->submapper && !c->prg_ram && c->prg_nvram==8192 &&
        !c->chr_size && c->chr_ram==8192 && !c->chr_nvram && c->prg_size<=524288;
    case 159: return !c->submapper && !c->prg_ram && c->prg_nvram==128 && c->prg_size<=262144 && c->chr_size<=262144;
    case 16: return (c->submapper==0 || c->submapper==4 || c->submapper==5) &&
        !c->prg_ram && (c->prg_nvram==0 || (c->submapper!=4 && c->prg_nvram==256)) &&
        c->prg_size<=262144 && c->chr_size<=262144;
    case 85: return c->submapper <= 2;
    case 21: return c->submapper <= 2;
    case 23: case 25: return c->submapper <= 3;
    case 1: {
        uint32_t ram=c->prg_ram+c->prg_nvram, chr=c->chr_size+c->chr_ram+c->chr_nvram;
        if (c->four_screen || c->prg_size>524288 || chr>131072 || ram>32768) return false;
        if (c->prg_size>262144 && chr>8192) return false;
        if (c->nes2 && ram>8192 && chr>8192 && !(ram==16384 && chr<=65536)) return false;
        switch (c->submapper) {
        case 0: case 7: return true;
        case 1: return c->prg_size==524288 && chr==8192 && ram==8192;
        case 2: return c->prg_size<=262144 && chr==8192 && ram==16384;
        case 4: return chr==8192 && ram==32768;
        case 5: return c->prg_size==32768;
        default: return false;
        }
    }
    case 2: case 3: case 7: case 34: return c->submapper <= 2;
    case 71: case 206: case 232: return c->submapper <= 1;
    default: return c->submapper == 0;
    }
}

static inline bool nes_cart_nina(const NesCartInfo *c)
{
    return c->submapper == 1 || (!c->submapper && c->chr_size > 8192);
}

/* Generated PRG code can fold fixed-bank reads. Matching PRG bytes alone is
 * insufficient when the same bytes are loaded with different board wiring. */
static inline uint32_t nes_cart_identity(const NesCartInfo *c)
{
    uint32_t fields[] = { c->prg_size, c->chr_size, c->prg_ram, c->prg_nvram,
        c->chr_ram, c->chr_nvram, c->mapper, c->submapper, c->nes2,
        c->four_screen, c->vertical, c->battery, c->trainer, c->timing, c->console };
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof(fields)/sizeof(fields[0]); ++i)
        for (unsigned shift = 0; shift < 32; shift += 8)
            h = (h ^ ((fields[i] >> shift) & 255)) * 16777619u;
    return h;
}
#endif
