/*
 * cyc_host.h - what the windowed host (cyc_sdl.c) calls back into cyc_host.c.
 */
#pragma once
#include "cyc_disk_action.h"
#include "../../common/nes_fds_hle.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* After every frame (frames completed so far): the FDS disk save cadence. */
void cyc_host_frame_done(long frames_done);
/* The player ejected the disk: save it now if it changed. */
void cyc_host_disk_ejected(void);
/* The disk save's state for the drive indicator ("SAVED F1234", "UNSAVED", ...). */
const char *cyc_host_disk_save_status(void);
/* The FDS HLE tier: flip one axis with a live toggle (a dev build's keys; the
 * axis is an index into nes_fds_hle_axes()), re-planned through
 * nes_fds_hle_plan(); returns the plan's text for the drive bar
 * ("HLE SWAP FAST", "HLE OFF", "HLE OFF (NO SWAP)"...). */
const char *cyc_host_hle_toggle(int axis);
const char *cyc_host_hle_text(void);
/* The HLE plan's axes (common/nes_fds_hle.h nes_fds_hle_axes() order) for
 * the runtime menu: the plan in force, what each source asked, and the
 * player's saved choice (the plan's "settings" source; -1 says nothing). */
const NesFdsHlePlan    *cyc_host_hle_plan(void);
const NesFdsHleRequest *cyc_host_hle_request(void);
void cyc_host_hle_user_set(unsigned axis, int8_t value);

/* The Disk action (cyc_disk_action.h) on the real drive, shared by the
 * window's shortcut, the runtime menu and the headless --input DISK_ACTION:
 * a press, a direct choice of side, and the per-frame hook (before every
 * frame). now_ms is the toast's clock: wall time in the window, emulated time
 * headless. */
CycDiskAction *cyc_host_disk_action(void);
CycDiskPress   cyc_host_disk_press(uint64_t now_ms, long frame);
CycDiskPress   cyc_host_disk_choose(uint64_t now_ms, long frame, unsigned side);
bool           cyc_host_disk_frame(uint64_t now_ms, long frame);
/* Direct drive changes (dev keys, the menu's eject/insert), which cancel a
 * swap the Disk action scheduled. */
bool     cyc_host_disk_eject(long frame);
bool     cyc_host_disk_insert(long frame, unsigned side);
unsigned cyc_host_disk_selected(void);          /* the side an insert puts in */
void     cyc_host_disk_select(unsigned side);

/* The window's settings and launcher (cyc_sdl.c), before the image loads:
 * reads config.ini, runs recomp-ui's launcher when the build has it (which
 * may change *rom_path), and hands back the player's saved FDS HLE choices
 * and FDS BIOS file (config.ini [FDS] Bios; NULL: none saved). cli_bios is
 * --fds-bios (NULL: none), which the launcher's BIOS state reflects.
 * Returns 0 to go on, 1 when the player quit the launcher, 2 on startup failure. */
int cyc_sdl_prelaunch(const char **rom_path, const char *cli_bios, NesFdsHleAsk *saved_hle,
                      const char **saved_bios);
/* A windowed start's fatal error (a missing FDS BIOS) as a message box, so a
 * window that never opens does not exit silently. Nothing on a --hidden
 * window. Returns whether the box was shown. */
bool cyc_sdl_error_box(const char *title, const char *text);

/* Pacing: seconds per frame, and whether the frame just run may skip pacing
 * and presentation (fast load during a disk load). */
double cyc_host_frame_seconds(void);
bool   cyc_host_frame_unpaced(void);

#ifdef __cplusplus
}
#endif
