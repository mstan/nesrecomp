/* cyc_native_none.c - stands in for a generated <prefix>_cyc.c so the host can
 * run any NROM program on the interpreter alone (cyc_interp). */
#include "cyc_recomp.h"

#include <stddef.h>

const char    *cyc_native_display_name = NULL;
const char    *cyc_native_program_name = NULL;  /* no PRG ROM check */
const uint32_t cyc_native_prg_hash = 0;
const uint32_t cyc_native_cart_hash = 0;
const uint32_t cyc_native_fds_bios_crc32 = 0;
const char    *cyc_native_fds_bios_path = NULL;
const char    *cyc_native_fds_image_path = NULL;
const char    *cyc_native_fds_hle = NULL;
const uint8_t  cyc_native_console = 0;          /* the board's default */
const CycRamView *const cyc_native_ram_views[1] = { NULL };
const uint32_t cyc_native_ram_view_count = 0;
const CycHookSite cyc_native_hook_sites[1] = { { NULL, 0, 0, 0 } };
const uint32_t cyc_native_hook_site_count = 0;

bool cyc_native_has(uint16_t addr) {
    (void)addr;
    return false;
}
#if NESRECOMP_NATIVE_PRG_GUARDS
bool cyc_native_prg_guard_available(void) { return false; }
#endif

void cyc_native_run(void) {
}
bool cyc_native_prg_code_byte(uint32_t offset) {
    (void)offset;
    return false;
}
