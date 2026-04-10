/*
 * function_finder.c — JSR/RTS graph walk for function boundary detection
 *
 * Strategy:
 * 1. Start from RESET, NMI, IRQ vectors (all in fixed bank 15)
 * 2. Linear sweep: decode each instruction
 * 3. On JSR $XXXX: add target as new function entry point (queue it)
 * 4. Stop at RTS, RTI, or JMP abs (treat JMP abs as tail-call → stop current func)
 * 5. JMP (ind) → log as dispatch point, stop current func
 * 6. Bank-aware: $C000-$FFFF = fixed bank 15; $8000-$BFFF = switchable
 *    For fixed bank analysis we walk bank 15 only (sufficient for RESET/NMI)
 *    Switchable bank code discovered via JSR from fixed bank
 *
 * Enhancement: Register state propagation tracks known A/X/Y values through
 * the instruction stream to detect bank switches and resolve computed targets.
 */
#include "function_finder.h"
#include "cpu6502_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool env_var_enabled(const char *name) {
    const char *value = getenv(name);
    if (!value || !*value) return false;
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "YES") == 0 ||
           strcmp(value, "on") == 0 ||
           strcmp(value, "ON") == 0;
}

/* Work queue for BFS */
typedef struct {
    uint16_t addr;
    int bank;
} WorkItem;

static WorkItem s_queue[MAX_FUNCTIONS * 2];
static int s_queue_head = 0;
static int s_queue_tail = 0;

static void queue_push(uint16_t addr, int bank) {
    if (s_queue_tail < MAX_FUNCTIONS * 2) {
        s_queue[s_queue_tail++] = (WorkItem){addr, bank};
    }
}

static bool queue_pop(WorkItem *out) {
    if (s_queue_head >= s_queue_tail) return false;
    *out = s_queue[s_queue_head++];
    return true;
}

static int function_list_find_index(const FunctionList *list, uint16_t addr, int bank) {
    for (int i = 0; i < list->count; i++) {
        if (list->entries[i].addr == addr && list->entries[i].bank == bank)
            return i;
    }
    return -1;
}

static void add_function_with_source(FunctionList *list, uint16_t addr, int bank,
                                     uint8_t source_flags) {
    int existing = function_list_find_index(list, addr, bank);
    if (existing >= 0) {
        list->entries[existing].source_flags |= source_flags;
        if (list->entries[existing].evidence_count < 0xFFFF)
            list->entries[existing].evidence_count++;
        return;
    }
    if (list->count >= MAX_FUNCTIONS) {
        fprintf(stderr, "function_finder: MAX_FUNCTIONS exceeded!\n");
        return;
    }
    list->entries[list->count++] = (FunctionEntry){
        addr, bank, 0, addr, bank, addr, bank, FUNCTION_KIND_STANDALONE, source_flags, 1
    };
    queue_push(addr, bank);
}

static void add_function(FunctionList *list, uint16_t addr, int bank) {
    add_function_with_source(list, addr, bank, FUNCTION_SOURCE_CONTROL);
}

static uint8_t propagated_discovery_source(uint8_t walk_source_flags) {
    uint8_t strong_sources =
        FUNCTION_SOURCE_CONTROL |
        FUNCTION_SOURCE_MANUAL |
        FUNCTION_SOURCE_KNOWN_TABLE |
        FUNCTION_SOURCE_SPLIT_TABLE;
    uint8_t weak_sources =
        FUNCTION_SOURCE_PTR_SCAN |
        FUNCTION_SOURCE_TABLE_RUN |
        FUNCTION_SOURCE_XBANK;

    if (walk_source_flags & strong_sources) return FUNCTION_SOURCE_CONTROL;
    if (walk_source_flags & weak_sources) return (walk_source_flags & weak_sources);
    return FUNCTION_SOURCE_CONTROL;
}

static void add_function_propagated(FunctionList *list, uint16_t addr, int bank,
                                    uint8_t walk_source_flags) {
    add_function_with_source(list, addr, bank,
                             propagated_discovery_source(walk_source_flags));
}

static void add_function_uncertain_bank(FunctionList *list, uint16_t addr, int bank) {
    add_function_with_source(list, addr, bank, FUNCTION_SOURCE_XBANK);
}

bool function_list_contains(const FunctionList *list, uint16_t addr, int bank) {
    return function_list_find_index(list, addr, bank) >= 0;
}

/* Translate an SRAM address to ROM address via game config sram_map.
 * Returns the ROM address (>= $8000) on success, or 0 if not mapped. */
static uint16_t sram_translate(const GameConfig *cfg, uint16_t addr, int *out_bank) {
    for (int i = 0; i < cfg->sram_map_count; i++) {
        const SramMap *m = &cfg->sram_maps[i];
        if (addr >= m->sram_start && addr < m->sram_start + m->size) {
            *out_bank = m->bank;
            return m->rom_start + (addr - m->sram_start);
        }
    }
    return 0;
}

/* Auto-detect contiguous zero-fill runs in each bank and add them as
 * data_region entries.  Zero-fill is unused ROM — not code, not data —
 * and must never be decoded as instructions.  Minimum run length of 16
 * bytes avoids flagging legitimate short zero sequences (e.g. a BRK
 * followed by padding inside a function). */
#define ZERO_FILL_MIN_RUN 16

static void detect_zero_fill_regions(const NESRom *rom, GameConfig *cfg) {
    int total_added = 0;
    for (int bank = 0; bank < rom->prg_banks; bank++) {
        uint16_t base = (bank == rom->prg_banks - 1) ? 0xC000 : 0x8000;
        uint16_t end  = (bank == rom->prg_banks - 1) ? 0xFFFA : 0xC000;
        /* 0xFFFA: stop before interrupt vectors */
        uint16_t run_start = 0;
        bool in_run = false;

        for (uint16_t addr = base; addr < end; addr++) {
            uint8_t b = rom_read(rom, bank, addr);
            if (b == 0x00) {
                if (!in_run) {
                    run_start = addr;
                    in_run = true;
                }
            } else {
                if (in_run) {
                    uint16_t run_len = addr - run_start;
                    if (run_len >= ZERO_FILL_MIN_RUN &&
                        cfg->data_region_count < GAME_CFG_MAX_DATA_REGIONS) {
                        DataRegion *dr = &cfg->data_regions[cfg->data_region_count++];
                        dr->bank  = bank;
                        dr->start = run_start;
                        dr->end   = addr;  /* exclusive */
                        total_added++;
                    }
                    in_run = false;
                }
            }
        }
        /* Close trailing run */
        if (in_run) {
            uint16_t run_len = end - run_start;
            if (run_len >= ZERO_FILL_MIN_RUN &&
                cfg->data_region_count < GAME_CFG_MAX_DATA_REGIONS) {
                DataRegion *dr = &cfg->data_regions[cfg->data_region_count++];
                dr->bank  = bank;
                dr->start = run_start;
                dr->end   = end;
                total_added++;
            }
        }
    }
    if (total_added > 0)
        printf("[FuncFinder] Zero-fill scan: excluded %d regions across all banks\n",
               total_added);
}

/* Check if an address falls within a declared data region */
static bool is_data_region(const GameConfig *cfg, int bank, uint16_t addr) {
    for (int i = 0; i < cfg->data_region_count; i++) {
        const DataRegion *dr = &cfg->data_regions[i];
        if ((dr->bank == bank || dr->bank == -1) &&
            addr >= dr->start && addr < dr->end)
            return true;
    }
    return false;
}

/* Deep-decode validation: decode min_valid instructions at target.
 * Returns true if the stream is valid code (no illegal opcodes).
 * RTS/JMP/RTI/JMP(ind) count as clean terminators.
 * BRK at position 0 is rejected. RTS at position 0 = null handler = accepted. */
#define TABLE_RUN_MIN_VALID 7
#define TABLE_RUN_MIN_RUN   4

static bool validate_code_target(const NESRom *rom, int bank,
                                 uint16_t addr, int min_valid) {
    uint8_t first = rom_read(rom, bank, addr);
    if (g_opcode_table[first].mnemonic == MN_ILLEGAL) return false;
    if (first == 0x00) return false;  /* BRK at function entry = suspicious */
    if (first == 0x60) return true;   /* RTS = null handler */
    int count = 0;
    uint16_t pc = addr;
    for (int i = 0; i < min_valid; i++) {
        uint8_t op = rom_read(rom, bank, pc);
        if (g_opcode_table[op].mnemonic == MN_ILLEGAL) return false;
        int sz = g_opcode_table[op].size;
        if (sz == 0) sz = 1;
        pc += sz;
        count++;
        if (op == 0x60 || op == 0x4C || op == 0x6C || op == 0x40)
            return true;  /* clean terminator */
    }
    return true;
}

void scan_function_boundaries(const NESRom *rom, uint16_t start_addr,
                              int bank, const GameConfig *cfg,
                              uint16_t *out_addrs, int *out_count,
                              int max_addrs) {
    uint16_t pc = start_addr;
    uint16_t pending[256];
    int pending_count = 0;

    *out_count = 0;

    for (int i = 0; i < MAX_INSNS_PER_FUNC && *out_count < max_addrs; i++) {
        if (pc < 0x8000) break;

        for (int p = 0; p < pending_count; p++) {
            if (pending[p] == pc) {
                pending[p] = pending[--pending_count];
                break;
            }
        }

        out_addrs[(*out_count)++] = pc;

        int read_bank = (pc >= 0xC000) ? (rom->prg_banks - 1) : bank;
        uint8_t op = rom_read(rom, read_bank, pc);
        const OpcodeEntry *e = &g_opcode_table[op];
        int sz = (e->size > 0) ? e->size : 1;

        if (e->mnemonic == MN_JSR) {
            uint16_t target = rom_read(rom, read_bank, pc + 1) |
                              ((uint16_t)rom_read(rom, read_bank, pc + 2) << 8);
            for (int ti = 0; ti < cfg->trampoline_count; ti++) {
                if (target == cfg->trampolines[ti].addr) {
                    sz = 3 + cfg->trampolines[ti].inline_bytes;
                    break;
                }
            }
            for (int ti = 0; ti < cfg->inline_dispatch_count; ti++) {
                if (target == cfg->inline_dispatches[ti].addr) {
                    uint16_t tpc = pc + 3;
                    while (rom_read(rom, read_bank, tpc + 1) >= 0x80) tpc += 2;
                    sz = (int)(tpc - pc);
                    break;
                }
            }
            for (int ti = 0; ti < cfg->inline_pointer_count; ti++) {
                if (target == cfg->inline_pointers[ti].addr) {
                    sz = 5;
                    break;
                }
            }
            for (int ti = 0; ti < cfg->nop_jsr_count; ti++) {
                if (target == cfg->nop_jsrs[ti]) {
                    sz = 3;
                    break;
                }
            }
        }

        switch (e->mnemonic) {
            case MN_BCC: case MN_BCS: case MN_BEQ: case MN_BNE:
            case MN_BMI: case MN_BPL: case MN_BVC: case MN_BVS: {
                int8_t off = (int8_t)rom_read(rom, read_bank, pc + 1);
                uint16_t tgt = (uint16_t)(pc + 2 + off);
                if (tgt > pc && tgt >= start_addr && pending_count < 256) {
                    bool found = false;
                    for (int p = 0; p < pending_count; p++)
                        if (pending[p] == tgt) { found = true; break; }
                    if (!found) pending[pending_count++] = tgt;
                }
                break;
            }
            default: break;
        }

        if (e->mnemonic == MN_ILLEGAL) {
            pc += sz;
            continue;
        }

        if (e->mnemonic == MN_RTS && pc >= 0x800D &&
            rom_read(rom, read_bank, pc - 1)  == 0x48 &&
            rom_read(rom, read_bank, pc - 5)  == 0x48 &&
            rom_read(rom, read_bank, pc - 9)  == 0x48 &&
            rom_read(rom, read_bank, pc - 12) == 0x48) {
            uint16_t cont = pc + 1;
            bool found = false;
            for (int p = 0; p < pending_count; p++)
                if (pending[p] == cont) { found = true; break; }
            if (!found && pending_count < 256) pending[pending_count++] = cont;
        }

        if (e->mnemonic == MN_RTS || e->mnemonic == MN_RTI ||
            e->mnemonic == MN_BRK || e->mnemonic == MN_JMP) {
            if (pending_count == 0) break;
        }

        pc += sz;
    }
}

typedef struct {
    int idx;
    int bank;
    uint16_t addr;
    int priority;
} FunctionOrderEntry;

static int function_entry_priority(const FunctionEntry *entry) {
    if (entry->source_flags & FUNCTION_SOURCE_CONTROL) return 4;
    if (entry->source_flags & FUNCTION_SOURCE_MANUAL) return 3;
    if (entry->source_flags & FUNCTION_SOURCE_KNOWN_TABLE) return 2;
    if (entry->source_flags & FUNCTION_SOURCE_SPLIT_TABLE) return 1;
    return 0;
}

static bool can_demote_control_stub(const NESRom *rom, const FunctionEntry *entry) {
    int read_bank = (entry->addr >= 0xC000) ? (rom->prg_banks - 1) : entry->bank;
    uint8_t op0 = rom_read(rom, read_bank, entry->addr);
    if (entry->size <= 1) {
        return op0 == 0x60 || op0 == 0x40;
    }
    if (entry->size == 2 && (op0 == 0x4C || op0 == 0x6C)) {
        return true;
    }
    return false;
}

static const char *classify_missing_manual_entry(const NESRom *rom, const GameConfig *cfg,
                                                 uint16_t addr, int bank) {
    int fixed_bank = rom->prg_banks - 1;
    if (bank >= 0 && bank < fixed_bank && addr >= 0xC000)
        return "LIKELY_BAD_SEED_WRONG_BANK";
    if (bank >= 0 && bank <= fixed_bank && is_data_region(cfg, bank, addr))
        return "LIKELY_BAD_SEED_DATA_REGION";
    if (addr >= 0x8000 && bank >= 0 && bank <= fixed_bank) {
        if (!validate_code_target(rom, bank, addr, TABLE_RUN_MIN_VALID)) {
            uint16_t a1 = addr + 1;
            if (a1 >= 0x8000 && a1 <= 0xFFFD &&
                !is_data_region(cfg, bank, a1) &&
                validate_code_target(rom, bank, a1, TABLE_RUN_MIN_VALID)) {
                return "LIKELY_BAD_SEED_OFF_BY_ONE_PLUS1";
            }
            if (addr > 0x8000) {
                uint16_t am1 = addr - 1;
                if (!is_data_region(cfg, bank, am1) &&
                    validate_code_target(rom, bank, am1, TABLE_RUN_MIN_VALID)) {
                    return "LIKELY_BAD_SEED_OFF_BY_ONE_MINUS1";
                }
            }
            return "MISSING";
        }
    }
    return "MISSING";
}

static void source_flags_to_string(uint8_t flags, char *buf, size_t buf_size) {
    size_t used = 0;
    buf[0] = '\0';

    struct SourceName { uint8_t flag; const char *name; };
    static const struct SourceName names[] = {
        { FUNCTION_SOURCE_CONTROL,     "control" },
        { FUNCTION_SOURCE_PTR_SCAN,    "ptr_scan" },
        { FUNCTION_SOURCE_TABLE_RUN,   "table_run" },
        { FUNCTION_SOURCE_SPLIT_TABLE, "split_table" },
        { FUNCTION_SOURCE_KNOWN_TABLE, "known_table" },
        { FUNCTION_SOURCE_XBANK,       "xbank" },
        { FUNCTION_SOURCE_MANUAL,      "manual" },
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if ((flags & names[i].flag) == 0) continue;
        if (used > 0 && used + 1 < buf_size)
            buf[used++] = '|';
        for (const char *p = names[i].name; *p && used + 1 < buf_size; p++)
            buf[used++] = *p;
        buf[used] = '\0';
    }

    if (used == 0 && buf_size > 0) {
        snprintf(buf, buf_size, "none");
    }
}

static int compare_function_order(const void *lhs, const void *rhs) {
    const FunctionOrderEntry *a = (const FunctionOrderEntry *)lhs;
    const FunctionOrderEntry *b = (const FunctionOrderEntry *)rhs;
    if (a->priority != b->priority) return b->priority - a->priority;
    if (a->bank != b->bank) return a->bank - b->bank;
    if (a->addr < b->addr) return -1;
    if (a->addr > b->addr) return 1;
    return a->idx - b->idx;
}

static int classify_secondary_entries(const NESRom *rom, FunctionList *list,
                                      const GameConfig *cfg, int *out_coverage) {
    int fixed_bank = rom->prg_banks - 1;
    int bank_count = fixed_bank + 1;
    size_t table_elems = (size_t)bank_count * 65536u;
    int *entry_lookup = (int *)malloc(table_elems * sizeof(int));
    FunctionOrderEntry *order = (FunctionOrderEntry *)malloc((size_t)list->count * sizeof(FunctionOrderEntry));
    uint16_t boundaries[MAX_INSNS_PER_FUNC];
    int secondary_count = 0;

    if (!entry_lookup || !order) {
        free(entry_lookup);
        free(order);
        return 0;
    }

    for (size_t i = 0; i < table_elems; i++) {
        out_coverage[i] = -1;
        entry_lookup[i] = -1;
    }

    for (int i = 0; i < list->count; i++) {
        list->entries[i].kind = FUNCTION_KIND_STANDALONE;
        list->entries[i].canonical_addr = list->entries[i].addr;
        list->entries[i].canonical_bank = list->entries[i].bank;
        list->entries[i].covering_addr = list->entries[i].addr;
        list->entries[i].covering_bank = list->entries[i].bank;
        order[i].idx = i;
        order[i].bank = list->entries[i].bank;
        order[i].addr = list->entries[i].addr;
        order[i].priority = function_entry_priority(&list->entries[i]);
        if (list->entries[i].bank >= 0 && list->entries[i].bank < bank_count)
            entry_lookup[(size_t)list->entries[i].bank * 65536u + list->entries[i].addr] = i;
    }

    qsort(order, (size_t)list->count, sizeof(FunctionOrderEntry), compare_function_order);

    for (int oi = 0; oi < list->count; oi++) {
        int idx = order[oi].idx;
        FunctionEntry *entry = &list->entries[idx];
        if (entry->kind == FUNCTION_KIND_SECONDARY) continue;
        if (order[oi].priority == 0)
            continue;

        int boundary_count = 0;
        scan_function_boundaries(rom, entry->addr, entry->bank, cfg,
                                 boundaries, &boundary_count, MAX_INSNS_PER_FUNC);

        for (int bi = 0; bi < boundary_count; bi++) {
            uint16_t boundary = boundaries[bi];
            if (entry->addr < 0xC000 && boundary >= 0xC000) continue;
            int boundary_bank = (boundary >= 0xC000) ? fixed_bank : entry->bank;
            if (boundary_bank < 0 || boundary_bank >= bank_count) continue;
            size_t key = (size_t)boundary_bank * 65536u + boundary;
            if (out_coverage[key] < 0) out_coverage[key] = idx;
            if (boundary == entry->addr) continue;

            int other_idx = entry_lookup[key];
            if (other_idx < 0 || other_idx == idx) continue;

            FunctionEntry *other = &list->entries[other_idx];
            if (other->covering_addr == other->addr &&
                other->covering_bank == other->bank) {
                other->covering_addr = entry->addr;
                other->covering_bank = entry->bank;
            }
            if ((other->source_flags & FUNCTION_SOURCE_CONTROL) &&
                !can_demote_control_stub(rom, other)) {
                continue;
            }
            if (other->kind == FUNCTION_KIND_SECONDARY) {
                int prev_idx = function_list_find_index(list,
                                                        other->canonical_addr,
                                                        other->canonical_bank);
                int prev_priority = 0;
                if (prev_idx >= 0)
                    prev_priority = function_entry_priority(&list->entries[prev_idx]);
                if (prev_priority > order[oi].priority) continue;
                if (prev_priority == order[oi].priority &&
                    (other->canonical_bank < entry->bank ||
                     (other->canonical_bank == entry->bank &&
                      other->canonical_addr <= entry->addr))) {
                    continue;
                }
            }

            other->kind = FUNCTION_KIND_SECONDARY;
            other->canonical_addr = entry->addr;
            other->canonical_bank = entry->bank;
            other->covering_addr = entry->addr;
            other->covering_bank = entry->bank;
            secondary_count++;
        }
    }

    free(entry_lookup);
    free(order);
    return secondary_count;
}

/* Register propagation statistics */
static int s_bank_switches_detected = 0;
static int s_regprop_targeted_adds = 0;

/* Walk one function starting at addr in the given bank context.
 * Returns number of instructions decoded.
 *
 * Pending-branch tracking: branches can jump forward past RTS/JMP into code
 * that's only reachable via that branch. We continue past stop instructions
 * while pending forward targets remain, so JSR calls in those sections are
 * discovered.
 *
 * Register propagation: tracks known_a/x/y constant values to detect bank
 * switches and provide precise bank context for target resolution. */
static int walk_function(const NESRom *rom, FunctionList *list,
                         uint16_t start_addr, int switchable_bank,
                         const GameConfig *cfg, uint8_t walk_source_flags) {
    uint16_t pc = start_addr;
    int insn_count = 0;
    int fixed_bank = rom->prg_banks - 1;

    /* Pending forward branch targets */
    uint16_t pending[256];
    int pending_count = 0;

    /* Register state propagation (known constant values, -1 = unknown) */
    int known_a = -1, known_x = -1, known_y = -1;

    /* Effective switchable bank — starts as the function's bank context,
     * updated when a bank-switch call is detected with known A register. */
    int effective_bank = switchable_bank;

    for (int i = 0; i < MAX_INSNS_PER_FUNC; i++) {
        /* Remove this address from pending now that we're visiting it */
        for (int p = 0; p < pending_count; p++) {
            if (pending[p] == pc) {
                pending[p] = pending[--pending_count];
                break;
            }
        }

        /* Determine which bank to read from (instruction decoding always
         * uses the function's original bank, not effective_bank) */
        int read_bank = (pc >= 0xC000) ? fixed_bank : switchable_bank;
        if (pc < 0x8000) break; /* Not ROM */

        uint8_t opcode = rom_read(rom, read_bank, pc);
        const OpcodeEntry *entry = &g_opcode_table[opcode];
        insn_count++;

        if (entry->mnemonic == MN_ILLEGAL) {
            int sz = entry->size;
            pc += (sz > 0) ? sz : 1;
            continue;
        }

        /* ── Register state propagation ────────────────────────────────
         * Track known constant values in A, X, Y registers.
         * JSR is excluded here — handled separately below for bank-switch
         * detection before tainting. */
        if (entry->mnemonic != MN_JSR) {
            uint8_t operand8 = (entry->size >= 2) ?
                rom_read(rom, read_bank, pc + 1) : 0;
            uint16_t operand16 = (entry->size >= 3) ?
                (rom_read(rom, read_bank, pc + 1) |
                 ((uint16_t)rom_read(rom, read_bank, pc + 2) << 8)) : 0;

            switch (entry->mnemonic) {
                /* ── Loads ── */
                case MN_LDA:
                    if (entry->addr_mode == AM_IMM) {
                        known_a = operand8;
                    } else if (entry->addr_mode == AM_ABS && operand16 >= 0x8000) {
                        int rb = (operand16 >= 0xC000) ? fixed_bank : effective_bank;
                        known_a = rom_read(rom, rb, operand16);
                    } else if (entry->addr_mode == AM_ABSX && known_x != -1) {
                        uint16_t ea = operand16 + (uint16_t)known_x;
                        if (ea >= 0x8000) {
                            int rb = (ea >= 0xC000) ? fixed_bank : effective_bank;
                            known_a = rom_read(rom, rb, ea);
                        } else { known_a = -1; }
                    } else if (entry->addr_mode == AM_ABSY && known_y != -1) {
                        uint16_t ea = operand16 + (uint16_t)known_y;
                        if (ea >= 0x8000) {
                            int rb = (ea >= 0xC000) ? fixed_bank : effective_bank;
                            known_a = rom_read(rom, rb, ea);
                        } else { known_a = -1; }
                    } else {
                        known_a = -1; /* ZP, (ZP,X), (ZP),Y, or unknown index */
                    }
                    break;
                case MN_LDX:
                    known_x = (entry->addr_mode == AM_IMM) ? operand8 : -1;
                    break;
                case MN_LDY:
                    known_y = (entry->addr_mode == AM_IMM) ? operand8 : -1;
                    break;

                /* ── Register transfers ── */
                case MN_TAX: known_x = known_a; break;
                case MN_TAY: known_y = known_a; break;
                case MN_TXA: known_a = known_x; break;
                case MN_TYA: known_a = known_y; break;
                case MN_TSX: known_x = -1; break;

                /* ── Bitwise ALU (computable with known A + immediate) ── */
                case MN_AND:
                    if (entry->addr_mode == AM_IMM && known_a != -1)
                        known_a &= operand8;
                    else
                        known_a = -1;
                    break;
                case MN_ORA:
                    if (entry->addr_mode == AM_IMM && known_a != -1)
                        known_a |= operand8;
                    else
                        known_a = -1;
                    break;
                case MN_EOR:
                    if (entry->addr_mode == AM_IMM && known_a != -1)
                        known_a ^= operand8;
                    else
                        known_a = -1;
                    break;

                /* ── Shifts (accumulator mode, no carry needed) ── */
                case MN_ASL:
                    if (entry->addr_mode == AM_ACC)
                        known_a = (known_a != -1) ? ((known_a << 1) & 0xFF) : -1;
                    break;
                case MN_LSR:
                    if (entry->addr_mode == AM_ACC)
                        known_a = (known_a != -1) ? ((known_a >> 1) & 0x7F) : -1;
                    break;

                /* ── Rotates & arithmetic (carry-dependent → taint) ── */
                case MN_ROL: case MN_ROR:
                    if (entry->addr_mode == AM_ACC) known_a = -1;
                    break;
                case MN_ADC: case MN_SBC:
                    known_a = -1;
                    break;

                /* ── Increment / Decrement ── */
                case MN_INX:
                    known_x = (known_x != -1) ? ((known_x + 1) & 0xFF) : -1;
                    break;
                case MN_DEX:
                    known_x = (known_x != -1) ? ((known_x - 1) & 0xFF) : -1;
                    break;
                case MN_INY:
                    known_y = (known_y != -1) ? ((known_y + 1) & 0xFF) : -1;
                    break;
                case MN_DEY:
                    known_y = (known_y != -1) ? ((known_y - 1) & 0xFF) : -1;
                    break;

                /* ── Stack (unknown value) ── */
                case MN_PLA: known_a = -1; break;

                /* ── Undocumented LAX: A = X = mem (unknown) ── */
                case MN_LAX: known_a = -1; known_x = -1; break;

                /* ── Everything else: no register effect ── */
                default: break;
            }
        }

        /* Track forward branch targets so we continue past RTS/JMP when needed.
         * Also: backward branches before this function's start address mean there
         * is code only reachable from here — register it as a new function entry. */
        switch (entry->mnemonic) {
            case MN_BCC: case MN_BCS: case MN_BEQ: case MN_BNE:
            case MN_BMI: case MN_BPL: case MN_BVC: case MN_BVS: {
                int8_t off = (int8_t)rom_read(rom, read_bank, pc + 1);
                uint16_t tgt = (uint16_t)(pc + 2 + off);
                if (tgt > pc && pending_count < 256) {
                    /* Forward — track as pending */
                    bool found = false;
                    for (int p = 0; p < pending_count; p++)
                        if (pending[p] == tgt) { found = true; break; }
                    if (!found) pending[pending_count++] = tgt;
                } else if (tgt < start_addr && tgt >= 0x8000) {
                    /* Backward branch before this function's start: that code
                     * is only reachable via this branch, register as new function. */
                    int tbank = (tgt >= 0xC000) ? fixed_bank : switchable_bank;
                    add_function_propagated(list, tgt, tbank, walk_source_flags);
                }
                break;
            }
            default: break;
        }

        if (entry->mnemonic == MN_RTS || entry->mnemonic == MN_RTI) {
            if (pending_count == 0) break;
            pc += entry->size;
            continue;
        }

        if (entry->mnemonic == MN_JSR) {
            uint8_t lo = rom_read(rom, read_bank, pc + 1);
            uint8_t hi = rom_read(rom, read_bank, pc + 2);
            uint16_t target = lo | ((uint16_t)hi << 8);

            /* ── Bank switch detection ─────────────────────────────────
             * If JSR target is a known bank-switch routine and A holds a
             * known constant, update effective_bank for subsequent target
             * resolution in this function walk. */
            for (int bi = 0; bi < cfg->bank_switch_count; bi++) {
                if (target == cfg->bank_switches[bi].addr && known_a != -1) {
                    int new_bank = known_a;
                    /* Clamp to valid switchable bank range */
                    if (new_bank >= rom->prg_banks - 1)
                        new_bank = rom->prg_banks - 2;
                    if (new_bank != effective_bank) {
                        effective_bank = new_bank;
                        s_bank_switches_detected++;
                    }
                    break;
                }
            }

            /* Taint all registers: callee may clobber A/X/Y */
            known_a = -1; known_x = -1; known_y = -1;

            /* Check against game-config trampolines: JSR to a known bank-switch
             * dispatch routine where inline data bytes follow (not instructions). */
            {
                const TrampolineEntry *tramp = NULL;
                for (int ti = 0; ti < cfg->trampoline_count; ti++) {
                    if (target == cfg->trampolines[ti].addr) {
                        tramp = &cfg->trampolines[ti];
                        break;
                    }
                }
                if (tramp) {
                    add_function_propagated(list, tramp->addr, fixed_bank, walk_source_flags);
                    uint8_t disp_bank = rom_read(rom, read_bank, pc + 3);
                    uint8_t disp_lo   = rom_read(rom, read_bank, pc + 4);
                    uint8_t disp_hi   = rom_read(rom, read_bank, pc + 5);
                    uint16_t disp_addr = (disp_lo | ((uint16_t)disp_hi << 8)) + tramp->addr_adjust;
                    if (disp_addr >= 0x8000) {
                        int tbank = (disp_addr >= 0xC000) ? fixed_bank : (int)disp_bank;
                        add_function_propagated(list, disp_addr, tbank, walk_source_flags);
                    }
                    pc += 3 + tramp->inline_bytes;
                    continue;
                }
            }

            /* Check against inline_dispatch: JSR to an indexed-dispatch routine
             * where the return address is used to find an inline address table.
             * Entries are 2-byte LE absolute addresses; table ends at hi < 0x80. */
            {
                const InlineDispatch *idsp = NULL;
                for (int ti = 0; ti < cfg->inline_dispatch_count; ti++) {
                    if (target == cfg->inline_dispatches[ti].addr) {
                        idsp = &cfg->inline_dispatches[ti];
                        break;
                    }
                }
                if (idsp) {
                    /* Don't add the dispatch routine itself — its body uses
                     * PLA-return-address patterns incompatible with recomp.
                     * All call sites get inlined switch tables instead. */
                    uint16_t tpc = pc + 3;
                    while (1) {
                        uint8_t tlo = rom_read(rom, read_bank, tpc);
                        uint8_t thi = rom_read(rom, read_bank, tpc + 1);
                        uint16_t dest = (uint16_t)tlo | ((uint16_t)thi << 8);
                        if (dest >= 0xC000) {
                            add_function_propagated(list, dest, fixed_bank, walk_source_flags);
                        } else if (dest >= 0x8000) {
                            if (effective_bank != fixed_bank) {
                                add_function_propagated(list, dest, effective_bank, walk_source_flags);
                                if (effective_bank != switchable_bank)
                                    s_regprop_targeted_adds++;
                            } else {
                                for (int _b = 0; _b < fixed_bank; _b++)
                                    add_function_uncertain_bank(list, dest, _b);
                            }
                        } else {
                            /* Entry < $8000 — try SRAM translation */
                            int sram_bank;
                            uint16_t rom_addr = sram_translate(cfg, dest, &sram_bank);
                            if (rom_addr >= 0x8000) {
                                add_function_propagated(list, rom_addr, sram_bank, walk_source_flags);
                            } else {
                                break; /* Not ROM and not mapped SRAM — end of table */
                            }
                        }
                        tpc += 2;
                    }
                    pc = tpc;
                    continue;
                }
            }

            /* Check nop_jsr: skip this JSR entirely */
            {
                int is_nop = 0;
                for (int ni = 0; ni < cfg->nop_jsr_count; ni++) {
                    if (target == cfg->nop_jsrs[ni]) { is_nop = 1; break; }
                }
                if (is_nop) { pc += 3; continue; }
            }
            /* Check inline_pointer: skip 2 inline bytes after JSR */
            {
                const InlinePointer *ipp = NULL;
                for (int ti = 0; ti < cfg->inline_pointer_count; ti++) {
                    if (target == cfg->inline_pointers[ti].addr) { ipp = &cfg->inline_pointers[ti]; break; }
                }
                if (ipp) {
                    add_function_propagated(list, ipp->addr, fixed_bank, walk_source_flags);
                    pc += 5; continue;
                }
            }

            if (target >= 0xC000) {
                /* Fixed bank target — always knowable */
                add_function_propagated(list, target, fixed_bank, walk_source_flags);
            } else if (target >= 0x8000) {
                if (effective_bank != fixed_bank) {
                    /* Bank known via walk context or bank-switch detection */
                    add_function_propagated(list, target, effective_bank, walk_source_flags);
                    if (effective_bank != switchable_bank)
                        s_regprop_targeted_adds++;
                    /* MMC3: cross-8KB boundary — also add the remapped target.
                     * When code at $A000+ calls $8xxx, the $8xxx range may be
                     * from a different 8KB bank at runtime. The remapped address
                     * ($8xxx + $2000 = $Axxx) represents this target in the
                     * recompiler's view of the same 16KB bank. */
                    if (rom->mapper == 4 && target >= 0x8000 && target < 0xA000 &&
                        pc >= 0xA000 && pc < 0xC000)
                        add_function_propagated(list, target + 0x2000, effective_bank, walk_source_flags);
                    else if (rom->mapper == 4 && target >= 0xA000 && target < 0xC000 &&
                             pc >= 0x8000 && pc < 0xA000)
                        add_function_propagated(list, target - 0x2000, effective_bank, walk_source_flags);
                } else {
                    /* Fixed bank calling switchable region: bank unknown statically.
                     * Add for ALL switchable banks so each gets a correct body.
                     * At runtime call_by_address() selects via g_current_bank. */
                    for (int _b = 0; _b < fixed_bank; _b++)
                        add_function_uncertain_bank(list, target, _b);
                }
            } else {
                /* Target in RAM — try SRAM-to-ROM translation */
                int sram_bank;
                uint16_t rom_addr = sram_translate(cfg, target, &sram_bank);
                if (rom_addr >= 0x8000)
                    add_function_propagated(list, rom_addr, sram_bank, walk_source_flags);
            }
            pc += entry->size;
            continue;
        }

        if (entry->mnemonic == MN_JMP && entry->addr_mode == AM_ABS) {
            uint8_t lo = rom_read(rom, read_bank, pc + 1);
            uint8_t hi = rom_read(rom, read_bank, pc + 2);
            uint16_t target = lo | ((uint16_t)hi << 8);
            if (target >= 0xC000) {
                add_function_propagated(list, target, fixed_bank, walk_source_flags);
            } else if (target >= 0x8000) {
                if (effective_bank != fixed_bank) {
                    add_function_propagated(list, target, effective_bank, walk_source_flags);
                    if (effective_bank != switchable_bank)
                        s_regprop_targeted_adds++;
                    /* MMC3 cross-8KB mirror (same as JSR path above) */
                    if (rom->mapper == 4 && target >= 0x8000 && target < 0xA000 &&
                        pc >= 0xA000 && pc < 0xC000)
                        add_function_propagated(list, target + 0x2000, effective_bank, walk_source_flags);
                    else if (rom->mapper == 4 && target >= 0xA000 && target < 0xC000 &&
                             pc >= 0x8000 && pc < 0xA000)
                        add_function_propagated(list, target - 0x2000, effective_bank, walk_source_flags);
                } else {
                    for (int _b = 0; _b < fixed_bank; _b++)
                        add_function_uncertain_bank(list, target, _b);
                }
            } else {
                /* Target in RAM — try SRAM-to-ROM translation */
                int sram_bank;
                uint16_t rom_addr = sram_translate(cfg, target, &sram_bank);
                if (rom_addr >= 0x8000)
                    add_function_propagated(list, rom_addr, sram_bank, walk_source_flags);
            }
            if (pending_count == 0) break;
            pc += entry->size;
            continue;
        }

        if (entry->mnemonic == MN_JMP && entry->addr_mode == AM_IND) {
            /* Indirect JMP: if the vector address is in ROM, read the target
             * statically and add it as a function entry. */
            uint8_t lo_ptr = rom_read(rom, read_bank, pc + 1);
            uint8_t hi_ptr = rom_read(rom, read_bank, pc + 2);
            uint16_t vec_addr = lo_ptr | ((uint16_t)hi_ptr << 8);
            if (vec_addr >= 0x8000) {
                int vec_bank = (vec_addr >= 0xC000) ? fixed_bank : effective_bank;
                uint8_t tlo = rom_read(rom, vec_bank, vec_addr);
                uint8_t thi = rom_read(rom, vec_bank, vec_addr + 1);
                uint16_t target = tlo | ((uint16_t)thi << 8);
                if (target >= 0x8000) {
                    int tbank = (target >= 0xC000) ? fixed_bank : effective_bank;
                    add_function_propagated(list, target, tbank, walk_source_flags);
                }
            }
            if (pending_count == 0) break;
            pc += entry->size;
            continue;
        }

        if (entry->mnemonic == MN_BRK) {
            if (pending_count == 0) break;
            pc += entry->size;
            continue;
        }

        pc += entry->size;
    }

    return insn_count;
}

void function_finder_run(const NESRom *rom, FunctionList *out, const GameConfig *cfg) {
    memset(out, 0, sizeof(*out));
    s_queue_head = 0;
    s_queue_tail = 0;
    bool legacy_mode = env_var_enabled("NESRECOMP_LEGACY_FUNCTION_FINDER");
    FILE *audit = NULL;
    FILE *entries_audit = NULL;
    char audit_path[256];
    char entries_audit_path[256];

    /* Reset propagation stats */
    s_bank_switches_detected = 0;
    s_regprop_targeted_adds = 0;

    int fixed_bank = rom->prg_banks - 1;

    /* Auto-detect zero-fill regions before any function discovery */
    detect_zero_fill_regions(rom, cfg);

    /* Seed from interrupt vectors */
    printf("[FuncFinder] Seeding from NMI=$%04X RESET=$%04X IRQ=$%04X\n",
           rom->nmi_vector, rom->reset_vector, rom->irq_vector);

    /* Log register propagation config */
    if (cfg->bank_switch_count > 0) {
        printf("[RegProp] Bank switch routines:");
        for (int i = 0; i < cfg->bank_switch_count; i++)
            printf(" $%04X", cfg->bank_switches[i].addr);
        printf("\n");
    }

    /* Seed SRAM-mapped code regions as functions */
    for (int i = 0; i < cfg->sram_map_count; i++) {
        printf("[SramMap] SRAM $%04X → ROM bank %d $%04X (size=$%04X)\n",
               cfg->sram_maps[i].sram_start, cfg->sram_maps[i].bank,
               cfg->sram_maps[i].rom_start, cfg->sram_maps[i].size);
        add_function(out, cfg->sram_maps[i].rom_start, cfg->sram_maps[i].bank);
    }

    /* Seed vectors with their correct bank: fixed bank if in $C000+, else bank 0 */
#define VEC_BANK(addr) ((addr) >= 0xC000 ? fixed_bank : 0)
    add_function(out, rom->reset_vector, VEC_BANK(rom->reset_vector));
    add_function(out, rom->nmi_vector,   VEC_BANK(rom->nmi_vector));
    add_function(out, rom->irq_vector,   VEC_BANK(rom->irq_vector));
#undef VEC_BANK

    /* BFS */
    WorkItem item;
    while (queue_pop(&item)) {
        /* Find this entry and walk it */
        for (int i = 0; i < out->count; i++) {
            if (out->entries[i].addr == item.addr &&
                out->entries[i].bank == item.bank &&
                out->entries[i].size == 0)
            {
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg,
                                                    out->entries[i].source_flags);
                break;
            }
        }
    }

    if (legacy_mode) {
        printf("[FuncFinder] Legacy mode enabled via NESRECOMP_LEGACY_FUNCTION_FINDER; "
               "skipping heuristic discovery passes\n");
    } else {
    /* ROM pointer scan: scan fixed bank for 16-bit values that look like
     * function pointers into the fixed bank ($C000-$FFFD).
     * Criteria: first byte at target is non-illegal opcode.
     * Also checks candidate+1 for RTS-as-JMP dispatch tables. */
    int before_scan = out->count;
    for (uint16_t off = 0; off <= 0x3FFD; off++) {
        uint16_t addr = 0xC000 + off;
        if (is_data_region(cfg, fixed_bank, addr)) continue;
        uint8_t lo = rom_read(rom, fixed_bank, addr);
        uint8_t hi = rom_read(rom, fixed_bank, addr + 1);
        uint16_t candidate = lo | ((uint16_t)hi << 8);
        if (candidate < 0xC000 || candidate > 0xFFFD) continue;
        if (is_data_region(cfg, fixed_bank, candidate)) continue;
        uint8_t first_byte = rom_read(rom, fixed_bank, candidate);
        if (g_opcode_table[first_byte].mnemonic != MN_ILLEGAL &&
            !function_list_contains(out, candidate, fixed_bank))
            add_function_with_source(out, candidate, fixed_bank, FUNCTION_SOURCE_PTR_SCAN);
        uint16_t candidate1 = candidate + 1;
        if (candidate1 >= 0xC000 && candidate1 <= 0xFFFD &&
            !function_list_contains(out, candidate1, fixed_bank)) {
            uint8_t fb1 = rom_read(rom, fixed_bank, candidate1);
            if (g_opcode_table[fb1].mnemonic != MN_ILLEGAL)
                add_function_with_source(out, candidate1, fixed_bank, FUNCTION_SOURCE_PTR_SCAN);
        }
    }
    /* BFS for newly added candidates */
    while (queue_pop(&item)) {
        for (int i = 0; i < out->count; i++) {
            if (out->entries[i].addr == item.addr &&
                out->entries[i].bank == item.bank &&
                out->entries[i].size == 0)
            {
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg,
                                                    out->entries[i].source_flags);
                break;
            }
        }
    }
    printf("[FuncFinder] Fixed-bank pointer scan added %d candidates\n", out->count - before_scan);

    if (cfg->disable_ptr_scan) {
        printf("[FuncFinder] Switchable->fixed pointer scan skipped via disable_ptr_scan\n");
    } else {
    /* Switchable-bank → fixed-bank pointer scan: scan each switchable bank
     * for 16-bit LE values pointing into the fixed bank ($C000-$FFFD).
     * Uses validate_code_target for deeper validation since cross-bank
     * pointer matches have higher false-positive risk. */
    int before_sw_fixed_scan = out->count;
    for (int b = 0; b < fixed_bank; b++) {
        for (uint16_t off = 0; off <= 0x3FFD; off++) {
            uint16_t addr = 0x8000 + off;
            if (is_data_region(cfg, b, addr)) continue;
            uint8_t lo = rom_read(rom, b, addr);
            uint8_t hi = rom_read(rom, b, addr + 1);
            uint16_t candidate = lo | ((uint16_t)hi << 8);
            if (candidate < 0xC000 || candidate > 0xFFFD) continue;
            if (is_data_region(cfg, fixed_bank, candidate)) continue;
            if (validate_code_target(rom, fixed_bank, candidate, TABLE_RUN_MIN_VALID) &&
                !function_list_contains(out, candidate, fixed_bank))
                add_function_with_source(out, candidate, fixed_bank, FUNCTION_SOURCE_PTR_SCAN);
        }
    }
    while (queue_pop(&item)) {
        for (int i = 0; i < out->count; i++) {
            if (out->entries[i].addr == item.addr &&
                out->entries[i].bank == item.bank &&
                out->entries[i].size == 0)
            {
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg,
                                                    out->entries[i].source_flags);
                break;
            }
        }
    }
    printf("[FuncFinder] Switchable->fixed pointer scan added %d candidates\n",
           out->count - before_sw_fixed_scan);
    } /* end disable_ptr_scan guard */

    /* Switchable-bank table-run scanner: find runs of TABLE_RUN_MIN_RUN+
     * consecutive 16-bit LE values in $8000-$BFFF where each target passes
     * deep-decode validation (TABLE_RUN_MIN_VALID valid instructions without
     * hitting an illegal opcode, or clean termination via RTS/JMP/RTI).
     *
     * Scans both the switchable bank itself and the fixed bank for pointer
     * tables targeting the switchable range. This catches:
     * - Inline dispatch tables embedded in switchable bank code
     * - Fixed-bank dispatch tables that index into switchable bank handlers
     * - Indirect (ZP),Y dispatch tables (not referenced by abs,X/Y)
     *
     * data_region exclusions apply to both scan source and target addresses.
     * Harmless FPs (valid code in wrong bank context) are accepted — they
     * generate unused but valid functions. Harmful FPs (data decoded as code)
     * are eliminated by the deep-decode check + data_region exclusions. */
    int before_sw_scan = out->count;
    for (int b = 0; b < fixed_bank; b++) {
        /* Scan source: switchable bank b (tables within the bank itself) */
        for (uint16_t off = 0; off <= 0x3FFD; ) {
            uint16_t scan_addr = 0x8000 + off;
            if (is_data_region(cfg, b, scan_addr)) { off++; continue; }
            /* Try to build a run of consecutive valid pointer entries */
            uint16_t run_start = off;
            int run_len = 0;
            uint16_t run_targets[256];
            while (off <= 0x3FFD && run_len < 256) {
                uint16_t a = 0x8000 + off;
                if (is_data_region(cfg, b, a)) break;
                uint8_t lo = rom_read(rom, b, a);
                uint8_t hi = rom_read(rom, b, a + 1);
                uint16_t candidate = lo | ((uint16_t)hi << 8);
                if (candidate < 0x8000 || candidate > 0xBFFD) break;
                if (is_data_region(cfg, b, candidate)) break;
                if (!validate_code_target(rom, b, candidate, TABLE_RUN_MIN_VALID)) break;
                run_targets[run_len++] = candidate;
                off += 2;
            }
            if (run_len >= TABLE_RUN_MIN_RUN) {
                for (int r = 0; r < run_len; r++) {
                    if (!function_list_contains(out, run_targets[r], b))
                        add_function_with_source(out, run_targets[r], b, FUNCTION_SOURCE_TABLE_RUN);
                }
            } else {
                off = run_start + 1;
            }
        }
        /* Scan source: fixed bank, targeting switchable bank b */
        for (uint16_t off = 0; off <= 0x3FFD; ) {
            uint16_t scan_addr = 0xC000 + off;
            if (is_data_region(cfg, fixed_bank, scan_addr)) { off++; continue; }
            uint16_t run_start = off;
            int run_len = 0;
            uint16_t run_targets[256];
            while (off <= 0x3FFD && run_len < 256) {
                uint16_t a = 0xC000 + off;
                if (is_data_region(cfg, fixed_bank, a)) break;
                uint8_t lo = rom_read(rom, fixed_bank, a);
                uint8_t hi = rom_read(rom, fixed_bank, a + 1);
                uint16_t candidate = lo | ((uint16_t)hi << 8);
                if (candidate < 0x8000 || candidate > 0xBFFD) break;
                if (is_data_region(cfg, b, candidate)) break;
                if (!validate_code_target(rom, b, candidate, TABLE_RUN_MIN_VALID)) break;
                run_targets[run_len++] = candidate;
                off += 2;
            }
            if (run_len >= TABLE_RUN_MIN_RUN) {
                for (int r = 0; r < run_len; r++) {
                    if (!function_list_contains(out, run_targets[r], b))
                        add_function_with_source(out, run_targets[r], b, FUNCTION_SOURCE_TABLE_RUN);
                }
            } else {
                off = run_start + 1;
            }
        }
    }
    /* BFS for switchable-bank scan candidates */
    while (queue_pop(&item)) {
        for (int i = 0; i < out->count; i++) {
            if (out->entries[i].addr == item.addr &&
                out->entries[i].bank == item.bank &&
                out->entries[i].size == 0)
            {
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg,
                                                    out->entries[i].source_flags);
                break;
            }
        }
    }
    printf("[FuncFinder] Switchable-bank table-run scanner added %d candidates\n",
           out->count - before_sw_scan);

    /* Split-table indirect JMP dispatch scanner (auto-detected).
     *
     * 6502 idiom for dispatching on a state index:
     *     LDA lo_table,Y    ; B9/BD lo hi
     *     STA zp
     *     LDA hi_table,Y    ; B9/BD lo hi
     *     STA zp+1
     *     JMP (zp)          ; 6C zp 00
     *
     * Two parallel byte tables store the low and high halves of the target
     * addresses separately; the combined (hi<<8 | lo) pair is the handler
     * address. This cannot be detected by the contiguous-pointer scanners
     * above because the tables aren't 16-bit LE runs.
     *
     * The existing JMP (ind) handler in walk_function only resolves vectors
     * stored in ROM — indirect JMPs through zero page (where the pointer is
     * computed at runtime from ROM tables) were invisible. This pass scans
     * every bank for the exact byte pattern, extracts both tables, and seeds
     * every table entry as a function. Table length is bounded by
     * validate_code_target on each candidate. */
    int before_split = out->count;
    int split_sites = 0;
    for (int b = 0; b <= fixed_bank; b++) {
        uint16_t scan_base = (b == fixed_bank) ? 0xC000 : 0x8000;
        uint16_t scan_end  = (b == fixed_bank) ? 0xFFFE : 0xBFFE;
        for (uint16_t pc = scan_base; pc + 12 <= scan_end; pc++) {
            uint8_t b0  = rom_read(rom, b, pc);
            if (b0 != 0xB9 && b0 != 0xBD) continue;
            uint8_t b3  = rom_read(rom, b, pc + 3);
            if (b3 != 0x85) continue;
            uint8_t b5  = rom_read(rom, b, pc + 5);
            if (b5 != 0xB9 && b5 != 0xBD) continue;
            uint8_t b8  = rom_read(rom, b, pc + 8);
            if (b8 != 0x85) continue;
            uint8_t z1    = rom_read(rom, b, pc + 4);
            uint8_t z2    = rom_read(rom, b, pc + 9);
            if (z2 != (uint8_t)(z1 + 1)) continue;

            /* Locate the JMP (zp) that consumes the staged pointer.
             *
             * Two emission shapes appear:
             *   (a) STA z2 immediately followed by JMP (zp)
             *           ... 85 z2  6C z1 00
             *   (b) STA z2 followed by a PHA/PHA fake-return prologue and
             *       then JMP (zp). This is the "manual JSR" idiom — the
             *       called handler RTSes to (high<<8|low)+1, where high/low
             *       are the two pushed bytes. Bytes:
             *           ... 85 z2  A9 hi 48  A9 lo 48  6C z1 00
             *
             * To stay generic without overfitting MM3, we accept either
             * shape. The PHA/PHA prologue is recognised by the literal
             * sequence (LDA #imm; PHA) twice. */
            uint16_t jmp_off;
            uint8_t b10 = rom_read(rom, b, pc + 10);
            if (b10 == 0x6C) {
                jmp_off = 10;                       /* shape (a) */
            } else if (b10 == 0xA9 &&
                       rom_read(rom, b, pc + 12) == 0x48 &&
                       rom_read(rom, b, pc + 13) == 0xA9 &&
                       rom_read(rom, b, pc + 15) == 0x48 &&
                       (uint32_t)pc + 18 <= (uint32_t)scan_end &&
                       rom_read(rom, b, pc + 16) == 0x6C) {
                jmp_off = 16;                       /* shape (b) */
            } else {
                continue;
            }
            uint8_t zj_lo = rom_read(rom, b, pc + jmp_off + 1);
            uint8_t zj_hi = rom_read(rom, b, pc + jmp_off + 2);
            if (zj_hi != 0 || zj_lo != z1) continue;
            uint16_t lo_tbl = rom_read(rom, b, pc + 1) |
                              ((uint16_t)rom_read(rom, b, pc + 2) << 8);
            uint16_t hi_tbl = rom_read(rom, b, pc + 6) |
                              ((uint16_t)rom_read(rom, b, pc + 7) << 8);
            if (lo_tbl < 0x8000 || hi_tbl < 0x8000) continue;
            int lo_bank = (lo_tbl >= 0xC000) ? fixed_bank : b;
            int hi_bank = (hi_tbl >= 0xC000) ? fixed_bank : b;
            int site_added = 0;
            /* Contiguous table detection: when hi_tbl == lo_tbl + 1, the
             * lo/hi bytes are interleaved in a single table. The Y/X index
             * is already doubled (ASL A or equivalent), so entries are at
             * stride 2. For true split tables (separate lo/hi arrays),
             * entries are at stride 1. */
            int entry_stride = (hi_tbl == lo_tbl + 1) ? 2 : 1;
            /* Per-entry validation strategy:
             *
             * Each table entry is INDEPENDENTLY a pointer; failures at one
             * index don't imply the table has ended. We therefore skip past
             * individual failures rather than breaking — but cap on
             * consecutive failures so we don't run off the end of the table
             * indefinitely.
             *
             * Note: is_data_region is NOT used to gate targets. Auto-generated
             * data_regions often span the lo/hi tables AND a few bytes past,
             * which can swallow the first handler (e.g. a "do nothing" RTS
             * stub immediately following the hi table). Whether the target
             * byte is code is determined by validate_code_target alone. */
            int consecutive_fail = 0;
            const int MAX_CONSEC_FAIL = 16;
            for (int i = 0; i < 256; i++) {
                int off = i * entry_stride;
                if (lo_tbl + off > 0xFFFF || hi_tbl + off > 0xFFFF) break;
                uint8_t tl = rom_read(rom, lo_bank, (uint16_t)(lo_tbl + off));
                uint8_t th = rom_read(rom, hi_bank, (uint16_t)(hi_tbl + off));
                uint16_t target = tl | ((uint16_t)th << 8);
                if (target < 0x8000 || target > 0xFFFE) {
                    if (++consecutive_fail >= MAX_CONSEC_FAIL) break;
                    continue;
                }
                int tbank = (target >= 0xC000) ? fixed_bank : b;
                if (!validate_code_target(rom, tbank, target, TABLE_RUN_MIN_VALID)) {
                    if (++consecutive_fail >= MAX_CONSEC_FAIL) break;
                    continue;
                }
                consecutive_fail = 0;
                if (!function_list_contains(out, target, tbank)) {
                    add_function_with_source(out, target, tbank, FUNCTION_SOURCE_SPLIT_TABLE);
                    site_added++;
                }
            }
            printf("[SplitTbl] site=$%04X bank=%d lo=$%04X hi=$%04X jmp_off=%u +%d\n",
                   pc, b, lo_tbl, hi_tbl, (unsigned)jmp_off, site_added);
            split_sites++;
            pc += jmp_off + 2; /* skip past the matched pattern */
        }
    }
    /* BFS for split-table candidates */
    while (queue_pop(&item)) {
        for (int i = 0; i < out->count; i++) {
            if (out->entries[i].addr == item.addr &&
                out->entries[i].bank == item.bank &&
                out->entries[i].size == 0)
            {
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg,
                                                    out->entries[i].source_flags);
                break;
            }
        }
    }
    printf("[FuncFinder] Split-table scanner: %d sites, %d new functions\n",
           split_sites, out->count - before_split);

    /* Known dispatch table scanner (game-config-driven).
     * These are PHA/PHA/RTS jump tables that use (target-1) entries where the
     * byte at (target-1) is a VALID opcode, so the conservative illegal-opcode
     * rule above misses them.
     * Format: 2-byte LE entries [LO, HI]; actual target = stored_value + 1. */
    int before_kt = out->count;
    for (int t = 0; t < cfg->known_table_count; t++) {
        int kb   = cfg->known_tables[t].bank;
        for (uint16_t a = cfg->known_tables[t].start; a < cfg->known_tables[t].end; a += 2) {
            uint8_t lo = rom_read(rom, kb, a);
            uint8_t hi = rom_read(rom, kb, a + 1);
            uint16_t target = (lo | ((uint16_t)hi << 8)) + 1;
            if (target >= 0x8000 && target <= 0xBFFD) {
                if (!function_list_contains(out, target, kb))
                    add_function_with_source(out, target, kb, FUNCTION_SOURCE_KNOWN_TABLE);
            } else if (target >= 0xC000 && target <= 0xFFFD) {
                if (!function_list_contains(out, target, fixed_bank))
                    add_function_with_source(out, target, fixed_bank, FUNCTION_SOURCE_KNOWN_TABLE);
            }
        }
    }

    /* Split-format dispatch table scanner (game-config-driven).
     * Some dispatch functions store lo-bytes and hi-bytes in separate arrays.
     * target = (hi[i]<<8 | lo[i]) + 1.
     * stride=1: packed arrays; stride=2: interleaved pairs. */
    for (int t = 0; t < cfg->known_split_table_count; t++) {
        int kb = cfg->known_split_tables[t].bank;
        int st = cfg->known_split_tables[t].stride;
        for (int si = 0; si < cfg->known_split_tables[t].count; si++) {
            uint8_t lo = rom_read(rom, kb, cfg->known_split_tables[t].lo_start + si * st);
            uint8_t hi = rom_read(rom, kb, cfg->known_split_tables[t].hi_start + si * st);
            uint16_t target = (lo | ((uint16_t)hi << 8)) + 1;
            if (target >= 0x8000 && target <= 0xBFFD) {
                if (!function_list_contains(out, target, kb))
                    add_function_with_source(out, target, kb, FUNCTION_SOURCE_KNOWN_TABLE);
            } else if (target >= 0xC000 && target <= 0xFFFD) {
                if (!function_list_contains(out, target, fixed_bank))
                    add_function_with_source(out, target, fixed_bank, FUNCTION_SOURCE_KNOWN_TABLE);
            }
        }
    }
    /* BFS for known-table candidates */
    while (queue_pop(&item)) {
        for (int i = 0; i < out->count; i++) {
            if (out->entries[i].addr == item.addr &&
                out->entries[i].bank == item.bank &&
                out->entries[i].size == 0)
            {
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg,
                                                    out->entries[i].source_flags);
                break;
            }
        }
    }
    printf("[FuncFinder] Known dispatch table scan added %d candidates\n",
           out->count - before_kt);

    /* Cross-bank function propagation: for each function discovered in a
     * switchable bank ($8000-$BFFF), check if other switchable banks have
     * identical code bytes at the same address. If the first 8 bytes match
     * and the target isn't a data region, propagate the function to that
     * bank. This catches shared code regions (e.g., Metroid's common
     * routines duplicated across all PRG banks). */
    int before_xbank = out->count;
    {
        #define XBANK_MATCH_BYTES 8
        int snapshot_count = out->count;
        for (int i = 0; i < snapshot_count; i++) {
            uint16_t addr = out->entries[i].addr;
            int src_bank = out->entries[i].bank;
            if (src_bank == fixed_bank) continue;
            if (addr < 0x8000 || addr >= 0xC000) continue;
            if (is_data_region(cfg, src_bank, addr)) continue;
            uint8_t ref[XBANK_MATCH_BYTES];
            for (int j = 0; j < XBANK_MATCH_BYTES; j++)
                ref[j] = rom_read(rom, src_bank, addr + j);
            for (int ob = 0; ob < fixed_bank; ob++) {
                if (ob == src_bank) continue;
                if (function_list_contains(out, addr, ob)) continue;
                if (is_data_region(cfg, ob, addr)) continue;
                bool match = true;
                for (int j = 0; j < XBANK_MATCH_BYTES && match; j++) {
                    if (rom_read(rom, ob, addr + j) != ref[j])
                        match = false;
                }
                if (match)
                    add_function_with_source(out, addr, ob, FUNCTION_SOURCE_XBANK);
            }
        }
        #undef XBANK_MATCH_BYTES
    }
    while (queue_pop(&item)) {
        for (int i = 0; i < out->count; i++) {
            if (out->entries[i].addr == item.addr &&
                out->entries[i].bank == item.bank &&
                out->entries[i].size == 0)
            {
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg,
                                                    out->entries[i].source_flags);
                break;
            }
        }
    }
    printf("[FuncFinder] Cross-bank propagation added %d candidates\n",
           out->count - before_xbank);
    }

    int bank_count = fixed_bank + 1;
    size_t coverage_elems = (size_t)bank_count * 65536u;
    int *coverage_map = (int *)malloc(coverage_elems * sizeof(int));
    if (coverage_map && !cfg->disable_secondary) {
        int secondary_count = classify_secondary_entries(rom, out, cfg, coverage_map);
        printf("[FuncFinder] Structural classification marked %d secondary entries\n",
               secondary_count);
    } else if (cfg->disable_secondary) {
        printf("[FuncFinder] Secondary entry classification skipped via disable_secondary\n");
    }
    if (cfg->output_prefix[0])
        snprintf(audit_path, sizeof(audit_path), "%s_finder_audit.csv", cfg->output_prefix);
    else
        snprintf(audit_path, sizeof(audit_path), "finder_audit.csv");
    if (cfg->output_prefix[0])
        snprintf(entries_audit_path, sizeof(entries_audit_path), "%s_auto_entries.csv", cfg->output_prefix);
    else
        snprintf(entries_audit_path, sizeof(entries_audit_path), "auto_entries.csv");
    audit = fopen(audit_path, "w");
    if (audit)
        fprintf(audit, "entry_type,addr,bank,status,canonical_addr,canonical_bank\n");
    entries_audit = fopen(entries_audit_path, "w");
    if (entries_audit)
        fprintf(entries_audit, "addr,bank,kind,canonical_addr,canonical_bank,covering_addr,covering_bank,evidence_count,source_flags_hex,source_flags\n");

    /* ── Evaluation: report extra_func coverage ─────────────────────────── */
    int auto_discovered_before_extras = out->count;
    {
        int standalone = 0, secondary = 0, suspicious = 0, missed = 0;
        for (int i = 0; i < cfg->extra_func_count; i++) {
            uint16_t addr = cfg->extra_funcs[i].addr;
            int bank = (cfg->extra_funcs[i].bank < 0) ? fixed_bank : cfg->extra_funcs[i].bank;
            int found_idx = function_list_find_index(out, addr, bank);
            bool is_found = (found_idx >= 0);
            if (!is_found && cfg->extra_funcs[i].bank < 0 &&
                addr >= 0x8000 && addr < 0xC000) {
                for (int b = 0; b < fixed_bank; b++) {
                    found_idx = function_list_find_index(out, addr, b);
                    if (found_idx >= 0) {
                        is_found = true; bank = b; break;
                    }
                }
            }
            if (is_found) {
                const FunctionEntry *fe = &out->entries[found_idx];
                if (fe->kind == FUNCTION_KIND_SECONDARY) {
                    printf("[Eval] extra_func $%04X bank=%d: AUTO_DISCOVERED_AS_SECONDARY via $%04X bank=%d\n",
                           addr, bank, fe->canonical_addr, fe->canonical_bank);
                    if (audit) fprintf(audit, "extra_func,%04X,%d,AUTO_DISCOVERED_AS_SECONDARY,%04X,%d\n",
                                       addr, bank, fe->canonical_addr, fe->canonical_bank);
                    secondary++;
                } else {
                    printf("[Eval] extra_func $%04X bank=%d: AUTO_DISCOVERED\n", addr, bank);
                    if (audit) fprintf(audit, "extra_func,%04X,%d,AUTO_DISCOVERED,,\n",
                                       addr, bank);
                    standalone++;
                }
            } else if (coverage_map &&
                       bank >= 0 && bank < bank_count &&
                       coverage_map[(size_t)bank * 65536u + addr] >= 0) {
                int cover_idx = coverage_map[(size_t)bank * 65536u + addr];
                printf("[Eval] extra_func $%04X bank=%d: DISCOVERABLE_AS_SECONDARY via $%04X bank=%d\n",
                       addr, bank, out->entries[cover_idx].addr, out->entries[cover_idx].bank);
                if (audit) fprintf(audit, "extra_func,%04X,%d,DISCOVERABLE_AS_SECONDARY,%04X,%d\n",
                                   addr, bank, out->entries[cover_idx].addr, out->entries[cover_idx].bank);
                secondary++;
            } else {
                const char *status = classify_missing_manual_entry(rom, cfg, addr, bank);
                printf("[Eval] extra_func $%04X bank=%d: %s\n", addr, bank, status);
                if (audit) fprintf(audit, "extra_func,%04X,%d,%s,,\n", addr, bank, status);
                if (strcmp(status, "MISSING") == 0)
                    missed++;
                else
                    suspicious++;
            }
        }
        printf("[Eval] Coverage: %d/%d extra_funcs resolved (%d standalone, %d secondary, %d suspicious, %d missing)\n",
               standalone + secondary, cfg->extra_func_count,
               standalone, secondary, suspicious, missed);
    }
    {
        int standalone = 0, secondary = 0, missing = 0;
        for (int i = 0; i < cfg->extra_label_count; i++) {
            uint16_t addr = cfg->extra_labels[i].addr;
            int bank = (cfg->extra_labels[i].bank < 0) ? fixed_bank : cfg->extra_labels[i].bank;
            int found_idx = function_list_find_index(out, addr, bank);
            if (found_idx >= 0) {
                if (out->entries[found_idx].kind == FUNCTION_KIND_SECONDARY) {
                    printf("[Eval] extra_label $%04X bank=%d: AUTO_DISCOVERED_AS_SECONDARY via $%04X bank=%d\n",
                           addr, bank,
                           out->entries[found_idx].canonical_addr,
                           out->entries[found_idx].canonical_bank);
                    if (audit) fprintf(audit, "extra_label,%04X,%d,AUTO_DISCOVERED_AS_SECONDARY,%04X,%d\n",
                                       addr, bank,
                                       out->entries[found_idx].canonical_addr,
                                       out->entries[found_idx].canonical_bank);
                    secondary++;
                } else {
                    printf("[Eval] extra_label $%04X bank=%d: AUTO_DISCOVERED_AS_STANDALONE\n",
                           addr, bank);
                    if (audit) fprintf(audit, "extra_label,%04X,%d,AUTO_DISCOVERED_AS_STANDALONE,,\n",
                                       addr, bank);
                    standalone++;
                }
            } else if (coverage_map &&
                       bank >= 0 && bank < bank_count &&
                       coverage_map[(size_t)bank * 65536u + addr] >= 0) {
                int cover_idx = coverage_map[(size_t)bank * 65536u + addr];
                printf("[Eval] extra_label $%04X bank=%d: DISCOVERABLE_AS_SECONDARY via $%04X bank=%d\n",
                       addr, bank, out->entries[cover_idx].addr, out->entries[cover_idx].bank);
                if (audit) fprintf(audit, "extra_label,%04X,%d,DISCOVERABLE_AS_SECONDARY,%04X,%d\n",
                                   addr, bank, out->entries[cover_idx].addr, out->entries[cover_idx].bank);
                secondary++;
            } else {
                printf("[Eval] extra_label $%04X bank=%d: MISSING\n", addr, bank);
                if (audit) fprintf(audit, "extra_label,%04X,%d,MISSING,,\n", addr, bank);
                missing++;
            }
        }
        printf("[Eval] Extra-label coverage: %d/%d secondary, %d standalone, %d missing\n",
               secondary, cfg->extra_label_count, standalone, missing);
    }
    {
        int fp_count = 0;
        for (int i = 0; i < auto_discovered_before_extras; i++) {
            uint16_t addr = out->entries[i].addr;
            int bank = out->entries[i].bank;
            if (is_data_region(cfg, bank, addr)) {
                printf("[Eval] FP_SUSPECT $%04X bank=%d: in data_region\n", addr, bank);
                fp_count++; continue;
            }
            if (!validate_code_target(rom, bank, addr, TABLE_RUN_MIN_VALID)) {
                printf("[Eval] FP_SUSPECT $%04X bank=%d: fails decode validation\n", addr, bank);
                fp_count++;
            }
        }
        printf("[Eval] False positive suspects: %d/%d auto-discovered functions\n",
               fp_count, auto_discovered_before_extras);
    }
    if (entries_audit) {
        for (int i = 0; i < auto_discovered_before_extras; i++) {
            char source_buf[128];
            source_flags_to_string(out->entries[i].source_flags, source_buf, sizeof(source_buf));
            fprintf(entries_audit, "%04X,%d,%s,%04X,%d,%04X,%d,%u,0x%02X,%s\n",
                    out->entries[i].addr,
                    out->entries[i].bank,
                    (out->entries[i].kind == FUNCTION_KIND_SECONDARY) ? "secondary" : "standalone",
                    out->entries[i].canonical_addr,
                    out->entries[i].canonical_bank,
                    out->entries[i].covering_addr,
                    out->entries[i].covering_bank,
                    out->entries[i].evidence_count,
                    out->entries[i].source_flags,
                    source_buf);
        }
    }

    /* Extra function seeds from game config */
    for (int i = 0; i < cfg->extra_func_count; i++) {
        int bank = (cfg->extra_funcs[i].bank < 0) ? fixed_bank : cfg->extra_funcs[i].bank;
        add_function_with_source(out, cfg->extra_funcs[i].addr, bank, FUNCTION_SOURCE_MANUAL);
    }
    while (queue_pop(&item)) {
        for (int i = 0; i < out->count; i++) {
            if (out->entries[i].addr == item.addr &&
                out->entries[i].bank == item.bank &&
                out->entries[i].size == 0)
            {
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg,
                                                    out->entries[i].source_flags);
                break;
            }
        }
    }

    if (coverage_map) {
        if (!cfg->disable_secondary)
            classify_secondary_entries(rom, out, cfg, coverage_map);
        free(coverage_map);
    }
    if (audit) {
        fclose(audit);
        printf("[Eval] Finder audit written: %s\n", audit_path);
    }
    if (entries_audit) {
        fclose(entries_audit);
        printf("[Eval] Auto-entry audit written: %s\n", entries_audit_path);
    }

    printf("[FuncFinder] Total functions found: %d (auto=%d + extras=%d)\n",
           out->count, auto_discovered_before_extras,
           out->count - auto_discovered_before_extras);
    printf("[RegProp] Bank switches detected: %d, targeted adds via propagation: %d\n",
           s_bank_switches_detected, s_regprop_targeted_adds);
}

void function_list_free(FunctionList *list) {
    (void)list; /* Static storage — nothing to free */
}
