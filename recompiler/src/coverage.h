/*
 * coverage.h — per-ROM CPU compatibility report.
 *
 * Walks the discovered FunctionList and accumulates a reachable-code opcode
 * histogram, BRK sites, and JMP ($xxFF) page-wrap erratum sites.  Also tracks
 * deduplicated discovery-time rejections (illegal-opcode / BRK-at-entry).
 *
 * Runtime miss counters (dispatch misses, inline-dispatch misses,
 * call_by_address unknown targets) are reported separately by the runner's
 * debug TCP surface — that part is in dispatch-miss policy work.
 *
 * Output: generated/<prefix>_coverage.txt
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "rom_parser.h"
#include "function_finder.h"
#include "game_config.h"

typedef struct {
    int      bank;
    uint16_t addr;
} CovSite;

/* Logical opcode classification for histogram presentation.  This is finer
 * than OpMnemonic because the decoder lumps every unofficial opcode into
 * MN_ILLEGAL — the report needs to tell the user *which* unofficial opcodes
 * the ROM actually uses (e.g. "$EB SBC alias: 12 sites" vs "$93 SHA: 0"). */
typedef enum {
    CIK_OFFICIAL = 0,
    CIK_OFFICIAL_BRK,        /* official but treated specially in the report */
    CIK_UNOFFICIAL_LAX,      /* implemented */
    CIK_UNOFFICIAL_NOP,      /* unofficial NOP / DOP / TOP */
    CIK_UNOFFICIAL_SBC,      /* $EB SBC #imm alias */
    CIK_UNOFFICIAL_SAX,
    CIK_UNOFFICIAL_DCP,
    CIK_UNOFFICIAL_ISC,
    CIK_UNOFFICIAL_SLO,
    CIK_UNOFFICIAL_RLA,
    CIK_UNOFFICIAL_SRE,
    CIK_UNOFFICIAL_RRA,
    CIK_UNOFFICIAL_AAC,      /* ANC */
    CIK_UNOFFICIAL_ALR,
    CIK_UNOFFICIAL_ARR,
    CIK_UNOFFICIAL_AXS,
    CIK_UNOFFICIAL_XAA,
    CIK_UNOFFICIAL_LAS,
    CIK_UNOFFICIAL_TAS,
    CIK_UNOFFICIAL_SHA,
    CIK_UNOFFICIAL_SHX,
    CIK_UNOFFICIAL_SHY,
    CIK_UNOFFICIAL_KIL,
    CIK_COUNT
} CovInsnKind;

/* Per-category capacity for the listed (bank, addr) sites. The unique-count
 * field on each category is unbounded; the array just retains the first N for
 * the report. Generous enough for real ROMs, bounded for memory safety. */
#define COV_MAX_SITES 1024

typedef struct {
    /* Reachable-code opcode byte histogram (one bucket per opcode byte). */
    uint32_t opcode_count[256];
    /* Reachable instruction total (sum of opcode_count). */
    uint64_t reachable_insn_total;
    /* Number of distinct (bank, addr) function bodies the histogram walked. */
    int      analyzed_function_count;

    /* Reachable BRK ($00) sites. unique_count is unbounded; the listed array
     * retains the first COV_MAX_SITES distinct (bank, addr) entries. */
    CovSite  brk_sites[COV_MAX_SITES];
    int      brk_site_listed;
    int      brk_site_unique_count;

    /* JMP ($xxFF) page-wrap erratum sites. jmp_indirect_xxff_indirects[i] is
     * the indirect operand of site[i]. */
    CovSite  jmp_indirect_xxff_sites[COV_MAX_SITES];
    uint16_t jmp_indirect_xxff_indirects[COV_MAX_SITES];
    int      jmp_indirect_xxff_site_listed;
    int      jmp_indirect_xxff_site_unique_count;

    /* Discovery-time rejection sites, deduplicated by (bank, addr). */
    CovSite  rejected_illegal_sites[COV_MAX_SITES];
    int      rejected_illegal_site_listed;
    int      rejected_illegal_site_unique_count;
    CovSite  rejected_brk_sites[COV_MAX_SITES];
    int      rejected_brk_site_listed;
    int      rejected_brk_site_unique_count;

    /* Emitted-function set populated during codegen.  Used to filter the
     * histogram walk to actually-emitted code, excluding functions that
     * were discovered but rejected by the emission filter
     * (entry_emits_standalone). */
    CovSite  emitted_funcs[MAX_FUNCTIONS];
    int      emitted_func_count;
} Coverage;

/* Global pointer the function_finder hooks read.  Set before
 * function_finder_run, cleared after.  NULL disables collection. */
extern Coverage *g_active_coverage;

void coverage_init(Coverage *cov);

/* Discovery-time hooks (no-op if g_active_coverage is NULL). */
void coverage_record_rejected_target_illegal(int bank, uint16_t addr);
void coverage_record_rejected_target_brk(int bank, uint16_t addr);

/* Codegen-time hook: called from code_generator.c::codegen_emit for every
 * function that passes entry_emits_standalone (i.e. actually written to the
 * generated C). Used by coverage_collect_from_funcs to filter out
 * discovery false-positives that the emitter rejected. No-op if
 * g_active_coverage is NULL. */
void coverage_record_emitted_function(int bank, uint16_t addr);

/* Post-discovery histogram + BRK/JMP-bug walk over every emit-eligible
 * function in `funcs`. Deduplicates by (bank, addr) before walking. */
void coverage_collect_from_funcs(const NESRom *rom, const FunctionList *funcs,
                                 const GameConfig *cfg, Coverage *cov);

/* Classification helpers. */
CovInsnKind coverage_classify_opcode(uint8_t opcode);
const char *coverage_insn_kind_name(CovInsnKind k);
/* "" for official opcodes; mnemonic string ("SLO", "DCP", "SBC") for unofficial. */
const char *coverage_unofficial_mnemonic(uint8_t opcode);

/* Write the human-readable text report. */
bool coverage_write_text_report(const Coverage *cov, const NESRom *rom,
                                const char *path);
