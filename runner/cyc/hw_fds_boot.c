/*
 * hw_fds_boot.c - the Famicom Disk System's boot, as the RAM Adapter sees it:
 * where the BIOS hands the machine to the game, the stops the boot skip needs
 * inside the BIOS, and auto insert.
 *
 * Observation (always on once the BIOS's jump into the game is known; the
 * machine is untouched): the BIOS enters the game with JMP ($DFFC)
 * (disksys.rom $EE9F). The RAM Adapter holds the BIOS ROM and the PRG RAM,
 * so it sees that instruction's opcode and operand fetches and the two vector
 * reads on the address bus, in native code, the interpreter and cyc_interp
 * alike; the next opcode fetch is the game's first instruction -> ring
 * fds.boot entry (the entry address, the PPU's position, whether the boot
 * skip started it). A host that set hw_entry_stop also gets a scheduler stop
 * at that instruction boundary (cyc_run_entry_observer), to look at the whole
 * machine there (cyc_host --boot-state-out).
 *
 * Stops for the boot skip (cyc_fds_skip.c): fds_boot_stop_after(pc) makes the
 * scheduler stop at the instruction boundary after the next execution of the
 * instruction at pc (its opcode fetch: the read of pc, then of pc + 1 on the
 * next CPU read); native code yields only at instruction boundaries, and this
 * is how the skip gets one inside the BIOS. A host's debugging watch
 * (cyc_fds_boot_watch, cyc_host --state-at-pc) works the same way.
 *
 * Auto insert (plan axis, common/nes_fds_hle.h; default on): armed at power-on
 * when the drive is empty. The first frame in which the boot (before the
 * game has started) reads $4032 with the drive empty is the BIOS waiting for a
 * disk (PLEASE SET DISK CARD; disksys.rom polls it once a frame from its
 * reset path). At that frame's end, where a host's disk events land, disk 1
 * side A goes in through the drive's insert, as a player's would: ring
 * fds.boot wait and auto-insert, fds.side (auto-insert). A host disk change
 * first, or the game starting, stands it down (fds.boot auto-insert off).
 * Judgment call, not oracle-verified: the insert comes at the first frame of
 * the wait, as soon as the console is on and waiting; the check against
 * nesref is a scripted DISK_INSERT at that frame (tools/cyc/fds_oracle_gates.py
 * --gate insert). Its state is hashed only in runs it can act in (the drive
 * empty at power-on), so a run with a disk in hashes exactly as before.
 */
#include "hw_fds.h"

#include "cyc_core.h"
#include "cyc_ring.h"
#include "hw_internal.h"

#include <string.h>

static struct {
    CycFdsBoot cfg;
    /* the jump into the game, as the bus shows it */
    uint8_t  seq;
    CycFdsBootStatus st;
    /* stops: the skip's, and a host's debugging watch */
    uint16_t stop_pc, watch_pc;
    uint8_t  stop_seq, watch_seq;
    bool     stop_hit, watch_done;
    /* auto insert (hashed while armed or once it acted) */
    uint8_t  armed, polled, acted;
    uint32_t polls, wait_frame;
} B;

void cyc_fds_boot_configure(const CycFdsBoot *cfg) { B.cfg = *cfg; }
void cyc_fds_boot_watch(uint16_t pc) { B.watch_pc = pc; B.watch_seq = 0; B.watch_done = false; }
bool cyc_fds_boot_watch_hit(void) { return B.watch_done; }
void fds_boot_stop_after(uint16_t pc) { B.stop_pc = pc; B.stop_seq = 0; B.stop_hit = false; }
bool fds_boot_stop_taken(void)
{
    bool hit = B.stop_hit;
    B.stop_hit = false;
    return hit;
}

void fds_boot_power_on(void)
{
    CycFdsBoot cfg = B.cfg;
    uint16_t watch = B.watch_pc;
    memset(&B, 0, sizeof(B));
    B.cfg = cfg;
    B.watch_pc = watch;
    B.armed = cfg.auto_insert && cyc_fds_side() < 0 && cyc_fds_side_count() > 0;
    if (cfg.boot_jump || cfg.auto_insert)
        cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_PLAN,
                          (cfg.boot_skip ? 1u : 0) | (B.armed ? 2u : 0) | (cfg.boot_skip_refused ? 4u : 0),
                          cfg.boot_jump);
}

void fds_boot_skipped(void) { B.st.skipped = true; }

static void entered(uint16_t pc)
{
    B.st.entered = true;
    B.st.entry_pc = pc;
    B.st.entry_frame = cyc_ring_frame;
    B.st.entry_cycle = hw.cycles;
    B.st.entry_line = ppu.scanline;
    B.st.entry_dot = ppu.dot;
    cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_ENTRY,
                      (uint32_t)pc | (B.st.skipped ? 0x10000u : 0) | (ppu.odd_frame ? 0x20000u : 0),
                      (uint32_t)ppu.scanline << 16 | ppu.dot);
    if (B.armed) {
        B.armed = 0;
        cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_YIELD, 2, cyc_ring_frame);
    }
    if (hw_entry_stop) hw_entry_hit = hw_frame_done = true;
}

/* One opcode-fetch watch: pc's read, then pc + 1's on the next CPU read. */
static bool fetched(uint16_t pc, uint8_t *seq, uint16_t addr)
{
    bool hit = *seq && addr == (uint16_t)(pc + 1);
    *seq = addr == pc;
    return hit;
}

/* Every CPU read of $8000-$FFFF. The BIOS's jump into the game is JMP ($DFFC)
 * at cfg.boot_jump: its opcode and operand fetches, then the two vector
 * reads; the next opcode fetch is the game's first instruction. DMA reads
 * (the CPU holds another address) and interrupts break the sequence. */
void fds_boot_snoop(uint16_t addr)
{
    if (addr != hw.cpu_addr || !hw.cpu_reading) return;
    if (B.stop_pc && fetched(B.stop_pc, &B.stop_seq, addr)) {
        B.stop_pc = 0;
        B.stop_hit = true;
        hw_entry_hit = hw_frame_done = true;
    }
    if (B.watch_pc && !B.watch_done && fetched(B.watch_pc, &B.watch_seq, addr)) {
        B.watch_done = true;
        if (hw_entry_stop) hw_entry_hit = hw_frame_done = true;
    }
    uint16_t j = B.cfg.boot_jump;
    if (!j || B.st.entered) return;
    static const uint16_t STEP[5] = { 0, 1, 2, 0xDFFC, 0xDFFD };
    uint16_t want = B.seq < 3 ? (uint16_t)(j + STEP[B.seq]) : STEP[B.seq];
    if (addr == want) {
        if (++B.seq == 5) {
            B.seq = 0;
            uint8_t lo = 0, hi = 0;
            cyc_debug_peek(0xDFFC, &lo);
            cyc_debug_peek(0xDFFD, &hi);
            entered((uint16_t)(lo | hi << 8));
        }
    } else {
        B.seq = addr == j ? 1 : 0;
    }
}

/* ---- auto insert ---- */

void fds_boot_status_read(void)
{
    if (!B.armed || cyc_fds_side() >= 0) return;
    if (!B.polls++) B.wait_frame = cyc_ring_frame;
    B.polled = 1;
}

void fds_boot_host_disk_change(void)
{
    if (!B.armed) return;
    B.armed = 0;
    cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_YIELD, 1, cyc_ring_frame);
}

void fds_boot_frame_end(void)
{
    if (!B.armed || !B.polled) return;
    B.armed = 0;
    B.acted = 1;
    cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_WAIT, 0, B.polls);
    if (fds_drive_insert(0, CYC_FDS_SIDE_AUTO)) {
        B.st.auto_inserted = true;
        B.st.insert_frame = cyc_ring_frame;
        cyc_ring_push_len(CYC_EV_FDS_BOOT, CYC_FDS_BOOT_INSERT, 0, B.wait_frame);
    }
}

uint64_t fds_boot_state_hash(uint64_t acc)
{
    if (!B.armed && !B.acted) return acc;
    const uint8_t v[] = { B.armed, B.polled, B.acted, (uint8_t)B.polls, (uint8_t)(B.polls >> 8), 0x5A };
    for (size_t i = 0; i < sizeof(v); ++i) acc = acc * 131 + v[i];
    return acc;
}

void cyc_fds_boot_status(CycFdsBootStatus *out)
{
    *out = B.st;
    out->insert_armed = B.armed;
}
