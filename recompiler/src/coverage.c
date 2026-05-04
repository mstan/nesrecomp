/*
 * coverage.c — per-ROM CPU compatibility report.  See coverage.h.
 */
#include "coverage.h"
#include "cpu6502_decoder.h"
#include <stdio.h>
#include <string.h>

Coverage *g_active_coverage = NULL;

void coverage_init(Coverage *cov) {
    memset(cov, 0, sizeof(*cov));
}

/* ---- discovery-time rejection hooks ---- */

static bool cov_site_present(const CovSite *sites, int listed, int bank, uint16_t addr) {
    for (int i = 0; i < listed; i++)
        if (sites[i].bank == bank && sites[i].addr == addr) return true;
    return false;
}

/* Add a (bank, addr) site, deduplicating against the listed prefix.
 * unique_count tracks all distinct sites (unbounded); listed grows up to
 * COV_MAX_SITES, then further unique sites still bump unique_count but
 * are not retained for the per-site report. */
static void cov_site_add(CovSite *sites, int *listed, int *unique_count,
                         int bank, uint16_t addr) {
    if (*listed > 0 && cov_site_present(sites, *listed, bank, addr)) return;
    /* Beyond the listed prefix we can't dedup cheaply, so the unique_count
     * is exact only while we're under the cap. Past it, unique_count
     * counts all remaining additions — a slight over-count, but the
     * presence check on the listed prefix means duplicates that fall
     * within the first COV_MAX_SITES distinct sites are still rejected. */
    if (*listed < COV_MAX_SITES) {
        sites[*listed].bank = bank;
        sites[*listed].addr = addr;
        (*listed)++;
    }
    (*unique_count)++;
}

void coverage_record_rejected_target_illegal(int bank, uint16_t addr) {
    Coverage *c = g_active_coverage;
    if (!c) return;
    cov_site_add(c->rejected_illegal_sites,
                 &c->rejected_illegal_site_listed,
                 &c->rejected_illegal_site_unique_count, bank, addr);
}

void coverage_record_rejected_target_brk(int bank, uint16_t addr) {
    Coverage *c = g_active_coverage;
    if (!c) return;
    cov_site_add(c->rejected_brk_sites,
                 &c->rejected_brk_site_listed,
                 &c->rejected_brk_site_unique_count, bank, addr);
}

void coverage_record_emitted_function(int bank, uint16_t addr) {
    Coverage *c = g_active_coverage;
    if (!c) return;
    if (c->emitted_func_count >= MAX_FUNCTIONS) return;
    /* Dedup against earlier records — codegen may call this for both the
     * canonical body and merge-range secondaries. */
    for (int i = 0; i < c->emitted_func_count; i++)
        if (c->emitted_funcs[i].bank == bank && c->emitted_funcs[i].addr == addr) return;
    c->emitted_funcs[c->emitted_func_count].bank = bank;
    c->emitted_funcs[c->emitted_func_count].addr = addr;
    c->emitted_func_count++;
}

/* ---- opcode classification ----
 * Authority: NESdev unofficial-opcode reference and the comments in
 * cpu6502_decoder.c.  Official opcodes return CIK_OFFICIAL; BRK returns
 * CIK_OFFICIAL_BRK so the report can highlight it separately. */
CovInsnKind coverage_classify_opcode(uint8_t op) {
    if (op == 0x00) return CIK_OFFICIAL_BRK;
    /* Unofficial bytes (98 of them) — by group from the decoder map. */
    switch (op) {
        /* KIL (a.k.a. STP/JAM) */
        case 0x02: case 0x12: case 0x22: case 0x32: case 0x42:
        case 0x52: case 0x62: case 0x72: case 0x92: case 0xB2:
        case 0xD2: case 0xF2:
            return CIK_UNOFFICIAL_KIL;
        /* SLO */
        case 0x03: case 0x07: case 0x0F: case 0x13: case 0x17: case 0x1B: case 0x1F:
            return CIK_UNOFFICIAL_SLO;
        /* RLA */
        case 0x23: case 0x27: case 0x2F: case 0x33: case 0x37: case 0x3B: case 0x3F:
            return CIK_UNOFFICIAL_RLA;
        /* SRE */
        case 0x43: case 0x47: case 0x4F: case 0x53: case 0x57: case 0x5B: case 0x5F:
            return CIK_UNOFFICIAL_SRE;
        /* RRA */
        case 0x63: case 0x67: case 0x6F: case 0x73: case 0x77: case 0x7B: case 0x7F:
            return CIK_UNOFFICIAL_RRA;
        /* SAX */
        case 0x83: case 0x87: case 0x8F: case 0x97:
            return CIK_UNOFFICIAL_SAX;
        /* LAX (implemented mnemonic in decoder, but we still flag for the report) */
        case 0xA3: case 0xA7: case 0xAB: case 0xAF: case 0xB3: case 0xB7: case 0xBF:
            return CIK_UNOFFICIAL_LAX;
        /* DCP */
        case 0xC3: case 0xC7: case 0xCF: case 0xD3: case 0xD7: case 0xDB: case 0xDF:
            return CIK_UNOFFICIAL_DCP;
        /* ISC (a.k.a. ISB) */
        case 0xE3: case 0xE7: case 0xEF: case 0xF3: case 0xF7: case 0xFB: case 0xFF:
            return CIK_UNOFFICIAL_ISC;
        /* AAC / ANC */
        case 0x0B: case 0x2B:
            return CIK_UNOFFICIAL_AAC;
        /* ALR */
        case 0x4B:
            return CIK_UNOFFICIAL_ALR;
        /* ARR */
        case 0x6B:
            return CIK_UNOFFICIAL_ARR;
        /* AXS / SBX */
        case 0xCB:
            return CIK_UNOFFICIAL_AXS;
        /* XAA */
        case 0x8B:
            return CIK_UNOFFICIAL_XAA;
        /* LAS */
        case 0xBB:
            return CIK_UNOFFICIAL_LAS;
        /* TAS */
        case 0x9B:
            return CIK_UNOFFICIAL_TAS;
        /* SHA */
        case 0x93: case 0x9F:
            return CIK_UNOFFICIAL_SHA;
        /* SHX */
        case 0x9E:
            return CIK_UNOFFICIAL_SHX;
        /* SHY */
        case 0x9C:
            return CIK_UNOFFICIAL_SHY;
        /* SBC #imm alias */
        case 0xEB:
            return CIK_UNOFFICIAL_SBC;
        /* DOP / TOP / 1-byte unofficial NOP */
        case 0x04: case 0x14: case 0x34: case 0x44: case 0x54: case 0x64: case 0x74:
        case 0x80: case 0x82: case 0x89: case 0xC2: case 0xD4: case 0xE2: case 0xF4:
        case 0x0C: case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
        case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
            return CIK_UNOFFICIAL_NOP;
        default:
            return CIK_OFFICIAL;
    }
}

const char *coverage_insn_kind_name(CovInsnKind k) {
    switch (k) {
        case CIK_OFFICIAL:        return "OFFICIAL";
        case CIK_OFFICIAL_BRK:    return "BRK (official)";
        case CIK_UNOFFICIAL_LAX:  return "LAX (unofficial, implemented)";
        case CIK_UNOFFICIAL_NOP:  return "NOP/DOP/TOP (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_SBC:  return "SBC #imm alias \\$EB (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_SAX:  return "SAX (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_DCP:  return "DCP (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_ISC:  return "ISC (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_SLO:  return "SLO (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_RLA:  return "RLA (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_SRE:  return "SRE (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_RRA:  return "RRA (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_AAC:  return "AAC/ANC (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_ALR:  return "ALR (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_ARR:  return "ARR (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_AXS:  return "AXS (unofficial, sized-skip)";
        case CIK_UNOFFICIAL_XAA:  return "XAA (unofficial UNSTABLE, sized-skip)";
        case CIK_UNOFFICIAL_LAS:  return "LAS (unofficial UNSTABLE, sized-skip)";
        case CIK_UNOFFICIAL_TAS:  return "TAS (unofficial UNSTABLE, sized-skip)";
        case CIK_UNOFFICIAL_SHA:  return "SHA (unofficial UNSTABLE, sized-skip)";
        case CIK_UNOFFICIAL_SHX:  return "SHX (unofficial UNSTABLE, sized-skip)";
        case CIK_UNOFFICIAL_SHY:  return "SHY (unofficial UNSTABLE, sized-skip)";
        case CIK_UNOFFICIAL_KIL:  return "KIL/STP/JAM (unofficial HALT)";
        default:                  return "?";
    }
}

const char *coverage_unofficial_mnemonic(uint8_t op) {
    CovInsnKind k = coverage_classify_opcode(op);
    switch (k) {
        case CIK_UNOFFICIAL_LAX: return "LAX";
        case CIK_UNOFFICIAL_NOP: return "NOP*";
        case CIK_UNOFFICIAL_SBC: return "SBC*";
        case CIK_UNOFFICIAL_SAX: return "SAX";
        case CIK_UNOFFICIAL_DCP: return "DCP";
        case CIK_UNOFFICIAL_ISC: return "ISC";
        case CIK_UNOFFICIAL_SLO: return "SLO";
        case CIK_UNOFFICIAL_RLA: return "RLA";
        case CIK_UNOFFICIAL_SRE: return "SRE";
        case CIK_UNOFFICIAL_RRA: return "RRA";
        case CIK_UNOFFICIAL_AAC: return "AAC";
        case CIK_UNOFFICIAL_ALR: return "ALR";
        case CIK_UNOFFICIAL_ARR: return "ARR";
        case CIK_UNOFFICIAL_AXS: return "AXS";
        case CIK_UNOFFICIAL_XAA: return "XAA";
        case CIK_UNOFFICIAL_LAS: return "LAS";
        case CIK_UNOFFICIAL_TAS: return "TAS";
        case CIK_UNOFFICIAL_SHA: return "SHA";
        case CIK_UNOFFICIAL_SHX: return "SHX";
        case CIK_UNOFFICIAL_SHY: return "SHY";
        case CIK_UNOFFICIAL_KIL: return "KIL";
        default: return "";
    }
}

/* ---- post-discovery walk ---- */

void coverage_collect_from_funcs(const NESRom *rom, const FunctionList *funcs,
                                 const GameConfig *cfg, Coverage *cov) {
    static uint16_t insn_addrs[MAX_INSNS_PER_FUNC];
    int fixed_bank = rom->prg_banks - 1;
    (void)funcs;  /* Walk uses the recorded emitted_funcs set, not the raw
                   * FunctionList. funcs is kept in the API for symmetry and
                   * for future callers that want to walk pre-emission. */

    for (int fi = 0; fi < cov->emitted_func_count; fi++) {
        int bank = cov->emitted_funcs[fi].bank;
        uint16_t addr = cov->emitted_funcs[fi].addr;
        if (addr < 0x8000) continue;
        cov->analyzed_function_count++;

        int insn_count = 0;
        scan_function_boundaries(rom, addr, bank, cfg,
                                 insn_addrs, &insn_count, MAX_INSNS_PER_FUNC);
        for (int ii = 0; ii < insn_count; ii++) {
            uint16_t pc = insn_addrs[ii];
            int read_bank = (pc >= 0xC000) ? fixed_bank : bank;
            uint8_t op = rom_read(rom, read_bank, pc);
            cov->opcode_count[op]++;
            cov->reachable_insn_total++;

            if (op == 0x00) {
                cov_site_add(cov->brk_sites, &cov->brk_site_listed,
                             &cov->brk_site_unique_count, bank, pc);
            } else if (op == 0x6C) {
                /* JMP (abs) — check for $xxFF page-wrap erratum */
                uint8_t lo = rom_read(rom, read_bank, pc + 1);
                uint8_t hi = rom_read(rom, read_bank, pc + 2);
                uint16_t indirect = (uint16_t)lo | ((uint16_t)hi << 8);
                if ((indirect & 0xFF) == 0xFF) {
                    bool dup = false;
                    for (int j = 0; j < cov->jmp_indirect_xxff_site_listed; j++) {
                        if (cov->jmp_indirect_xxff_sites[j].bank == bank &&
                            cov->jmp_indirect_xxff_sites[j].addr == pc) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        if (cov->jmp_indirect_xxff_site_listed < COV_MAX_SITES) {
                            int n = cov->jmp_indirect_xxff_site_listed;
                            cov->jmp_indirect_xxff_sites[n].bank = bank;
                            cov->jmp_indirect_xxff_sites[n].addr = pc;
                            cov->jmp_indirect_xxff_indirects[n] = indirect;
                            cov->jmp_indirect_xxff_site_listed++;
                        }
                        cov->jmp_indirect_xxff_site_unique_count++;
                    }
                }
            }
        }
    }
}

/* ---- text report ---- */

static void write_site_list(FILE *f, const char *header,
                            const CovSite *sites, int listed, int unique_count) {
    if (unique_count <= listed) {
        fprintf(f, "  %s: unique=%d\n", header, unique_count);
    } else {
        fprintf(f, "  %s: unique=%d  (listing first %d)\n",
                header, unique_count, listed);
    }
    for (int i = 0; i < listed; i++)
        fprintf(f, "    bank=%-3d  addr=$%04X\n", sites[i].bank, sites[i].addr);
}

bool coverage_write_text_report(const Coverage *cov, const NESRom *rom,
                                const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "# NESRecomp coverage report\n");
    fprintf(f, "# ROM: %d PRG banks x 16KB, mapper %d\n", rom->prg_banks, rom->mapper);
    fprintf(f, "# Analyzed functions: %d  Reachable instructions: %llu\n\n",
            cov->analyzed_function_count,
            (unsigned long long)cov->reachable_insn_total);

    /* --- per-kind summary --- */
    uint64_t kind_count[CIK_COUNT] = {0};
    int kind_distinct_opcodes[CIK_COUNT] = {0};
    bool kind_opcode_seen[CIK_COUNT][256] = {0};
    for (int op = 0; op < 256; op++) {
        if (cov->opcode_count[op] == 0) continue;
        CovInsnKind k = coverage_classify_opcode((uint8_t)op);
        kind_count[k] += cov->opcode_count[op];
        if (!kind_opcode_seen[k][op]) {
            kind_opcode_seen[k][op] = true;
            kind_distinct_opcodes[k]++;
        }
    }

    fprintf(f, "## Reachable opcode classes\n");
    for (int k = 0; k < CIK_COUNT; k++) {
        if (kind_count[k] == 0 && k != CIK_OFFICIAL) continue;
        fprintf(f, "  %-50s  count=%-8llu  distinct_bytes=%d\n",
                coverage_insn_kind_name((CovInsnKind)k),
                (unsigned long long)kind_count[k],
                kind_distinct_opcodes[k]);
    }
    fprintf(f, "\n");

    /* --- unofficial opcode call-out (per-byte) --- */
    fprintf(f, "## Unofficial opcodes seen (per byte)\n");
    int unofficial_seen = 0;
    for (int op = 0; op < 256; op++) {
        if (cov->opcode_count[op] == 0) continue;
        CovInsnKind k = coverage_classify_opcode((uint8_t)op);
        if (k == CIK_OFFICIAL || k == CIK_OFFICIAL_BRK) continue;
        const char *mn = coverage_unofficial_mnemonic((uint8_t)op);
        fprintf(f, "  $%02X  %-5s  %u sites\n", op, mn, cov->opcode_count[op]);
        unofficial_seen++;
    }
    if (unofficial_seen == 0)
        fprintf(f, "  (none)\n");
    fprintf(f, "\n");

    /* --- BRK sites --- */
    fprintf(f, "## BRK ($00) reachable sites\n");
    write_site_list(f, "count", cov->brk_sites,
                    cov->brk_site_listed, cov->brk_site_unique_count);
    fprintf(f, "\n");

    /* --- JMP ($xxFF) sites --- */
    fprintf(f, "## JMP (\\$xxFF) page-wrap erratum sites\n");
    if (cov->jmp_indirect_xxff_site_unique_count <= cov->jmp_indirect_xxff_site_listed)
        fprintf(f, "  count: unique=%d\n", cov->jmp_indirect_xxff_site_unique_count);
    else
        fprintf(f, "  count: unique=%d  (listing first %d)\n",
                cov->jmp_indirect_xxff_site_unique_count,
                cov->jmp_indirect_xxff_site_listed);
    for (int i = 0; i < cov->jmp_indirect_xxff_site_listed; i++) {
        fprintf(f, "    bank=%-3d  addr=$%04X  indirect=$%04X (hi-byte fetched from $%04X)\n",
                cov->jmp_indirect_xxff_sites[i].bank,
                cov->jmp_indirect_xxff_sites[i].addr,
                cov->jmp_indirect_xxff_indirects[i],
                (uint16_t)(cov->jmp_indirect_xxff_indirects[i] & 0xFF00));
    }
    fprintf(f, "\n");

    /* --- discovery rejections --- */
    fprintf(f, "## Discovery rejections (deduplicated by (bank, addr))\n");
    write_site_list(f, "rejected_due_to_illegal_opcode",
                    cov->rejected_illegal_sites,
                    cov->rejected_illegal_site_listed,
                    cov->rejected_illegal_site_unique_count);
    write_site_list(f, "rejected_due_to_brk_at_entry",
                    cov->rejected_brk_sites,
                    cov->rejected_brk_site_listed,
                    cov->rejected_brk_site_unique_count);
    fprintf(f, "\n");

    /* --- full per-byte histogram (compact, only non-zero) --- */
    fprintf(f, "## Full opcode histogram (non-zero buckets)\n");
    for (int op = 0; op < 256; op++) {
        if (cov->opcode_count[op] == 0) continue;
        const OpcodeEntry *e = &g_opcode_table[op];
        const char *unof = coverage_unofficial_mnemonic((uint8_t)op);
        if (unof[0])
            fprintf(f, "  $%02X  %-3s  %-5s mode=%-4s  %u\n",
                    op, mnemonic_name(e->mnemonic), unof,
                    addrmode_name(e->addr_mode), cov->opcode_count[op]);
        else
            fprintf(f, "  $%02X  %-3s        mode=%-4s  %u\n",
                    op, mnemonic_name(e->mnemonic),
                    addrmode_name(e->addr_mode), cov->opcode_count[op]);
    }

    fclose(f);
    return true;
}
