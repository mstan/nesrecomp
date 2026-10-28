/* Public mod writes/commits, current-build snapshots and stable MMC1 gates. */
#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_mod.h"
#include "cyc_ramview.h"
#include "cyc_run.h"
#include "cyc_state.h"
#include "hw_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(bool ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

static void serial(uint16_t addr, unsigned value)
{
    for (unsigned i = 0; i < 5; ++i) {
        cpu_write(addr, (uint8_t)((value >> i) & 1), CYC_DONE);
        (void)cpu_read(0, CYC_DONE);
    }
}

int main(int argc, char **argv)
{
    check(argc == 2, "fixture path");
    uint8_t image[16 + 32768];
    FILE *f = fopen(argv[1], "rb"); check(f != NULL, "fixture open");
    check(fread(image, 1, sizeof(image), f) == sizeof(image), "fixture read"); fclose(f);
    for (unsigned align = 0; align < 4; ++align) {
        check(cyc_load_ines(image, sizeof(image)), "SNROM load");
        cyc_power_on((uint8_t)align); cyc_run_power_on();
        for (unsigned i = 0; i < 256; ++i)
            check(cyc_mod_poke((uint16_t)(0x6000 + i), image[16 + 0x2000 + i]), "copy via public poke");
        check(hw_cart_code_ram_supported() && hw_cart_code_ram_ready(), "fixed SNROM profile");
        check(cyc_ramview_find(0x6000) >= 0, "copied image validates");
        CycSnapshot *snapshot = cyc_snapshot_new(); check(snapshot != NULL, "snapshot allocation");
        cyc_snapshot_take(snapshot);
        check(cyc_mod_poke(0x6009, 0x23), "debug/mod code store");
        check(cyc_ram_code_dirty && cyc_ramview_find(0x6000) < 0, "mod invalidation");
        cyc_snapshot_restore(snapshot);
        check(cyc_ramview_find(0x6000) >= 0, "current-build snapshot restores image/validity");
        check(cyc_mod_isolate_begin(), "isolate begin");
        check(cyc_mod_poke(0x6009, 0x23) && cyc_ramview_find(0x6000) < 0, "isolated code store");
        cyc_mod_isolate_end();
        check(cyc_mod_peek(0x6009) == 0x22 && cyc_ramview_find(0x6000) >= 0, "scope restores image/validity");
        CycModRegs regs = {0}; regs.s = 0xFD; regs.p = 4;
        check(cyc_mod_call_commit(0xC300, &regs), "memory-only committed call");
        check(cyc_mod_peek(0x6009) == 0x23 && cyc_ramview_find(0x6000) < 0, "committed delta invalidates restored view");
        check(cyc_mod_poke(0x6009, 0x22) && cyc_ramview_find(0x6000) >= 0, "restored code validates again");
        cpu_write(0x6009, 0x23, CYC_DONE);
        check(cyc_ramview_find(0x6000) < 0, "CPU store invalidates");
        cpu_write(0x6009, 0x22, CYC_DONE);
        check(cyc_ramview_find(0x6000) >= 0, "CPU store revalidates");
        serial(0x8000, 0x1C); serial(0xC000, 0x10);
        hw_cart_ppu_addr(0);
        check(hw_cart.wram_readable && !hw_cart_code_ram_ready() && cyc_ramview_find(0x6000) < 0,
              "A12-low readable alone cannot qualify");
        hw_cart_ppu_addr(0x1000);
        check(!hw_cart.wram_readable && cyc_ramview_find(0x6000) < 0, "A12-high closed gate refuses");
        serial(0xC000, 0); hw_cart_ppu_addr(0);
        check(hw_cart_code_ram_ready() && cyc_ramview_find(0x6000) >= 0, "restored gates reuse valid physical image");
        size_t size = 0; check(cyc_mod_prg_data_rw(&size) != NULL && size == 32768, "writable PRG policy");
        uint8_t payload[32768];
        check(cyc_mod_copy_cart_payload(payload, sizeof(payload)) == sizeof(payload), "copy cart payload");
        payload[0x4400] ^= 1;
        check(cyc_mod_apply_cart_data_payload(payload, sizeof(payload)), "data overlay policy");
        check(hw_prg_modified && hw_prg_data_modified, "PRG overlay active");
        check(cyc_mod_isolate_begin(), "patched-PRG isolate begin");
        uint64_t entries = cyc_ramview_stats.entries;
        check(cyc_mod_call(0x6008, &regs), "native SRAM call under writable PRG policy");
        check(cyc_ramview_stats.entries > entries && cyc_mod_peek(0x31) == 0x22, "guarded SRAM still selected with ROM disabled");
        cyc_mod_isolate_end();
        cyc_snapshot_take(snapshot);
        uint8_t ordinary = cpu_read(0x6009, CYC_POLL | CYC_DONE);
        uint64_t ordinary_hw = cyc_hw_state_hash();
        cyc_snapshot_restore(snapshot);
        uint8_t folded = cpu_read_code_ram(0x6009, 0x22, CYC_POLL | CYC_DONE);
        check(ordinary == folded && ordinary_hw == cyc_hw_state_hash(), "folded SRAM ignores ROM overlay and preserves bus/device clock");
        cyc_snapshot_free(snapshot);
    }
    puts("PASS: code writes, isolated/committed deltas, snapshots, A12 gates and PRG overlays at four alignments");
    return 0;
}
