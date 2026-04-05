/*
 * main_nes.c — NESRecomp entry point
 * Usage: NESRecomp.exe <rom.nes> [--game <path/to/game.cfg>]
 * Output: generated/<prefix>_full.c + generated/<prefix>_dispatch.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rom_parser.h"
#include "function_finder.h"
#include "code_generator.h"
#include "annotations.h"
#include "game_config.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: NESRecomp <rom.nes> [--game <path/to/game.cfg>]\n");
        return 1;
    }

    const char *rom_path  = argv[1];
    const char *game_path = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--game") == 0 && i+1 < argc) game_path = argv[++i];
    }

    /* Auto-detect game.toml / game.cfg in current directory if not specified */
    if (!game_path) {
        FILE *f = fopen("game.toml", "r");
        if (f) { fclose(f); game_path = "game.toml"; }
    }
    if (!game_path) {
        FILE *f = fopen("game.cfg", "r");
        if (f) { fclose(f); game_path = "game.cfg"; }
    }

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

    /* Load game config */
    GameConfig cfg = {0};
    if (game_path) {
        if (game_config_load(&cfg, game_path))
            printf("[NESRecomp] Game config: %s  (prefix='%s', %d trampolines, "
                   "%d known tables, %d split tables, %d extra funcs)\n",
                   game_path, cfg.output_prefix,
                   cfg.trampoline_count, cfg.known_table_count,
                   cfg.known_split_table_count, cfg.extra_func_count);
        else
            fprintf(stderr, "[NESRecomp] Warning: could not load game config '%s'\n", game_path);
    } else {
        game_config_init_empty(&cfg);
        printf("[NESRecomp] No --game config; using empty dispatch tables\n");
    }

    /* Determine output prefix: from config, or derived from ROM basename */
    char output_prefix[128];
    if (cfg.output_prefix[0]) {
        snprintf(output_prefix, sizeof(output_prefix), "%s", cfg.output_prefix);
    } else {
        /* Derive from ROM filename without path or extension */
        const char *base = rom_path;
        const char *s = rom_path;
        while (*s) { if (*s == '/' || *s == '\\') base = s+1; s++; }
        size_t len = strlen(base);
        const char *dot = strrchr(base, '.');
        if (dot) len = (size_t)(dot - base);
        if (len >= sizeof(output_prefix)) len = sizeof(output_prefix) - 1;
        memcpy(output_prefix, base, len);
        output_prefix[len] = '\0';
        /* Replace spaces with underscores */
        for (char *p = output_prefix; *p; p++) if (*p == ' ') *p = '_';
    }

    /* Load annotations sidecar */
    AnnotationTable at = {0};
    {
        char ann_path[512];
        if (cfg.annotations_path[0]) {
            /* game config provided the annotations path */
            snprintf(ann_path, sizeof(ann_path), "%s", cfg.annotations_path);
        } else {
            /* Fall back to <rompath_without_extension>_annotations.csv */
            const char *dot = strrchr(rom_path, '.');
            if (dot) {
                size_t n = (size_t)(dot - rom_path);
                if (n >= sizeof(ann_path) - 20) n = sizeof(ann_path) - 20;
                memcpy(ann_path, rom_path, n);
                strcpy(ann_path + n, "_annotations.csv");
            } else {
                snprintf(ann_path, sizeof(ann_path), "%s_annotations.csv", rom_path);
            }
        }
        if (annotations_load(&at, ann_path))
            printf("[NESRecomp] Annotations: %d entries from %s\n", at.count, ann_path);
    }

    /* Find all functions via JSR/RTS graph walk */
    static FunctionList funcs = {0};
    function_finder_run(&rom, &funcs, &cfg);
    printf("[NESRecomp] Found %d functions\n", funcs.count);

    /* Emit C */
    char out_full[256], out_dispatch[256];
    snprintf(out_full,     sizeof(out_full),     "generated/%s_full.c",     output_prefix);
    snprintf(out_dispatch, sizeof(out_dispatch), "generated/%s_dispatch.c", output_prefix);

    if (!codegen_emit(&rom, &funcs, out_full, out_dispatch, &at, &cfg)) {
        fprintf(stderr, "[NESRecomp] Code generation failed\n");
        rom_free(&rom);
        function_list_free(&funcs);
        return 1;
    }

    printf("[NESRecomp] Done. Output:\n  %s\n  %s\n", out_full, out_dispatch);

    rom_free(&rom);
    function_list_free(&funcs);
    annotations_free(&at);
    return 0;
}
