/* cyc_mod.c - the machine as a mod sees it; see cyc_mod.h. */
#include "cyc_mod.h"

#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_hooks.h"
#include "cyc_ring.h"
#include "cyc_run.h"
#include "cyc_state.h"
#include "cyc_render.h"
#include "../include/mod_savestate.h"
#include "cyc_trace.h"
#include "hw_internal.h"
#include "../../common/nes_fds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The address a call returns to: in $4020-$5FFF, where no board compiles
 * code and no RAM view lives, so every dispatch path returns to the loop that
 * watches for it. The routine's RTS dummy-reads $5FFE; the scope undoes that
 * like everything else. */
#define RETURN_PC 0x5FFFu

bool cyc_background_enabled;
uint8_t cyc_background_pipe[4];
static uint16_t background_frame[256*240];
static bool background_registered;
static int save_background(uint8_t *out,int capacity) {
    if(!cyc_background_enabled)return 0;
    const int size=8+(int)sizeof background_frame;
    if(capacity<size)return -1;
    memcpy(out,"BG01",4);memcpy(out+4,cyc_background_pipe,4);
    memcpy(out+8,background_frame,sizeof background_frame);return size;
}
static int validate_background(const uint8_t *in,int length) {
    if(!cyc_background_enabled)return length==0;
    if(!in||length!=8+(int)sizeof background_frame||memcmp(in,"BG01",4))return 0;
    for(int i=0;i<4;i++)if(in[4+i]>63)return 0;
    for(unsigned i=0;i<256*240;i++){uint16_t value;memcpy(&value,in+8+i*2,2);if(value>511)return 0;}
    return 1;
}
static int load_background(const uint8_t *in,int length) {
    if(!validate_background(in,length))return 0;
    if(length){memcpy(cyc_background_pipe,in+4,4);memcpy(background_frame,in+8,sizeof background_frame);}return 1;
}
void cyc_render_capture_background(bool enabled) {
    if(enabled&&!background_registered){
        if(!nes_mod_register_savestate_hook("cyc.background",save_background,load_background)||
           !nes_mod_register_savestate_validator("cyc.background",validate_background)) {
            fprintf(stderr,"[cyc presentation] cannot register background state\n");abort();
        }
        background_registered=true;
    }
    cyc_background_enabled=enabled;
    memset(cyc_background_pipe,0,sizeof cyc_background_pipe);memset(background_frame,0,sizeof background_frame);
}
void cyc_render_background_output(unsigned pixel,unsigned emphasis,bool grey) {
    uint8_t value=cyc_background_pipe[3];if(grey)value&=0x30;
    background_frame[pixel]=(uint16_t)(value|(emphasis<<6));
}
bool cyc_render_background(uint32_t *out) {
    if(!cyc_background_enabled||!out)return false;
    for(unsigned i=0;i<256*240;i++)out[i]=hw_palette_argb[background_frame[i]];
    return true;
}

static CycSnapshot *s_snap;
static size_t       s_snap_chr;
static bool         s_open;
static bool         s_isolated_hooks;
static void (*s_return_hook)(void);
static void returned(void) {
    if (s_return_hook && (!s_open || s_isolated_hooks)) s_return_hook();
}
void cyc_mod_set_return_hook(void (*hook)(void)) {
    s_return_hook = hook;
    cyc_cpu_rts_observer = hook ? returned : NULL;
}
void cyc_mod_set_ram_read_hook(uint8_t (*hook)(uint16_t pc, uint16_t addr, uint8_t value)) {
    cyc_cpu_ram_read_policy = hook;
}
void cyc_mod_allow_isolated_hooks(bool allow) {
    if (!s_open) return;
    s_isolated_hooks = allow;
    cyc_hooks_suspend(!allow);
}
static bool         s_trace_was;
static uint64_t     s_budget = 2000000;
static CycModStats  s_stats;

/* per-frame MOD_CALL summaries, by routine */
typedef struct { uint16_t routine; uint32_t calls, cycles; } FrameCall;
static FrameCall s_frame[64];
static unsigned  s_frame_n;
static uint16_t  s_failed[64];          /* routines already reported failing (stderr once each) */
static unsigned  s_failed_n;

uint8_t cyc_mod_peek(uint16_t addr)
{
    uint8_t v = 0;
    cyc_debug_peek(addr, &v);
    return v;
}

bool cyc_mod_peek_ok(uint16_t addr, uint8_t *value) { return cyc_debug_peek(addr, value); }

uint8_t *cyc_mod_prg_data_rw(size_t *size) {
    if (size) *size = 0;
    if (s_open || hw_cart.mapper == NES_FDS_MAPPER || !hw_cart.prg) return NULL;
    hw_prg_modified = true;
    if (size) *size = hw_cart.prg_len;
    return hw_cart.prg;
}
bool cyc_mod_apply_cart_payload(const uint8_t *payload, size_t size) {
    size_t chr_rom=hw_cart.info.chr_size;
    if(s_open || hw_cart.mapper==NES_FDS_MAPPER || !payload || !hw_cart.prg ||
       size!=(size_t)hw_cart.prg_len+chr_rom || (chr_rom && !hw_cart.chr))return false;
    if(memcmp(hw_cart.prg,payload,hw_cart.prg_len)) {
        memcpy(hw_cart.prg,payload,hw_cart.prg_len);hw_prg_modified=true;
    }
    if(chr_rom) {
        memcpy(hw_cart.chr,payload+hw_cart.prg_len,chr_rom);
        ppu_state_reloaded();
    }
    return true;
}
bool cyc_mod_chr_poke(uint16_t addr, uint8_t value) {
    if (s_open || addr >= 0x2000 || !hw_cart.chr_write[addr >> 10]) return false;
    hw_cart.chr[hw_cart_chr_index(addr)] = value;
    ppu_state_reloaded();
    return true;
}

bool cyc_mod_poke(uint16_t addr, uint8_t value)
{
    if (addr < 0x2000) {
        hw_code_store(&hw.ram[addr & 0x7FF], addr & 0x7FFu, value);
        return true;
    }
    if (hw_cart.mapper == NES_FDS_MAPPER) {
        if (addr >= 0x6000 && addr < 0xE000) {
            hw_code_store(&hw_cart.wram[addr - 0x6000], HW_CODE_PRG_RAM + (addr - 0x6000u), value);
            return true;
        }
        return false;
    }
    if (addr >= 0x6000 && addr < 0x8000 && hw_cart.has_wram && hw_cart.wram_len) {
        hw_cart.wram[(hw_cart.wram_bank + (addr & 0x1FFF)) % hw_cart.wram_len] = value;
        return true;
    }
    return false;
}

void cyc_mod_regs(CycModRegs *out)
{
    out->a = cpu.a;
    out->x = cpu.x;
    out->y = cpu.y;
    out->s = cpu.s;
    out->p = cpu_get_p(0);
    out->pc = cpu.pc;
}

void cyc_mod_set_regs(const CycModRegs *in)
{
    cpu.a = in->a;
    cpu.x = in->x;
    cpu.y = in->y;
    cpu.s = in->s;
    cpu_set_p(in->p);
}

bool cyc_mod_isolated(void) { return s_open; }

void cyc_mod_set_call_budget(uint64_t cycles) { s_budget = cycles ? cycles : 1; }

bool cyc_mod_isolate_begin(void)
{
    if (s_open) {
        fprintf(stderr, "[cyc mod] isolated scopes do not nest\n");
        return false;
    }
    if (!s_snap || s_snap_chr != hw_cart.chr_ram_len) {
        cyc_snapshot_free(s_snap);
        s_snap = cyc_snapshot_new();
        s_snap_chr = hw_cart.chr_ram_len;
        if (!s_snap) {
            fprintf(stderr, "[cyc mod] out of memory for the machine snapshot\n");
            return false;
        }
    }
    cyc_snapshot_take(s_snap);
    s_open = true;
    s_isolated_hooks = false;
    s_stats.scopes++;
    /* Nothing but the routine: no clock, no interrupt about to be taken, no
     * hook sites, no device history, no comparison trace. */
    hw_isolated = true;
    cpu.do_nmi = cpu.do_irq = cpu.do_reset = 0;
    cpu.nmi_edge = 0;
    hw.irq_line = 0;
    hw_frame_done = hw_observe_hit = hw_frame_end_hit = false;
    cyc_hooks_suspend(true);
    cyc_ring_muted = 1;
    s_trace_was = cyc_trace_enabled;
    cyc_trace_enabled = false;
    return true;
}

static void note_call(uint16_t routine, uint64_t cycles)
{
    for (unsigned i = 0; i < s_frame_n; ++i)
        if (s_frame[i].routine == routine) {
            s_frame[i].calls++;
            s_frame[i].cycles += (uint32_t)cycles;
            return;
        }
    if (s_frame_n < sizeof(s_frame) / sizeof(s_frame[0])) {
        s_frame[s_frame_n].routine = routine;
        s_frame[s_frame_n].calls = 1;
        s_frame[s_frame_n].cycles = (uint32_t)cycles;
        s_frame_n++;
    }
}

bool cyc_mod_call(uint16_t routine, CycModRegs *regs)
{
    if (!s_open) {
        fprintf(stderr, "[cyc mod] cyc_mod_call($%04X) outside cyc_mod_isolate_begin/end\n", routine);
        return false;
    }
    cpu.a = regs->a;
    cpu.x = regs->x;
    cpu.y = regs->y;
    cpu_set_p(regs->p);
    uint8_t entry_s = regs->s;
    /* JSR: the return address minus one, high byte first */
    hw.ram[0x100 | entry_s] = (uint8_t)((RETURN_PC - 1) >> 8);
    hw.ram[0x100 | (uint8_t)(entry_s - 1)] = (uint8_t)(RETURN_PC - 1);
    cpu.s = (uint8_t)(entry_s - 2);
    cpu.pc = routine;
    cpu.jammed = 0;
    hw_isolated_cycles = 0;
    hw_isolated_budget = s_budget;
    hw_frame_done = false;
    int why = cyc_run_isolated((uint16_t)RETURN_PC, entry_s);
    uint64_t took = hw_isolated_cycles;
    hw_frame_done = false;
    cyc_mod_regs(regs);
    s_stats.calls++;
    s_stats.cycles += took;
    cyc_ring_muted = 0;
    note_call(routine, took);
    if (why) {
        s_stats.failures++;
        cyc_ring_push_len(CYC_EV_MOD_FAIL, routine, (uint32_t)why, (uint32_t)(took > UINT32_MAX ? UINT32_MAX : took));
        bool told = false;
        for (unsigned i = 0; i < s_failed_n; ++i) told = told || s_failed[i] == routine;
        if (!told) {
            if (s_failed_n < sizeof(s_failed) / sizeof(s_failed[0])) s_failed[s_failed_n++] = routine;
            fprintf(stderr, "[cyc mod] isolated call to $%04X did not return (%s after %llu cycles, pc $%04X)\n",
                    routine, why == CYC_MOD_FAIL_BUDGET ? "budget" : why == CYC_MOD_FAIL_JAM ? "jam" : "stack",
                    (unsigned long long)took, cpu.pc);
        }
    }
    cyc_ring_muted = 1;
    return why == 0;
}

void cyc_mod_isolate_end(void)
{
    if (!s_open) return;
    hw_isolated = false;
    cyc_snapshot_restore(s_snap);
    cyc_ring_muted = 0;
    cyc_trace_enabled = s_trace_was;
    cyc_hooks_suspend(false);
    s_open = false;
    s_isolated_hooks = false;
}

static bool call_commit(uint16_t routine, CycModRegs *regs, bool hooked)
{
    if (s_open) {
        fprintf(stderr, "[cyc mod] cyc_mod_call_commit($%04X) inside an isolated scope\n", routine);
        return false;
    }
    if (!cyc_mod_isolate_begin()) return false;
    cyc_mod_allow_isolated_hooks(hooked);
    hw_isolated_io_writes = 0;
    CycModRegs out = *regs;
    bool ok = cyc_mod_call(routine, &out);
    if (ok && hw_isolated_io_writes) {
        fprintf(stderr, "[cyc mod] committed call to $%04X stored to %llu device register(s); not committed\n", routine,
                (unsigned long long)hw_isolated_io_writes);
        ok = false;
    }
    /* What the routine left in memory, before the scope puts the machine back. */
    static uint8_t ram[0x800], cart[0x20000];
    size_t cart_len = 0;
    const uint8_t *before_cart = cyc_snapshot_cart_ram(s_snap, &cart_len);
    const uint8_t *before_ram = cyc_snapshot_cpu_ram(s_snap);
    static uint8_t before_ram_copy[0x800], before_cart_copy[0x20000];
    if (ok) {
        memcpy(ram, hw.ram, sizeof(ram));
        memcpy(cart, hw_cart.wram, cart_len);
        memcpy(before_ram_copy, before_ram, sizeof(before_ram_copy));
        memcpy(before_cart_copy, before_cart, cart_len);
    }
    cyc_mod_isolate_end();
    if (!ok) return false;
    bool fds = hw_cart.mapper == NES_FDS_MAPPER;
    for (unsigned a = 0; a < sizeof(ram); ++a)
        if (ram[a] != before_ram_copy[a]) hw_code_store(&hw.ram[a], a, ram[a]);
    for (size_t i = 0; i < cart_len; ++i) {
        if (cart[i] == before_cart_copy[i]) continue;
        if (fds) hw_code_store(&hw_cart.wram[i], (unsigned)(HW_CODE_PRG_RAM + i), cart[i]);
        else hw_cart.wram[i] = cart[i];
    }
    cpu.a = out.a;
    cpu.x = out.x;
    cpu.y = out.y;
    cpu_set_p(out.p);
    *regs = out;
    return true;
}

bool cyc_mod_call_commit(uint16_t routine, CycModRegs *regs) {
    return call_commit(routine, regs, false);
}
bool cyc_mod_call_commit_hooked(uint16_t routine, CycModRegs *regs) {
    return call_commit(routine, regs, true);
}
void cyc_mod_stats(CycModStats *out) { *out = s_stats; }

void cyc_mod_frame_end(void)
{
    for (unsigned i = 0; i < s_frame_n; ++i)
        cyc_ring_push_len(CYC_EV_MOD_CALL, s_frame[i].routine, s_frame[i].calls, s_frame[i].cycles);
    s_frame_n = 0;
}
