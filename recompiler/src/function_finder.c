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
 */
#include "function_finder.h"
#include "cpu6502_decoder.h"
#include <stdio.h>
#include <string.h>

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

static void add_function(FunctionList *list, uint16_t addr, int bank) {
    if (function_list_contains(list, addr, bank)) return;
    if (list->count >= MAX_FUNCTIONS) {
        fprintf(stderr, "function_finder: MAX_FUNCTIONS exceeded!\n");
        return;
    }
    list->entries[list->count++] = (FunctionEntry){addr, bank, 0};
    queue_push(addr, bank);
}

bool function_list_contains(const FunctionList *list, uint16_t addr, int bank) {
    for (int i = 0; i < list->count; i++) {
        if (list->entries[i].addr == addr && list->entries[i].bank == bank)
            return true;
    }
    return false;
}

/* Walk one function starting at addr in the given bank context.
 * Returns number of instructions decoded.
 *
 * Pending-branch tracking: branches can jump forward past RTS/JMP into code
 * that's only reachable via that branch. We continue past stop instructions
 * while pending forward targets remain, so JSR calls in those sections are
 * discovered. */
static int walk_function(const NESRom *rom, FunctionList *list,
                         uint16_t start_addr, int switchable_bank,
                         const GameConfig *cfg) {
    uint16_t pc = start_addr;
    int insn_count = 0;
    int fixed_bank = rom->prg_banks - 1;

    /* Pending forward branch targets */
    uint16_t pending[256];
    int pending_count = 0;

    for (int i = 0; i < MAX_INSNS_PER_FUNC; i++) {
        /* Remove this address from pending now that we're visiting it */
        for (int p = 0; p < pending_count; p++) {
            if (pending[p] == pc) {
                pending[p] = pending[--pending_count];
                break;
            }
        }

        /* Determine which bank to read from */
        int read_bank = (pc >= 0xC000) ? fixed_bank : switchable_bank;
        if (pc < 0x8000) break; /* Not ROM */

        uint8_t opcode = rom_read(rom, read_bank, pc);
        const OpcodeEntry *entry = &g_opcode_table[opcode];
        insn_count++;

        if (entry->mnemonic == MN_ILLEGAL) {
            pc += 1;
            continue;
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
                    add_function(list, tgt, tbank);
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
                    add_function(list, tramp->addr, fixed_bank);
                    uint8_t disp_bank = rom_read(rom, read_bank, pc + 3);
                    uint8_t disp_lo   = rom_read(rom, read_bank, pc + 4);
                    uint8_t disp_hi   = rom_read(rom, read_bank, pc + 5);
                    uint16_t disp_addr = (disp_lo | ((uint16_t)disp_hi << 8)) + 1;
                    if (disp_addr >= 0x8000) {
                        int tbank = (disp_addr >= 0xC000) ? fixed_bank : (int)disp_bank;
                        add_function(list, disp_addr, tbank);
                    }
                    pc += 3 + tramp->inline_bytes;
                    continue;
                }
            }

            if (target >= 0xC000) {
                /* Fixed bank target — always knowable */
                add_function(list, target, fixed_bank);
            } else if (target >= 0x8000) {
                if (switchable_bank != fixed_bank) {
                    /* Inside a switchable bank: bank is known */
                    add_function(list, target, switchable_bank);
                } else {
                    /* Fixed bank calling switchable region: bank unknown statically.
                     * Add for ALL switchable banks so each gets a correct body.
                     * At runtime call_by_address() selects via g_current_bank. */
                    for (int _b = 0; _b < fixed_bank; _b++)
                        add_function(list, target, _b);
                }
            }
            pc += entry->size;
            continue;
        }

        if (entry->mnemonic == MN_JMP && entry->addr_mode == AM_ABS) {
            uint8_t lo = rom_read(rom, read_bank, pc + 1);
            uint8_t hi = rom_read(rom, read_bank, pc + 2);
            uint16_t target = lo | ((uint16_t)hi << 8);
            if (target >= 0xC000) {
                add_function(list, target, fixed_bank);
            } else if (target >= 0x8000) {
                if (switchable_bank != fixed_bank) {
                    add_function(list, target, switchable_bank);
                } else {
                    for (int _b = 0; _b < fixed_bank; _b++)
                        add_function(list, target, _b);
                }
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
                int vec_bank = (vec_addr >= 0xC000) ? fixed_bank : switchable_bank;
                uint8_t tlo = rom_read(rom, vec_bank, vec_addr);
                uint8_t thi = rom_read(rom, vec_bank, vec_addr + 1);
                uint16_t target = tlo | ((uint16_t)thi << 8);
                if (target >= 0x8000) {
                    int tbank = (target >= 0xC000) ? fixed_bank : switchable_bank;
                    add_function(list, target, tbank);
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

    int fixed_bank = rom->prg_banks - 1;

    /* Seed from interrupt vectors */
    printf("[FuncFinder] Seeding from NMI=$%04X RESET=$%04X IRQ=$%04X\n",
           rom->nmi_vector, rom->reset_vector, rom->irq_vector);

    add_function(out, rom->reset_vector, fixed_bank);
    add_function(out, rom->nmi_vector,   fixed_bank);
    add_function(out, rom->irq_vector,   fixed_bank);

    /* Seed each switchable bank from $8000 so cross-bank calls from the
     * fixed bank get proper per-bank function bodies. */
    for (int b = 0; b < fixed_bank; b++) {
        uint8_t first = rom_read(rom, b, 0x8000);
        if (g_opcode_table[first].mnemonic != MN_ILLEGAL)
            add_function(out, 0x8000, b);
    }

    /* BFS */
    WorkItem item;
    while (queue_pop(&item)) {
        /* Find this entry and walk it */
        for (int i = 0; i < out->count; i++) {
            if (out->entries[i].addr == item.addr &&
                out->entries[i].bank == item.bank &&
                out->entries[i].size == 0)
            {
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg);
                break;
            }
        }
    }

    /* ROM pointer scan: scan fixed bank for 16-bit values that look like
     * function pointers into ROM. Discovers targets of RAM-based dispatch
     * tables (loaded from ROM at runtime, invisible to JSR BFS).
     * Criteria: value in $C000-$FFFD, first byte at target is non-illegal opcode. */
    int before_scan = out->count;
    for (uint16_t off = 0; off <= 0x3FFD; off++) {
        uint16_t addr = 0xC000 + off;
        uint8_t lo = rom_read(rom, fixed_bank, addr);
        uint8_t hi = rom_read(rom, fixed_bank, addr + 1);
        uint16_t candidate = lo | ((uint16_t)hi << 8);
        if (candidate < 0xC000 || candidate > 0xFFFD) continue;
        /* Add candidate if it has a valid opcode. */
        uint8_t first_byte = rom_read(rom, fixed_bank, candidate);
        if (g_opcode_table[first_byte].mnemonic != MN_ILLEGAL &&
            !function_list_contains(out, candidate, fixed_bank))
            add_function(out, candidate, fixed_bank);
        /* Also add candidate+1: RTS-as-JMP dispatch tables store target-1,
         * so the actual called function is one byte past the stored pointer.
         * Check this even when candidate itself has an illegal opcode. */
        uint16_t candidate1 = candidate + 1;
        if (candidate1 >= 0xC000 && candidate1 <= 0xFFFD &&
            !function_list_contains(out, candidate1, fixed_bank)) {
            uint8_t fb1 = rom_read(rom, fixed_bank, candidate1);
            if (g_opcode_table[fb1].mnemonic != MN_ILLEGAL)
                add_function(out, candidate1, fixed_bank);
        }
    }
    /* BFS again for newly added candidates */
    while (queue_pop(&item)) {
        for (int i = 0; i < out->count; i++) {
            if (out->entries[i].addr == item.addr &&
                out->entries[i].bank == item.bank &&
                out->entries[i].size == 0)
            {
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg);
                break;
            }
        }
    }
    printf("[FuncFinder] Fixed-bank pointer scan added %d candidates\n", out->count - before_scan);

    /* Switchable-bank pointer scan: scan for RTS-as-JMP dispatch table entries.
     * These tables store (target-1) as 16-bit LE values in the switchable bank.
     * Pattern: candidate byte = ILLEGAL opcode (not a real entry point),
     *          candidate+1 byte = valid opcode (the real function start).
     * Only add candidate+1 in this case — avoids false positives from data bytes
     * that happen to form valid-looking addresses pointing to real code. */
    int before_sw_scan = out->count;
    for (int b = 0; b < fixed_bank; b++) {
        for (uint16_t off = 0; off <= 0x3FFD; off++) {
            uint16_t addr = 0x8000 + off;
            uint8_t lo = rom_read(rom, b, addr);
            uint8_t hi = rom_read(rom, b, addr + 1);
            uint16_t candidate = lo | ((uint16_t)hi << 8);
            if (candidate < 0x8000 || candidate > 0xBFFD) continue;
            /* Only the RTS-as-JMP case: candidate has ILLEGAL opcode,
             * meaning the stored value is (target-1) — skip candidate itself,
             * add candidate+1 if it has a valid opcode. */
            uint8_t fb = rom_read(rom, b, candidate);
            if (g_opcode_table[fb].mnemonic != MN_ILLEGAL) continue;
            uint16_t candidate1 = candidate + 1;
            if (candidate1 > 0xBFFD) continue;
            if (function_list_contains(out, candidate1, b)) continue;
            uint8_t fb1 = rom_read(rom, b, candidate1);
            if (g_opcode_table[fb1].mnemonic != MN_ILLEGAL)
                add_function(out, candidate1, b);
        }
    }
    /* BFS for switchable-bank scan candidates */
    while (queue_pop(&item)) {
        for (int i = 0; i < out->count; i++) {
            if (out->entries[i].addr == item.addr &&
                out->entries[i].bank == item.bank &&
                out->entries[i].size == 0)
            {
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg);
                break;
            }
        }
    }
    printf("[FuncFinder] Switchable-bank pointer scan added %d candidates\n",
           out->count - before_sw_scan);

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
                    add_function(out, target, kb);
            } else if (target >= 0xC000 && target <= 0xFFFD) {
                if (!function_list_contains(out, target, fixed_bank))
                    add_function(out, target, fixed_bank);
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
        for (int i = 0; i < cfg->known_split_tables[t].count; i++) {
            uint8_t lo = rom_read(rom, kb, cfg->known_split_tables[t].lo_start + i * st);
            uint8_t hi = rom_read(rom, kb, cfg->known_split_tables[t].hi_start + i * st);
            uint16_t target = (lo | ((uint16_t)hi << 8)) + 1;
            if (target >= 0x8000 && target <= 0xBFFD) {
                if (!function_list_contains(out, target, kb))
                    add_function(out, target, kb);
            } else if (target >= 0xC000 && target <= 0xFFFD) {
                if (!function_list_contains(out, target, fixed_bank))
                    add_function(out, target, fixed_bank);
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
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg);
                break;
            }
        }
    }
    printf("[FuncFinder] Known dispatch table scan added %d candidates\n",
           out->count - before_kt);

    /* Extra function seeds from game config: functions dispatched dynamically
     * but not discoverable via static pointer/table scans. */
    for (int i = 0; i < cfg->extra_func_count; i++) {
        int bank = (cfg->extra_funcs[i].bank < 0) ? fixed_bank : cfg->extra_funcs[i].bank;
        add_function(out, cfg->extra_funcs[i].addr, bank);
    }
    /* BFS for hardcoded seeds */
    while (queue_pop(&item)) {
        for (int i = 0; i < out->count; i++) {
            if (out->entries[i].addr == item.addr &&
                out->entries[i].bank == item.bank &&
                out->entries[i].size == 0)
            {
                out->entries[i].size = walk_function(rom, out, item.addr, item.bank, cfg);
                break;
            }
        }
    }

    printf("[FuncFinder] Total functions found: %d\n", out->count);
}

void function_list_free(FunctionList *list) {
    (void)list; /* Static storage — nothing to free */
}
