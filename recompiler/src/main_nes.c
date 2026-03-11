/*
 * main_nes.c — NESRecomp entry point
 * Usage: NESRecomp.exe <baserom.nes>
 * Output: generated/faxanadu_full.c + generated/faxanadu_dispatch.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rom_parser.h"
#include "function_finder.h"
#include "code_generator.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: NESRecomp <baserom.nes>\n");
        return 1;
    }

    const char *rom_path = argv[1];
    printf("[NESRecomp] Loading ROM: %s\n", rom_path);

    /* Parse ROM */
    NESRom rom = {0};
    if (!rom_parse(rom_path, &rom)) {
        fprintf(stderr, "[NESRecomp] Failed to parse ROM\n");
        return 1;
    }

    printf("[NESRecomp] ROM: %d PRG banks x 16KB, Mapper %d\n",
           rom.prg_banks, rom.mapper);
    printf("[NESRecomp] Vectors: NMI=$%04X  RESET=$%04X  IRQ=$%04X\n",
           rom.nmi_vector, rom.reset_vector, rom.irq_vector);

    /* Find all functions via JSR/RTS graph walk */
    static FunctionList funcs = {0};
    function_finder_run(&rom, &funcs);
    printf("[NESRecomp] Found %d functions\n", funcs.count);

    /* Emit C */
    const char *out_full     = "generated/faxanadu_full.c";
    const char *out_dispatch = "generated/faxanadu_dispatch.c";
    if (!codegen_emit(&rom, &funcs, out_full, out_dispatch)) {
        fprintf(stderr, "[NESRecomp] Code generation failed\n");
        rom_free(&rom);
        function_list_free(&funcs);
        return 1;
    }

    printf("[NESRecomp] Done. Output:\n");
    printf("  %s\n", out_full);
    printf("  %s\n", out_dispatch);

    rom_free(&rom);
    function_list_free(&funcs);
    return 0;
}
