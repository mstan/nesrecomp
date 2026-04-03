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
#include "toml.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void game_config_init_empty(GameConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
}

static bool game_config_load_cfg(GameConfig *cfg, const char *path) {
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
            unsigned addr, bs_fn, bank_save = 0x100;
            int inline_bytes, adj = 1;
            char breg = 'X';
            int n = sscanf(rest, "%x %d %x %d %c %x",
                           &addr, &inline_bytes, &bs_fn, &adj, &breg, &bank_save);
            if (n >= 3 && cfg->trampoline_count < GAME_CFG_MAX_TRAMPOLINES) {
                int i = cfg->trampoline_count++;
                cfg->trampolines[i].addr           = (uint16_t)addr;
                cfg->trampolines[i].inline_bytes    = inline_bytes;
                cfg->trampolines[i].bs_fn_addr      = (uint16_t)bs_fn;
                cfg->trampolines[i].addr_adjust     = adj;
                cfg->trampolines[i].bank_reg        = (breg == 'A' || breg == 'a') ? 'A' : 'X';
                cfg->trampolines[i].bank_save_addr  = (uint16_t)bank_save;
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

        } else if (strcmp(key, "nop_jsr") == 0) {
            unsigned addr;
            if (sscanf(rest, "%x", &addr) == 1 &&
                cfg->nop_jsr_count < GAME_CFG_MAX_NOP_JSRS) {
                cfg->nop_jsrs[cfg->nop_jsr_count++] = (uint16_t)addr;
            }

        } else if (strcmp(key, "push_jsr") == 0) {
            unsigned addr;
            if (sscanf(rest, "%x", &addr) == 1 &&
                cfg->push_jsr_count < GAME_CFG_MAX_NOP_JSRS) {
                cfg->push_jsrs[cfg->push_jsr_count++] = (uint16_t)addr;
            }

        } else if (strcmp(key, "inline_pointer") == 0) {
            unsigned addr, zp_lo, zp_hi;
            char extra[16] = {0};
            if (sscanf(rest, "%x %x %x %15s", &addr, &zp_lo, &zp_hi, extra) >= 3 &&
                cfg->inline_pointer_count < GAME_CFG_MAX_INLINE_POINTERS) {
                int i = cfg->inline_pointer_count++;
                cfg->inline_pointers[i].addr  = (uint16_t)addr;
                cfg->inline_pointers[i].zp_lo = (uint8_t)zp_lo;
                cfg->inline_pointers[i].zp_hi = (uint8_t)zp_hi;
                cfg->inline_pointers[i].call  = (strcmp(extra, "call") == 0) ? 1 : 0;
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

        } else if (strcmp(key, "data_region") == 0) {
            int bank; unsigned start, end;
            if (sscanf(rest, "%d %x %x", &bank, &start, &end) == 3 &&
                cfg->data_region_count < GAME_CFG_MAX_DATA_REGIONS) {
                int i = cfg->data_region_count++;
                cfg->data_regions[i].bank  = bank;
                cfg->data_regions[i].start = (uint16_t)start;
                cfg->data_regions[i].end   = (uint16_t)end;
            }

        } else if (strcmp(key, "merge_func") == 0) {
            int bank; unsigned a1, a2;
            if (sscanf(rest, "%d %x %x", &bank, &a1, &a2) == 3 &&
                cfg->merge_func_count < GAME_CFG_MAX_MERGE_FUNCS) {
                int i = cfg->merge_func_count++;
                cfg->merge_funcs[i].bank    = bank;
                cfg->merge_funcs[i].addr_lo = (uint16_t)(a1 < a2 ? a1 : a2);
                cfg->merge_funcs[i].addr_hi = (uint16_t)(a1 < a2 ? a2 : a1);
            }

        } else if (strcmp(key, "stack_bail_func") == 0) {
            unsigned addr;
            if (sscanf(rest, "%x", &addr) == 1 &&
                cfg->stack_bail_func_count < GAME_CFG_MAX_STACK_BAIL_FUNCS) {
                cfg->stack_bail_funcs[cfg->stack_bail_func_count++] = (uint16_t)addr;
            }

        } else if (strcmp(key, "push_all_jsr") == 0) {
            cfg->push_all_jsr = true;

        } else if (strcmp(key, "replace_func") == 0) {
            int bank; unsigned addr;
            if (sscanf(rest, "%d %x", &bank, &addr) == 2 &&
                cfg->replace_func_count < GAME_CFG_MAX_EXTRA_FUNCS) {
                int i = cfg->replace_func_count++;
                cfg->replace_funcs[i].bank = bank;
                cfg->replace_funcs[i].addr = (uint16_t)addr;
            }

        } else {
            fprintf(stderr, "[GameConfig] Unknown directive '%s' at line %d\n", key, line_no);
        }
    }

    fclose(f);
    return true;
}

/* ── TOML format loader ───────────────────────────────────────────────────── */

static uint16_t toml_hex(toml_table_t *tbl, const char *key) {
    toml_datum_t d = toml_int_in(tbl, key);
    if (d.ok) return (uint16_t)d.u.i;
    return 0;
}

static int toml_int_or(toml_table_t *tbl, const char *key, int def) {
    toml_datum_t d = toml_int_in(tbl, key);
    return d.ok ? (int)d.u.i : def;
}

static uint16_t toml_hex_or(toml_table_t *tbl, const char *key, uint16_t def) {
    toml_datum_t d = toml_int_in(tbl, key);
    return d.ok ? (uint16_t)d.u.i : def;
}

static const char *toml_string_or(toml_table_t *tbl, const char *key, const char *def) {
    toml_datum_t d = toml_string_in(tbl, key);
    if (d.ok) return d.u.s;
    return def;
}

static bool game_config_load_toml(GameConfig *cfg, const char *path) {
    game_config_init_empty(cfg);

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);
    if (!root) {
        fprintf(stderr, "[GameConfig] TOML parse error: %s\n", errbuf);
        return false;
    }

    /* Derive annotations path */
    {
        const char *slash = NULL;
        const char *p = path;
        while (*p) { if (*p == '/' || *p == '\\') slash = p; p++; }
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

    /* [game] */
    toml_table_t *game = toml_table_in(root, "game");
    if (game) {
        toml_datum_t d = toml_string_in(game, "output_prefix");
        if (d.ok) { strncpy(cfg->output_prefix, d.u.s, sizeof(cfg->output_prefix) - 1); free(d.u.s); }
    }

    /* [mapper] */
    toml_table_t *mapper = toml_table_in(root, "mapper");
    if (mapper) {
        toml_array_t *bs = toml_array_in(mapper, "bank_switch");
        if (bs) for (int i = 0; i < toml_array_nelem(bs) && cfg->bank_switch_count < GAME_CFG_MAX_BANK_SWITCHES; i++) {
            toml_datum_t d = toml_int_at(bs, i);
            if (d.ok) cfg->bank_switches[cfg->bank_switch_count++].addr = (uint16_t)d.u.i;
        }
    }

    /* [[trampoline]] */
    toml_array_t *tramp = toml_array_in(root, "trampoline");
    if (tramp) for (int i = 0; i < toml_array_nelem(tramp) && cfg->trampoline_count < GAME_CFG_MAX_TRAMPOLINES; i++) {
        toml_table_t *t = toml_table_at(tramp, i);
        if (!t) continue;
        int idx = cfg->trampoline_count++;
        cfg->trampolines[idx].addr        = toml_hex(t, "addr");
        cfg->trampolines[idx].inline_bytes = toml_int_or(t, "inline_bytes", 0);
        cfg->trampolines[idx].bs_fn_addr  = toml_hex(t, "bs_fn_addr");
        cfg->trampolines[idx].addr_adjust  = toml_int_or(t, "addr_adjust", 1);
        const char *breg = toml_string_or(t, "bank_reg", "X");
        cfg->trampolines[idx].bank_reg    = (breg[0] == 'A' || breg[0] == 'a') ? 'A' : 'X';
        cfg->trampolines[idx].bank_save_addr = toml_hex_or(t, "bank_save_addr", 0x100);
    }

    /* [[known_table]] */
    toml_array_t *ktbl = toml_array_in(root, "known_table");
    if (ktbl) for (int i = 0; i < toml_array_nelem(ktbl) && cfg->known_table_count < GAME_CFG_MAX_KNOWN_TABLES; i++) {
        toml_table_t *t = toml_table_at(ktbl, i);
        if (!t) continue;
        int idx = cfg->known_table_count++;
        cfg->known_tables[idx].bank  = toml_int_or(t, "bank", -1);
        cfg->known_tables[idx].start = toml_hex(t, "start");
        cfg->known_tables[idx].end   = toml_hex(t, "end");
    }

    /* [[split_table]] */
    toml_array_t *stbl = toml_array_in(root, "split_table");
    if (stbl) for (int i = 0; i < toml_array_nelem(stbl) && cfg->known_split_table_count < GAME_CFG_MAX_SPLIT_TABLES; i++) {
        toml_table_t *t = toml_table_at(stbl, i);
        if (!t) continue;
        int idx = cfg->known_split_table_count++;
        cfg->known_split_tables[idx].bank     = toml_int_or(t, "bank", -1);
        cfg->known_split_tables[idx].lo_start = toml_hex(t, "lo_addr");
        cfg->known_split_tables[idx].hi_start = toml_hex(t, "hi_addr");
        cfg->known_split_tables[idx].count    = toml_int_or(t, "count", 0);
        cfg->known_split_tables[idx].stride   = toml_int_or(t, "stride", 1);
    }

    /* [[inline_dispatch]] */
    toml_array_t *idisp = toml_array_in(root, "inline_dispatch");
    if (idisp) for (int i = 0; i < toml_array_nelem(idisp) && cfg->inline_dispatch_count < GAME_CFG_MAX_INLINE_DISPATCHES; i++) {
        toml_table_t *t = toml_table_at(idisp, i);
        if (t) cfg->inline_dispatches[cfg->inline_dispatch_count++].addr = toml_hex(t, "addr");
    }

    /* [[inline_pointer]] */
    toml_array_t *iptr = toml_array_in(root, "inline_pointer");
    if (iptr) for (int i = 0; i < toml_array_nelem(iptr) && cfg->inline_pointer_count < GAME_CFG_MAX_INLINE_POINTERS; i++) {
        toml_table_t *t = toml_table_at(iptr, i);
        if (!t) continue;
        int idx = cfg->inline_pointer_count++;
        cfg->inline_pointers[idx].addr = toml_hex(t, "addr");
        toml_array_t *zp = toml_array_in(t, "zp");
        if (zp) {
            toml_datum_t lo = toml_int_at(zp, 0), hi = toml_int_at(zp, 1);
            if (lo.ok) cfg->inline_pointers[idx].zp_lo = (uint8_t)lo.u.i;
            if (hi.ok) cfg->inline_pointers[idx].zp_hi = (uint8_t)hi.u.i;
        }
        toml_datum_t call = toml_bool_in(t, "call");
        cfg->inline_pointers[idx].call = (call.ok && call.u.b) ? 1 : 0;
    }

    /* [[nop_jsr]] */
    toml_array_t *nj = toml_array_in(root, "nop_jsr");
    if (nj) for (int i = 0; i < toml_array_nelem(nj) && cfg->nop_jsr_count < GAME_CFG_MAX_NOP_JSRS; i++) {
        toml_table_t *t = toml_table_at(nj, i);
        if (t) cfg->nop_jsrs[cfg->nop_jsr_count++] = toml_hex(t, "addr");
    }

    /* [[extra_label]] */
    toml_array_t *elbl = toml_array_in(root, "extra_label");
    if (elbl) for (int i = 0; i < toml_array_nelem(elbl) && cfg->extra_label_count < GAME_CFG_MAX_EXTRA_LABELS; i++) {
        toml_table_t *t = toml_table_at(elbl, i);
        if (!t) continue;
        int idx = cfg->extra_label_count++;
        cfg->extra_labels[idx].addr = toml_hex(t, "addr");
        cfg->extra_labels[idx].bank = toml_int_or(t, "bank", -1);
    }

    /* [functions] */
    toml_table_t *funcs = toml_table_in(root, "functions");
    if (funcs) {
        toml_array_t *fixed = toml_array_in(funcs, "fixed");
        if (fixed) for (int i = 0; i < toml_array_nelem(fixed) && cfg->extra_func_count < GAME_CFG_MAX_EXTRA_FUNCS; i++) {
            toml_datum_t d = toml_int_at(fixed, i);
            if (d.ok) { int idx = cfg->extra_func_count++; cfg->extra_funcs[idx].addr = (uint16_t)d.u.i; cfg->extra_funcs[idx].bank = -1; }
        }
        for (int b = 0; b < 64; b++) {
            char key[16]; snprintf(key, sizeof(key), "bank%d", b);
            toml_array_t *ba = toml_array_in(funcs, key);
            if (!ba) continue;
            for (int i = 0; i < toml_array_nelem(ba) && cfg->extra_func_count < GAME_CFG_MAX_EXTRA_FUNCS; i++) {
                toml_datum_t d = toml_int_at(ba, i);
                if (d.ok) { int idx = cfg->extra_func_count++; cfg->extra_funcs[idx].addr = (uint16_t)d.u.i; cfg->extra_funcs[idx].bank = b; }
            }
        }
    }

    /* [[sram_map]] */
    toml_array_t *smap = toml_array_in(root, "sram_map");
    if (smap) for (int i = 0; i < toml_array_nelem(smap) && cfg->sram_map_count < GAME_CFG_MAX_SRAM_MAPS; i++) {
        toml_table_t *t = toml_table_at(smap, i);
        if (!t) continue;
        int idx = cfg->sram_map_count++;
        cfg->sram_maps[idx].sram_start = toml_hex(t, "sram_start");
        cfg->sram_maps[idx].rom_start  = toml_hex(t, "rom_start");
        cfg->sram_maps[idx].bank       = toml_int_or(t, "bank", -1);
        cfg->sram_maps[idx].size       = toml_hex(t, "size");
    }

    /* [[stack_bail_func]] */
    toml_array_t *sbf = toml_array_in(root, "stack_bail_func");
    if (sbf) for (int i = 0; i < toml_array_nelem(sbf) && cfg->stack_bail_func_count < GAME_CFG_MAX_STACK_BAIL_FUNCS; i++) {
        toml_table_t *t = toml_table_at(sbf, i);
        if (t) cfg->stack_bail_funcs[cfg->stack_bail_func_count++] = toml_hex(t, "addr");
    }

    /* [[data_region]] */
    toml_array_t *dr = toml_array_in(root, "data_region");
    if (dr) for (int i = 0; i < toml_array_nelem(dr) && cfg->data_region_count < GAME_CFG_MAX_DATA_REGIONS; i++) {
        toml_table_t *t = toml_table_at(dr, i);
        if (!t) continue;
        int idx = cfg->data_region_count++;
        cfg->data_regions[idx].bank  = toml_int_or(t, "bank", -1);
        cfg->data_regions[idx].start = toml_hex(t, "start");
        cfg->data_regions[idx].end   = toml_hex(t, "end");
    }

    toml_free(root);
    printf("[GameConfig] Loaded TOML: %s (prefix='%s', %d extra funcs)\n",
           path, cfg->output_prefix, cfg->extra_func_count);
    return true;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

static bool has_extension(const char *path, const char *ext) {
    size_t plen = strlen(path), elen = strlen(ext);
    return plen > elen && strcmp(path + plen - elen, ext) == 0;
}

bool game_config_load(GameConfig *cfg, const char *path) {
    if (has_extension(path, ".toml"))
        return game_config_load_toml(cfg, path);
    return game_config_load_cfg(cfg, path);
}
