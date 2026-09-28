/*
 * cyc_fds_skip.c - the Famicom Disk System boot skip: power on straight into
 * the game, with the machine as the BIOS leaves it when it jumps there.
 *
 * The plan (common/nes_fds_hle.h, axis boot_skip) decides whether it runs;
 * the BIOS's boot model (NesFdsBootModel there) says where the known BIOS
 * boots. For disksys.rom the boot is: reset and the Nintendo logo intro
 * ($EE24-$EF58, about 60 frames), the boot load (JSR $E1F8 LoadFiles at $EF59,
 * about 330 frames of disk), the license check ($F431, 4 frames), the license
 * screen (the loop at $EFAF, 319 frames), and a short tail that restores the
 * pattern tiles the license screen borrowed, sets $0102/$0103 and jumps to
 * ($DFFC). Of these the skip runs the BIOS itself for the license check, the
 * license loop's last pass and the tail (about 6 frames, rendering off), and
 * replaces the rest:
 *
 *   intro      Up to its LoadFiles call the boot depends on nothing on the
 *              disk (measured: the whole machine at that call, every register
 *              and memory byte, is identical for all five owner images at every
 *              alignment). cyc_fds_skip_prepare() runs the BIOS there once,
 *              before power-on, headless and unseen, and keeps the machine; at
 *              power-on the skip puts it back instead of running the reset
 *              sequence. Derived from the recompiled BIOS by running it; no
 *              BIOS-derived data is kept anywhere but in memory.
 *   load       The skip is LoadFiles: it reads the side's drive stream as the
 *              drive and LoadFiles do (common/nes_fds_boot.h) and writes every
 *              requested file where LoadFiles would (CPU memory, or the PPU's
 *              address space at the PPU's increment), in stream order, then
 *              leaves what LoadFiles leaves: its zero-page variables $00-$0E,
 *              $0101, the shadows $F9/$FA/$FE, the stack above the deepest
 *              frames its last block leaves (derived from the $E1F8-$E793 call
 *              structure), the registers it returns with, and the drive as it
 *              is once the head has run to the end of the side (it always has
 *              by the time the BIOS jumps: the license screen alone is 63,000
 *              byte periods; nes_fds_boot.h refuses a boot where it is not).
 *              It returns to the caller ($EF60) and the BIOS runs on.
 *   license    After the BIOS's license check branches into the license
 *   loop       screen loop ($EF65 -> $EFAF), the skip runs the loop's RAM
 *              effects forward, pass by pass: the $E9D3 timers (every frame to
 *              $9F, every tenth frame to $BF), the scroll $FC (+2 below $B0),
 *              until the pass that would end it, and the BIOS runs that last
 *              pass (its NMI wait, scroll and mask writes, the timer routine)
 *              and the tail itself. What each pass also does (the NMI handler,
 *              $2000/$2001/$2005 from the shadows, the idle sound engine) leaves
 *              nothing a later pass does not rewrite (measured: the machine
 *              after the loop differs from before it only in those bytes).
 *
 * The game's first instruction is then reached by the BIOS's own jump, which
 * hw_fds_boot.c observes as for a normal boot (ring fds.boot entry, marked
 * skipped). What stays different from a BIOS boot of the same image, all
 * documented in runner/cyc/README.md ("Boot skip"): the time (frame and
 * cycle counters, and the phases of what counts by itself: the APU frame
 * counter and DMC timer, the FDS modulator timer, the PPU's odd-frame flag and
 * I/O-bus decay clock, up to two CPU cycles of the BIOS's last NMI wait), the
 * PPU's rendering latches (the BIOS drew the license screen 319 more frames),
 * and the deepest stack bytes of LoadFiles's disk-read interrupts.
 *
 * Every decision and the evidence it used goes to the always-on ring
 * (fds.boot plan/check/file/skip/refuse).
 */
#include "cyc_run.h"

#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_ramview.h"
#include "cyc_ring.h"
#include "hw_fds.h"
#include "hw_internal.h"
#include "../../common/nes_fds_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PRERUN_FRAMES = 1200, MAX_PASSES = 100000 };
enum { STAGE_OFF, STAGE_PRERUN, STAGE_CAPTURED, STAGE_MASK, STAGE_MASKED, STAGE_LOOP, STAGE_DONE };

/* The machine at the boot's LoadFiles call. */
typedef struct {
    HwMachine hw;
    HwPpu     ppu;
    Cpu6502   cpu;
    HwFds     fds;
    uint8_t   mirroring;
    uint8_t  *apu;
    uint8_t   wram[NES_FDS_PRG_RAM_BYTES];
    uint8_t   chr[NES_FDS_CHR_RAM_BYTES];
    uint16_t  frame_index[256 * 240];
    uint64_t  cycles;
} Machine;

static Machine *p0;

static struct {
    CycFdsBootModel m;
    int        stage;
    bool       ready, enabled;
    NesFdsBoot boot;
    uint8_t    id[10], list[20];
    uint16_t   ret;          /* the JSR's return address (its last byte); the inline pointers follow */
    uint16_t   id_ptr, list_ptr;
    uint32_t   bytes;        /* bytes the load writes */
    /* the PPU as the load's mid-frame rendering-off leaves it: taken from the
     * pre-run one frame after that store (rendering state, see settled()) */
    bool       settled;
    HwPpu      settle;
} S;

/* What rendering leaves in the 2C02 and a stop mid-frame freezes: OAM and the
 * corruption the stop records (hw_ppu.c corrupt_oam), the sprite evaluation
 * and background pipelines. Nothing else touches these during the load: the
 * CPU does not write OAM and rendering stays off. */
static void pipelines(HwPpu *to, const HwPpu *from);
static void settled(HwPpu *to, const HwPpu *from)
{
#define TAKE(f) memcpy(&to->f, &from->f, sizeof(to->f))
    TAKE(oam);
    TAKE(oamc_disabled); TAKE(oamc_disabled_now); TAKE(oamc_pending); TAKE(oamc_index); TAKE(oamc_reenabled);
    pipelines(to, from);
}

/* The sprite evaluation and background pipelines alone. */
static void pipelines(HwPpu *to, const HwPpu *from)
{
    TAKE(oam2); TAKE(oam_addr); TAKE(oam2_addr); TAKE(oam2_full); TAKE(oam2_reset);
    TAKE(eval_tick); TAKE(eval_wrapped); TAKE(eval_nine); TAKE(eval_odd_corrupt); TAKE(sprite_row);
    TAKE(oam_buffer_in); TAKE(oam_buffer); TAKE(oam_latch); TAKE(spr_lo); TAKE(spr_hi); TAKE(spr_attr);
    TAKE(spr_x); TAKE(load_tile); TAKE(load_attr); TAKE(next_has_s0); TAKE(cur_has_s0);
    TAKE(par_chr); TAKE(fetch_data); TAKE(commit); TAKE(lo_plane); TAKE(hi_plane); TAKE(attribute);
    TAKE(attr_latch); TAKE(bg_lo); TAKE(bg_hi); TAKE(attr_lo); TAKE(attr_hi);
#undef TAKE
}

static bool save(Machine *m)
{
    if (!m->apu && !(m->apu = (uint8_t *)malloc(apu_snapshot_size()))) return false;
    m->hw = hw;
    m->ppu = ppu;
    m->cpu = cpu;
    m->fds = hw_cart.m.fds;
    m->mirroring = hw_cart.mirroring;
    apu_snapshot_save(m->apu);
    memcpy(m->wram, hw_cart.wram, sizeof(m->wram));
    memcpy(m->chr, hw_cart.chr + hw_cart.chr_ram_base, sizeof(m->chr));
    memcpy(m->frame_index, hw_frame_index, sizeof(m->frame_index));
    m->cycles = hw.cycles;
    return true;
}

/* Put the machine back, except the cycle counter: this run's time goes on. */
static void load(const Machine *m)
{
    uint64_t cycles = hw.cycles;
    hw = m->hw;
    hw.cycles = cycles;
    ppu = m->ppu;
    ppu_restored();
    cpu = m->cpu;
    hw_cart.m.fds = m->fds;
    hw_cart.mirroring = m->mirroring;
    apu_snapshot_load(m->apu);
    memcpy(hw_cart.wram, m->wram, sizeof(m->wram));
    memcpy(hw_cart.chr + hw_cart.chr_ram_base, m->chr, sizeof(m->chr));
    memcpy(hw_frame_index, m->frame_index, sizeof(m->frame_index));
}

static uint8_t peek(uint16_t addr)
{
    uint8_t v = 0;
    cyc_debug_peek(addr, &v);
    return v;
}

/* A CPU store as LoadFiles's STA ($0A),Y makes it: RAM through its mirrors,
 * the PRG RAM, nothing for the BIOS ROM (nes_fds_boot.h refused I/O). */
static void cpu_store(uint16_t addr, uint8_t value)
{
    if (addr < 0x2000) hw_code_store(&hw.ram[addr & 0x7FF], addr & 0x7FFu, value);
    else if (addr >= 0x6000 && addr < 0xE000)
        hw_code_store(&hw_cart.wram[addr - 0x6000], HW_CODE_PRG_RAM + (addr - 0x6000u), value);
}

static void ram_store(uint16_t addr, uint8_t value) { cpu_store(addr, value); }

/* The requested files, as LoadFiles writes them (the machine is at the
 * call). Returns the bytes written. */
static uint32_t apply_files(void)
{
    uint32_t len = 0, bytes = 0;
    const uint8_t *s = cyc_fds_side_stream(0, &len);
    unsigned step = ppu.inc32 ? 32u : 1u;
    for (unsigned k = 0; k < S.boot.files; ++k) {
        const NesFdsBootFile *f = &S.boot.file[k];
        cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_FILE, k << 8 | f->id,
                          (uint32_t)f->how << 24 | (uint32_t)f->type << 16 | f->addr);
        if (f->how != NES_FDS_BOOT_LOAD) continue;
        const uint8_t *d = s + f->data_pos;
        if (f->type == 0) {
            for (uint32_t i = 0; i < f->size; ++i) cpu_store((uint16_t)(f->addr + i), d[i]);
        } else {
            uint16_t v = f->addr & 0x3FFF;
            for (uint32_t i = 0; i < f->size; ++i) {
                ppu_hle_store(v, d[i]);
                v = (uint16_t)((v + step) & 0x3FFF);
            }
        }
        bytes += f->size;
    }
    return bytes;
}

/* ---- the pre-run and the analysis ---- */

static const char *prerun(uint8_t align);

static void keep_memories(bool save_them)
{
    static uint8_t wram[NES_FDS_PRG_RAM_BYTES], chr[NES_FDS_CHR_RAM_BYTES];
    size_t n = 0;
    uint8_t *c = (uint8_t *)cyc_chr_ram(&n);
    if (n > sizeof(chr)) n = sizeof(chr);
    if (save_them) {
        memcpy(wram, hw_cart.wram, sizeof(wram));
        if (c) memcpy(chr, c, n);
    } else {
        memcpy(hw_cart.wram, wram, sizeof(wram));
        if (c) memcpy(c, chr, n);
    }
}

const char *cyc_fds_skip_prepare(const CycFdsBootModel *model, uint8_t align)
{
    memset(&S.boot, 0, sizeof(S.boot));
    S.m = *model;
    S.ready = S.enabled = false;
    S.stage = STAGE_OFF;
    if (!S.m.load_call || !S.m.load_entry || !S.m.jump) return "no boot model is known for this BIOS";
    if (!cyc_is_fds() || cyc_fds_side() != 0)
        return "the boot needs disk 1 side A in the drive at power-on";
    if (!p0 && !(p0 = (Machine *)calloc(1, sizeof(*p0)))) return "out of memory";
    /* The PRG RAM and CHR RAM keep their contents over a power cycle: the
     * pre-run gives them back as it found them, so a refused skip leaves
     * exactly the machine without one. */
    keep_memories(true);
    const char *why = prerun(align);
    keep_memories(false);
    return why;
}

static const char *prerun(uint8_t align)
{
    S.stage = STAGE_PRERUN;
    cyc_power_on(align);
    cyc_run_power_on();
    fds_boot_stop_after(S.m.load_call);
    for (int f = 0; f < PRERUN_FRAMES && S.stage == STAGE_PRERUN; ++f) cyc_run_frame();
    if (S.stage != STAGE_CAPTURED) {
        S.stage = STAGE_OFF;
        return "the BIOS did not reach its boot load";
    }
    /* Go on to LoadFiles's first rendering-off store, and one frame more
     * for the PPU to record the OAM row it will corrupt: the one effect of the
     * load's timing on memory (the 2C02's OAM corruption, copied when
     * rendering restarts in the license loop's first pass). */
    S.settled = false;
    if (S.m.mask_store) {
        S.stage = STAGE_MASK;
        fds_boot_stop_after(S.m.mask_store);
        for (int f = 0; f < PRERUN_FRAMES && S.stage == STAGE_MASK; ++f) cyc_run_frame();
        if (S.stage == STAGE_MASKED) {
            cyc_run_frame();
            if (ppu.palc_disabled || ppu.palc_v_left) {
                S.stage = STAGE_OFF;
                return "the boot load turns rendering off where the PPU corrupts its palette";
            }
            if (ppu.oamc_disabled || ppu.oamc_disabled_now || ppu.show_bg || ppu.show_spr) {
                S.stage = STAGE_OFF;
                return "the boot load's rendering-off did not settle in the PPU";
            }
            S.settled = true;
            S.settle = ppu;
        }
    }
    S.stage = STAGE_OFF;
    load(p0);
    /* The call and its inline pointers: the disk ID and the file list. */
    S.ret = (uint16_t)(hw.ram[0x100 | (uint8_t)(cpu.s + 1)] | hw.ram[0x100 | (uint8_t)(cpu.s + 2)] << 8);
    if (S.ret != (uint16_t)(S.m.load_call + 2)) return "the boot's LoadFiles call is not where the model says";
    S.id_ptr = (uint16_t)(peek((uint16_t)(S.ret + 1)) | peek((uint16_t)(S.ret + 2)) << 8);
    S.list_ptr = (uint16_t)(peek((uint16_t)(S.ret + 3)) | peek((uint16_t)(S.ret + 4)) << 8);
    for (unsigned i = 0; i < 10; ++i) S.id[i] = peek((uint16_t)(S.id_ptr + i));
    for (unsigned i = 0; i < 20; ++i) S.list[i] = peek((uint16_t)(S.list_ptr + i));
    uint32_t len = 0;
    const uint8_t *stream = cyc_fds_side_stream(0, &len);
    if (!stream || !nes_fds_boot_read(stream, len, S.id, S.list, fds_crc_reported(), &S.boot)) return S.boot.why;
    /* The license check the BIOS makes next ($F431): the disk's license
     * screen in VRAM against the BIOS's own. */
    S.bytes = apply_files();
    if (S.m.license) {
        for (unsigned i = 0; i < S.m.license_len; ++i)
            if (ppu_hle_peek((uint16_t)(S.m.license_vram + i)) != peek((uint16_t)(S.m.license + i))) {
                S.boot.why_code = NES_FDS_BOOT_WHY_SIGNATURE;
                return S.boot.why = "the disk's license screen is not the one the BIOS checks for (it stops with an error)";
            }
    }
    S.ready = true;
    return NULL;
}

void cyc_fds_skip_enable(bool on) { S.enabled = on && S.ready; }
uint64_t cyc_fds_skip_prerun_cycles(void) { return S.enabled && p0 ? p0->cycles : 0; }

/* ---- power-on: the machine at the call, then LoadFiles ---- */

bool cyc_fds_skip_power_on(void)
{
    if (!S.enabled || S.stage == STAGE_PRERUN) {
        if (!S.enabled && S.boot.why && S.stage == STAGE_OFF && S.m.load_call && cyc_is_fds()) {
            cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_REFUSE, S.boot.why_code, S.boot.why_file);
            S.stage = STAGE_DONE;
        }
        return false;
    }
    load(p0);
    fds_boot_skipped();
    const NesFdsBoot *b = &S.boot;
    cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_CHECK, CYC_FDS_BOOT_CHECK_SIGNATURE, 1);
    cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_CHECK, CYC_FDS_BOOT_CHECK_ID,
                      (uint32_t)b->block1[21] | (uint32_t)b->block1[22] << 8);
    cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_CHECK, CYC_FDS_BOOT_CHECK_AMOUNT,
                      (uint32_t)b->amount | (uint32_t)b->boot_code << 8);
    uint32_t bytes = apply_files();
    cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_CHECK, CYC_FDS_BOOT_CHECK_LICENSE, S.m.license ? 1 : 0);
    cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_CHECK, CYC_FDS_BOOT_CHECK_CRC, fds_crc_reported() ? 1 : 0);

    /* What LoadFiles leaves ($E1F8-$E219). */
    uint32_t stream_len = 0;
    const uint8_t *stream = cyc_fds_side_stream(0, &stream_len);
    const NesFdsBootFile *last = b->files ? &b->file[b->files - 1] : NULL;
    bool asked = last && last->how != NES_FDS_BOOT_NOT_ASKED;
    uint8_t s0 = cpu.s;                                  /* at the call, after its JSR */
    uint8_t fa = (uint8_t)((hw.ram[0xFA] & 0x08) | 0x27); /* EE17 motor on, E786 ($FA & 9) | $26 */
    ram_store(0x00, (uint8_t)S.id_ptr);
    ram_store(0x01, (uint8_t)(S.id_ptr >> 8));
    ram_store(0x02, (uint8_t)S.list_ptr);
    ram_store(0x03, (uint8_t)(S.list_ptr >> 8));
    ram_store(0x04, (uint8_t)(s0 - 3));                  /* E3EB: TSX / DEX inside JSR $E3E7 */
    ram_store(0x05, 2);                                  /* retries left: the first try loads */
    ram_store(0x06, 0);
    ram_store(0x07, b->amount ? 4 : 2);                  /* the last block code E68F expected */
    ram_store(0x08, b->boot_code);
    if (last) {
        ram_store(0x09, asked ? 0x00 : 0xFF);
        uint16_t end = (uint16_t)(last->addr + (last->how == NES_FDS_BOOT_LOAD && !last->type ? last->size : 0));
        ram_store(0x0A, (uint8_t)end);
        ram_store(0x0B, (uint8_t)(end >> 8));
        ram_store(0x0C, 0xFF);
        ram_store(0x0D, 0xFF);
    }
    unsigned requested = b->requested;
    bool ppu_file = false;
    for (unsigned k = 0; k < b->files; ++k) ppu_file |= b->file[k].how == NES_FDS_BOOT_LOAD && b->file[k].type;
    ram_store(0x0E, (uint8_t)requested);
    ram_store(0xFA, fa);
    ram_store(0xF9, (uint8_t)(hw.ram[0xF9] | 0x80));     /* E660 */
    if (ppu_file) ram_store(0xFE, (uint8_t)(hw.ram[0xFE] & 0xE7));   /* E54D */
    /* LoadFiles turned rendering off for its first PPU file ($E54D: the
     * shadow, then $2001): the register now, and what the 2C02 recorded when
     * that store landed mid-frame, as the pre-run's PPU recorded it. */
    if (ppu_file) {
        ppu_write(0x2001, hw.ram[0xFE]);
        if (S.settled) settled(&ppu, &S.settle);
    }
    cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_CHECK, CYC_FDS_BOOT_CHECK_OAM,
                      S.settled && S.settle.oamc_pending ? S.settle.oamc_index : 0xFFu);
    /* The stack LoadFiles leaves under its caller's frame ($0101 pushed at
     * $E204, JSR $E21A, JSR $E778, and the last block's frames: $E4F9's
     * JSR $E706 with its two CRC-byte interrupts on the $E7A4 wait, or, for a
     * file it did not ask for, the two CRC bytes read at $E57A/$E57D). */
    uint16_t back = (uint16_t)(S.ret + 4);
    uint8_t st[16];
    unsigned n = 0;
    st[n++] = (uint8_t)(back >> 8); st[n++] = (uint8_t)back;               /* s0+2, s0+1 */
    st[n++] = hw.ram[0x101];                                                /* s0 */
    st[n++] = 0xE2; st[n++] = 0x0B;                                         /* JSR $E21A at $E209 */
    st[n++] = 0xE2; st[n++] = 0x35;                                         /* JSR $E778 at $E233 */
    if (last && asked) {
        st[n++] = 0xE5; st[n++] = 0x78;                                     /* JSR $E706 at $E576 */
        st[n++] = 0xE7; st[n++] = 0x5F;                                     /* JSR $E77C at $E75D */
        st[n++] = 0xE7; st[n++] = 0xA4; st[n++] = 0xE0;                     /* IRQ on $E7A4: N V 1 */
    } else if (last) {
        uint8_t crc_lo = stream[b->end_pos - 2];
        st[n++] = 0xE7; st[n++] = 0x5F;                                     /* JSR $E77C at $E75D */
        st[n++] = 0xE7; st[n++] = 0xA4;                                     /* IRQ on $E7A4 */
        st[n++] = (uint8_t)(0x60 | (crc_lo & 0x80) | (crc_lo ? 0 : 2));      /* after TXA of the first CRC byte */
    }
    for (unsigned i = 0; i < n; ++i) ram_store((uint16_t)(0x100 | (uint8_t)(s0 + 2 - i)), st[i]);
    /* The drive, once its head has run out: $4025 = $FA, the last byte it
     * transferred (the last block's second CRC byte), $4024 as the IRQ handler
     * last wrote it (A at the second CRC byte: $FA | $10 in $E706, or the first
     * CRC byte at $E57D), and the last CRC check's result. */
    {
        uint8_t wd = asked || !last ? (uint8_t)((fa & 0x08) | 0xF5) : stream[b->end_pos - 2];
        bool bad = last && !(asked ? last->data_crc_ok : last->header_crc_ok);
        fds_drive_boot_end(fa, b->last_byte, wd, bad);
    }
    /* Back to the caller ($EF60): A = X = 0 (no error), Y = the files asked
     * for, V from the IRQ handler's BIT $0101, I clear (E792 CLI). */
    cpu.s = (uint8_t)(s0 + 2);
    cpu.pc = (uint16_t)(back + 1);
    cpu.a = cpu.x = 0;
    cpu.y = (uint8_t)requested;
    cpu.c = 0; cpu.z = 1; cpu.i = 0; cpu.d = 0; cpu.v = 1; cpu.n = 0;
    cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_SKIP, requested, bytes);
    if (S.m.loop_branch) {
        fds_boot_stop_after(S.m.loop_branch);
        S.stage = STAGE_LOOP;
    } else {
        S.stage = STAGE_DONE;
    }
    return true;
}

/* ---- the license screen loop ---- */

static void pass(const CycFdsBootModel *m)
{
    uint8_t x = (uint8_t)(hw.ram[m->scroll] + m->scroll_step);          /* LDX $FC / INX / INX */
    if (x < m->scroll_limit) hw.ram[m->scroll] = x;                     /* CPX #$B0 / BCS / STX */
    uint8_t d = (uint8_t)(hw.ram[m->timer_divider] - 1);                /* $E9D3: DEC $00,X */
    unsigned last = m->fast_last;
    if (d & 0x80) { d = m->divider_reload; last = m->slow_last; }
    hw.ram[m->timer_divider] = d;
    for (unsigned t = last; t != m->timer_divider; --t)
        if (hw.ram[t]) hw.ram[t]--;
}

void cyc_fds_skip_boundary(void)
{
    if (S.stage == STAGE_PRERUN) {
        if (fds_boot_stop_taken() && cpu.pc == S.m.load_entry && save(p0)) S.stage = STAGE_CAPTURED;
        return;
    }
    if (S.stage == STAGE_MASK) {
        if (fds_boot_stop_taken()) S.stage = STAGE_MASKED;
        return;
    }
    if (S.stage != STAGE_LOOP || !fds_boot_stop_taken()) return;
    S.stage = STAGE_DONE;
    const CycFdsBootModel *m = &S.m;
    if (cpu.pc != m->loop_entry) {
        /* The license check failed after all: the BIOS goes on to its error. */
        cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_CHECK, CYC_FDS_BOOT_CHECK_LICENSE, 0);
        return;
    }
    /* The passes render whole frames again: the pipelines end each one as a
     * rendered frame leaves them at VBlank, as the intro's did at the call. */
    pipelines(&ppu, &p0->ppu);
    /* The first pass turns rendering on for a whole frame: a corruption the
     * load left pending happens there (hw_ppu.c sprite_evaluation). */
    if (ppu.oamc_pending) {
        ppu.oamc_pending = 0;
        if (!ppu.oamc_reenabled) {
            if (ppu.oamc_index == 0x20) ppu.oamc_index = 0;
            memcpy(&ppu.oam[ppu.oamc_index * 8], &ppu.oam[0], 8);
            ppu.oam2[ppu.oamc_index] = ppu.oam2[0];
        }
        ppu.oamc_reenabled = 0;
    }
    /* LDA #count / STA counter, then every pass but the last. */
    uint8_t count = peek((uint16_t)(m->loop_entry + 1));
    hw.ram[m->loop_counter] = count;
    uint32_t passes = 0;
    uint8_t keep[0x100];
    for (;;) {
        memcpy(keep, hw.ram, sizeof(keep));
        pass(m);
        if (!hw.ram[m->loop_counter] || passes >= MAX_PASSES) {
            memcpy(hw.ram, keep, sizeof(keep));     /* that one is the BIOS's to run */
            break;
        }
        passes++;
    }
    cpu.pc = m->loop_top;
    cpu.a = hw.ram[m->loop_counter];
    cpu.n = cpu.a >> 7;
    cpu.z = cpu.a == 0;
    if (passes) {
        /* as a pass leaves them: $E9D3 with X = divider, Y = its slow end */
        hw.ram[0x00] = m->timer_divider;
        cpu.x = m->timer_divider;
        cpu.y = m->slow_last;
        cpu.c = 1;
    }
    cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_CHECK, CYC_FDS_BOOT_CHECK_LOOP, passes);
}
