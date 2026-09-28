/*
 * cyc_ring.h - the machine's always-on event ring.
 *
 * Devices whose behaviour a run needs to be explained after the fact (the FDS
 * RAM Adapter and drive first) record every event here from power-on, in
 * Release builds too. Nothing arms it: a probe joins late and queries the
 * window it wants by event index or by frame. The ring keeps the newest
 * CYC_RING_CAPACITY events; older ones are evicted, and per-kind totals keep
 * counting from power-on, so an evicted window is visible as such rather than
 * silently missing.
 *
 * Identical consecutive reads of one register (a polling loop) fold into one
 * event whose `repeat` counts the extra reads, so a wait loop costs one slot;
 * an envelope's gain changes fold the same way within a frame.
 *
 * Host access: cyc_ring_dump() (cyc_host --ring-out FILE, --ring-frames A:B,
 * or NESRECOMP_CYC_RING_DUMP=FILE for any host, written at exit).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CYC_EV_NONE,
    /* FDS RAM Adapter registers ($4020-$4033, and the sound registers
     * $4040-$4092). addr = register, value = the byte read or written. */
    CYC_EV_FDS_READ,
    CYC_EV_FDS_WRITE,
    /* An interrupt source becoming asserted. value = FDS_IRQ_TIMER/DISK. */
    CYC_EV_FDS_IRQ,
    /* Asserted sources cleared by a register access. addr = register,
     * value = the sources it cleared. */
    CYC_EV_FDS_IRQ_ACK,
    /* The drive clocked one byte. addr = CYC_FDS_BYTE_* flags, value =
     * position (bits 0-23) | byte (bits 24-31): the byte read from the disk,
     * or the one written to it. */
    CYC_EV_FDS_BYTE,
    /* Motor line: value 1 running, 0 stopped. addr: 0 = $4025, 1 = the drive
     * stopped itself at the end of the side. */
    CYC_EV_FDS_MOTOR,
    /* The head went back to the start of the side and the spin-up delay began.
     * value = the delay in CPU cycles. */
    CYC_EV_FDS_REWIND,
    /* The first byte after a rewind: the drive reports ready ($4032.1 = 0). */
    CYC_EV_FDS_READY,
    /* The head passed the last byte of the side. value = side length. */
    CYC_EV_FDS_END,
    /* Drive contents changed. value = side index, or 0xFF when ejected.
     * addr: 0 = host/script, 1 = power-on. */
    CYC_EV_FDS_SIDE,
    /* The CRC check the drive makes when $4025.4 rises in read mode.
     * addr = 1 bad / 0 good, value = the accumulator. */
    CYC_EV_FDS_CRC,
    /* Disk writes (hw_fds.c). The event's `repeat` field holds a length for
     * these, not folded repeats:
     *   WRITE_RUN   the drive left write mode (or stopped): value = side << 24 |
     *               first position stored, addr = bytes that changed the disk
     *               (capped at $FFFF), length = bytes stored
     *   WRITE_BLOCK a block written the BIOS way ended with its second CRC byte:
     *               addr = side << 8 | block code, value = position of its
     *               $80 mark, length = mark through CRC
     * Host persistence (cyc_host.c disk saves):
     *   FDS_SAVE    a save file write: addr = reason << 1 | ok (reasons in
     *               CYC_FDS_SAVE_*), value = sides stored, length = file bytes
     *   FDS_LOAD    a saved disk put in at start: addr = 0 disk save file, 1 a
     *               Mesen .ips; value = sides replaced, length = file bytes */
    CYC_EV_FDS_WRITE_RUN,
    CYC_EV_FDS_WRITE_BLOCK,
    CYC_EV_FDS_SAVE,
    CYC_EV_FDS_LOAD,
    /* Compiled RAM views (cyc_ramview.c). value = view index (the host's
     * --ram-view-list names them), addr = CPU address:
     *   VIEW_VALID   a view was checked against RAM, matched, and entered at addr
     *   VIEW_REJECT  a view was checked at addr and RAM no longer holds its bytes
     *   VIEW_INVALID a store to addr changed a byte of a validated view
     *   VIEW_EXIT    a view returned at addr because a store invalidated a view
     *   RAM_INTERP   the interpreter ran a RAM instruction at addr that no view
     *                covers; value = its 1KB chunk; the following ones in the
     *                same chunk and frame fold into repeat
     *   VIEW_FRAME   end of a frame in which RAM views ran: value = entries
     *                into RAM views that frame, addr = views validated (<= $FFFF) */
    CYC_EV_VIEW_VALID,
    CYC_EV_VIEW_REJECT,
    CYC_EV_VIEW_INVALID,
    CYC_EV_VIEW_EXIT,
    CYC_EV_RAM_INTERP,
    CYC_EV_VIEW_FRAME,
    /* FDS sound unit (hw_fds_audio.c):
     *   FDS_ENV    an envelope tick changed a gain: addr 0 = volume, 1 = mod,
     *              value = the gain; the following ticks of that envelope in
     *              the same frame fold into repeat (value = the latest gain)
     *   FDS_AUDIO  end of a frame in which the sound unit stepped: value =
     *              wave position steps, addr = mod table steps (<= $FFFF) */
    CYC_EV_FDS_ENV,
    CYC_EV_FDS_AUDIO,
    /* FDS disk-ID requests and the HLE tier (hw_fds_hle.c). These carry a
     * length in `repeat`, like WRITE_RUN:
     *   FDS_IDREQ   the BIOS's disk-ID check ran (always recorded when its
     *               anchor is known): addr = where the requested ID is, value =
     *               the sides whose disk header it matches (bit per side),
     *               length = the side in the drive (0xFF: empty)
     *   FDS_IDBYTES the 10 ID bytes, right after FDS_IDREQ: value = bytes 0-3,
     *               length = bytes 4-7, addr = bytes 8-9 (little endian)
     *   FDS_SPAN    a load span ended (always recorded): value = first frame,
     *               length = frames, addr = load frames in it (<= $7FFF) |
     *               $8000 if fast load ran it unpaced
     *   FDS_HLE     an HLE decision or action: addr = CYC_FDS_HLE_*, value and
     *               length as listed there */
    CYC_EV_FDS_IDREQ,
    CYC_EV_FDS_IDBYTES,
    CYC_EV_FDS_SPAN,
    CYC_EV_FDS_HLE,
    /* FDS boot (hw_fds_boot.c): the BIOS handing control to the game, the
     * boot skip's decisions and the evidence it used, and auto insert. A
     * length in `repeat`, like FDS_HLE: addr = CYC_FDS_BOOT_*, value and
     * length as listed there. */
    CYC_EV_FDS_BOOT,
    CYC_EV_KINDS
} CycRingKind;

enum {
    CYC_FDS_SAVE_IDLE    = 1,   /* the drive stopped after changing the disk */
    CYC_FDS_SAVE_EJECT   = 2,   /* the disk was ejected */
    CYC_FDS_SAVE_EXIT    = 3,   /* the host is exiting */
    CYC_FDS_SAVE_TIMEOUT = 4,   /* dirty for the longest time a save may wait */
};

/* FDS_HLE codes: value, length */
enum {
    CYC_FDS_HLE_CONFIG    = 1,  /* plan applied: auto_swap | fast_load << 1, the ID check anchor */
    CYC_FDS_HLE_KEEP      = 2,  /* request satisfied by the side in the drive: side, match mask */
    CYC_FDS_HLE_SWAP      = 3,  /* request for one other side: that side, match mask */
    CYC_FDS_HLE_AMBIGUOUS = 4,  /* request matches several sides, none in the drive: mask, side in drive */
    CYC_FDS_HLE_NOMATCH   = 5,  /* request matches no side: 0, side in drive */
    CYC_FDS_HLE_WAIT      = 6,  /* program waits for an eject (polling): side to put back, poll frames */
    CYC_FDS_HLE_EJECT     = 7,  /* auto swap ejected: side, frames the drive stays empty */
    CYC_FDS_HLE_INSERT    = 8,  /* auto swap inserted: side, wait round (0: after a request) */
    CYC_FDS_HLE_CANCEL    = 9,  /* a host disk change cancelled an auto swap: its side, its step */
};

/* FDS_BOOT codes: value, length */
enum {
    CYC_FDS_BOOT_ENTRY  = 1,  /* the game's first instruction: pc | skipped << 16 | odd frame << 17,
                                 PPU scanline << 16 | dot */
    CYC_FDS_BOOT_PLAN   = 2,  /* boot plan applied: skip | auto insert << 1 | skip refused << 2 |
                                 insert refused << 3, the BIOS's jump into the game */
    CYC_FDS_BOOT_FILE   = 3,  /* the skip visited a boot file: index << 8 | file ID, how (CYC_FDS_BOOT_HOW_*)
                                 << 24 | type << 16 | load address */
    CYC_FDS_BOOT_CHECK  = 4,  /* a check the skip made on its evidence: CYC_FDS_BOOT_CHECK_*, result */
    CYC_FDS_BOOT_SKIP   = 5,  /* the skip built the machine: files loaded, bytes written */
    CYC_FDS_BOOT_REFUSE = 6,  /* the skip was asked for and refused: CYC_FDS_BOOT_WHY_*, detail */
    CYC_FDS_BOOT_WAIT   = 7,  /* the boot waits for a disk with the drive empty: BIOS pc of the poll, polls */
    CYC_FDS_BOOT_INSERT = 8,  /* auto insert put a side in: side, frame of the wait it answered */
    CYC_FDS_BOOT_YIELD  = 9,  /* auto insert stood down: 1 host disk change, 2 the game started; frame */
};

/* FDS_BOOT check codes (value of a CYC_FDS_BOOT_CHECK event; length the result) */
enum {
    CYC_FDS_BOOT_CHECK_SIGNATURE = 1,  /* block 1 reads with *NINTENDO-HVC* */
    CYC_FDS_BOOT_CHECK_ID        = 2,  /* the disk ID matched: side | disk number << 8 */
    CYC_FDS_BOOT_CHECK_AMOUNT    = 3,  /* file amount | boot file code << 8 */
    CYC_FDS_BOOT_CHECK_FILES     = 4,  /* files asked for */
    CYC_FDS_BOOT_CHECK_LICENSE   = 5,  /* 1: the license screen matched (0: the BIOS stops) */
    CYC_FDS_BOOT_CHECK_CRC       = 6,  /* 1: the drive reports CRC mismatches (all blocks checked good) */
    CYC_FDS_BOOT_CHECK_OAM       = 7,  /* the OAM row the load's rendering-off corrupts ($FF: none) */
    CYC_FDS_BOOT_CHECK_LOOP      = 8,  /* license screen passes run forward */
};

enum {
    CYC_FDS_IRQ_TIMER = 1,
    CYC_FDS_IRQ_DISK  = 2,
};

enum {
    CYC_FDS_BYTE_WRITE    = 0x01,  /* write mode ($4025.2 = 0) */
    CYC_FDS_BYTE_GAP_END  = 0x02,  /* this byte ended the gap (the $80 mark) */
    CYC_FDS_BYTE_TRANSFER = 0x04,  /* the byte transfer flag was raised */
    CYC_FDS_BYTE_IRQ      = 0x08,  /* ...and asserted the disk IRQ */
    CYC_FDS_BYTE_CRC      = 0x10,  /* $4025.4 (CRC transfer) was set */
    CYC_FDS_BYTE_STORED   = 0x20,  /* write mode: the byte reached the disk image */
};

typedef struct {
    uint64_t cycle;     /* CPU cycles since power-on (cyc_cycle_count) */
    uint32_t frame;     /* frames completed when the event happened */
    uint16_t kind;      /* CycRingKind */
    uint16_t addr;
    uint32_t value;
    uint32_t repeat;    /* identical reads folded into this event */
} CycRingEvent;

#define CYC_RING_CAPACITY (1u << 20)

/* Frames completed; the scheduler (cyc_run.c) advances it. */
extern uint32_t cyc_ring_frame;

void cyc_ring_reset(void);
/* Record an event at the current cycle. Reads fold into an identical
 * previous read, RAM_INTERP events into the previous one of the same chunk
 * and frame. */
void cyc_ring_push(CycRingKind kind, uint16_t addr, uint32_t value);
/* The kinds that carry a length (WRITE_RUN, WRITE_BLOCK, FDS_SAVE, FDS_LOAD). */
void cyc_ring_push_len(CycRingKind kind, uint16_t addr, uint32_t value, uint32_t length);

/* Events ever recorded (the index the next one gets), and the oldest index
 * still held. */
uint64_t cyc_ring_total(void);
uint64_t cyc_ring_oldest(void);
/* Events of each kind ever recorded, including folded repeats. */
uint64_t cyc_ring_kind_total(CycRingKind kind);
bool     cyc_ring_get(uint64_t index, CycRingEvent *out);
const char *cyc_ring_kind_name(unsigned kind);

/* Write the held events whose frame is in [first_frame, last_frame] as text,
 * one per line, with the per-kind totals first. last_frame UINT32_MAX = all. */
void cyc_ring_dump(void *file, uint32_t first_frame, uint32_t last_frame);
/* Install an atexit dump to $NESRECOMP_CYC_RING_DUMP if it is set. */
void cyc_ring_dump_at_exit_from_env(void);

#ifdef __cplusplus
}
#endif
