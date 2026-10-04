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
        bool wram = c->mapper == 1 || c->mapper == 155 || c->mapper == 4 || c->mapper == 118 || c->mapper == 5 ||
                    c->mapper == 69 || c->mapper == 68 ||
                    ((c->mapper == 19 || c->mapper == 210 || c->mapper == 18) && c->battery) ||
                    c->mapper == 10 || c->mapper == 21 || c->mapper == 23 || c->mapper == 25 || c->mapper == 73 ||
                    c->mapper == 24 || c->mapper == 26 || c->mapper == 85 ||
                    (c->mapper == 34 && c->chr_size > 8192) || (c->mapper==206 && c->battery);
        uint32_t ram = h[8] ? (uint32_t)h[8] * 8192 : wram ? 8192 : 0;
        /* Legacy MMC5 headers cannot identify EKROM/ETROM/EWROM. A 64K
         * compatibility allocation covers both chip selects; NES 2.0 gives
         * the actual geometry and consequently the actual open-bus holes. */
        if (c->mapper==5 && !h[8]) ram=65536;
        /* Old headers do not distinguish SNROM/SOROM/SXROM. Reserve all
         * four banks for compatibility; NES 2.0 supplies actual wiring. */
        if ((c->mapper==1 || c->mapper==155) && !h[8]) ram=32768;
        if (c->battery) c->prg_nvram = ram; else c->prg_ram = ram;
        if (c->mapper == 157) { c->prg_ram=0; c->prg_nvram=c->battery?128:0; }
        if (c->mapper == 153) { c->prg_ram=0; c->prg_nvram=8192; }
        if (c->mapper == 159) { c->prg_ram=0; c->prg_nvram=128; }
        if (c->mapper == 16) { c->prg_ram=0; c->prg_nvram=c->battery?256:0; }
        /* Taito X1-005 / X1-017 RAM is inside the chip: 128 bytes / 5 KiB. */
        if (c->mapper == 80 || c->mapper == 207 || c->mapper == 82) {
            uint32_t chip = c->mapper == 82 ? 5120 : 128;
            c->prg_ram = c->battery ? 0 : chip; c->prg_nvram = c->battery ? chip : 0;
        }
        if (!c->chr_size) c->chr_ram = c->mapper == 13 ? 16384 : c->mapper == 96 ? 32768 : 8192;
        /* Namco 340 has no RAM; its iNES battery bit is how nesdev tells the
         * submapper-0 boards apart (see nes_cart_image). */
        if (c->mapper == 210 && !c->battery) c->prg_ram = c->prg_nvram = 0;
        /* TQROM always pairs its CHR ROM with an 8 KiB CHR RAM chip. */
        if (c->mapper == 119) c->chr_ram = 8192;
        /* Napoleon Senki (mapper 77): CHR ROM at $0000-$07FF, and RAM behind
         * $0800-$2FFF, which gives it four distinct nametables. */
        if (c->mapper == 77) { c->chr_ram = 8192; c->four_screen = 1; }
    }
    return c->prg_size >= 4096;
}

static inline uint32_t nes_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

/* Known dumps whose iNES header omits or misnames the board. Many Namco
 * 175/340 games were dumped as mapper 19 before mapper 210 existed, Taito
 * TC0690 (IRQ) games as TC0190 mapper 33, Fudou Myouou Den (X1-005 with
 * CHR-controlled mirroring) as mapper 80, Bandai one-screen 74161/7432 boards
 * (152) as 70, NAMCOT-3453 (154) as 88, and LZ93D50 + 24C01 and Datach boards as mapper 16. iNES cannot carry mapper 185's CHR-enable submapper,
 * Bandai FCG vs LZ93D50 and its EEPROM, or whether a Konami VRC2/VRC4 board has PRG
 * RAM or only the VRC2 latch (nesdev wiki, INES Mapper 210, 048, 207, 185,
 * 152, 154, 078, 016, 157, 159, VRC2 and VRC4). Keyed by CRC-32 of PRG+CHR; board facts from
 * NewRisingSun's NES 2.0 header database, generated by tools/cyc/known_dumps.py.
 * mirror: 0 keeps the header, 1 horizontal, 2 vertical. sets_ram: the entry
 * replaces the header's PRG RAM/NVRAM sizes and battery bit. four_screen
 * always replaces the header's bit, which old dumps set spuriously. A NES 2.0
 * header is trusted as written, except a submapper-0 mapper 210 or 185. */
typedef struct {
    uint32_t crc; uint16_t mapper, dumped_as; uint8_t submapper, mirror, sets_ram;
    uint16_t prg_ram, prg_nvram; uint8_t four_screen;
} NesKnownDump;
static const NesKnownDump nes_known_dumps[] = {
#include "nes_known_dumps.inc"
};

static inline void nes_cart_known_dump(NesCartInfo *c, const uint8_t *data, size_t len)
{
    bool ambiguous = !c->nes2 || ((c->mapper == 210 || c->mapper == 185) && !c->submapper);
    if (ambiguous) {
        uint32_t crc = nes_crc32(0, data, len);
        for (size_t i = 0; i < sizeof(nes_known_dumps)/sizeof(nes_known_dumps[0]); ++i) {
            const NesKnownDump *k = &nes_known_dumps[i];
            /* Only the header each entry documents: its own mapper or the one it was dumped as. */
            if (k->crc != crc || (c->mapper != k->dumped_as && c->mapper != k->mapper)) continue;
            c->mapper = k->mapper; c->submapper = k->submapper;
            if (k->mirror) c->vertical = k->mirror == 2;
            c->four_screen = k->four_screen != 0;
            if (k->sets_ram) { c->prg_ram = k->prg_ram; c->prg_nvram = k->prg_nvram; c->battery = k->prg_nvram != 0; }
            return;
        }
    }
    /* Mapper 78 without a submapper: iNES headers set the four-screen bit
     * for Holy Diver (submapper 3, H/V mirroring) and clear it for Cosmo
     * Carrier (1, one-screen); neither board has four-screen VRAM (nesdev wiki,
     * INES Mapper 078). */
    if (c->mapper == 78 && !c->submapper && !c->nes2 && c->four_screen) { c->submapper = 3; c->four_screen = 0; }
    /* Taito X1 RAM is on the chip even when a NES 2.0 header omits it. */
    if ((c->mapper == 80 || c->mapper == 207 || c->mapper == 82 || c->mapper == 552) && !c->prg_ram && !c->prg_nvram)
        c->prg_ram = (c->mapper == 82 || c->mapper == 552) ? 5120 : 128;
    /* Unknown submapper-0 mapper 210: 175 if battery-backed, else 340. */
    if (c->mapper == 210 && !c->submapper) c->submapper = c->battery ? 1 : 2;
}

static inline bool nes_cart_image(const uint8_t *image, size_t size, NesCartInfo *c)
{
    if (!nes_cart_header(image, size, c) || c->data_offset > size) return false;
    size -= c->data_offset;
    if (c->prg_size > size) return false;
    if (c->chr_size > size - c->prg_size) return false;
    nes_cart_known_dump(c, image + c->data_offset, (size_t)c->prg_size + c->chr_size);
    return true;
}

/* Metadata combinations implemented by the cycle runtime. Unknown variants
 * must not silently acquire the behavior of submapper zero. */
static inline bool nes_cart_variant_supported(const NesCartInfo *c)
{
    if (c->console || (c->nes2 && c->timing == 3)) return false;
    if (c->prg_ram + c->prg_nvram > 0x20000 || c->chr_ram + c->chr_nvram > 0x100000)
        return false;
    /* TQROM: CHR A16 selects the RAM chip; ROM sees bank bits 0-5 only, and
     * the RAM decodes A10-A12, so exactly 8 KiB. No other board mixes them. */
    if (c->mapper == 119)
        return !c->submapper && !c->four_screen && c->chr_size && c->chr_size <= 65536 &&
               c->chr_ram == 8192 && !c->chr_nvram;
    /* Irem LROG017 (77): 2 KiB CHR ROM banks (4 bits) beside 6 KiB of CHR RAM
     * and four nametables of RAM (nesdev wiki, INES Mapper 077). */
    if (c->mapper == 77)
        return !c->submapper && c->four_screen && c->chr_size && c->chr_size <= 32768 &&
               c->chr_ram == 8192 && !c->chr_nvram && c->prg_size <= 524288;
    if (c->chr_size && (c->chr_ram || c->chr_nvram)) return false;
    if (!c->chr_size && !c->chr_ram && !c->chr_nvram) return false;
    switch (c->mapper) {
    /* The FDS RAM Adapter (common/nes_fds.h): only the board the runtime
     * builds from a BIOS and a disk; an iNES header cannot describe a disk. */
    case 20: return !c->submapper && !c->nes2 && !c->four_screen && !c->battery && !c->trainer &&
        c->prg_size == 8192 && c->chr_ram == 8192 && !c->chr_size && c->prg_ram == 32768 && !c->prg_nvram;
    case 40: return !c->submapper && c->prg_size==65536 && c->chr_size<=8192 &&
        !c->prg_ram && !c->prg_nvram && !c->four_screen;
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
    case 1: case 155: {
        uint32_t ram=c->prg_ram+c->prg_nvram, chr=c->chr_size+c->chr_ram+c->chr_nvram;
        if (c->four_screen || c->prg_size>524288 || chr>131072 || ram>32768) return false;
        if (c->prg_size>262144 && chr>8192) return false;
        if (c->nes2 && ram>8192 && chr>8192 && !(ram==16384 && chr<=65536)) return false;
        switch (c->submapper) {
        case 0: case 3: case 7: return true;
        case 1: return c->prg_size==524288 && chr==8192 && ram==8192;
        case 2: return c->prg_size<=262144 && chr==8192 && ram==16384;
        case 4: return chr==8192 && ram==32768;
        case 5: return c->prg_size==32768;
        default: return false;
        }
    }
    case 2: case 3: case 7: case 34: return c->submapper <= 2;
    case 71: case 206: case 232: return c->submapper <= 1;
    /* CIRAM A10 comes from CHR A17; the board has no four-screen RAM. */
    case 118: return !c->submapper && !c->four_screen;
    /* CNROM with copy protection: submappers 4-7 name the CHR-enabling chip
     * select; 0 uses the nesdev power-on heuristic. One 8 KiB CHR ROM. */
    /* Jaleco SS88006: up to 8 KiB work RAM. */
    /* Irem G-101: submapper 1 is Major League (one-screen, fixed PRG mode). */
    case 32: return c->submapper <= 1 && !c->four_screen;
    /* Mapper 78: 0 and 1 one-screen (Cosmo Carrier), 3 H/V (Holy Diver). */
    case 78: return (c->submapper == 0 || c->submapper == 1 || c->submapper == 3) && !c->four_screen;
    /* Oeka Kids (96): 32 KiB CHR RAM, 32 KiB PRG banks (2 bits). */
    case 96: return !c->submapper && !c->chr_size && c->chr_ram == 32768 && c->prg_size <= 131072;
    case 80: case 207: case 82: case 552:
        return !c->submapper && !c->four_screen && c->prg_ram + c->prg_nvram <= 8192;
    case 18: return !c->submapper && !c->four_screen && c->prg_ram + c->prg_nvram <= 8192;
    case 185: return (c->submapper == 0 || (c->submapper >= 4 && c->submapper <= 7)) &&
        c->chr_size == 8192 && c->prg_size <= 32768 && !c->prg_ram && !c->prg_nvram;
    /* Namco 163: submappers 0-5 (1 is the deprecated battery-backed internal
     * RAM without external RAM; 2 has no expansion sound; 3-5 are mixing
     * levels). 175 has optional RAM; 340 has none. All use CHR ROM. */
    case 19: return c->submapper <= 5 && c->chr_size && !c->four_screen &&
        c->prg_ram + c->prg_nvram <= 8192 && (c->submapper != 1 || !(c->prg_ram + c->prg_nvram));
    case 210: return (c->submapper == 1 || c->submapper == 2) && c->chr_size && !c->four_screen &&
        c->prg_ram + c->prg_nvram <= (c->submapper == 1 ? 8192u : 0u);
    /* Sunsoft-4: CHR ROM nametables; submapper 1 (Nantettatte!! Baseball's
     * licensing timer and external ROM) is not modeled. At most 8 KiB WRAM. */
    case 68: return !c->submapper && !c->four_screen && c->chr_size &&
        c->prg_ram + c->prg_nvram <= 8192;
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
