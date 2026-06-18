/*
 * interp.c — 6502 interpreter fallback tier.
 *
 * See interp.h and docs/PHASE1_INTERP_FALLBACK_PLAN.md for the design.
 *
 * The opcode SEMANTICS here are a faithful mirror of code_generator.c's
 * emit_instruction: same flag macros (FLAG_NZ / NZC_ADD / NZC_SUB), same
 * effective-address forms (operand_addr_expr), same bus helpers
 * (nes_read/nes_write/nes_read16zp/nes_read16_jmpbug). The decode table is
 * the SAME table the recompiler uses (recompiler/src/cpu6502_decoder.c),
 * compiled into the runner — so interpreted and recompiled execution cannot
 * diverge in decode.
 *
 * The BOUNDARY CONTRACT (interp_run): the interpreter runs its own program
 * counter and never call_by_address()es a return address (that re-enters the
 * world from scratch and grows the C stack without bound — see the depth-510
 * note in code_generator.c). Nested missed calls stay inside one C frame on
 * the shared 6502 RAM stack. Control returns to native code when an RTS/RTI
 * (or an unbalanced stack pop) lifts g_cpu.S above the entry level S_floor —
 * at which point the native C call stack / continuation carries the return.
 */
#include "interp.h"
#include "nes_runtime.h"
#include "mapper.h"
#include "cpu6502_decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Tunables ---- */
#define INTERP_STEP_CAP   2000000   /* per top-level run instruction cap (watchdog) */
#define INTERP_MAX_DEPTH  64        /* nested interp_run guard (native callee misses) */

/* ---- Config (lazily initialised on first dispatch) ---- */
static int s_enabled = -1;          /* -1 = uninitialised */
extern int g_recomp_push_all_jsr;   /* defined by the generated dispatch TU */

/* ---- Covered-ness probe ----
 * interp_run dispatches a JSR/JMP target through call_by_address. If the
 * target is covered, call_by_address runs it natively and returns 1. If not,
 * the generated miss path calls back into nes_interp_dispatch; when that
 * call's address matches the armed probe we answer "miss" (return 0) WITHOUT
 * recursing, so interp_run handles the target inline on the 6502 stack. */
static int      s_probe_armed = 0;
static uint16_t s_probe_addr  = 0;

static int s_depth = 0;             /* interp_run nesting depth */

/* ---- Stats ---- */
static NesInterpStats s_stats;

void nes_interp_get_stats(NesInterpStats *out) { if (out) *out = s_stats; }
void nes_interp_frame_boundary(void) { s_stats.instrs_this_frame = 0; }

void nes_interp_set_enabled(int enabled) { s_enabled = enabled ? 1 : 0; }
int  nes_interp_is_enabled(void) { return s_enabled == 1; }

static void interp_lazy_init(void) {
    if (s_enabled != -1) return;
    /* Default: on when the build supports the stack contract; env can force off. */
    int on = g_recomp_push_all_jsr ? 1 : 0;
    const char *e = getenv("NESRECOMP_INTERP_FALLBACK");
    if (e) {
        if (!strcmp(e, "off") || !strcmp(e, "0")) on = 0;
        else if (!strcmp(e, "on") || !strcmp(e, "1")) on = g_recomp_push_all_jsr ? 1 : 0;
    }
    s_enabled = on;
    if (on && !g_recomp_push_all_jsr) s_enabled = 0; /* contract needs push_all_jsr */
}

/* ---- Side-effect-free instruction fetch (bank-correct) ---- */
static inline uint8_t interp_fetch(uint16_t pc) {
    if (pc >= 0x8000)              return mapper_peek_prg(pc);
    if (pc < 0x2000)              return g_ram[pc & 0x07FF];
    if (pc >= 0x6000)             return g_sram[pc - 0x6000];      /* $6000-$7FFF SRAM */
    return 0x00; /* $2000-$5FFF: not a code region — decodes as BRK and bails */
}

/* ---- Effective address — mirrors operand_addr_expr() exactly ---- */
static inline uint16_t interp_ea(AddrMode am, uint8_t op1, uint8_t op2) {
    uint16_t abs16 = (uint16_t)(op1 | ((uint16_t)op2 << 8));
    switch (am) {
        case AM_ZP:   return op1;
        case AM_ZPX:  return (uint8_t)(op1 + g_cpu.X);
        case AM_ZPY:  return (uint8_t)(op1 + g_cpu.Y);
        case AM_ABS:  return abs16;
        case AM_ABSX: return (uint16_t)(abs16 + g_cpu.X);
        case AM_ABSY: return (uint16_t)(abs16 + g_cpu.Y);
        case AM_INDX: return nes_read16zp((uint8_t)(op1 + g_cpu.X));
        case AM_INDY: return (uint16_t)(nes_read16zp(op1) + g_cpu.Y);
        default:      return 0;
    }
}

/* Read the operand value for a read-type op (immediate vs memory). */
static inline uint8_t interp_rd(AddrMode am, uint8_t op1, uint8_t op2) {
    if (am == AM_IMM) return op1;
    return nes_read(interp_ea(am, op1, op2));
}

/* ---- Flag helpers (identical to generated FLAG_NZ / NZC_ADD / NZC_SUB) ---- */
#define I_NZ(v) do { g_cpu.N = ((uint8_t)(v) >> 7) & 1; g_cpu.Z = ((uint8_t)(v) == 0) ? 1 : 0; } while (0)

/* Forward decl: the generated dispatcher. */
extern int call_by_address(uint16_t addr);

/* Probe + dispatch a control-transfer target.
 * Returns 1 if the target was covered and executed natively; 0 on miss
 * (caller interprets the target inline). */
static int interp_dispatch_target(uint16_t target) {
    s_probe_armed = 1;
    s_probe_addr  = target;
    int hit = call_by_address(target);
    s_probe_armed = 0;
    if (hit) s_stats.native_handoffs++;
    return hit;
}

/*
 * interp_run — execute the missed routine at `entry`, returning when control
 * leaves it back to native code (RTS/RTI/unbalanced-pop lifting S above the
 * entry level). Returns 1 if it ran to a clean boundary, 0 if it bailed
 * (watchdog / depth guard / non-code region) so the caller applies policy.
 */
static int interp_run(uint16_t entry) {
    if (s_depth >= INTERP_MAX_DEPTH) return 0;
    s_depth++;
    s_stats.runs++;

    const uint8_t S_floor = g_cpu.S;
    uint16_t ipc = entry;
    long budget = INTERP_STEP_CAP;
    uint32_t this_run = 0;
    int result = 1;

    for (;;) {
        if (--budget < 0) {
            fprintf(stderr, "[interp] WATCHDOG: run from $%04X exceeded %d instrs "
                            "(bank=%d, ipc=$%04X) — bailing\n",
                    entry, INTERP_STEP_CAP, g_current_bank, ipc);
            s_stats.watchdog_trips++;
            result = 0;
            break;
        }

        uint8_t opcode = interp_fetch(ipc);
        const OpcodeEntry *e = &g_opcode_table[opcode];
        uint8_t op1 = (e->size > 1) ? interp_fetch((uint16_t)(ipc + 1)) : 0;
        uint8_t op2 = (e->size > 2) ? interp_fetch((uint16_t)(ipc + 2)) : 0;
        uint16_t abs16 = (uint16_t)(op1 | ((uint16_t)op2 << 8));

        /* NMI is sampled between instructions (mirrors codegen's per-insn call). */
        maybe_trigger_vblank(e->cycles);
        s_stats.instrs_total++;
        s_stats.instrs_this_frame++;
        this_run++;

        uint16_t next = (uint16_t)(ipc + e->size); /* default sequential advance */

        switch (e->mnemonic) {
            /* ---- Load / Store ---- */
            case MN_LDA: g_cpu.A = interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.A); break;
            case MN_LDX: g_cpu.X = interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.X); break;
            case MN_LDY: g_cpu.Y = interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.Y); break;
            case MN_LAX: g_cpu.A = g_cpu.X = interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.A); break;
            case MN_STA: nes_write(interp_ea(e->addr_mode, op1, op2), g_cpu.A); break;
            case MN_STX: nes_write(interp_ea(e->addr_mode, op1, op2), g_cpu.X); break;
            case MN_STY: nes_write(interp_ea(e->addr_mode, op1, op2), g_cpu.Y); break;
            case MN_SAX: nes_write(interp_ea(e->addr_mode, op1, op2), (uint8_t)(g_cpu.A & g_cpu.X)); break;

            /* ---- Transfers ---- */
            case MN_TAX: g_cpu.X = g_cpu.A; I_NZ(g_cpu.X); break;
            case MN_TAY: g_cpu.Y = g_cpu.A; I_NZ(g_cpu.Y); break;
            case MN_TXA: g_cpu.A = g_cpu.X; I_NZ(g_cpu.A); break;
            case MN_TYA: g_cpu.A = g_cpu.Y; I_NZ(g_cpu.A); break;
            case MN_TSX: g_cpu.X = g_cpu.S; I_NZ(g_cpu.X); break;
            case MN_TXS: g_cpu.S = g_cpu.X; break;

            /* ---- Stack ---- */
            case MN_PHA: g_ram[0x100 + g_cpu.S] = g_cpu.A; g_cpu.S--; break;
            case MN_PLA: g_cpu.S++; g_cpu.A = g_ram[0x100 + g_cpu.S]; I_NZ(g_cpu.A); break;
            case MN_PHP: {
                uint8_t p = (uint8_t)((g_cpu.N << 7) | (g_cpu.V << 6) | 0x30 |
                                      (g_cpu.D << 3) | (g_cpu.I << 2) | (g_cpu.Z << 1) | g_cpu.C);
                g_ram[0x100 + g_cpu.S] = p; g_cpu.S--;
                break;
            }
            case MN_PLP: {
                g_cpu.S++; uint8_t p = g_ram[0x100 + g_cpu.S];
                g_cpu.N = (p >> 7) & 1; g_cpu.V = (p >> 6) & 1; g_cpu.D = (p >> 3) & 1;
                g_cpu.I = (p >> 2) & 1; g_cpu.Z = (p >> 1) & 1; g_cpu.C = p & 1;
                break;
            }

            /* ---- ALU ---- */
            case MN_ADC: {
                uint8_t m = interp_rd(e->addr_mode, op1, op2);
                uint16_t r = (uint16_t)(g_cpu.A + m + g_cpu.C);
                g_cpu.C = (r > 0xFF) ? 1 : 0;
                g_cpu.N = (r >> 7) & 1;
                g_cpu.Z = ((r & 0xFF) == 0) ? 1 : 0;
                g_cpu.V = (~((g_cpu.A) ^ (m)) & ((g_cpu.A) ^ r) & 0x80) ? 1 : 0;
                g_cpu.A = (uint8_t)(r & 0xFF);
                break;
            }
            case MN_SBC: {
                uint8_t m = interp_rd(e->addr_mode, op1, op2);
                int16_t r = (int16_t)(g_cpu.A - m - (1 - g_cpu.C));
                g_cpu.C = (r >= 0) ? 1 : 0;
                g_cpu.N = ((r & 0xFF) >> 7);
                g_cpu.Z = ((r & 0xFF) == 0) ? 1 : 0;
                g_cpu.V = (((g_cpu.A) ^ (m)) & ((g_cpu.A) ^ r) & 0x80) ? 1 : 0;
                g_cpu.A = (uint8_t)(r & 0xFF);
                break;
            }
            case MN_AND: g_cpu.A &= interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.A); break;
            case MN_ORA: g_cpu.A |= interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.A); break;
            case MN_EOR: g_cpu.A ^= interp_rd(e->addr_mode, op1, op2); I_NZ(g_cpu.A); break;

            /* ---- Shifts / Rotates ---- */
            case MN_ASL:
                if (e->addr_mode == AM_ACC) {
                    g_cpu.C = (g_cpu.A >> 7) & 1; g_cpu.A = (uint8_t)(g_cpu.A << 1); I_NZ(g_cpu.A);
                } else {
                    uint16_t a = interp_ea(e->addr_mode, op1, op2); uint8_t v = nes_read(a);
                    g_cpu.C = (v >> 7) & 1; v = (uint8_t)(v << 1); nes_write(a, v); I_NZ(v);
                }
                break;
            case MN_LSR:
                if (e->addr_mode == AM_ACC) {
                    g_cpu.C = g_cpu.A & 1; g_cpu.A >>= 1; I_NZ(g_cpu.A);
                } else {
                    uint16_t a = interp_ea(e->addr_mode, op1, op2); uint8_t v = nes_read(a);
                    g_cpu.C = v & 1; v >>= 1; nes_write(a, v); I_NZ(v);
                }
                break;
            case MN_ROL:
                if (e->addr_mode == AM_ACC) {
                    uint8_t c = g_cpu.C; g_cpu.C = (g_cpu.A >> 7) & 1;
                    g_cpu.A = (uint8_t)((g_cpu.A << 1) | c); I_NZ(g_cpu.A);
                } else {
                    uint16_t a = interp_ea(e->addr_mode, op1, op2); uint8_t v = nes_read(a);
                    uint8_t c = g_cpu.C; g_cpu.C = (v >> 7) & 1;
                    v = (uint8_t)((v << 1) | c); nes_write(a, v); I_NZ(v);
                }
                break;
            case MN_ROR:
                if (e->addr_mode == AM_ACC) {
                    uint8_t c = g_cpu.C; g_cpu.C = g_cpu.A & 1;
                    g_cpu.A = (uint8_t)((g_cpu.A >> 1) | (c << 7)); I_NZ(g_cpu.A);
                } else {
                    uint16_t a = interp_ea(e->addr_mode, op1, op2); uint8_t v = nes_read(a);
                    uint8_t c = g_cpu.C; g_cpu.C = v & 1;
                    v = (uint8_t)((v >> 1) | (c << 7)); nes_write(a, v); I_NZ(v);
                }
                break;

            /* ---- Inc / Dec ---- */
            case MN_INC: {
                uint16_t a = interp_ea(e->addr_mode, op1, op2);
                uint8_t v = (uint8_t)(nes_read(a) + 1); nes_write(a, v); I_NZ(v);
                break;
            }
            case MN_DEC: {
                uint16_t a = interp_ea(e->addr_mode, op1, op2);
                uint8_t v = (uint8_t)(nes_read(a) - 1); nes_write(a, v); I_NZ(v);
                break;
            }
            case MN_INX: g_cpu.X = (uint8_t)(g_cpu.X + 1); I_NZ(g_cpu.X); break;
            case MN_DEX: g_cpu.X = (uint8_t)(g_cpu.X - 1); I_NZ(g_cpu.X); break;
            case MN_INY: g_cpu.Y = (uint8_t)(g_cpu.Y + 1); I_NZ(g_cpu.Y); break;
            case MN_DEY: g_cpu.Y = (uint8_t)(g_cpu.Y - 1); I_NZ(g_cpu.Y); break;

            /* ---- Compare ---- */
            case MN_CMP: { uint8_t m = interp_rd(e->addr_mode, op1, op2); int r = g_cpu.A - m; g_cpu.C = (g_cpu.A >= m) ? 1 : 0; I_NZ(r & 0xFF); break; }
            case MN_CPX: { uint8_t m = interp_rd(e->addr_mode, op1, op2); int r = g_cpu.X - m; g_cpu.C = (g_cpu.X >= m) ? 1 : 0; I_NZ(r & 0xFF); break; }
            case MN_CPY: { uint8_t m = interp_rd(e->addr_mode, op1, op2); int r = g_cpu.Y - m; g_cpu.C = (g_cpu.Y >= m) ? 1 : 0; I_NZ(r & 0xFF); break; }
            case MN_BIT: {
                uint8_t m = interp_rd(e->addr_mode, op1, op2);
                g_cpu.Z = (g_cpu.A & m) ? 0 : 1; g_cpu.N = (m >> 7) & 1; g_cpu.V = (m >> 6) & 1;
                break;
            }

            /* ---- Flags ---- */
            case MN_CLC: g_cpu.C = 0; break;
            case MN_SEC: g_cpu.C = 1; break;
            case MN_CLD: g_cpu.D = 0; break;
            case MN_SED: g_cpu.D = 1; break;
            case MN_CLI: g_cpu.I = 0; break;
            case MN_SEI: g_cpu.I = 1; break;
            case MN_CLV: g_cpu.V = 0; break;

            /* ---- NOPs (NOP_READ performs the operand read for MMIO side effects) ---- */
            case MN_NOP: break;
            case MN_NOP_READ:
                if (e->addr_mode != AM_IMP && e->addr_mode != AM_ACC && e->addr_mode != AM_IMM)
                    (void)nes_read(interp_ea(e->addr_mode, op1, op2));
                break;

            /* ---- Branches (relative) ---- */
            case MN_BCC: if (!g_cpu.C) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BCS: if ( g_cpu.C) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BEQ: if ( g_cpu.Z) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BNE: if (!g_cpu.Z) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BMI: if ( g_cpu.N) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BPL: if (!g_cpu.N) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BVC: if (!g_cpu.V) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;
            case MN_BVS: if ( g_cpu.V) next = (uint16_t)(ipc + 2 + (int8_t)op1); break;

            /* ---- Jumps / Calls / Returns (the boundary contract) ---- */
            case MN_JSR: {
                uint16_t target = abs16;
                uint16_t ret = (uint16_t)(ipc + 2);   /* 6502 pushes PC+2 */
                g_ram[0x100 + g_cpu.S] = (uint8_t)((ret >> 8) & 0xFF); g_cpu.S--;
                g_ram[0x100 + g_cpu.S] = (uint8_t)(ret & 0xFF);        g_cpu.S--;
                if (interp_dispatch_target(target)) {
                    /* covered: ran natively; its RTS popped our push. Continue. */
                    next = (uint16_t)(ipc + 3);
                } else {
                    next = target;  /* miss: interpret inline (push stays on 6502 stack) */
                }
                break;
            }
            case MN_JMP: {
                uint16_t target = (e->addr_mode == AM_IND)
                                  ? nes_read16_jmpbug(abs16) : abs16;
                if (interp_dispatch_target(target)) {
                    result = 1; goto done;   /* covered: native tail-ran it */
                }
                next = target;               /* miss: interpret inline */
                break;
            }
            case MN_RTS: {
                g_cpu.S++; uint8_t lo = g_ram[0x100 + g_cpu.S];
                g_cpu.S++; uint8_t hi = g_ram[0x100 + g_cpu.S];
                uint16_t ret = (uint16_t)(((uint16_t)hi << 8) | lo) + 1;
                if (g_cpu.S > S_floor) { result = 1; goto done; }  /* returned to caller */
                next = ret;                                        /* nested return */
                break;
            }
            case MN_RTI: {
                g_cpu.S++; uint8_t p = g_ram[0x100 + g_cpu.S];
                g_cpu.N = (p >> 7) & 1; g_cpu.V = (p >> 6) & 1; g_cpu.D = (p >> 3) & 1;
                g_cpu.I = (p >> 2) & 1; g_cpu.Z = (p >> 1) & 1; g_cpu.C = p & 1;
                g_cpu.S++; uint8_t lo = g_ram[0x100 + g_cpu.S];
                g_cpu.S++; uint8_t hi = g_ram[0x100 + g_cpu.S];
                uint16_t ret = (uint16_t)(((uint16_t)hi << 8) | lo);  /* RTI does NOT +1 */
                if (g_cpu.S > S_floor) { result = 1; goto done; }
                next = ret;
                break;
            }

            /* ---- BRK / illegal: mirror codegen (BRK hook; illegal = sized skip) ---- */
            case MN_BRK:
                nes_brk_executed(ipc);
                result = 1; goto done;   /* codegen returns from the enclosing fn at BRK */
            case MN_ILLEGAL:
            default:
                /* code_generator.c treats MN_ILLEGAL as a sized NOP skip — match it. */
                break;
        }

        ipc = next;

        /* Unifying boundary rule: any instruction that lifts S above the entry
         * frame (terminating RTS handled above, or a PLA/PLA-style bail that
         * unwinds the caller's frame) means we've returned to native code. */
        if (g_cpu.S > S_floor) { result = 1; goto done; }
    }

done:
    if (this_run > s_stats.max_instrs_run) s_stats.max_instrs_run = this_run;
    s_depth--;
    return result;
}

/* ---- Entry from the generated dispatcher ---- */
int nes_interp_dispatch(uint16_t addr) {
    interp_lazy_init();

    /* Per-game manual override still wins (e.g. Zelda SRAM remap). */
    if (game_dispatch_override(addr)) return 1;

    /* Always record the miss so the discovery/manifest loop sees every one,
     * even though the game keeps running. */
    nes_record_dispatch_miss(addr);

    /* Covered-ness probe answer for an in-flight interp_run dispatch. */
    if (s_probe_armed && addr == s_probe_addr) {
        s_probe_armed = 0;
        return 0;   /* "miss" — interp_run will handle the target inline */
    }

    if (s_enabled == 1) {
        if (interp_run(addr)) return 1;   /* handled — game continues */
        /* interp declined (watchdog / depth) — fall through to legacy policy. */
    }

    nes_dispatch_miss_apply_policy(addr);
    return 0;
}
