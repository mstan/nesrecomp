/*
 * cyc_mod.h - what a game's trusted mod code can do with the machine on the
 * cycle backend.
 *
 * A mod is game code linked into the program (nesrecomp_add_cycle_game ...
 * HOST_EXTRAS, registered through runner/include/mod_runtime.h) that runs at
 * hook sites (cyc_hooks.h), at the end of each frame (cyc_host_extras.h
 * frame_end) and when the window presents a picture (cyc_render.h). Everything
 * below acts at an instruction boundary; none of it is guest time.
 *
 * Memory. cyc_mod_peek reads what a CPU read would return from RAM or ROM
 * without the read's side effects (I/O and open bus read 0 and return false
 * through cyc_mod_peek_ok). cyc_mod_poke stores to CPU RAM (any mirror), the
 * FDS's PRG RAM ($6000-$DFFF) or a cartridge's work RAM at $6000-$7FFF as
 * mapped now, through the compiled-code write watch, exactly as a CPU store
 * would change them; anything else is refused. Outside an isolated call these
 * change the running machine.
 *
 * Registers. cyc_mod_regs / cyc_mod_set_regs read and write A, X, Y, S and P
 * at the boundary (a hook site's callback sees the registers the program
 * entered the routine with). The PC is the scheduler's: a hook changes control
 * flow only by handling the routine (cyc_hooks.h).
 *
 * Isolated calls. Between cyc_mod_isolate_begin() and cyc_mod_isolate_end()
 * the machine is a scratch copy: the mod may poke RAM and call the program's
 * own routines with cyc_mod_call(), which JSRs to a routine with the given
 * registers and runs recompiled code, RAM views or the interpreter - whatever
 * would run there - until that routine returns. No clock runs meanwhile: no
 * PPU dot, APU or mapper clock, DMA, interrupt or disk activity, so a routine
 * that computes from memory (a level decoder, an object's movement) runs as it
 * would in the game, while one that waits for the hardware runs out of its
 * cycle budget and fails. cyc_mod_isolate_end() then restores everything the
 * calls and pokes changed - CPU, RAM, PRG/work RAM, CHR RAM, PPU, APU, mapper
 * and FDS drive state, compiled-view validity - bit for bit, so the game's
 * time line is exactly as if nothing ran. Hook sites do not fire and device
 * events are not recorded inside (the ring keeps a MOD_CALL summary per
 * routine per frame, and a MOD_FAIL event for a call that did not return).
 * Scopes do not nest.
 *
 * Committed calls. cyc_mod_call_commit() (outside a scope) runs a routine the
 * same way and then makes its memory effects real: every byte of CPU RAM and
 * PRG / work RAM it changed is stored into the running machine (through the
 * write watch, as its own stores would have been), and the registers it
 * returned with become the CPU's. It takes no guest time - the routine is
 * replaced by its result - so it is only for a hook site the mod handles
 * (cyc_hooks.h) whose original work the mod still wants done: the routine
 * must talk to memory only, and a call that stores to a device register is
 * refused (nothing committed), since its device effects would be lost.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t cyc_mod_peek(uint16_t addr);
bool    cyc_mod_peek_ok(uint16_t addr, uint8_t *value);
bool    cyc_mod_poke(uint16_t addr, uint8_t value);

/* Optional trusted content tools. Obtaining writable PRG disables generated
 * ROM dispatch until another image is loaded: generated opcodes/operands and
 * folded NROM data may otherwise disagree with the edited image (also from
 * compiled RAM views). Image files on disk are never changed. */
uint8_t *cyc_mod_prg_data_rw(size_t *size);
/* Mapped CHR RAM only; for a finished tile transfer, outside isolated calls. */
bool cyc_mod_chr_poke(uint16_t ppu_addr, uint8_t value);
/* CPU writes to $2006 (completed address) and $2007 (address/value before the
 * write). Optional content-tool callback; suppressed during isolated calls.
 * The hook is host policy and is not serialized as machine state. */
typedef void (*CycModPpuWriteHook)(unsigned reg, uint16_t addr, uint8_t value, unsigned increment);
void cyc_mod_set_ppu_write_hook(CycModPpuWriteHook hook);

typedef struct {
    uint8_t  a, x, y, s, p;     /* p: N V - B D I Z C as pushed (B and bit 5 ignored on set) */
    uint16_t pc;                /* read only */
} CycModRegs;
void cyc_mod_regs(CycModRegs *out);
void cyc_mod_set_regs(const CycModRegs *in);

bool cyc_mod_isolate_begin(void);
bool cyc_mod_isolated(void);
/* JSR `routine` with A, X, Y, S and P from *regs (S is where the return
 * address is pushed), run it until it returns, and give back the registers it
 * returned with. False when it did not return within the budget
 * (cyc_mod_set_call_budget, CPU cycles; default 2,000,000), jammed, or
 * returned past the caller's frame. */
bool cyc_mod_call(uint16_t routine, CycModRegs *regs);
void cyc_mod_isolate_end(void);
void cyc_mod_set_call_budget(uint64_t cycles);
/* Optional trusted routine observation. Defaults: no return observer and no
 * hooks in isolated scopes. Enable isolated hooks explicitly AFTER begin;
 * scope end resets the permission. The game's callbacks must keep their own
 * speculative presentation/simulation state separate from live state. */
void cyc_mod_set_return_hook(void (*hook)(void));
void cyc_mod_allow_isolated_hooks(bool allow);
bool cyc_mod_call_commit_hooked(uint16_t routine, CycModRegs *regs);
bool cyc_mod_call_commit(uint16_t routine, CycModRegs *regs);

typedef struct {
    uint64_t scopes, calls, failures, cycles;
} CycModStats;
void cyc_mod_stats(CycModStats *out);

/* Host: end of frame (the ring's MOD_CALL summaries). */
void cyc_mod_frame_end(void);

#ifdef __cplusplus
}
#endif
