/* nes_fds_boot.h - the Famicom Disk System BIOS's boot load, read off a side
 * the way the BIOS reads it, for the boot skip (runner/cyc/cyc_fds_skip.c).
 *
 * The boot skip replaces the BIOS's LoadFiles call at boot (disksys.rom $EF59
 * JSR $E1F8 with the inline disk ID and file list $EFF5/$EFF5) by loading the
 * files itself. This header is the pure half: it walks one side's drive
 * stream as the drive and LoadFiles see it, decides what LoadFiles would do
 * with every file, and says why an image is outside what the skip reproduces
 * exactly (then the skip is refused and the BIOS loads the disk itself).
 *
 * How LoadFiles reads (disksys.rom, CRC32 5E607DCF; addresses are its):
 *   The drive: after a gap of zero bytes, the first nonzero byte ends the gap
 *   and is not transferred; every byte after it is, until the BIOS turns CRC
 *   mode off after the block's two CRC bytes (hw_fds.c clock_byte).
 *   $E445 disk-ID check: block 1 (code 1, "*NINTENDO-HVC*" at bytes 1-14,
 *     $E6E3-$E705), then bytes 15-24 against the 10-byte ID ($FF = any,
 *     $E44E-$E471), byte 25 = the boot file code into $08 ($E473), 30 more.
 *   $E484 block 2: the file amount into $06.
 *   $E224-$E231: exactly that many header/data pairs, never more (hidden files
 *     are never read).
 *   $E4A0 block 3: number, ID; the file is requested when the list's first
 *     byte is $FF and ID <= boot code, or its ID is in the list (up to 20
 *     entries, $FF-terminated).
 *   $E4F9: load address, size, type; $E51A-$E531 for type 0 (CPU memory) a
 *     requested file is still skipped when address + size - 1 passes $FFFF,
 *     or when the address is below $2000 with (high byte & 7) < 2 ($0000-$01FF
 *     and its mirrors: the BIOS's variables and stack); type != 0 goes through
 *     $2006/$2007 to the PPU with rendering off ($E549-$E570), no checks.
 *   A skipped file's data is read and dropped.
 *   CRCs: $E706 checks each block's CRC (ERR.27) as the drive reports it
 *     ($4030.4); hw_fds.c reports mismatches only with CRC checking on.
 *
 * What the skip does not reproduce, and so refuses (the BIOS then loads the
 * disk itself, as with the skip off):
 *   - anything the BIOS would stop on: no disk header, the wrong disk or side,
 *     a missing block, a bad CRC with CRC checking on;
 *   - a requested CPU file that writes $2000-$5FFF (PPU, APU and RAM Adapter
 *     registers: the writes have side effects mid-load) or reaches $0000-$01FF
 *     through the RAM mirrors (the BIOS's own variables and stack, under it);
 *   - a zero-length requested file (the BIOS's counter wraps: 65536 bytes);
 *   - a gap too short for the BIOS to re-arm between blocks, or a lead-in too
 *     short for its spin-up wait;
 *   - a boot whose last block ends so early that the head is still turning
 *     when the game starts (the BIOS's license screen runs about 319 frames,
 *     63,000 byte periods; every side the loaders build ends inside that).
 *
 * Header-only C11, no machine state: unit-tested directly
 * (runner/cyc/fds_hle_plan_test.c). */
#ifndef NES_FDS_BOOT_H
#define NES_FDS_BOOT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nes_fds.h"

#define NES_FDS_BOOT_MAX_FILES 256u
/* Bytes the drive clocks between the end of the boot load and the game's
 * start that are certain to pass: the license screen's 319 frames at
 * 29780 CPU cycles per frame and 150 cycles per byte, rounded down. */
#define NES_FDS_BOOT_TAIL_BYTES 63000u
/* The BIOS re-arms the gap search (E68F: $E153 with Y=5, ~6400 cycles) about
 * 43 byte periods after a block; a gap must still be running then. */
#define NES_FDS_BOOT_MIN_GAP 64u
/* Motor on to the first read of block 1 (E6E3: spin-up and two waits). */
#define NES_FDS_BOOT_MIN_LEAD_IN 1000u

typedef enum {
    NES_FDS_BOOT_LOAD,         /* requested and written */
    NES_FDS_BOOT_NOT_ASKED,    /* not requested (ID above the boot code / not in the list) */
    NES_FDS_BOOT_SKIP_LOW,     /* requested CPU file at $0000-$01FF or a mirror: dropped */
    NES_FDS_BOOT_SKIP_WRAP,    /* requested CPU file past $FFFF: dropped */
} NesFdsBootHow;

typedef struct {
    uint8_t  number, id, type, how;
    uint8_t  name[8];
    uint16_t addr, size;
    uint32_t header_pos;       /* stream position of block 3's code */
    uint32_t data_pos;         /* of the first data byte (after block 4's code) */
    uint32_t end_pos;          /* after block 4's second CRC byte */
    bool     header_crc_ok, data_crc_ok;
} NesFdsBootFile;

/* Refusal codes (why != NULL); also the ring's fds.boot refuse detail. */
enum {
    NES_FDS_BOOT_OK = 0,
    NES_FDS_BOOT_WHY_NO_HEADER, NES_FDS_BOOT_WHY_SIGNATURE, NES_FDS_BOOT_WHY_ID, NES_FDS_BOOT_WHY_BLOCK,
    NES_FDS_BOOT_WHY_CRC, NES_FDS_BOOT_WHY_IO, NES_FDS_BOOT_WHY_LOW, NES_FDS_BOOT_WHY_EMPTY,
    NES_FDS_BOOT_WHY_GAP, NES_FDS_BOOT_WHY_TURNING,
};

typedef struct {
    uint8_t  block1[56];
    uint8_t  boot_code, amount;
    unsigned files;                          /* header/data pairs read (= amount when the boot is reproducible) */
    NesFdsBootFile file[NES_FDS_BOOT_MAX_FILES];
    unsigned requested;                      /* files asked for, dropped ones included ($0E) */
    unsigned loaded;                         /* files written */
    uint32_t end_pos;                        /* after the last block's second CRC byte */
    uint8_t  last_byte;                      /* the last byte the drive transferred (that CRC byte) */
    unsigned why_code, why_file;             /* NES_FDS_BOOT_WHY_*, the file it concerns */
    const char *why;                         /* NULL: the skip reproduces this boot */
} NesFdsBoot;

/* A block as the drive delivers it from pos: gap zeros, the byte ending the
 * gap, then n block bytes and 2 CRC bytes. False if the stream ends first. */
typedef struct {
    uint32_t gap, mark_pos, pos;             /* zeros skipped, where the gap ended, first block byte */
    bool     crc_ok;
} NesFdsBootBlock;

static inline bool nes_fds_boot_block(const uint8_t *s, uint32_t len, uint32_t at, uint32_t n, NesFdsBootBlock *b)
{
    uint32_t p = at;
    while (p < len && s[p] == 0) ++p;
    if (p >= len || (uint64_t)p + 1 + n + 2 > len) return false;
    b->gap = p - at;
    b->mark_pos = p;
    b->pos = p + 1;
    uint16_t crc = nes_fds_crc16(0, s + p, 1 + n);          /* mark + block (common/nes_fds.h) */
    b->crc_ok = (uint8_t)crc == s[p + 1 + n] && (uint8_t)(crc >> 8) == s[p + 2 + n];
    return true;
}

static inline bool nes_fds_boot_refuse(NesFdsBoot *o, unsigned code, unsigned file, const char *why)
{
    o->why_code = code;
    o->why_file = file;
    o->why = why;
    return false;
}

/* Read the boot load from a side stream. id: the 10 bytes the boot asks for
 * (disksys: FF FF FF FF FF FF 00 00 FF FF = disk 1 side A); list: the file
 * list (list[0] == $FF: every file with ID <= the boot code). crc_checked:
 * the drive reports CRC mismatches. True when the skip reproduces the load;
 * otherwise o->why says why not (o->files etc. describe what was read). */
static inline bool nes_fds_boot_read(const uint8_t *s, uint32_t len, const uint8_t id[10], const uint8_t list[20],
                                     bool crc_checked, NesFdsBoot *o)
{
    memset(o, 0, sizeof(*o));
    NesFdsBootBlock b;
    uint32_t p = 0;
    if (!nes_fds_boot_block(s, len, 0, 56, &b) || s[b.pos] != 1)
        return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_NO_HEADER, 0, "the side has no disk header (block 1)");
    if (b.gap < NES_FDS_BOOT_MIN_LEAD_IN || s[b.mark_pos] != NES_FDS_GAP_MARK)
        return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_GAP, 0, "the lead-in is shorter than the BIOS's spin-up wait");
    memcpy(o->block1, s + b.pos, 56);
    if (memcmp(o->block1 + 1, "*NINTENDO-HVC*", 14))
        return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_SIGNATURE, 0, "the disk header lacks *NINTENDO-HVC*");
    for (unsigned i = 0; i < 10; ++i)
        if (id[i] != 0xFF && id[i] != o->block1[15 + i])
            return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_ID, 0, "the side in the drive is not the disk the boot asks for");
    if (crc_checked && !b.crc_ok) return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_CRC, 0, "block 1 has a bad CRC");
    o->boot_code = o->block1[25];
    p = b.pos + 56 + 2;
    if (!nes_fds_boot_block(s, len, p, 2, &b) || s[b.pos] != 2)
        return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_BLOCK, 0, "block 2 (the file amount) is missing");
    if (b.gap < NES_FDS_BOOT_MIN_GAP || s[b.mark_pos] != NES_FDS_GAP_MARK)
        return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_GAP, 0, "the gap before block 2 is too short for the BIOS");
    if (crc_checked && !b.crc_ok) return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_CRC, 0, "block 2 has a bad CRC");
    o->amount = s[b.pos + 1];
    p = b.pos + 2 + 2;
    o->last_byte = s[p - 1];
    for (unsigned k = 0; k < o->amount; ++k) {
        NesFdsBootFile *f = &o->file[k];
        if (!nes_fds_boot_block(s, len, p, 16, &b) || s[b.pos] != 3)
            return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_BLOCK, k, "a file header (block 3) is missing");
        if (b.gap < NES_FDS_BOOT_MIN_GAP || s[b.mark_pos] != NES_FDS_GAP_MARK)
            return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_GAP, k, "a gap before a file header is too short for the BIOS");
        if (crc_checked && !b.crc_ok) return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_CRC, k, "a file header has a bad CRC");
        const uint8_t *h = s + b.pos;
        f->header_pos = b.pos;
        f->header_crc_ok = b.crc_ok;
        f->number = h[1];
        f->id = h[2];
        memcpy(f->name, h + 3, 8);
        f->addr = (uint16_t)(h[11] | h[12] << 8);
        f->size = (uint16_t)(h[13] | h[14] << 8);
        f->type = h[15];
        p = b.pos + 16 + 2;
        if (!nes_fds_boot_block(s, len, p, 1u + f->size, &b) || s[b.pos] != 4)
            return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_BLOCK, k, "a file's data (block 4) is missing");
        if (b.gap < NES_FDS_BOOT_MIN_GAP || s[b.mark_pos] != NES_FDS_GAP_MARK)
            return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_GAP, k, "a gap before a file's data is too short for the BIOS");
        f->data_pos = b.pos + 1;
        f->data_crc_ok = b.crc_ok;
        f->end_pos = b.pos + 1 + f->size + 2;
        p = f->end_pos;
        o->files = k + 1;
        /* $E4AC-$E4D2: requested? */
        bool asked = false;
        if (list[0] == 0xFF) asked = f->id <= o->boot_code;
        else
            for (unsigned i = 0; i < 20 && list[i] != 0xFF; ++i)
                if (list[i] == f->id) { asked = true; break; }
        if (!asked) { f->how = NES_FDS_BOOT_NOT_ASKED; continue; }
        if (f->type == 0) {
            /* $E51A-$E531 */
            if ((uint32_t)f->addr + f->size - 1u > 0xFFFFu || !f->size) f->how = NES_FDS_BOOT_SKIP_WRAP;
            else if (f->addr < 0x2000 && ((f->addr >> 8) & 7) < 2) f->how = NES_FDS_BOOT_SKIP_LOW;
            else f->how = NES_FDS_BOOT_LOAD;
            if (!f->size)
                return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_EMPTY, k, "a boot file is empty (the BIOS reads 65536 bytes)");
            if (f->how == NES_FDS_BOOT_LOAD) {
                uint32_t a = f->addr, e = (uint32_t)f->addr + f->size;        /* [a, e) */
                if (a < 0x6000 && e > 0x2000)
                    return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_IO, k, "a boot file writes $2000-$5FFF (registers)");
                for (uint32_t m = 0; m < 0x2000; m += 0x800)
                    if (a < m + 0x200 && e > m)
                        return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_LOW, k,
                                                   "a boot file reaches $0000-$01FF (the BIOS's variables and stack)");
            }
        } else {
            if (!f->size)
                return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_EMPTY, k, "a boot file is empty (the BIOS reads 65536 bytes)");
            f->how = NES_FDS_BOOT_LOAD;
        }
        /* $E572: a requested file's data CRC is checked (also when its
         * address made LoadFiles drop the bytes); an unrequested one's is not. */
        if (crc_checked && !b.crc_ok) return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_CRC, k, "a boot file has a bad CRC");
        o->requested++;
        if (f->how == NES_FDS_BOOT_LOAD) o->loaded++;
    }
    o->end_pos = p;
    o->last_byte = s[p - 1];
    if ((uint64_t)o->end_pos + NES_FDS_BOOT_TAIL_BYTES < len)
        return nes_fds_boot_refuse(o, NES_FDS_BOOT_WHY_TURNING, 0,
                                   "the drive would still be reading when the game starts (the boot load ends early "
                                   "on a long side)");
    return true;
}

/* ---- proofs ----
 *
 * The skip is granted only for a disk whose skipped boot has been shown
 * equivalent to the BIOS's boot of it (tools/cyc/fds_boot_equiv.py: the whole
 * machine at the game's first instruction, skip against BIOS, every
 * difference in a documented class; runner/cyc/README.md "Boot skip").
 * The disk is named by the CRC-32 of side 0's bytes as the image holds them
 * (a headered image and its raw twin share it). Proofs are built in here, or
 * listed in the BIOS's identity file (hle_boot_proven = ["0x...", ...], as
 * the synthetic test BIOSes do). */
static inline uint32_t nes_fds_boot_disk_id(const NesFdsImage *img)
{
    if (!img->sides) return 0;
    size_t off = img->data_offset, n = img->side_bytes;
    if (off >= img->size) return 0;
    if (n > img->size - off) n = img->size - off;
    return nes_crc32(0, img->image + off, n);
}

static inline bool nes_fds_boot_proven(uint32_t disk_id)
{
    switch (disk_id) {
    case 0xF04CD4CDu:   /* Super Mario Bros. 2 (Japan) (Debug Value 0): 2026-09-28, 4 alignments */
        return true;
    default:
        return false;
    }
}

static inline const char *nes_fds_boot_how_name(unsigned how)
{
    static const char *const N[] = { "loaded", "not-boot", "skipped-address", "skipped-range" };
    return how < 4 ? N[how] : "?";
}

#endif /* NES_FDS_BOOT_H */
