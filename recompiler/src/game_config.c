/*
 * game_config.c — TOML-only game config loader
 *
 * The old .cfg text format has been removed.  All game projects must use
 * game.toml.  If a .cfg path is passed, the loader prints a migration
 * message and returns failure.
 */
#include "game_config.h"
#include "toml.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void game_config_init_empty(GameConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
}

/* ── TOML helpers ────────────────────────────────────────────────────────── */

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

/* ── TOML loader ─────────────────────────────────────────────────────────── */

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
        toml_datum_t paj = toml_bool_in(game, "push_all_jsr");
        if (paj.ok) cfg->push_all_jsr = paj.u.b;
        toml_datum_t dps = toml_bool_in(game, "disable_ptr_scan");
        if (dps.ok) cfg->disable_ptr_scan = dps.u.b;
        toml_datum_t ds = toml_bool_in(game, "disable_secondary");
        if (ds.ok) cfg->disable_secondary = ds.u.b;
        toml_datum_t sf = toml_string_in(game, "symbol_file");
        if (sf.ok) { strncpy(cfg->symbol_file, sf.u.s, sizeof(cfg->symbol_file) - 1); free(sf.u.s); }
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

    /* [[ram_read_hook]] */
    toml_array_t *rrh = toml_array_in(root, "ram_read_hook");
    if (rrh) for (int i = 0; i < toml_array_nelem(rrh) && cfg->ram_read_hook_count < GAME_CFG_MAX_RAM_READ_HOOKS; i++) {
        toml_table_t *t = toml_table_at(rrh, i);
        if (t) cfg->ram_read_hooks[cfg->ram_read_hook_count++].addr = toml_hex(t, "addr");
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

    /* [functions] — preferred bulk format: fixed = [...], bankN = [...] */
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

    /* [[extra_func]] — individual entries: bank + addr */
    toml_array_t *ef = toml_array_in(root, "extra_func");
    if (ef) for (int i = 0; i < toml_array_nelem(ef) && cfg->extra_func_count < GAME_CFG_MAX_EXTRA_FUNCS; i++) {
        toml_table_t *t = toml_table_at(ef, i);
        if (!t) continue;
        int idx = cfg->extra_func_count++;
        cfg->extra_funcs[idx].addr = toml_hex(t, "addr");
        cfg->extra_funcs[idx].bank = toml_int_or(t, "bank", -1);
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

    /* [[cond_bail_func]] — functions containing inline bail code that fires conditionally */
    toml_array_t *cbf = toml_array_in(root, "cond_bail_func");
    if (cbf) for (int i = 0; i < toml_array_nelem(cbf) && cfg->cond_bail_func_count < GAME_CFG_MAX_STACK_BAIL_FUNCS; i++) {
        toml_table_t *t = toml_table_at(cbf, i);
        if (t) cfg->cond_bail_funcs[cfg->cond_bail_func_count++] = toml_hex(t, "addr");
    }

    /* [[merge_func]] */
    toml_array_t *mf = toml_array_in(root, "merge_func");
    if (mf) for (int i = 0; i < toml_array_nelem(mf) && cfg->merge_func_count < GAME_CFG_MAX_MERGE_FUNCS; i++) {
        toml_table_t *t = toml_table_at(mf, i);
        if (!t) continue;
        int idx = cfg->merge_func_count++;
        cfg->merge_funcs[idx].bank = toml_int_or(t, "bank", -1);
        uint16_t a1 = toml_hex(t, "addr_lo");
        uint16_t a2 = toml_hex(t, "addr_hi");
        cfg->merge_funcs[idx].addr_lo = (a1 < a2) ? a1 : a2;
        cfg->merge_funcs[idx].addr_hi = (a1 < a2) ? a2 : a1;
    }

    /* [[merge_range]] — merge ALL function entry points within an address range */
    toml_array_t *mrng = toml_array_in(root, "merge_range");
    if (mrng) for (int i = 0; i < toml_array_nelem(mrng) && cfg->merge_range_count < GAME_CFG_MAX_MERGE_RANGES; i++) {
        toml_table_t *t = toml_table_at(mrng, i);
        if (!t) continue;
        int idx = cfg->merge_range_count++;
        cfg->merge_ranges[idx].bank    = toml_int_or(t, "bank", -1);
        cfg->merge_ranges[idx].addr_lo = toml_hex(t, "addr_lo");
        cfg->merge_ranges[idx].addr_hi = toml_hex(t, "addr_hi");
    }

    /* [[push_jsr]] */
    toml_array_t *pj = toml_array_in(root, "push_jsr");
    if (pj) for (int i = 0; i < toml_array_nelem(pj) && cfg->push_jsr_count < GAME_CFG_MAX_NOP_JSRS; i++) {
        toml_table_t *t = toml_table_at(pj, i);
        if (t) cfg->push_jsrs[cfg->push_jsr_count++] = toml_hex(t, "addr");
    }

    /* [[push_jmp]] — JMP targets that need a dummy push (bail-containing funcs).
     * Optional `source` field restricts the push to a specific JMP site PC. */
    toml_array_t *pjm = toml_array_in(root, "push_jmp");
    if (pjm) for (int i = 0; i < toml_array_nelem(pjm) && cfg->push_jmp_count < GAME_CFG_MAX_NOP_JSRS; i++) {
        toml_table_t *t = toml_table_at(pjm, i);
        if (t) {
            int idx = cfg->push_jmp_count++;
            cfg->push_jmps[idx].target = toml_hex(t, "addr");
            cfg->push_jmps[idx].source = toml_hex(t, "source"); /* 0 if absent */
        }
    }

    /* [[replace_func]] — functions replaced by extras.c, exclude from codegen */
    toml_array_t *rf = toml_array_in(root, "replace_func");
    if (rf) for (int i = 0; i < toml_array_nelem(rf) && cfg->replace_func_count < GAME_CFG_MAX_EXTRA_FUNCS; i++) {
        toml_table_t *t = toml_table_at(rf, i);
        if (!t) continue;
        int idx = cfg->replace_func_count++;
        cfg->replace_funcs[idx].bank = toml_int_or(t, "bank", -1);
        cfg->replace_funcs[idx].addr = toml_hex(t, "addr");
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
    if (has_extension(path, ".cfg")) {
        fprintf(stderr,
            "[GameConfig] ERROR: .cfg format is no longer supported.\n"
            "  Migrate '%s' to game.toml (TOML format).\n"
            "  See nesrecomp CLAUDE.md for the TOML schema.\n", path);
        return false;
    }
    return game_config_load_toml(cfg, path);
}
