/*
 * cyc_mm4_donor.c - the player's Mega Man 4 (USA) image.
 *
 * The backports borrow Mega Man 4's graphics, constants and sound scripts.
 * None of it is in this repository or in a release: the player selects their
 * own image in the Mods screen (manifest [[external_rom]], verified there by
 * normalized SHA-1), and the plugin re-reads it here at activation, checks
 * the payload CRC32 again, and keeps the PRG in memory for the run.  There is
 * no cache on disk.  Same contract as the Mega Man X mods
 * (snesrecomp MegamanXRecomp/docs/mod-source-roms.md).
 */
#include "cyc_mm4_donor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CycMm4Donor cyc_mm4_donor;
static uint8_t *s_prg;

/* payload CRC32 of Mega Man 4 (USA): 2BC67AA8 (iNES file CRC32 ADBD4E48) */
#define MM4_USA_PAYLOAD_CRC32 0x2BC67AA8u

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void fail(char *why, size_t n, const char *msg) {
    if (why && n) snprintf(why, n, "%s", msg);
}

void cyc_mm4_donor_unload(void) {
    free(s_prg);
    s_prg = NULL;
    cyc_mm4_donor.prg = NULL;
}

bool cyc_mm4_donor_load(const char *path, char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!path || !*path) { fail(why, why_size, "Select your Mega Man 4 (USA) ROM in Mods"); return false; }
    FILE *f = fopen(path, "rb");
    if (!f) { fail(why, why_size, "cannot open the Mega Man 4 ROM"); return false; }
    uint8_t header[16];
    bool ok = fread(header, 1, 16, f) == 16 && !memcmp(header, "NES\x1A", 4);
    long payload = 0;
    if (ok) {
        /* iNES 1.0: PRG 16 KiB units in byte 4, CHR 8 KiB in byte 5; trainer bit 2 of byte 6 */
        payload = (long)header[4] * 0x4000 + (long)header[5] * 0x2000;
        ok = (header[6] & 4) == 0 && payload == CYC_MM4_PAYLOAD_SIZE;
    }
    if (!ok) {
        fclose(f);
        fail(why, why_size, "that is not an iNES Mega Man 4 (512 KiB PRG, CHR-RAM) image");
        return false;
    }
    uint8_t *buf = (uint8_t *)malloc(CYC_MM4_PAYLOAD_SIZE);
    if (!buf || fread(buf, 1, CYC_MM4_PAYLOAD_SIZE, f) != CYC_MM4_PAYLOAD_SIZE) {
        free(buf);
        fclose(f);
        fail(why, why_size, "cannot read the Mega Man 4 ROM");
        return false;
    }
    fclose(f);
    if (crc32_of(buf, CYC_MM4_PAYLOAD_SIZE) != MM4_USA_PAYLOAD_CRC32) {
        free(buf);
        fail(why, why_size, "that is not the Mega Man 4 (USA) revision (payload CRC32 2BC67AA8)");
        return false;
    }
    if (s_prg) { free(buf); return true; } /* resident bytes stay stable, after revalidation */
    s_prg = buf;
    cyc_mm4_donor.prg = buf;
    return true;
}
