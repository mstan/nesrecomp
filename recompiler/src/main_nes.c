/*
 * main_nes.c — NESRecomp entry point
 * Usage: NESRecomp.exe <rom.nes> [--game <path/to/game.toml>]
 * Output: generated/<prefix>_full.c + generated/<prefix>_dispatch.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <direct.h>
#  define make_dir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define make_dir(p) mkdir((p), 0755)
#endif
#include "rom_parser.h"
#include "cpu6502_decoder.h"
#include "function_finder.h"
#include "code_generator.h"
#include "annotations.h"
#include "symbol_table.h"
#include "game_config.h"
#include "coverage.h"
#include "cyc_codegen.h"
#include "toml.h"
#include "../../common/nes_fds.h"
#include "../../common/nes_fds_hle.h"

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
    if (make_dir("generated") == 0) {
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

static bool rom_addr_in_data_region(const GameConfig *cfg, int bank, uint16_t addr) {
    for (int i = 0; i < cfg->data_region_count; i++) {
        const DataRegion *dr = &cfg->data_regions[i];
        if (dr->bank != bank) continue;
        if (addr >= dr->start && addr < dr->end) return true;
    }
    return false;
}

static bool is_stop_mnemonic(OpMnemonic mn) {
    return mn == MN_JMP || mn == MN_RTS || mn == MN_RTI || mn == MN_BRK;
}

static bool proposal_emit_standalone_basic(const NESRom *rom, const GameConfig *cfg,
                                           const FunctionEntry *fe) {
    int fixed_bank = rom->prg_banks - 1;
    uint8_t curated_sources =
        FUNCTION_SOURCE_MANUAL |
        FUNCTION_SOURCE_KNOWN_TABLE |
        FUNCTION_SOURCE_SPLIT_TABLE;
    uint8_t fixed_bank_sources = curated_sources | FUNCTION_SOURCE_CONTROL;
    if (fe->kind != FUNCTION_KIND_STANDALONE) return false;
    if (rom_addr_in_data_region(cfg, fe->bank, fe->addr)) return false;
    if (fe->source_flags == FUNCTION_SOURCE_CONTROL && fe->evidence_count <= 1)
        return false;
    if (fe->bank == fixed_bank && fe->addr >= 0xFFEB)
        return false;
    if (fe->bank == fixed_bank &&
        fe->covering_addr != fe->addr &&
        fe->covering_bank == fe->bank &&
        fe->addr < 0xFE00 &&
        (fe->source_flags & curated_sources) == 0) {
        return false;
    }
    if (fe->bank == fixed_bank)
        return (fe->source_flags & fixed_bank_sources) != 0;
    return (fe->source_flags & curated_sources) != 0;
}

static const FunctionEntry *find_shadow_sibling(const NESRom *rom, const GameConfig *cfg,
                                                const FunctionList *funcs,
                                                const FunctionEntry *fe,
                                                bool want_next) {
    const FunctionEntry *best = NULL;
    for (int i = 0; i < funcs->count; i++) {
        const FunctionEntry *other = &funcs->entries[i];
        if (other == fe) continue;
        if (!proposal_emit_standalone_basic(rom, cfg, other)) continue;
        if (other->bank != fe->bank) continue;
        if (other->covering_bank != fe->covering_bank ||
            other->covering_addr != fe->covering_addr) {
            continue;
        }
        if (want_next) {
            if (other->addr <= fe->addr) continue;
            if (!best || other->addr < best->addr) best = other;
        } else {
            if (other->addr >= fe->addr) continue;
            if (!best || other->addr > best->addr) best = other;
        }
    }
    return best;
}

static bool shadowed_fixed_bank_internal_split(const NESRom *rom, const GameConfig *cfg,
                                               const FunctionList *funcs,
                                               const FunctionEntry *fe) {
    int fixed_bank = rom->prg_banks - 1;
    uint8_t curated_sources =
        FUNCTION_SOURCE_MANUAL |
        FUNCTION_SOURCE_KNOWN_TABLE |
        FUNCTION_SOURCE_SPLIT_TABLE;

    if (fe->bank != fixed_bank) return false;
    if (fe->covering_bank != fe->bank || fe->covering_addr == fe->addr) return false;
    if (fe->addr < 0xFE00 || fe->addr >= 0xFFEB) return false;
    if (fe->source_flags & curated_sources) return false;

    const FunctionEntry *prev = find_shadow_sibling(rom, cfg, funcs, fe, false);
    uint8_t opcode = rom_read(rom, fe->bank, fe->addr);
    const OpcodeEntry *entry = &g_opcode_table[opcode];

    {
        uint16_t later_siblings[8];
        int later_count = 0;
        for (int i = 0; i < funcs->count && later_count < 8; i++) {
            const FunctionEntry *other = &funcs->entries[i];
            if (other == fe) continue;
            if (!proposal_emit_standalone_basic(rom, cfg, other)) continue;
            if (other->bank != fe->bank) continue;
            if (other->covering_bank != fe->covering_bank ||
                other->covering_addr != fe->covering_addr) {
                continue;
            }
            if (other->addr <= fe->addr || other->addr - fe->addr > 0x10) continue;
            later_siblings[later_count++] = other->addr;
        }

        for (int off = 0; off < 8; ) {
            uint16_t pc = (uint16_t)(fe->addr + off);
            uint8_t op = rom_read(rom, fe->bank, pc);
            const OpcodeEntry *e = &g_opcode_table[op];
            int size = (e->size > 0) ? e->size : 1;
            if (e->mnemonic == MN_ILLEGAL) break;
            if (e->addr_mode == AM_REL) {
                int8_t rel = (int8_t)rom_read(rom, fe->bank, pc + 1);
                uint16_t tgt = (uint16_t)(pc + size + rel);
                for (int li = 0; li < later_count; li++) {
                    if (tgt == later_siblings[li]) return true;
                }
            }
            if (is_stop_mnemonic(e->mnemonic)) break;
            off += size;
        }
    }

    if (prev && fe->addr - prev->addr <= 0x10) {
        if (entry->mnemonic == MN_STA || entry->mnemonic == MN_STX || entry->mnemonic == MN_STY) {
            for (int off = 0; off < 8; ) {
                uint16_t pc = (uint16_t)(fe->addr + off);
                uint8_t op = rom_read(rom, fe->bank, pc);
                const OpcodeEntry *e = &g_opcode_table[op];
                int size = (e->size > 0) ? e->size : 1;
                if (e->mnemonic == MN_JMP) return true;
                if (e->mnemonic == MN_ILLEGAL || is_stop_mnemonic(e->mnemonic)) break;
                off += size;
            }
        }
    }

    return false;
}

static bool proposal_emit_standalone(const NESRom *rom, const GameConfig *cfg,
                                     const FunctionList *funcs,
                                     const FunctionEntry *fe) {
    if (!proposal_emit_standalone_basic(rom, cfg, fe)) return false;
    if (shadowed_fixed_bank_internal_split(rom, cfg, funcs, fe)) return false;
    return true;
}

static bool proposal_emit_extra_label(const NESRom *rom, const GameConfig *cfg,
                                      const FunctionEntry *fe, const FunctionList *funcs) {
    bool in_sram = rom_addr_in_sram_map(cfg, fe->bank, fe->addr);

    if (in_sram) {
        if (fe->kind == FUNCTION_KIND_STANDALONE &&
            (fe->source_flags & FUNCTION_SOURCE_CONTROL) &&
            (fe->covering_addr != fe->addr || fe->covering_bank != fe->bank)) {
            return true;
        }
    }

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
        return proposal_emit_standalone(rom, cfg, funcs, &funcs->entries[i]);
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
        if (!proposal_emit_standalone(rom, cfg, funcs, fe)) continue;
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
            if (!proposal_emit_standalone(rom, cfg, funcs, fe)) continue;
            if (fe->bank != bank) continue;
            count++;
        }
        if (count == 0) continue;
        fprintf(f, "\nbank%d = [", bank);
        int emitted = 0;
        for (int i = 0; i < funcs->count; i++) {
            const FunctionEntry *fe = &funcs->entries[i];
            if (!proposal_emit_standalone(rom, cfg, funcs, fe)) continue;
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
        if (proposal_emit_standalone(rom, cfg, funcs, &funcs->entries[i])) count++;
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

static void print_usage(void) {
    fprintf(stderr,
        "NESRecomp — static NES recompiler (6502 → C)\n"
        "\n"
        "Usage:\n"
        "  NESRecomp <rom.nes>                        Recompile ROM to C\n"
        "  NESRecomp <rom.nes> --game <game.toml>     Recompile with game config\n"
        "\n"
        "Options:\n"
        "  --game <path>          Game-specific config (trampolines, dispatch tables, etc.)\n"
        "                         If omitted, auto-detects ./game.toml or uses defaults.\n"
        "  --output-prefix <name> Override game.toml output_prefix. Lets a multi-variant\n"
        "                         game regen one game.toml under distinct prefixes so the\n"
        "                         per-bank split files never collide (e.g. zelda_stock /\n"
        "                         zelda_hd).\n"
        "  --proposal-out <path>  Write a proposed game.toml based on auto-discovery.\n"
        "  --cycle-accurate       Emit generated/<prefix>_cyc.c (per-CPU-cycle code for\n"
        "                         runner/cyc) instead of the function-level output. Also\n"
        "                         enabled by game.toml [game] cycle_accurate = true.\n"
        "  --cycle-seed-file <path> Override the cycle seed file from game.toml.\n"
        "  --cycle-capture-file <path> Override the cycle RAM capture file from game.toml\n"
        "                         (the cycle host's --capture-log).\n"
        "  --emit-cycle-interpreter <path>\n"
        "                         Write the cycle-accurate 6502 interpreter generated\n"
        "                         from the same templates (runner/cyc/cpu6502_interp.c)\n"
        "                         and exit. No ROM needed.\n"
        "  --cart-info            Read ROM paths (one per line) from stdin and print\n"
        "                         'CART <mapper> <submapper> <path>' for each, decoded as\n"
        "                         the cycle runtime does (known-dump corrections\n"
        "                         included), or 'CART ? ? <path>' if it does not load.\n"
        "  --fds-info <image>     Print a Famicom Disk System image's format, sides, disk\n"
        "                         info and file table (every file, hidden ones too).\n"
        "                         Exit 0 if every side parses cleanly, 2 on defects.\n"
        "  --fds-stream <image> <side> <out>\n"
        "                         Write the side's drive byte stream (gaps, $80 marks,\n"
        "                         CRCs restored). --fds-profile mesen099|mesen2 picks\n"
        "                         the layout (default mesen099, the nesref core);\n"
        "                         --fds-crc computed|mesen picks real CRCs (default)\n"
        "                         or Mesen's constant $4D $62.\n"
        "  --fds-bios <path>      FDS: the RAM Adapter BIOS to compile (default game.toml\n"
        "                         [fds] bios, else bios/disksys.rom beside the image).\n"
        "                         Only the image its <name>.toml ([program] size, crc32;\n"
        "                         bios/disksys.toml) identifies, else CRC32 5E607DCF.\n"
        "                         An FDS image (.fds/.qd, or game.toml [fds] image) is\n"
        "                         compiled for the cycle backend only: the BIOS as a\n"
        "                         fixed ROM at $E000-$FFFF; disk code runs interpreted.\n"
        "  --fds-bios-only        FDS: compile the BIOS alone, no disk (the BIOS\n"
        "                         showcase, runner/cyc/fds-bios); the BIOS from\n"
        "                         --fds-bios or game.toml [fds] bios, --cycle-accurate.\n"
        "  --help, -h             Show this help message.\n"
        "\n"
        "Output:\n"
        "  generated/<prefix>_full.c       All recompiled functions\n"
        "  generated/<prefix>_dispatch.c   Address-to-function dispatch table\n"
        "\n"
        "  The output prefix comes from --output-prefix, else game.toml\n"
        "  [game]/output_prefix, else the ROM filename if no config is provided.\n"
        "\n"
        "Examples:\n"
        "  NESRecomp \"Super Mario Bros.nes\"                    # quick start\n"
        "  NESRecomp \"Super Mario Bros.nes\" --game game.toml   # with config\n"
    );
}

/* --cart-info: board identity of each ROM, for catalogs of a ROM library. */
static int cart_info(void) {
    char path[4096];
    while (fgets(path, sizeof(path), stdin)) {
        path[strcspn(path, "\r\n")] = 0;
        if (!path[0]) continue;
        NESRom rom;
        memset(&rom, 0, sizeof(rom));
        if (rom_parse(path, &rom)) {
            printf("CART %u %u %s\n", (unsigned)rom.cart.mapper, (unsigned)rom.cart.submapper, path);
            rom_free(&rom);
        } else {
            printf("CART ? ? %s\n", path);
        }
    }
    return 0;
}

/* ---- Famicom Disk System images (common/nes_fds.h) ---- */
static uint8_t *read_whole_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : 1 << 20;
            uint8_t *grown = (uint8_t *)realloc(buf, cap);
            if (!grown) { free(buf); fclose(f); return NULL; }
            buf = grown;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) break;
    }
    int failed = ferror(f);
    fclose(f);
    if (failed) { free(buf); return NULL; }
    *size = n;
    return buf;
}

static bool fds_open_file(const char *path, uint8_t **data, NesFdsImage *img) {
    size_t size = 0, len = strlen(path);
    *data = read_whole_file(path, &size);
    if (!*data) { fprintf(stderr, "Error: cannot read '%s'\n", path); return false; }
    /* Mesen2 selects the .qd layout by extension; detection covers the rest. */
    bool qd = len >= 3 && path[len-3] == '.' && (path[len-2] | 32) == 'q' && (path[len-1] | 32) == 'd';
    if (nes_fds_image(*data, size, qd ? NES_FDS_QD : NES_FDS_NONE, img)) return true;
    fprintf(stderr, "Error: '%s' is not an FDS image (%s)\n", path,
            img->error == NES_FDS_ERR_NO_SIDES ? "no whole side" : "no FDS header or block 1");
    free(*data);
    *data = NULL;
    return false;
}

static void print_fds_flags(unsigned flags, const char *const *names, unsigned count) {
    bool any = false;
    for (unsigned i = 0; i < count; ++i)
        if (flags & (1u << i)) { printf("%s%s", any ? "|" : "", names[i]); any = true; }
    if (!any) printf("none");
}

static void print_fds_text(const char *s, size_t n) {
    putchar('"');
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c >= 0x20 && c < 0x7F) putchar(c);
        else printf("\\x%02X", c);
    }
    putchar('"');
}

static int fds_info(const char *path) {
    static const char *const img_flags[] = { "truncated", "trailing" };
    static const char *const side_flags[] = { "no_info", "bad_signature", "no_amount",
        "no_data", "overrun", "truncated", "missing_files", "bad_crc", "hidden_files", "tail_data" };
    uint8_t *data;
    NesFdsImage img;
    if (!fds_open_file(path, &data, &img)) return 1;
    printf("FDS %s\n", path);
    printf("IMAGE format=%s sides=%u side_bytes=%u image_bytes=%zu flags=",
           nes_fds_format_name(img.format), img.sides, (unsigned)img.side_bytes, img.size);
    print_fds_flags(img.flags, img_flags, 2);
    printf("\n");
    bool clean = !img.flags;
    for (unsigned side = 0; side < img.sides; ++side) {
        NesFdsSide s;
        NesFdsWalk w;
        NesFdsFile f;
        const NesFdsDiskInfo *d = &s.info;
        bool begun = nes_fds_side_begin(&img, side, &s, &w);
        printf("SIDE %u game=", side);
        print_fds_text(d->game_name, 3);
        printf(" licensee=$%02X type=$%02X revision=$%02X side_number=$%02X disk_number=$%02X"
               " disk_type=$%02X boot_file_id=$%02X mfg_date=%02X%02X%02X country=$%02X"
               " rewrite_count=$%02X actual_side=$%02X disk_type_other=$%02X disk_version=$%02X"
               " file_amount=%u\n",
               d->licensee, d->game_type, d->revision, d->side_number, d->disk_number, d->disk_type,
               d->boot_file_id, d->mfg_date[0], d->mfg_date[1], d->mfg_date[2], d->country,
               d->rewrite_count, d->actual_side, d->disk_type_other, d->disk_version, s.file_amount);
        unsigned files = 0;
        while (begun && nes_fds_next_file(&w, &f)) {
            files++;
            printf("FILE %u %u number=$%02X id=$%02X name=", side, f.index, f.number, f.id);
            print_fds_text(f.name, 8);
            printf(" load=$%04X size=$%04X type=%u(%s) header=$%04X data=", f.load_addr, f.size,
                   f.type, nes_fds_file_type_name(f.type), (unsigned)f.header_pos);
            if (f.has_data) printf("$%04X", (unsigned)f.data_pos); else printf("missing");
            printf(" hidden=%s\n", f.hidden ? "yes" : "no");
        }
        printf("END %u files=%u end=$%04X end_code=$%02X flags=", side, files, (unsigned)w.pos,
               w.pos < img.side_bytes ? nes_fds_byte(&img, side, w.pos, true) : 0);
        print_fds_flags(w.flags, side_flags, 10);
        printf("\n");
        if (w.flags & NES_FDS_SIDE_DEFECTS) clean = false;
    }
    free(data);
    return clean ? 0 : 2;
}

static int fds_stream(const char *path, const char *side_arg, const char *out_path,
                      NesFdsProfile profile, NesFdsCrc crc) {
    uint8_t *data;
    NesFdsImage img;
    char *end;
    unsigned long side = strtoul(side_arg, &end, 10);
    if (!*side_arg || *end) { fprintf(stderr, "Error: bad side '%s'\n", side_arg); return 1; }
    if (!fds_open_file(path, &data, &img)) return 1;
    if (side >= img.sides) {
        fprintf(stderr, "Error: side %lu of %u\n", side, img.sides);
        free(data);
        return 1;
    }
    size_t len = nes_fds_side_stream(&img, (unsigned)side, profile, crc, NULL, 0);
    uint8_t *stream = (uint8_t *)malloc(len);
    FILE *f = stream ? fopen(out_path, "wb") : NULL;
    bool ok = f && nes_fds_side_stream(&img, (unsigned)side, profile, crc, stream, len) == len &&
              fwrite(stream, 1, len, f) == len;
    if (f && fclose(f)) ok = false;
    free(stream);
    free(data);
    if (!ok) { fprintf(stderr, "Error: cannot write '%s'\n", out_path); return 1; }
    printf("STREAM side=%lu bytes=%zu %s\n", side, len, out_path);
    return 0;
}

/* ---- FDS programs: the RAM Adapter, whose ROM is the BIOS ---- */

/* The identity bios/disksys.toml records ([program] size, crc32) for the BIOS
 * at bios_path (<stem>.toml beside it); the known disksys.rom otherwise. */
static void fds_bios_identity(const char *bios_path, uint32_t *crc, uint32_t *size, char *source, size_t n) {
    *crc = NES_FDS_BIOS_CRC32;
    *size = NES_FDS_BIOS_BYTES;
    snprintf(source, n, "built-in (disksys.rom)");
    char toml[1024];
    snprintf(toml, sizeof(toml), "%s", bios_path);
    char *dot = strrchr(toml, '.'), *slash = strrchr(toml, '/'), *bslash = strrchr(toml, '\\');
    if (dot && dot > slash && dot > bslash) *dot = 0;
    strncat(toml, ".toml", sizeof(toml) - strlen(toml) - 1);
    FILE *f = fopen(toml, "r");
    if (!f) return;
    char err[256];
    toml_table_t *root = toml_parse_file(f, err, sizeof(err));
    fclose(f);
    if (!root) { fprintf(stderr, "[NESRecomp] Warning: cannot parse %s: %s\n", toml, err); return; }
    toml_table_t *prog = toml_table_in(root, "program");
    if (prog) {
        toml_datum_t c = toml_string_in(prog, "crc32");
        if (c.ok) { *crc = (uint32_t)strtoul(c.u.s, NULL, 16); free(c.u.s); snprintf(source, n, "%s", toml); }
        toml_datum_t z = toml_int_in(prog, "size");
        if (z.ok) *size = (uint32_t)z.u.i;
    }
    toml_free(root);
}

static void absolute_path(const char *path, char *out, size_t n) {
#ifdef _WIN32
    if (!_fullpath(out, path, n)) snprintf(out, n, "%s", path);
#else
    char *r = realpath(path, NULL);
    snprintf(out, n, "%s", r ? r : path);
    free(r);
#endif
}

/* Build the program's NESRom from the BIOS and describe the media. */
static bool fds_program(const char *image_path, const char *bios_arg, const GameConfig *cfg, NESRom *rom,
                        CycFdsProgram *prog) {
    /* image_path NULL: the BIOS alone, with no disk (--fds-bios-only). */
    char beside[1024] = "bios/disksys.rom";
    if (image_path) {
        uint8_t *data;
        NesFdsImage img;
        if (!fds_open_file(image_path, &data, &img)) return false;
        printf("[NESRecomp] FDS image: %s (%s, %u side%s)\n", image_path, nes_fds_format_name(img.format), img.sides,
               img.sides == 1 ? "" : "s");
        free(data);
        const char *slash = strrchr(image_path, '/'), *bslash = strrchr(image_path, '\\');
        const char *sep = slash > bslash ? slash : bslash;
        snprintf(beside, sizeof(beside), "%.*sbios/disksys.rom", sep ? (int)(sep - image_path + 1) : 0, image_path);
    } else {
        printf("[NESRecomp] FDS: the BIOS alone, no disk\n");
    }
    const char *bios_path = bios_arg ? bios_arg : cfg->fds_bios[0] ? cfg->fds_bios : beside;
    size_t n = 0;
    uint8_t *bios = read_whole_file(bios_path, &n);
    if (!bios) {
        fprintf(stderr, "[NESRecomp] FDS: cannot read the BIOS '%s' (give --fds-bios or game.toml [fds] bios)\n", bios_path);
        return false;
    }
    uint32_t want_crc, want_size;
    char source[1024];
    fds_bios_identity(bios_path, &want_crc, &want_size, source, sizeof(source));
    uint32_t crc = nes_crc32(0, bios, n);
    if (n != want_size || !nes_fds_bios_matches(bios, n, want_crc)) {
        fprintf(stderr, "[NESRecomp] FDS: '%s' is not the expected BIOS (%zu bytes, CRC32 %08X; %s expects "
                        "%u bytes, CRC32 %08X)\n", bios_path, n, crc, source, want_size, want_crc);
        free(bios);
        return false;
    }
    memset(rom, 0, sizeof(*rom));
    nes_fds_cart_info(&rom->cart);
    rom->prg_data = bios;
    rom->prg_banks = 1;
    rom->mapper = NES_FDS_MAPPER;
    rom->nmi_vector = (uint16_t)(bios[0x1FFA] | bios[0x1FFB] << 8);
    rom->reset_vector = (uint16_t)(bios[0x1FFC] | bios[0x1FFD] << 8);
    rom->irq_vector = (uint16_t)(bios[0x1FFE] | bios[0x1FFF] << 8);
    memset(prog, 0, sizeof(*prog));
    NesFdsHleAsk ask;
    const char *bad = NULL;
    if (!nes_fds_hle_parse(cfg->fds_hle, &ask, &bad)) {
        fprintf(stderr, "[NESRecomp] game.toml [fds] hle: unknown word at '%s' (boot-skip, auto-swap, fast-load, auto-insert, all, off, "
                        "no-auto-swap, no-fast-load)\n", bad);
        free(bios);
        return false;
    }
    snprintf(prog->hle, sizeof(prog->hle), "%s", cfg->fds_hle);
    if (cfg->fds_hle[0]) printf("[NESRecomp] FDS HLE default: %s\n", cfg->fds_hle);
    prog->bios_crc32 = crc;
    absolute_path(bios_path, prog->bios_path, sizeof(prog->bios_path));
    if (image_path) absolute_path(image_path, prog->image_path, sizeof(prog->image_path));
    printf("[NESRecomp] FDS BIOS: %s (CRC32 %08X, identity from %s)\n", bios_path, crc, source);
    printf("[NESRecomp] Vectors: NMI=$%04X  RESET=$%04X  IRQ=$%04X\n", rom->nmi_vector, rom->reset_vector,
           rom->irq_vector);
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *rom_path  = NULL;
    const char *game_path = NULL;
    const char *proposal_out = NULL;
    const char *prefix_override = NULL;
    const char *cycle_seed_override = NULL, *cycle_capture_override = NULL;
    bool cycle_accurate = false;
    const char *fds_info_path = NULL;
    const char *fds_stream_args[3] = { NULL, NULL, NULL };
    NesFdsProfile fds_profile = NES_FDS_PROFILE_MESEN099;
    NesFdsCrc fds_crc = NES_FDS_CRC_COMPUTED;
    const char *fds_bios_arg = NULL;
    bool fds_bios_only = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "--cart-info") == 0) {
            return cart_info();
        } else if (strcmp(argv[i], "--fds-info") == 0 && i+1 < argc) {
            fds_info_path = argv[++i];
        } else if (strcmp(argv[i], "--fds-stream") == 0 && i+3 < argc) {
            fds_stream_args[0] = argv[++i];
            fds_stream_args[1] = argv[++i];
            fds_stream_args[2] = argv[++i];
        } else if (strcmp(argv[i], "--fds-profile") == 0 && i+1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "mesen099") == 0) fds_profile = NES_FDS_PROFILE_MESEN099;
            else if (strcmp(v, "mesen2") == 0) fds_profile = NES_FDS_PROFILE_MESEN2;
            else { fprintf(stderr, "Error: --fds-profile '%s'\n", v); return 1; }
        } else if (strcmp(argv[i], "--fds-crc") == 0 && i+1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "computed") == 0) fds_crc = NES_FDS_CRC_COMPUTED;
            else if (strcmp(v, "mesen") == 0) fds_crc = NES_FDS_CRC_MESEN;
            else { fprintf(stderr, "Error: --fds-crc '%s'\n", v); return 1; }
        } else if (strcmp(argv[i], "--fds-bios") == 0 && i+1 < argc) {
            fds_bios_arg = argv[++i];
        } else if (strcmp(argv[i], "--fds-bios-only") == 0) {
            fds_bios_only = true;
        } else if (strcmp(argv[i], "--cycle-accurate") == 0) {
            cycle_accurate = true;
        } else if (strcmp(argv[i], "--emit-cycle-interpreter") == 0 && i+1 < argc) {
            return cyc_codegen_emit_interpreter(argv[++i]) ? 0 : 1;
        } else if (strcmp(argv[i], "--game") == 0 && i+1 < argc) {
            game_path = argv[++i];
        } else if (strcmp(argv[i], "--output-prefix") == 0 && i+1 < argc) {
            prefix_override = argv[++i];
        } else if (strcmp(argv[i], "--cycle-seed-file") == 0 && i+1 < argc) {
            cycle_seed_override = argv[++i];
        } else if (strcmp(argv[i], "--cycle-capture-file") == 0 && i+1 < argc) {
            cycle_capture_override = argv[++i];
        } else if (strcmp(argv[i], "--proposal-out") == 0 && i+1 < argc) {
            proposal_out = argv[++i];
        } else if (!rom_path) {
            rom_path = argv[i];
        } else {
            fprintf(stderr, "Error: unexpected argument '%s'\n\n", argv[i]);
            print_usage();
            return 1;
        }
    }

    if (fds_info_path) return fds_info(fds_info_path);
    if (fds_stream_args[0])
        return fds_stream(fds_stream_args[0], fds_stream_args[1], fds_stream_args[2], fds_profile, fds_crc);

    /* Auto-detect game.toml in current directory if not specified */
    if (!game_path) {
        FILE *f = fopen("game.toml", "r");
        if (f) { fclose(f); game_path = "game.toml"; }
    }

    /* The BIOS alone: no ROM, no disk. */
    if (fds_bios_only) {
        static GameConfig bios_cfg;
        if (game_path) game_config_load(&bios_cfg, game_path);
        else game_config_init_empty(&bios_cfg);
        if (!fds_bios_arg && !bios_cfg.fds_bios[0]) {
            fprintf(stderr, "Error: --fds-bios-only needs --fds-bios (or game.toml [fds] bios)\n");
            return 1;
        }
        if (!(cycle_accurate || bios_cfg.cycle_accurate)) {
            fprintf(stderr, "Error: --fds-bios-only builds on the cycle backend only (--cycle-accurate)\n");
            return 1;
        }
        char prefix[128];
        snprintf(prefix, sizeof(prefix), "%s", prefix_override ? prefix_override
                                                : bios_cfg.output_prefix[0] ? bios_cfg.output_prefix : "fdsbios");
        NESRom rom = {0};
        static CycFdsProgram prog;
        if (!fds_program(NULL, fds_bios_arg, &bios_cfg, &rom, &prog)) return 1;
        bool ok = cyc_codegen_emit(&rom, &bios_cfg, prefix, &prog);
        rom_free(&rom);
        return ok ? 0 : 1;
    }

    /* An FDS title may name its disk in game.toml ([fds] image) instead. */
    static GameConfig fds_probe_cfg;
    if (!rom_path && game_path && game_config_load(&fds_probe_cfg, game_path) && fds_probe_cfg.fds_image[0])
        rom_path = fds_probe_cfg.fds_image;

    if (!rom_path) {
        fprintf(stderr, "Error: no ROM file specified.\n\n");
        print_usage();
        return 1;
    }

    printf("[NESRecomp] Loading ROM: %s\n", rom_path);

    /* Parse ROM */
    NESRom rom = {0};
    bool fds_input = false;
    {
        /* A disk image is not an iNES file: recognize it first. */
        size_t probe_size = 0;
        uint8_t *probe = read_whole_file(rom_path, &probe_size);
        NesFdsImage probe_img;
        size_t len = strlen(rom_path);
        bool qd = len > 3 && (!strcmp(rom_path + len - 3, ".qd") || !strcmp(rom_path + len - 3, ".QD"));
        fds_input = probe && (probe_size < 4 || memcmp(probe, "NES\x1a", 4)) &&
                    nes_fds_image(probe, probe_size, qd ? NES_FDS_QD : NES_FDS_NONE, &probe_img);
        free(probe);
    }
    if (!fds_input && !rom_parse(rom_path, &rom)) {
        fprintf(stderr, "[NESRecomp] Failed to parse ROM\n");
        return 1;
    } else if (!fds_input) {
        printf("[NESRecomp] ROM: %d PRG banks x 16KB, Mapper %d\n",
               rom.prg_banks, rom.mapper);
        printf("[NESRecomp] Vectors: NMI=$%04X  RESET=$%04X  IRQ=$%04X\n",
               rom.nmi_vector, rom.reset_vector, rom.irq_vector);
    }

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

    /* Determine output prefix: --output-prefix wins (lets a multi-variant game
     * regen the same game.toml under distinct prefixes so per-bank split files
     * never collide — e.g. zelda_stock_* vs zelda_hd_*), then game.toml's
     * output_prefix, else the ROM basename. */
    char output_prefix[128];
    if (prefix_override && prefix_override[0]) {
        snprintf(output_prefix, sizeof(output_prefix), "%s", prefix_override);
    } else if (cfg.output_prefix[0]) {
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

    if (cycle_seed_override) {
        if (strlen(cycle_seed_override) >= sizeof(cfg.cycle_seed_file)) {
            fprintf(stderr, "Error: cycle seed path is too long\n");
            return 1;
        }
        strcpy(cfg.cycle_seed_file, cycle_seed_override);
    }
    if (cycle_capture_override) {
        if (strlen(cycle_capture_override) >= sizeof(cfg.cycle_capture_file)) {
            fprintf(stderr, "Error: cycle capture path is too long\n");
            return 1;
        }
        strcpy(cfg.cycle_capture_file, cycle_capture_override);
    }
    if (fds_input) {
        if (!(cycle_accurate || cfg.cycle_accurate)) {
            fprintf(stderr, "[NESRecomp] '%s' is a Famicom Disk System image: FDS titles build on the cycle "
                            "backend only (--cycle-accurate or [game] cycle_accurate = true)\n", rom_path);
            return 1;
        }
        static CycFdsProgram fds_prog;
        if (!fds_program(rom_path, fds_bios_arg, &cfg, &rom, &fds_prog)) return 1;
        bool ok = cyc_codegen_emit(&rom, &cfg, output_prefix, &fds_prog);
        rom_free(&rom);
        return ok ? 0 : 1;
    }
    if (cycle_accurate || cfg.cycle_accurate)
        return cyc_codegen_emit(&rom, &cfg, output_prefix, NULL) ? 0 : 1;

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

    /* Set up coverage collection: discovery hooks see g_active_coverage and
     * deduplicate rejection sites by (bank, addr).  Cleared after the run. */
    static Coverage cov;
    coverage_init(&cov);
    g_active_coverage = &cov;

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

    /* Remove clearly-bogus entries that would cause codegen to reference
     * undefined symbols.  Targets: CONTROL-only with evidence_count <= 1
     * that point to zero-fill (BRK opcode).  These pass function_list_contains
     * but never get emitted as C code by the full emission filter. */
    {
        int dst = 0;
        for (int i = 0; i < funcs.count; i++) {
            const FunctionEntry *fe = &funcs.entries[i];
            bool reject = false;
            if (fe->kind == FUNCTION_KIND_STANDALONE &&
                fe->source_flags == FUNCTION_SOURCE_CONTROL &&
                fe->evidence_count <= 1) {
                /* Check if the target is BRK ($00) — zero-fill */
                int rb = (fe->addr >= 0xC000) ? rom.prg_banks - 1 : fe->bank;
                if (rom_read(&rom, rb, fe->addr) == 0x00)
                    reject = true;
            }
            /* Warn about entries in data regions — may indicate game.toml
             * data_region overlaps with real code */
            if (rom_addr_in_data_region(&cfg, fe->bank, fe->addr))
                printf("[NESRecomp] Warning: function $%04X bank=%d is inside a data_region\n",
                       fe->addr, fe->bank);
            if (!reject)
                funcs.entries[dst++] = funcs.entries[i];
        }
        int removed = funcs.count - dst;
        funcs.count = dst;
        if (removed > 0)
            printf("[NESRecomp] Filtered %d bogus entries before codegen\n", removed);
    }

    /* Load symbol table (optional) */
    SymbolTable symtab = {0};
    if (cfg.symbol_file[0]) {
        /* Resolve symbol_file relative to game.toml directory */
        char sym_path[512];
        if (game_path) {
            const char *slash = NULL;
            const char *p = game_path;
            while (*p) { if (*p == '/' || *p == '\\') slash = p; p++; }
            if (slash) {
                size_t dir_len = (size_t)(slash - game_path) + 1;
                if (dir_len + strlen(cfg.symbol_file) < sizeof(sym_path)) {
                    memcpy(sym_path, game_path, dir_len);
                    strcpy(sym_path + dir_len, cfg.symbol_file);
                } else {
                    strncpy(sym_path, cfg.symbol_file, sizeof(sym_path) - 1);
                }
            } else {
                strncpy(sym_path, cfg.symbol_file, sizeof(sym_path) - 1);
            }
        } else {
            strncpy(sym_path, cfg.symbol_file, sizeof(sym_path) - 1);
        }
        sym_path[sizeof(sym_path) - 1] = '\0';
        if (symbol_table_load(&symtab, sym_path))
            printf("[NESRecomp] Symbols: %d entries from %s\n", symtab.count, sym_path);
        else
            fprintf(stderr, "[NESRecomp] Warning: could not load symbol file '%s'\n", sym_path);
    }

    /* Emit C */
    ensure_output_dir_exists();
    char out_full[256], out_dispatch[256];
    snprintf(out_full,     sizeof(out_full),     "generated/%s_full.c",     output_prefix);
    snprintf(out_dispatch, sizeof(out_dispatch), "generated/%s_dispatch.c", output_prefix);

    if (!codegen_emit(&rom, &funcs, out_full, out_dispatch, &at, &cfg, &symtab)) {
        fprintf(stderr, "[NESRecomp] Code generation failed\n");
        rom_free(&rom);
        function_list_free(&funcs);
        return 1;
    }

    printf("[NESRecomp] Done. Output:\n  %s\n  %s\n", out_full, out_dispatch);

    /* Coverage report: histogram + BRK/JMP-bug walk over emitted functions,
     * combined with discovery-time rejection sites already accumulated. */
    coverage_collect_from_funcs(&rom, &funcs, &cfg, &cov);
    char out_coverage[256];
    snprintf(out_coverage, sizeof(out_coverage), "generated/%s_coverage.txt", output_prefix);
    if (coverage_write_text_report(&cov, &rom, out_coverage)) {
        printf("[NESRecomp] Coverage: %s  (%d functions analyzed, %llu reachable insns,\n"
               "             %d BRK sites, %d JMP (\\$xxFF) sites,\n"
               "             %d illegal-rejected, %d BRK-rejected discovery targets)\n",
               out_coverage,
               cov.analyzed_function_count,
               (unsigned long long)cov.reachable_insn_total,
               cov.brk_site_unique_count,
               cov.jmp_indirect_xxff_site_unique_count,
               cov.rejected_illegal_site_unique_count,
               cov.rejected_brk_site_unique_count);
    } else {
        fprintf(stderr, "[NESRecomp] Warning: failed to write coverage report '%s'\n",
                out_coverage);
    }
    g_active_coverage = NULL;

    rom_free(&rom);
    function_list_free(&funcs);
    annotations_free(&at);
    symbol_table_free(&symtab);
    return 0;
}
