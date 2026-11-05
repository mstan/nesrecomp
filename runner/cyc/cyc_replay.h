/*
 * cyc_replay.h - always-on diagnostic replay recording for the windowed cycle
 * host, and what headless playback of it needs (tools/cyc/replay_diagnostic.py).
 *
 * The window records, from power-on, rolling SEGMENTS of play. A segment is a
 * pure continuous run of the machine:
 *
 *   <stem>-NNNNNN.cycreplay          text: header, then per frame the inputs the
 *                                    machine consumed and the state hashes after
 *   <stem>-NNNNNN.cycreplay.state    the machine at the segment's start, an
 *                                    ordinary save state (cyc_state.h): it loads
 *                                    with --load-state in any build of the same
 *                                    program
 *   <stem>-NNNNNN.cycreplay.config.ini / .mods.toml   copies of the player's
 *                                    configuration and mod selection (when present)
 *
 * Anything that is not "the same machine running the next frame" ends the
 * segment and starts a new one with a fresh checkpoint: a save-state load or
 * rewind, a mod or FDS HLE selection change, a skipped frame, a netplay
 * session. A segment also ends after CYC_REPLAY_SEGMENT_FRAMES frames. So
 * playback never has to reconstruct a machine: each segment replays alone.
 *
 * Record lines (hex unless noted), in execution order. All of a frame's lines
 * come before its H line; a frame whose H is missing never finished (a hang or
 * crash while it ran):
 *   P <frame> <addr> <value>         a QA cheat/poke changed this byte before the frame
 *   D <frame> E | D <frame> I <side> the FDS drive ejected / took a side before the frame
 *   Z <frame> <x> <y> <trigger>      the Zapper (decimal x/y, -1 off) when it changed
 *   I <frame> b0 b1 l0 l1 l2 l3 f0 f1
 *                                    b0 b1: the pads before the game's input hook,
 *                                    after bindings, TCP holds and the menu's
 *                                    release gate; l0..l3: the logical seats mods
 *                                    see; f0 f1: the bytes the machine latched
 *   H <frame> mem hw pc a x y s p cycles
 *                                    the --hash-out hashes after the frame
 *
 * Recording runs on the emulation thread only to copy a few hundred bytes per
 * frame into a fixed 16 MiB queue; a writer thread owns every file. A queue that
 * fills or a write that fails STOPS recording loudly (log line, ring event); it
 * never blocks the game and never grows. Frames are flushed every 100 ms and a
 * normal exit drains the queue.
 *
 * Retention: CYC_REPLAY_SEGMENTS segments (one hour at 600 frames) and a
 * CYC_REPLAY_HISTORY_BYTES budget for checkpoints, oldest evicted first.
 *
 * Environment: NESRECOMP_REPLAY_RECORD=0 turns recording off;
 * NESRECOMP_REPLAY_DIR names the destination (default <exe dir>/diagnostics, or
 * NESRECOMP_DIAGNOSTICS_DIR); NESRECOMP_REPLAY_HISTORY_MIB (2..256) shrinks the
 * checkpoint budget and NESRECOMP_REPLAY_SEGMENT_FRAMES (2..600) the segment
 * length, for tests. Recording is suspended during online play (the rollback
 * session owns the machine) and resumes with a new segment afterwards.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYC_REPLAY_SEGMENT_FRAMES 600
#define CYC_REPLAY_SEGMENTS       360
#define CYC_REPLAY_HISTORY_BYTES  (256u * 1024u * 1024u)
#define CYC_REPLAY_QUEUE_BYTES    (16u * 1024u * 1024u)

/* CYC_EV_REPLAY addr codes (cyc_ring.h): value = segment, length = frame. */
enum {
    CYC_REPLAY_EV_OPEN    = 0,   /* a segment opened */
    CYC_REPLAY_EV_CLOSE   = 1,   /* a segment closed (cap reached) */
    CYC_REPLAY_EV_DISCONT = 2,   /* a discontinuity ended one; see CYC_REPLAY_WHY_* in length's high bits */
    CYC_REPLAY_EV_FAILED  = 3,   /* recording stopped for good */
    CYC_REPLAY_EV_SUSPEND = 4,   /* online play: recording paused */
    CYC_REPLAY_EV_EVICT   = 5,   /* the oldest segment was removed (value = its number) */
};

/* The window, before its loop (after power-on and the session start). title
 * names the files. False when recording is off or cannot start; the reason
 * is logged and the game runs on. */
/* The configuration file the window uses (copied beside each segment); before cyc_replay_init. */
void cyc_replay_set_config_path(const char *path);
bool cyc_replay_init(const char *title);
/* Settings the host knows that change what the machine does but live outside
 * a save state (the game's view mode, ...): a change starts a new segment. */
void cyc_replay_set_policy(uint32_t digest);
/* Just before the machine runs a frame (after the inputs, Zapper and cheats
 * for it are in place): decide whether a new segment opens, write the frame's
 * events and its input line. phys: pads before the game's input hook (2),
 * logical: the seats mods see (4), final: the bytes the machine latches (2). */
void cyc_replay_frame_begin(const uint8_t phys[2], const uint8_t logical[4], const uint8_t final_pads[2]);
/* After the frame ran (and its frame_end hooks): its hashes. */
void cyc_replay_frame_end(void);
/* Online play has the machine: pause recording (once logged). */
void cyc_replay_suspend(const char *why);
/* Drain the queue and stop the writer (the host calls it before SDL_Quit). */
void cyc_replay_shutdown(void);
/* Recording now (started, not failed or suspended). */
bool cyc_replay_active(void);
/* The path of the segment being written ("" when none). */
const char *cyc_replay_path(void);

/* Hooks other modules call through function pointers they own, so a program
 * that does not link this module (tests, the oracle) is unaffected. */
extern uint32_t cyc_state_load_count;       /* cyc_state.c: loads completed */
extern void (*cyc_cheat_change_hook)(uint16_t addr, uint8_t value);   /* cyc_cheat.c: a byte a cheat changed */
extern void (*cyc_fds_change_hook)(char action, unsigned side);       /* hw_fds.c: host eject ('E') / insert ('I') */
extern uint32_t cyc_host_settings_revision; /* cyc_host.c: FDS HLE plan changes */
extern uint32_t cyc_session_mods_revision;  /* cyc_session.c: mod activations */

#ifdef __cplusplus
}
#endif
