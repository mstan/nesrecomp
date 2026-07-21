/*
 * nes_runtime.h — NES runtime interface
 * Shared between runner/ and generated/ code.
 * Generated code includes this; runner implements it.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ---- CPU State ---- */
typedef struct {
    uint8_t A, X, Y, S, P;  /* Accumulator, X, Y, Stack Pointer, Processor Status */
    /* Exploded flags for easier codegen */
    uint8_t N, V, D, I, Z, C;
} CPU6502State;

extern CPU6502State g_cpu;
extern uint8_t      g_ram[0x0800];     /* 2KB work RAM */
extern int          g_bail_active;     /* set by stack_bail_func return; checked at JSR sites */

/* (Legacy write_bp_* and follower mechanisms removed — superseded by
 *  Tier 2.5 rdb_watch_add in reverse_debug.c.  See REVERSE_DEBUGGER.md.) */
extern uint8_t      g_sram[0x2000];    /* 8KB battery-backed SRAM ($6000-$7FFF) */
extern uint8_t      g_chr_ram[0x2000]; /* 8KB CHR RAM/ROM */
extern int          g_chr_is_rom;      /* 1 = CHR ROM (ignore $2007 writes to $0000-$1FFF) */
extern uint8_t      g_ppu_oam[0x100];  /* 64 sprites x 4 bytes */
extern uint8_t      g_ppu_pal[0x20];   /* Palette $3F00-$3F1F */
extern uint8_t      g_ppu_nt[0x1000];  /* Nametable RAM $2000-$2FFF */

/* ---- Memory Interface ---- */
uint8_t  nes_read(uint16_t addr);
void     nes_write(uint16_t addr, uint8_t val);

/* Hooked read emitted by the recompiler for game.toml [[ram_read_hook]]
 * addresses (absolute and, with `indexed = true`, absolute-indexed reads).
 * Game policy in extras.c dispatches on (pc, addr) and returns either the
 * real value or a virtualized one; the default hook returns val unchanged. */
uint8_t game_ram_read_hook(uint16_t pc, uint16_t addr, uint8_t val);
static inline uint8_t nes_read_hooked(uint16_t pc, uint16_t addr) {
    return game_ram_read_hook(pc, addr, nes_read(addr));
}
uint16_t nes_read16(uint16_t addr);       /* Read 16-bit little-endian */
uint16_t nes_read16zp(uint8_t zp_addr);  /* Zero-page 16-bit (wraps at $FF) */

/* JMP (indirect) NMOS 6502 page-wrap erratum: when the indirect operand is
 * $xxFF, the high byte is fetched from $xx00 (same page) instead of $(xx+1)00.
 * This reproduces the bug exactly. Use ONLY for JMP (abs) — every other 16-bit
 * read should keep using nes_read16. See NESdev errata.
 *   addr      = vector pointer (the operand of JMP (abs))
 *   returns   = (hi << 8) | lo where lo = nes_read(addr) and
 *               hi = nes_read((addr & 0xFF00) | ((addr+1) & 0xFF)) */
uint16_t nes_read16_jmpbug(uint16_t addr);
void     nes_trace_sram_fetch(uint16_t addr, uint8_t val);

/* ---- Dispatch ---- */
/* Called for JMP (indirect) — dispatch to the correct recompiled function */
int call_by_address(uint16_t addr);  /* returns 1 on hit, 0 on miss */
/* Cross-8KB dispatch with a caller-bank fallback: if the runtime bank-register
 * lookup misses (stale g_current_bank), retry the dispatch keyed on the caller's
 * statically-known bank.  caller_bank < 0 disables the fallback (== call_by_address). */
int call_by_address_cb(uint16_t addr, int caller_bank);
/* Depth-counted dispatch used by generated JSR sites; drives deferred JMP-tail
 * targets from a flat loop at the outermost frame (see runtime.c trampoline). */
int nes_dispatch_call(uint16_t addr, int caller_bank);
/* Dispatch for generated JMP tails: defers when already inside a dispatch so
 * JMP loop chains cannot grow the C stack. */
int call_by_address_tail(uint16_t addr, int caller_bank);
/* Dump the always-on ring of recent dispatches (post-mortem attribution). */
void nes_dump_dispatch_ring(void);
/* Push a context marker into the dispatch ring (kind e.g. 'N'/'n' = NMI
 * enter/exit; tag = vblank depth or other context id). */
void nes_dring_mark(char kind, uint16_t tag);
/* Window base of the currently executing generated function (see runtime.c);
 * generated JSR pushes use it to mint true CPU return addresses. */
extern uint16_t g_code_window_base;

/* Logging for dispatch misses.
 * nes_log_dispatch_miss_bank is the full form used by generated dispatch on
 * banked mappers: gen_addr is the address in the recompiler's layout (what an
 * [[extra_func]] entry needs), cpu_addr the original 6502 target (for byte
 * classification), bank the window-resolved bank. The legacy single-arg form
 * delegates with g_current_bank (kept for committed generated code). */
void nes_log_dispatch_miss(uint16_t addr);
void nes_log_dispatch_miss_bank(uint16_t gen_addr, uint16_t cpu_addr, int bank);
void nes_log_inline_miss(uint16_t dispatch_pc, uint8_t a_val);

/* Split halves of nes_log_dispatch_miss (see runtime.c). The interpreter
 * fallback records the miss, interprets, and only applies the policy if it
 * declines. */
void nes_record_dispatch_miss(uint16_t addr);
void nes_record_dispatch_miss_bank(uint16_t gen_addr, uint16_t cpu_addr, int bank);
void nes_dispatch_miss_apply_policy(uint16_t addr);

/* Interpreter fallback entry, invoked from the generated call_by_address miss
 * paths. Returns 1 if the miss was interpreted (game continues), else 0 (the
 * miss policy has been applied; caller behaves as the legacy `return 0`).
 * Implemented in interp.c. */
int nes_interp_dispatch(uint16_t addr);
/* Bank-aware form used by generated banked-mapper dispatch: interprets at the
 * live cpu address, records the miss in gen-layout coordinates. */
int nes_interp_dispatch_bank(uint16_t cpu_addr, uint16_t gen_addr, int bank);

/* Defined by the generated dispatch TU: 1 if the game was recompiled with
 * push_all_jsr (the interpreter's stack-boundary contract requires it). */
extern int g_recomp_push_all_jsr;

/* ---- Dispatch-miss policy ----
 * Configures what happens after nes_log_dispatch_miss / nes_log_inline_miss
 * has recorded the miss into the ring buffer and dispatch_misses.log.
 *   LOG_RETURN  log + record + return (legacy default; control flow continues
 *               from the generated `return 0` after the miss callsite).
 *   FATAL       log + record + flush diagnostic to stderr + exit(1).
 *               Loud failure mode for new ports — silent control-flow loss
 *               cannot be missed.
 *   TRAP        log + record + raise the debug-server pause flag, return.
 *               The next debug_server_wait_if_paused will block in the TCP
 *               loop until a `resume` command clears the pause.
 *
 * Selected at runtime_init() from the NESRECOMP_DISPATCH_MISS env var
 * (`log-return` | `fatal` | `trap`). Default LOG_RETURN. Programmatic
 * override via nes_set_dispatch_miss_policy. */
typedef enum {
    DISPATCH_MISS_LOG_RETURN = 0,
    DISPATCH_MISS_FATAL      = 1,
    DISPATCH_MISS_TRAP       = 2,
} DispatchMissPolicy;

extern DispatchMissPolicy g_dispatch_miss_policy;

/* Per-category counters (monotonic, reset only on process start). */
extern uint64_t g_dispatch_miss_count;        /* call_by_address misses */
extern uint64_t g_inline_dispatch_miss_count; /* inline_dispatch defaults */

void nes_set_dispatch_miss_policy(DispatchMissPolicy policy);

/* ---- Nested-NMI policy ----
 * What the frame-boundary callback does when a vblank fires while the game is
 * ALREADY inside its NMI handler (vblank depth > 1):
 *   POKE_SPIN_FLAGS  legacy: skip the handler, set the $1A/$20 spin-wait
 *                    resolver bytes (SMB-tuned; wrong for other games).
 *   RUN_HANDLER      run the game's NMI handler nested, with full register/
 *                    stack save-restore. For games whose NMI is re-entrancy-
 *                    aware by design and whose progression DEPENDS on the
 *                    nested run (SMB3: nested IntNMI takes the light path and
 *                    DECs VBlank_Tick, releasing the in-NMI wait loops).
 * Set from extras.c game_on_init(); default POKE_SPIN_FLAGS. */
typedef enum {
    NESTED_NMI_POKE_SPIN_FLAGS = 0,
    NESTED_NMI_RUN_HANDLER     = 1,
} NestedNmiPolicy;
extern int g_nested_nmi_policy;

/* Implemented by debug_server.c. Sets the pause flag so the next
 * debug_server_wait_if_paused blocks in the TCP loop. Safe to call from
 * any thread; the pause flag is volatile. Defined as a weak symbol so a
 * minimal runner without the debug server (smoke harness) still links. */
void debug_server_request_pause(const char *reason);

/* ---- BRK policy ----
 * Controls runtime behavior when the recompiler emits a BRK ($00) site.
 *   DIAG    log first occurrence to stderr (loud but non-fatal); function
 *           returns from the BRK site as if it were RTS. Default — better
 *           than the legacy "silent skip" behavior because reachable BRK
 *           is now visible.
 *   FATAL   log + exit(1). For new ports where reaching BRK is a bug.
 *   TRAP    log + raise debug-server pause flag. Inspect at the miss site
 *           via TCP, then `continue` to resume. Same mechanism as
 *           dispatch-miss trap.
 *
 * Real BRK semantics (push PC+2, push P with B set, set I, dispatch via
 * IRQ vector) are not yet implemented — most licensed NES games never
 * reach BRK in normal play, so DIAG/FATAL/TRAP are sufficient until a
 * title proves otherwise. The runtime hook nes_brk_executed always
 * returns to the caller; callers expect to return from the enclosing
 * function immediately (codegen emits `return;` after the call).
 *
 * Selected via NESRECOMP_BRK=diag|fatal|trap env var, parsed once at
 * runtime_init. Programmatic override via nes_set_brk_policy. */
typedef enum {
    BRK_DIAG  = 0,
    BRK_FATAL = 1,
    BRK_TRAP  = 2,
} BrkPolicy;

extern BrkPolicy g_brk_policy;
extern uint64_t  g_brk_count;

void nes_set_brk_policy(BrkPolicy policy);
void nes_brk_executed(uint16_t pc);

/* ---- Entry Points (defined in faxanadu_full.c) ---- */
void func_RESET(void);
void func_NMI(void);
void func_IRQ(void);

/* Deliver an IRQ through the generated IRQ vector while preserving the
 * interrupted CPU context if generated code returns before its RTI epilogue. */
void runtime_call_irq_handler(void);

/* ---- PPU Interface ---- */
/* Called by runtime.c when PPU registers are written */
void ppu_write_reg(uint16_t reg, uint8_t val);
uint8_t ppu_read_reg(uint16_t reg);

/* Render one frame to the framebuffer */
/* framebuf: 256*240 ARGB8888 pixels */
void ppu_render_frame(uint32_t *framebuf);

/* Render OAM debug view: 8x8 grid of 64 sprite slots at 4x scale.
 * buf must be 256*256 ARGB8888 pixels. */
void ppu_render_oam_debug(uint32_t *buf);

/* ---- Mapper Interface ---- */
void mapper_write(uint16_t addr, uint8_t val);
void mapper_init(const uint8_t *prg_data, int prg_banks,
                 int mapper_type, int initial_mirroring);

/* ---- Runtime Init ---- */
void runtime_init(void);

/* ---- PRG ROM writable accessor ----
 * Returns a writable pointer to the start of the given 16KB PRG bank (0-based).
 * Use from game_on_init() to patch data overrides into the ROM shadow buffer.
 * Returns NULL if bank_num is out of range or ROM not yet loaded.
 * Example: runner_get_prg_bank_rw(12)[0x9DBD - 0x8000] = new_tile_byte; */
uint8_t *runner_get_prg_bank_rw(int bank_num);

/* ---- VBlank Callback ---- */
/* Called by ppu_read_reg when simulated VBlank fires (game reading $2002 with bit7 set).
 * Runner implements this to call func_NMI() + render the frame.
 * This is the NES architectural fix: RESET never returns, so NMI must be
 * injected inline whenever the game reads $2002 during a VBlank period. */
void nes_vblank_callback(void);

/* Check elapsed time and fire VBlank if >=16ms has passed.
 * Called from generated JMP instructions to ensure games with tight idle
 * loops (no memory reads) still receive timely NMI callbacks. */
void maybe_trigger_vblank(int cycles);
void maybe_fire_pending_vblank(void);
void runtime_set_vblank_firing(int active);
int  runtime_get_vblank_depth(void);
void runtime_reset_vblank_depth(void);
void runtime_begin_post_nmi(void);
void runtime_end_post_nmi(void);
/* Debug cadence counters: per wall-clock frame accounting. */
uint32_t runtime_pop_nmi_fires(void);
uint32_t runtime_pop_cycle_budget_used(void);
uint32_t runtime_pop_instrs_ticked(void);
uint32_t runtime_pop_forced_caps(void);

/* ---- Always-on frame-event ring ----
 * Continuous capture of the frame-boundary machinery: every VBlank fire
 * ('F' immediate at depth 0, 'P' pending-set, 'C' cap-forced fire, 'V'
 * deferred fire consumed), every $4014 OAM DMA ('D'), and the runner's
 * NMI-handler entries ('N' nested branch, 'T' top-level branch). Probes
 * QUERY this history (TCP `fring`, or exit dump via
 * NESRECOMP_FRING_DUMP=<path>); capture is never armed at probe time.
 * aux packs (visible_sprite_count << 8) | page: for 'D' the count is over
 * the just-copied g_ppu_oam; for fire events over the RAM shadow page
 * (last DMA source, default $02) — the phase discriminator for
 * DMA-vs-render beat bugs. For 'N'/'T', aux = vblank depth. */
typedef struct {
    uint64_t cyc;      /* g_nes_cycles at event */
    uint32_t ops;      /* intra-frame cycle counter at event */
    uint32_t budget;   /* frame budget the counter runs against */
    uint16_t aux;      /* kind-specific (see above) */
    uint8_t  depth;    /* VBlank nesting depth at event */
    char     kind;
} NesFrameEvt;
void     nes_fring_push(char kind, uint16_t aux);
uint16_t nes_fring_shadow_digest(void);           /* fire-time aux helper */
int      nes_fring_last(int n, NesFrameEvt *dst); /* newest n, oldest-first */
void     nes_fring_set_dma_page(uint8_t page);
void     nes_fring_init_dump(void);               /* arm NESRECOMP_FRING_DUMP */

/* PPU registers */
extern uint8_t g_ppuctrl;
extern uint8_t g_ppumask;
extern uint8_t g_ppustatus;
extern uint8_t g_ppuscroll_x;
extern uint8_t g_ppuscroll_y;

/* Split-screen: scroll/ppuctrl captured at sprite-0 hit (= HUD values).
 * g_spr0_split_active is 1 if a split occurred this frame. When active,
 * scanlines 0-15 use *_hud values; 16+ use g_ppuscroll_x/y + g_ppuctrl. */
extern uint8_t g_ppuscroll_x_hud;
extern uint8_t g_ppuscroll_y_hud;
extern uint8_t g_ppuctrl_hud;
extern int     g_spr0_split_active;
extern int     g_spr0_reads_ctr_legacy; /* used only when g_spr0_predict_disable=1 */
extern int     g_spr0_predict_disable;  /* 0 (default) = cycle-accurate sprite-0-hit predictor;
                                         * 1 = legacy 3-read pulse fallback (emergency opt-out). */
extern int     g_predicted_spr0_scanline; /* 0..240; sprite-0 hit scanline this frame, or 240 if none */
extern int     g_spr0_split_write_scanline; /* scanline of post-hit playfield-scroll write, or -1 */
int            ppu_predict_spr0_hit_scanline(void);  /* implemented in ppu_renderer.c */

/* Widescreen rendering: games set these in game_on_init() to widen the
 * BG render pass.  Default 256/0/0 = standard 4:3 NES output.
 * g_render_width  = total output width in pixels (e.g. 512 for 2x)
 * g_widescreen_left  = extra pixels rendered left of column 0
 * g_widescreen_right = extra pixels rendered right of column 255
 * Invariant: g_render_width == 256 + g_widescreen_left + g_widescreen_right */
extern int g_render_width;
extern int g_widescreen_left;
extern int g_widescreen_right;

/* Per-frame effective margins for presentation gating (e.g. pillarbox on
 * title/menu/demo screens).  g_widescreen_left/right define the fixed
 * framebuffer geometry; the effective values bound what is actually drawn
 * this frame.  Pixels between the effective and configured margins render
 * black.  Games set these per frame (game_on_frame / game_post_nmi); the
 * renderer clamps them to the configured margins.  -1 (default) = follow
 * the configured margins, so games that never touch them are unaffected. */
extern int g_ws_eff_left;
extern int g_ws_eff_right;

/* ---- Widescreen sprite-X sidecar (runner capability; inert by default) ----
 *
 * NES OAM X is a single byte, so a game's own 8-bit math wraps any sprite
 * whose screen X leaves [0,255] — in a widened viewport such a sprite would
 * teleport to the opposite edge.  The sidecar keeps a parallel signed 16-bit
 * screen X per OAM slot that the renderer consumes instead of the raw byte.
 *
 * Population model: every store in generated code funnels through
 * nes_write(), so writes to the shadow-OAM page are observed centrally.
 * Game policy publishes a "current draw object" context — the true 16-bit
 * screen X and the 8-bit relative X the game itself just computed (e.g. from
 * a ram_read_hook inside its relative-position routine).  Each subsequent
 * shadow-OAM X write re-derives the unwrapped X as
 *     true_rel + (int8)(written_byte - rel8)
 * which is exact for any per-sprite layout offset in [-128,127] and reduces
 * to the written byte when the object is fully on the vanilla screen.
 * Writes with no valid context (or implausible offsets) record the plain
 * byte, i.e. vanilla placement.
 *
 * The shadow sidecar is copied to g_oam_x16[] at OAM DMA ($4014) so it
 * stays paired with the OAM snapshot the renderer sees.  $2004 writes
 * update g_oam_x16 directly with the plain byte.
 *
 * With g_ws_oam_sidecar == 0 (default) nothing is recorded and the renderer
 * uses the vanilla 8-bit OAM X — behavior is byte-identical.  None of this
 * state is part of the savestate snapshot; it repopulates at the next DMA. */
extern int     g_ws_oam_sidecar;     /* master enable; games set in game_on_init() */
extern int16_t g_oam_x16[64];        /* render-side sidecar, paired with g_ppu_oam */
extern int16_t g_ws_shadow_x16[64];  /* shadow-OAM-side sidecar ($0200 page) */
extern int16_t g_ws_obj_true_rel;    /* context: true 16-bit screen X of current object */
extern uint8_t g_ws_obj_rel8;        /* context: the 8-bit rel X the game computed */
extern uint8_t g_ws_obj_ctx_valid;   /* context valid flag (game policy sets/clears) */

/* Frame counter incremented each VBlank */
extern uint64_t g_frame_count;

/* Monotonic guest CPU-cycle counter — advanced by exactly the same _c that
 * feeds the per-frame s_ops_count accumulator (real instruction cycles + DMC
 * DMA steal + OAM DMA steal). Never reset. This is the co-sim's shared
 * alignment ruler and the faithful cycle stamp for the APU trace; it replaces
 * the old g_frame_count*OPS_PER_FRAME + s_ops_count estimate, which inherited
 * the fixed-frame-length (29781) error. See DIFFERENTIAL-COSIM-PROPOSAL Rung 1. */
extern uint64_t g_nes_cycles;

/* Save the current native framebuffer as a PNG */
void runner_screenshot(const char *path);

/* Current switchable PRG bank (set by mapper_write) */
extern int g_current_bank;

/* MMC3 8KB bank alignment flags.
 * The recompiler uses 16KB banks, but MMC3 switches 8KB banks.
 * When R6 is odd, $8000-$9FFF contains the upper 8KB of the 16KB bank,
 * so dispatch addresses in that range need +$2000 offset.
 * When R7 is even, $A000-$BFFF contains the lower 8KB of the 16KB bank,
 * so dispatch addresses in that range need -$2000 offset. */
extern int g_mmc3_r6_odd;     /* 1 if R6 is odd — $8000 addresses need +$2000 */
extern int g_mmc3_r7_even;   /* 1 if R7 is even — $A000 addresses need -$2000 */
extern int g_mmc3_bank_a000; /* R7/2 — 16KB bank index for $A000-$BFFF dispatch */
extern int g_mmc3_win_bank8k[4]; /* live 8KB bank per CPU window, mode-aware */

/* ---- Controller ---- */
/* Button bitmask: bit7=A, bit6=B, bit5=Select, bit4=Start,
 *                 bit3=Up, bit2=Down, bit1=Left, bit0=Right */
extern uint8_t g_controller1_buttons;
extern uint8_t g_controller2_buttons;

/* ---- Zapper (light gun) on port 2 ---- */
extern int g_zapper_enabled;         /* set to 1 to enable Zapper on port 2 */
extern int g_zapper_x, g_zapper_y;   /* aim coordinates (pixels, 0-255 x 0-239) */
extern int g_zapper_trigger;         /* 1 = trigger pulled */
void runtime_set_zapper_framebuf(const uint32_t *fb); /* present (1-frame-delayed) framebuf */
/* On-demand render callback for Zapper detection.  The dot-PPU publishes its
 * framebuffer with a 1-frame pipeline delay, so the present buffer is always
 * stale relative to the moment the game reads $4017.  The runner registers a
 * callback that renders a LIVE snapshot of current PPU state into a private
 * buffer and hands it back via runtime_set_zapper_snapshot().  The photodiode
 * samples that snapshot, matching what the CRT beam shows at read time. */
typedef void (*zapper_render_fn)(void);
void runtime_set_zapper_render_callback(zapper_render_fn fn);
void runtime_set_zapper_snapshot(const uint32_t *fb); /* live snapshot for light probe */

/* ---- State accessors for debug ring buffer ---- */
/* These expose private statics from runtime.c for exhaustive state capture. */
void    runtime_get_vblank_state(uint32_t *ops_count, int *vblank_depth);
void    runtime_set_vblank_state(uint32_t ops_count, int vblank_depth);
void    runtime_get_controller_shift(uint8_t *shift1, uint8_t *shift2, uint8_t *strobe);
void    runtime_set_controller_shift(uint8_t shift1, uint8_t shift2, uint8_t strobe);
void    runtime_sync_scroll_from_t(void);  /* Derive scroll_x/y from PPU t register */
void    runtime_sync_scroll_from_v(void);  /* Derive scroll_x/y from PPU v (g_ppuaddr) */
uint8_t runtime_get_ppudata_buf(void);
void    runtime_set_ppudata_buf(uint8_t val);
uint16_t runtime_get_ppuaddr(void);
void     runtime_set_ppuaddr(uint16_t addr);
uint16_t runtime_get_ppu_t(void);
int      runtime_scroll_from_t_valid(void);
void     runtime_get_latch_state(uint8_t *ppuaddr_latch, uint8_t *scroll_latch);
void     runtime_set_latch_state(uint8_t ppuaddr_latch, uint8_t scroll_latch);
extern uint8_t g_oamaddr;

/* ---- Dispatch miss monitor ---- */
extern uint32_t g_miss_count_any;
extern uint16_t g_miss_last_addr;
extern uint64_t g_miss_last_frame;
#define MAX_MISS_UNIQUE 12
extern uint16_t g_miss_unique_addrs[MAX_MISS_UNIQUE];
extern int      g_miss_unique_count;

/* Target-byte classification for a miss address.
 * Used to tell "dispatcher was called with garbage input" (ZERO / RTS_STUB)
 * apart from "real function exists in ROM but finder missed it" (CODE). */
typedef enum {
    MISS_TARGET_ZERO     = 0,  /* 8 bytes at addr are all 0x00 — padding */
    MISS_TARGET_RTS_STUB = 1,  /* first byte is 0x60 — 1-byte return stub */
    MISS_TARGET_CODE     = 2   /* anything else — plausible code */
} MissTargetClass;

/* Per-miss snapshot — stored in a ring so a flood of distinct misses
 * doesn't overwrite early ones before they can be inspected. */
typedef struct {
    uint16_t addr;
    int      bank;
    uint64_t frame;
    uint8_t  cpu_a, cpu_x, cpu_y, cpu_p, cpu_s;
    uint16_t call_site_pc;        /* decoded from hardware-pushed return on stack
                                   * (= PC+2 of the 6502 JSR that reached here) */
    uint8_t  target_bytes[8];     /* bytes at currently-mapped (addr..addr+7) */
    uint8_t  target_class;        /* MissTargetClass */
    char     caller[64];          /* top of recomp call stack at miss time */
    char     caller2[64];         /* caller-1 */
    uint8_t  stack_bytes[16];     /* 16 bytes above 6502 SP */
} MissRecord;

#define MAX_MISS_RING 16
extern MissRecord g_miss_ring[MAX_MISS_RING];
extern int        g_miss_ring_head;   /* next write position */
extern int        g_miss_ring_count;  /* total stored, ≤ MAX_MISS_RING */

/* ---- Exe directory (for writing logs next to the binary) ---- */
extern char g_exe_dir[260];  /* set once at startup by launcher */

/* ---- Logger ---- */
void log_on_change(const char *label, uint32_t value);
