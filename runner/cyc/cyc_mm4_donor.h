/* Verified runtime-only MM4 data shared by title-owned ability adapters. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { CYC_MM4_PAYLOAD_SIZE = 0x80000 };
typedef struct { const uint8_t *prg; } CycMm4Donor;
extern CycMm4Donor cyc_mm4_donor;
/* Verify the supplied path on every call, even when a resident image exists.
 * Failure leaves the previous verified image intact and MUST refuse the new
 * activation/admission. Nothing is cached on disk or sent to another player. */
bool cyc_mm4_donor_load(const char *path, char *why, size_t why_size);
void cyc_mm4_donor_unload(void);
static inline uint32_t cyc_mm4_off(unsigned bank, unsigned address)
{ return bank * 0x2000u + (address & 0x1FFFu); }
static inline uint8_t cyc_mm4_byte(unsigned bank, unsigned address)
{ return cyc_mm4_donor.prg && bank < 64 ? cyc_mm4_donor.prg[cyc_mm4_off(bank, address)] : 0; }
#ifdef __cplusplus
}
#endif
