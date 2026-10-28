/*
 * cyc_ramview.h - compiled views of code in RAM (see cyc_recomp.h CycRamView).
 *
 * Code a program runs from RAM (a disk file the FDS BIOS loaded, a routine it
 * copied) is compiled ahead of time from every image of RAM the compiler
 * knows: declared ROM-to-SRAM copies, disk files at their load addresses, and snapshots a run
 * captured (--capture-log, game.toml [game] cycle_capture_file). A view folds
 * only its own instruction bytes. It is entered only while RAM holds them:
 *
 *   - every view starts UNKNOWN at power-on; the first dispatch to one of its
 *     instruction starts compares its dependency bytes with RAM and marks it
 *     VALID or INVALID;
 *   - the write watch (hw_code_watch) sees every store to a byte some view
 *     folds. A store that changes it demotes the views folding it to UNKNOWN
 *     (a VALID one also sets cyc_ram_code_dirty, which ends the running
 *     block after that store). Stores to bytes no view folds, such as
 *     variables next to code, cost one table load and change nothing;
 *   - an address no VALID view starts an instruction at runs on the
 *     interpreter, which is correct by construction, and is captured for the
 *     next compile when capturing.
 *
 * Everything here observes the machine; nothing changes what it does, so
 * native, --interp-only and cyc_interp runs stay identical cycle for cycle.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cyc_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t native_cycles;     /* CPU cycles RAM views ran (part of cyc_run_native_cycles) */
    uint64_t entries;           /* times the scheduler entered a RAM view */
    uint64_t validated;         /* UNKNOWN views compared with RAM that matched */
    uint64_t rejected;          /* ... that did not */
    uint64_t invalidated;       /* VALID views a store changed */
    uint64_t code_write_exits;  /* view returns after such a store */
    uint64_t interp_insns;      /* RAM instructions the interpreter ran that no view covers */
} CycRamViewStats;
extern CycRamViewStats cyc_ramview_stats;

/* Views this board can use (linear FDS RAM or qualified fixed SNROM SRAM). */
uint32_t cyc_ramview_count(void);
/* Build the lookup tables for the loaded board and mark every view UNKNOWN
 * (cyc_run_power_on). */
void cyc_ramview_power_on(void);
/* The view to run at pc, validating candidates as needed; -1 if none. */
int  cyc_ramview_find(uint16_t pc);
/* Run from view `index` (cyc_ramview_find) until control leaves RAM views. */
void cyc_ramview_run(int index);
/* Called before the interpreter runs the instruction at pc, a RAM address. */
void cyc_ramview_interp(uint16_t pc);
/* End of a frame: the per-frame VIEW_FRAME ring event. */
void cyc_ramview_frame_end(void);
/* One line per view: index, identity hash, entry range and count,
 * dependency bytes, and its state now. */
void cyc_ramview_list(void *file);

/* State (cyc_state.c). The validity of every view, for the snapshot around
 * an isolated mod call (bytes: cyc_ramview_validity_size()); and, after a
 * save state replaced RAM, every usable view back to UNKNOWN, as at
 * power-on (the next dispatch compares it with RAM again). */
size_t cyc_ramview_validity_size(void);
void   cyc_ramview_validity_get(uint8_t *buf);
void   cyc_ramview_validity_set(const uint8_t *buf);
void   cyc_ramview_revalidate(void);

/* ---- capture: RAM code that ran with no view, for the next compile ----
 * Records, from the call on: each instruction variant (address and bytes) the
 * interpreter ran in RAM with no view; per 1KB chunk, snapshots of the chunk
 * taken when such code ran (one per distinct code layout); and the values
 * stores wrote over bytes views fold that no view holds there (code the
 * program rewrites at run time). Written as
 * text and merged with an existing file (format: cyc_ramview.c). */
void cyc_ramview_capture_start(void);
bool cyc_ramview_capturing(void);
/* Returns the number of instruction variants in the merged file, or -1. */
long cyc_ramview_capture_write(const char *path, const char *program);

#ifdef __cplusplus
}
#endif
