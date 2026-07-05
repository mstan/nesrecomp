/*
 * interp_selftest.c — deterministic unit test of the Phase-1 interpreter.
 *
 * Links the real interp.c + cpu6502_decoder.c against a minimal stubbed
 * runtime, then drives nes_interp_dispatch over hand-crafted 6502 programs in
 * g_ram and asserts CPU/memory results. Exercises the actual integration path:
 *   - arithmetic + flags
 *   - the S_floor boundary contract (RTS returns control above the entry frame)
 *   - nested MISSED calls handled inside one C frame on the 6502 stack
 *     (via the call_by_address probe) — i.e. the depth-510-safe path
 *   - branch loops
 *   - covered-target handoff (call_by_address returns a "native" result)
 *   - indexed / (indirect),Y / read-modify-write addressing
 *
 * Build/run from a VS dev shell:
 *   cl /nologo /I runner/include /I recompiler/src \
 *      runner/src/interp.c recompiler/src/cpu6502_decoder.c \
 *      tests/interp_selftest.c /Fe:interp_selftest.exe
 *   ./interp_selftest.exe        # exit code == number of failed checks (0 = pass)
 */
#include "nes_runtime.h"
#include "mapper.h"
#include "interp.h"
#include "cpu6502_decoder.h"
#include <stdio.h>
#include <string.h>

/* ---- Stubbed runtime (only what interp.c references) ---- */
CPU6502State g_cpu;
uint8_t      g_ram[0x0800];
uint8_t      g_sram[0x2000];
int          g_current_bank = 0;
int          g_recomp_push_all_jsr = 1;   /* interpreter precondition satisfied */

static int s_native_calls = 0;

uint8_t nes_read(uint16_t a) {
    if (a < 0x2000)                return g_ram[a & 0x07FF];
    if (a >= 0x6000 && a < 0x8000) return g_sram[a - 0x6000];
    return 0;
}
void nes_write(uint16_t a, uint8_t v) {
    if (a < 0x2000)                g_ram[a & 0x07FF] = v;
    else if (a >= 0x6000 && a < 0x8000) g_sram[a - 0x6000] = v;
}
uint16_t nes_read16(uint16_t a)  { return (uint16_t)(nes_read(a) | (nes_read((uint16_t)(a + 1)) << 8)); }
uint16_t nes_read16zp(uint8_t z) { return (uint16_t)(g_ram[z] | (g_ram[(uint8_t)(z + 1)] << 8)); }
uint16_t nes_read16_jmpbug(uint16_t a) {
    uint16_t hi = (uint16_t)((a & 0xFF00) | ((a + 1) & 0xFF));
    return (uint16_t)(nes_read(a) | (nes_read(hi) << 8));
}
uint8_t mapper_peek_prg(uint16_t a) { return g_ram[a & 0x07FF]; } /* unused by RAM-resident tests */
void maybe_trigger_vblank(int c) { (void)c; }
int  game_dispatch_override(uint16_t a) { (void)a; return 0; }
void nes_record_dispatch_miss(uint16_t a) { (void)a; }
void nes_dispatch_miss_apply_policy(uint16_t a) { (void)a; }
void nes_brk_executed(uint16_t pc) { (void)pc; }

/* Mimics the generated dispatcher: a "covered" address runs natively (here a
 * side effect + the native callee's RTS pop, S+=2, then return 1); an
 * uncovered address routes to nes_interp_dispatch exactly like emit_dispatch's
 * `default: return nes_interp_dispatch(addr)` (the armed probe answers it). */
int call_by_address(uint16_t a) {
    if (a == 0x0700) {            /* the one "covered/native" function */
        s_native_calls++;
        g_ram[0x20] = 0x99;       /* observable native side effect */
        g_cpu.S += 2;             /* native RTS pops the caller's JSR push */
        return 1;
    }
    return nes_interp_dispatch(a);
}

/* ---- Test scaffolding ---- */
static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg) do { g_checks++; if (!(cond)) { printf("  FAIL: %s\n", msg); g_fails++; } \
                              else { printf("  ok:   %s\n", msg); } } while (0)

static void fresh(void) {
    memset(g_ram, 0, sizeof g_ram);
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.S = 0xFD;               /* simulate a caller having pushed a return */
    g_ram[0x1FE] = 0x00;          /* dummy caller return addr lo */
    g_ram[0x1FF] = 0x80;          /* dummy caller return addr hi */
}
static void load(uint16_t addr, const uint8_t *code, int n) {
    for (int i = 0; i < n; i++) g_ram[(addr + i) & 0x07FF] = code[i];
}

int main(void) {
    int r;

    printf("[T1] arithmetic + RTS boundary\n");
    fresh();
    { uint8_t p[] = {0xA9,0x05, 0x18, 0x69,0x03, 0x85,0x10, 0x60}; /* LDA#5;CLC;ADC#3;STA$10;RTS */
      load(0x0600, p, sizeof p); }
    r = nes_interp_dispatch(0x0600);
    CHECK(r == 1,            "T1 dispatch handled");
    CHECK(g_cpu.A == 0x08,   "T1 A == 0x08");
    CHECK(g_ram[0x10] == 0x08,"T1 [$10] == 0x08");
    CHECK(g_cpu.C == 0,      "T1 C == 0");
    CHECK(g_cpu.Z == 0 && g_cpu.N == 0, "T1 Z=0 N=0");
    CHECK(g_cpu.S == 0xFF,   "T1 S returned above floor (0xFF)");

    printf("[T2] nested MISSED JSR handled in one frame\n");
    fresh();
    { uint8_t a[] = {0x20,0x10,0x06, 0xA9,0xAA, 0x85,0x11, 0x60}; /* JSR$0610;LDA#AA;STA$11;RTS */
      uint8_t b[] = {0xA2,0x07, 0x86,0x12, 0x60};                  /* LDX#7;STX$12;RTS */
      load(0x0600, a, sizeof a); load(0x0610, b, sizeof b); }
    r = nes_interp_dispatch(0x0600);
    CHECK(r == 1,             "T2 dispatch handled");
    CHECK(g_ram[0x12] == 0x07,"T2 nested routine ran ([$12]==0x07)");
    CHECK(g_cpu.X == 0x07,    "T2 X == 0x07");
    CHECK(g_ram[0x11] == 0xAA,"T2 caller resumed after nested RTS ([$11]==0xAA)");
    CHECK(g_cpu.A == 0xAA,    "T2 A == 0xAA");
    CHECK(g_cpu.S == 0xFF,    "T2 S returned above floor");

    printf("[T3] branch loop (DEX/BNE)\n");
    fresh();
    { uint8_t p[] = {0xA2,0x03, 0xCA, 0xD0,0xFD, 0x86,0x13, 0x60}; /* LDX#3; DEX; BNE -3; STX$13; RTS */
      load(0x0600, p, sizeof p); }
    r = nes_interp_dispatch(0x0600);
    CHECK(r == 1,             "T3 dispatch handled");
    CHECK(g_cpu.X == 0x00,    "T3 loop ran to X==0");
    CHECK(g_cpu.Z == 1,       "T3 Z==1 at exit");
    CHECK(g_ram[0x13] == 0x00,"T3 [$13]==0x00");

    printf("[T4] covered-target handoff (call_by_address native)\n");
    fresh();
    s_native_calls = 0;
    { uint8_t p[] = {0x20,0x00,0x07, 0xA9,0xBB, 0x85,0x14, 0x60}; /* JSR$0700(covered);LDA#BB;STA$14;RTS */
      load(0x0600, p, sizeof p); }
    r = nes_interp_dispatch(0x0600);
    CHECK(r == 1,             "T4 dispatch handled");
    CHECK(s_native_calls == 1,"T4 native function invoked once");
    CHECK(g_ram[0x20] == 0x99,"T4 native side effect observed ([$20]==0x99)");
    CHECK(g_ram[0x14] == 0xBB,"T4 interp resumed after native call ([$14]==0xBB)");
    CHECK(g_cpu.A == 0xBB,    "T4 A == 0xBB");
    CHECK(g_cpu.S == 0xFF,    "T4 S returned above floor");

    printf("[T5] SBC with carry (no borrow)\n");
    fresh();
    { uint8_t p[] = {0xA9,0x05, 0x38, 0xE9,0x03, 0x85,0x15, 0x60}; /* LDA#5;SEC;SBC#3;STA$15;RTS */
      load(0x0600, p, sizeof p); }
    r = nes_interp_dispatch(0x0600);
    CHECK(r == 1,             "T5 dispatch handled");
    CHECK(g_cpu.A == 0x02,    "T5 A == 0x02");
    CHECK(g_ram[0x15] == 0x02,"T5 [$15]==0x02");
    CHECK(g_cpu.C == 1,       "T5 C==1 (no borrow)");

    printf("[T6] absolute,X store (EA = base + X)\n");
    fresh();
    { uint8_t p[] = {0xA2,0x03, 0xA9,0x5A, 0x9D,0x40,0x02, 0x60}; /* LDX#3;LDA#5A;STA $0240,X;RTS */
      load(0x0600, p, sizeof p); }
    r = nes_interp_dispatch(0x0600);
    CHECK(r == 1,              "T6 dispatch handled");
    CHECK(g_ram[0x243] == 0x5A,"T6 STA $0240,X wrote $0243");

    printf("[T7] (indirect),Y store (EA = [zp] + Y)\n");
    fresh();
    { uint8_t p[] = {0xA9,0x50, 0x85,0x30, 0xA9,0x02, 0x85,0x31, 0xA0,0x02, 0xA9,0xC3, 0x91,0x30, 0x60};
      /* ptr $30/$31 = $0250; LDY#2; LDA#C3; STA ($30),Y -> $0252; RTS */
      load(0x0600, p, sizeof p); }
    r = nes_interp_dispatch(0x0600);
    CHECK(r == 1,              "T7 dispatch handled");
    CHECK(g_ram[0x252] == 0xC3,"T7 STA ($30),Y wrote $0252");

    printf("[T8] INC zp read-modify-write\n");
    fresh();
    { uint8_t p[] = {0xA9,0x41, 0x85,0x18, 0xE6,0x18, 0xE6,0x18, 0x60}; /* LDA#41;STA$18;INC$18;INC$18;RTS */
      load(0x0600, p, sizeof p); }
    r = nes_interp_dispatch(0x0600);
    CHECK(r == 1,              "T8 dispatch handled");
    CHECK(g_ram[0x18] == 0x43, "T8 [$18] incremented 0x41 -> 0x43");
    CHECK(g_cpu.Z == 0 && g_cpu.N == 0, "T8 flags from INC result");

    printf("\n==== interp self-test: %d checks, %d failures ====\n", g_checks, g_fails);
    return g_fails;
}
