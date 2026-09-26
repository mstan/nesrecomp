/* cyc_native_none.c - stands in for a generated <prefix>_cyc.c so the host can
 * run any NROM program on the interpreter alone (cyc_interp). */
#include "cyc_recomp.h"

#include <stddef.h>

const char    *cyc_native_program_name = NULL;  /* no PRG ROM check */
const uint32_t cyc_native_prg_hash = 0;
const uint32_t cyc_native_cart_hash = 0;

bool cyc_native_has(uint16_t addr) {
    (void)addr;
    return false;
}

void cyc_native_run(void) {
}
