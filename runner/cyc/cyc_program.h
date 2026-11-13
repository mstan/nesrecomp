/* One statically compiled native program. Optional variants are selected before
 * power-on; generated units retain the ordinary machine/CPU interfaces. */
#pragma once
#include "cyc_recomp.h"

typedef struct {
    const char *name, *display_name;
    uint32_t prg_hash, cart_hash;
    uint32_t fds_bios_crc32;
    const char *fds_bios_path, *fds_image_path, *fds_hle;
    uint8_t console;
    bool (*has)(uint16_t addr);
    void (*run)(void);
    bool (*prg_code_byte)(uint32_t offset);
#if NESRECOMP_NATIVE_PRG_GUARDS
    bool (*prg_guard_available)(void);
#endif
    const CycRamView *const *ram_views;
    uint32_t ram_view_count;
    const CycHookSite *hook_sites;
    uint32_t hook_site_count;
    const char *id;
} CycProgram;

#if CYC_NATIVE_PROGRAMS && !defined(CYC_PROGRAM_UNIT)
#ifdef __cplusplus
extern "C" {
#endif
/* Supplied by the title. Records and their strings/tables live for the process;
 * the catalog is immutable and initialized on the main thread before power-on. */
const CycProgram *cyc_program_catalog(size_t *count);
extern const CycProgram *cyc_program_active;
const CycProgram *cyc_program_initialize(void);
/* A new boot only: stop execution, select the exact loaded program, then power
 * on. An unknown or ambiguous identity fails without changing the selection. */
bool cyc_program_select(uint32_t prg_hash, uint32_t cart_hash);
/* Match without changing the active program or the current machine. */
const CycProgram *cyc_program_match_image(const uint8_t *image, size_t size);
static inline const CycProgram *cyc_program_current(void) {
    return cyc_program_active ? cyc_program_active : cyc_program_initialize();
}
#ifdef __cplusplus
}
#endif
#endif
