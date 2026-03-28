/*
 * game_config.c — game.cfg file parser
 *
 * Format (# comments, blank lines ignored):
 *   output_prefix    <name>
 *   trampoline       <hex_addr> <inline_bytes> <hex_bs_fn_addr>
 *   known_table      <bank> <hex_start> <hex_end>
 *   split_table      <bank> <hex_lo> <hex_hi> <count> <stride>
 *   extra_func       <bank> <hex_addr>
 *   inline_dispatch  <hex_addr>
 */
#include "game_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void game_config_init_empty(GameConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
}

bool game_config_load(GameConfig *cfg, const char *path) {
    game_config_init_empty(cfg);

    FILE *f = fopen(path, "r");
    if (!f) return false;

    /* Derive annotations path: same directory as game.cfg, file "annotations.csv" */
    {
        const char *slash = NULL;
        const char *p = path;
        while (*p) {
            if (*p == '/' || *p == '\\') slash = p;
            p++;
        }
        if (slash) {
            size_t dir_len = (size_t)(slash - path) + 1;
            if (dir_len + 20 < sizeof(cfg->annotations_path)) {
                memcpy(cfg->annotations_path, path, dir_len);
                strcpy(cfg->annotations_path + dir_len, "annotations.csv");
            }
        } else {
            strcpy(cfg->annotations_path, "annotations.csv");
        }
    }

    char line[512];
    int line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;

        /* Strip comment */
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        /* Trim leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '\r') continue;

        char key[64];
        if (sscanf(p, "%63s", key) != 1) continue;
        char *rest = p + strlen(key);
        while (*rest == ' ' || *rest == '\t') rest++;

        if (strcmp(key, "output_prefix") == 0) {
            sscanf(rest, "%63s", cfg->output_prefix);

        } else if (strcmp(key, "trampoline") == 0) {
            unsigned addr, bs_fn;
            int inline_bytes;
            if (sscanf(rest, "%x %d %x", &addr, &inline_bytes, &bs_fn) == 3 &&
                cfg->trampoline_count < GAME_CFG_MAX_TRAMPOLINES) {
                int i = cfg->trampoline_count++;
                cfg->trampolines[i].addr         = (uint16_t)addr;
                cfg->trampolines[i].inline_bytes  = inline_bytes;
                cfg->trampolines[i].bs_fn_addr    = (uint16_t)bs_fn;
            }

        } else if (strcmp(key, "known_table") == 0) {
            int bank; unsigned start, end;
            if (sscanf(rest, "%d %x %x", &bank, &start, &end) == 3 &&
                cfg->known_table_count < GAME_CFG_MAX_KNOWN_TABLES) {
                int i = cfg->known_table_count++;
                cfg->known_tables[i].bank  = bank;
                cfg->known_tables[i].start = (uint16_t)start;
                cfg->known_tables[i].end   = (uint16_t)end;
            }

        } else if (strcmp(key, "split_table") == 0) {
            int bank, count, stride; unsigned lo, hi;
            if (sscanf(rest, "%d %x %x %d %d", &bank, &lo, &hi, &count, &stride) == 5 &&
                cfg->known_split_table_count < GAME_CFG_MAX_SPLIT_TABLES) {
                int i = cfg->known_split_table_count++;
                cfg->known_split_tables[i].bank      = bank;
                cfg->known_split_tables[i].lo_start  = (uint16_t)lo;
                cfg->known_split_tables[i].hi_start  = (uint16_t)hi;
                cfg->known_split_tables[i].count     = count;
                cfg->known_split_tables[i].stride    = stride;
            }

        } else if (strcmp(key, "extra_func") == 0) {
            int bank; unsigned addr;
            if (sscanf(rest, "%d %x", &bank, &addr) == 2 &&
                cfg->extra_func_count < GAME_CFG_MAX_EXTRA_FUNCS) {
                int i = cfg->extra_func_count++;
                cfg->extra_funcs[i].addr = (uint16_t)addr;
                cfg->extra_funcs[i].bank = bank;
            }

        } else if (strcmp(key, "extra_label") == 0) {
            int bank; unsigned addr;
            if (sscanf(rest, "%d %x", &bank, &addr) == 2 &&
                cfg->extra_label_count < GAME_CFG_MAX_EXTRA_LABELS) {
                int i = cfg->extra_label_count++;
                cfg->extra_labels[i].addr = (uint16_t)addr;
                cfg->extra_labels[i].bank = bank;
            }

        } else if (strcmp(key, "inline_dispatch") == 0) {
            unsigned addr;
            if (sscanf(rest, "%x", &addr) == 1 &&
                cfg->inline_dispatch_count < GAME_CFG_MAX_INLINE_DISPATCHES) {
                int i = cfg->inline_dispatch_count++;
                cfg->inline_dispatches[i].addr = (uint16_t)addr;
            }

        } else if (strcmp(key, "ram_read_hook") == 0) {
            unsigned addr;
            if (sscanf(rest, "%x", &addr) == 1 &&
                cfg->ram_read_hook_count < GAME_CFG_MAX_RAM_READ_HOOKS) {
                int i = cfg->ram_read_hook_count++;
                cfg->ram_read_hooks[i].addr = (uint16_t)addr;
            }

        } else if (strcmp(key, "sram_map") == 0) {
            unsigned sram_start, rom_start, size;
            int bank;
            if (sscanf(rest, "%x %x %d %x", &sram_start, &rom_start, &bank, &size) == 4 &&
                cfg->sram_map_count < GAME_CFG_MAX_SRAM_MAPS) {
                int i = cfg->sram_map_count++;
                cfg->sram_maps[i].sram_start = (uint16_t)sram_start;
                cfg->sram_maps[i].rom_start  = (uint16_t)rom_start;
                cfg->sram_maps[i].bank       = bank;
                cfg->sram_maps[i].size       = (uint16_t)size;
            }

        } else if (strcmp(key, "bank_switch") == 0) {
            unsigned addr;
            if (sscanf(rest, "%x", &addr) == 1 &&
                cfg->bank_switch_count < GAME_CFG_MAX_BANK_SWITCHES) {
                int i = cfg->bank_switch_count++;
                cfg->bank_switches[i].addr = (uint16_t)addr;
            }

        } else {
            fprintf(stderr, "[GameConfig] Unknown directive '%s' at line %d\n", key, line_no);
        }
    }

    fclose(f);
    return true;
}
