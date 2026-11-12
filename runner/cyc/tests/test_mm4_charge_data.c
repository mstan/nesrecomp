/* Verified local MM4 fixture only; no source image or decoded art is bundled. */
#include "cyc_mm4_charge_data.h"
#include "cyc_mm4_donor.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    CycMm4ChargeData data, before;
    memset(&data, 0x5A, sizeof data); before = data;
    if (cyc_mm4_charge_data_load(&data) || memcmp(&data, &before, sizeof data)) return 1;
    char why[160];
    if (!cyc_mm4_donor_load(argv[1], why, sizeof why)) { fprintf(stderr, "%s\n", why); return 2; }
    if (cyc_mm4_charge_data_load(NULL) || !cyc_mm4_charge_data_load(&data)) return 1;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < sizeof data; i++) hash = (hash ^ ((const uint8_t *)&data)[i]) * 16777619u;
    printf("decoded bytes=%u FNV1a=%08X\n", (unsigned)sizeof data, (unsigned)hash);
    /* Actual final MM2 load_data, both facings, plus native MM4 palette init. */
    if (sizeof data != 531 || hash != 0xB147E05Au) return 1;
    cyc_mm4_donor_unload(); before = data;
    return cyc_mm4_charge_data_load(&data) || memcmp(&data, &before, sizeof data);
}
