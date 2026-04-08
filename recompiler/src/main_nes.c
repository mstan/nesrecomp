/*
 * main_nes.c — NESRecomp entry point
 * Usage: NESRecomp.exe <rom.nes> [--game <path/to/game.toml>]
 * Output: generated/<prefix>_full.c + generated/<prefix>_dispatch.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include "rom_parser.h"
#include "function_finder.h"
#include "code_generator.h"
#include "annotations.h"
#include "game_config.h"

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

static void delete_if_exists(const char *path) {
    if (file_exists(path)) remove(path);
}

static void ensure_output_dir_exists(void) {
    if (_mkdir("generated") == 0) {
        printf("[NESRecomp] Created output directory: generated\n");
    }
}

static bool rom_addr_in_sram_map(const GameConfig *cfg, int bank, uint16_t addr) {
    for (int i = 0; i < cfg->sram_map_count; i++) {
        const SramMap *map = &cfg->sram_maps[i];
        if (map->bank != bank) continue;
        if (addr >= map->rom_start && addr < (uint16_t)(map->rom_start + map->size))
            return true;
    }
    return false;
}

static bool proposal_emit_standalone(const NESRom *rom, const FunctionEntry *fe) {
    int fixed_bank = rom->prg_banks - 1;
    uint8_t curated_sources =
        FUNCTION_SOURCE_MANUAL |
        FUNCTION_SOURCE_KNOWN_TABLE |
        FUNCTION_SOURCE_SPLIT_TABLE;
    uint8_t fixed_bank_sources = curated_sources | FUNCTION_SOURCE_CONTROL;

    if (fe->kind != FUNCTION_KIND_STANDALONE) return false;
    if (fe->source_flags & FUNCTION_SOURCE_BANK_SEED) return false;
    if (fe->bank == fixed_bank)
        return (fe->source_flags & fixed_bank_sources) != 0;
    return (fe->source_flags & curated_sources) != 0;
}

static bool proposal_emit_extra_label(const NESRom *rom, const GameConfig *cfg,
                                      const FunctionEntry *fe, const FunctionList *funcs) {
    if (fe->kind == FUNCTION_KIND_STANDALONE &&
        (fe->source_flags & FUNCTION_SOURCE_CONTROL) &&
        rom_addr_in_sram_map(cfg, fe->bank, fe->addr)) {
        return true;
    }

    if (fe->source_flags & FUNCTION_SOURCE_BANK_SEED) return false;

    if (fe->kind != FUNCTION_KIND_SECONDARY) return false;
    if ((fe->source_flags & (FUNCTION_SOURCE_MANUAL |
                             FUNCTION_SOURCE_KNOWN_TABLE |
                             FUNCTION_SOURCE_SPLIT_TABLE)) == 0)
        return false;

    for (int i = 0; i < funcs->count; i++) {
        if (funcs->entries[i].addr != fe->canonical_addr ||
            funcs->entries[i].bank != fe->canonical_bank) {
            continue;
        }
        return proposal_emit_standalone(rom, &funcs->entries[i]);
    }

    return false;
}

static void emit_game_toml_proposal(const char *path, const char *output_prefix,
                                    const NESRom *rom, const GameConfig *cfg,
                                    const FunctionList *funcs,
                                    bool overwrite_existing) {
    if (!overwrite_existing && file_exists(path)) {
        printf("[NESRecomp] Proposal config already exists, leaving it untouched: %s\n", path);
        return;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[NESRecomp] Warning: could not write proposal config '%s'\n", path);
        return;
    }

    int fixed_bank = rom->prg_banks - 1;
    int emitted_standalone = 0;

    fprintf(f,
        "# AUTO-GENERATED best-effort proposal.\n"
        "# Review before adopting as game.toml.\n"
        "# Generated only because no game.toml was supplied or found.\n\n"
        "[game]\n"
        "output_prefix = \"%s\"\n\n"
        "[functions]\n",
        output_prefix);

    fprintf(f, "fixed = [");
    int fixed_count = 0;
    for (int i = 0; i < funcs->count; i++) {
        const FunctionEntry *fe = &funcs->entries[i];
        if (!proposal_emit_standalone(rom, fe)) continue;
        if (fe->bank != fixed_bank) continue;
        emitted_standalone++;
        if (fixed_count++ > 0) fprintf(f, ",");
        if ((fixed_count % 8) == 1) fprintf(f, "\n    ");
        else fprintf(f, " ");
        fprintf(f, "0x%04X", fe->addr);
    }
    if (fixed_count > 0) fprintf(f, "\n");
    fprintf(f, "]\n");

    for (int bank = 0; bank < fixed_bank; bank++) {
        int count = 0;
        for (int i = 0; i < funcs->count; i++) {
            const FunctionEntry *fe = &funcs->entries[i];
            if (!proposal_emit_standalone(rom, fe)) continue;
            if (fe->bank != bank) continue;
            count++;
        }
        if (count == 0) continue;
        fprintf(f, "\nbank%d = [", bank);
        int emitted = 0;
        for (int i = 0; i < funcs->count; i++) {
            const FunctionEntry *fe = &funcs->entries[i];
            if (!proposal_emit_standalone(rom, fe)) continue;
            if (fe->bank != bank) continue;
            emitted_standalone++;
            if (emitted++ > 0) fprintf(f, ",");
            if ((emitted % 8) == 1) fprintf(f, "\n    ");
            else fprintf(f, " ");
            fprintf(f, "0x%04X", fe->addr);
        }
        fprintf(f, "\n]\n");
    }

    int emitted_labels = 0;
    for (int i = 0; i < funcs->count; i++) {
        const FunctionEntry *fe = &funcs->entries[i];
        if (!proposal_emit_extra_label(rom, cfg, fe, funcs)) continue;
        fprintf(f, "\n[[extra_label]]\n");
        fprintf(f, "addr = 0x%04X\n", fe->addr);
        fprintf(f, "bank = %d\n", fe->bank);
        emitted_labels++;
    }

    fclose(f);
    printf("[NESRecomp] Wrote proposal config: %s (%d standalone, %d extra_label)\n",
           path, emitted_standalone, emitted_labels);
}

static int count_proposal_entries(const NESRom *rom, const GameConfig *cfg,
                                  const FunctionList *funcs) {
    int count = 0;
    for (int i = 0; i < funcs->count; i++) {
        if (proposal_emit_standalone(rom, &funcs->entries[i])) count++;
        if (proposal_emit_extra_label(rom, cfg, &funcs->entries[i], funcs)) count++;
    }
    return count;
}

static void run_iterative_proposal(const NESRom *rom, const char *output_prefix,
                                   const GameConfig *base_cfg, FunctionList *funcs) {
    enum { MAX_PROPOSAL_PASSES = 4 };
    char proposal_path[256];
    snprintf(proposal_path, sizeof(proposal_path), "generated/__proposal_pass.toml");

    int last_score = -1;
    for (int pass = 0; pass < MAX_PROPOSAL_PASSES; pass++) {
        int current_score = count_proposal_entries(rom, base_cfg, funcs);
        if (current_score == last_score) break;
        last_score = current_score;

        delete_if_exists(proposal_path);
        emit_game_toml_proposal(proposal_path, output_prefix, rom, base_cfg, funcs, true);

        GameConfig iter_cfg = {0};
        if (!game_config_load(&iter_cfg, proposal_path)) break;

        static FunctionList iter_funcs = {0};
        memset(&iter_funcs, 0, sizeof(iter_funcs));
        function_finder_run(rom, &iter_funcs, &iter_cfg);

        int iter_score = count_proposal_entries(rom, base_cfg, &iter_funcs);
        printf("[NESRecomp] Proposal pass %d: %d -> %d emitted entries\n",
               pass + 1, current_score, iter_score);

        *funcs = iter_funcs;
        if (iter_score == current_score) break;
    }

    delete_if_exists(proposal_path);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: NESRecomp <rom.nes> [--game <path/to/game.toml>] [--proposal-out <path/to/proposal.toml>]\n");
        return 1;
    }

    const char *rom_path  = argv[1];
    const char *game_path = NULL;
    const char *proposal_out = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--game") == 0 && i+1 < argc) game_path = argv[++i];
        else if (strcmp(argv[i], "--proposal-out") == 0 && i+1 < argc) proposal_out = argv[++i];
    }

    /* Auto-detect game.toml in current directory if not specified */
    if (!game_path) {
        FILE *f = fopen("game.toml", "r");
        if (f) { fclose(f); game_path = "game.toml"; }
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

    if (!game_path) {
        ensure_output_dir_exists();
        run_iterative_proposal(&rom, output_prefix, &cfg, &funcs);
        emit_game_toml_proposal("game.toml", output_prefix, &rom, &cfg, &funcs, false);
    } else if (proposal_out) {
        emit_game_toml_proposal(proposal_out, output_prefix, &rom, &cfg, &funcs, true);
    }

    /* Emit C */
    ensure_output_dir_exists();
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
