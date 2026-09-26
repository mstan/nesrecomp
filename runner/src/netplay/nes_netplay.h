#ifndef NES_NETPLAY_H
#define NES_NETPLAY_H
/*
 * nes_netplay.h -- NES session facade over recomp-net.
 *
 * Every NES netplay match runs recomp-net's rollback episode driver
 * (recomp_net/rb_driver.h) through nes_netplay_rb.c. "Delay" mode is the same
 * driver with prediction pinned off (NES_RB_LOCKSTEP=1), so there is one admit
 * path and one set of wire messages whatever the lobby settled.
 *
 * Host loop (main_runner.c, top of the OUTERMOST frame callback, before the
 * NMI handler runs -- see docs/NETPLAY.md):
 *
 *     nes_netplay_frame_end();          // finish the tick that just ran
 *     do { stage local pad; a = nes_netplay_poll_admit(); } while (!a);
 *     ... rows arrive through the publish hook, applied right before the NMI
 *     ... a == NES_NETPLAY_ADMIT_REPLAY: run the tick, present nothing
 *
 * Input: 1 byte per seat (the NES pad byte, active high, 0 = neutral), up to
 * NES_NETPLAY_MAX_SLOTS seats. Seat i's byte reaches the guest as seat i+1 of
 * nes_input_seat(): seats 0/1 are the two controller ports, seats 2/3 the
 * logical seats a multi-player mod reads (g_logical_input). While a session is
 * active the published rows are the ONLY source of any seat's input.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NES_NETPLAY_PAD_BYTES 1
#define NES_NETPLAY_MAX_SLOTS 4

enum {
    NES_NETPLAY_ADMIT_STALL = 0,
    NES_NETPLAY_ADMIT_LIVE = 1,
    NES_NETPLAY_ADMIT_REPLAY = 2
};

typedef struct NesNetplayConfig {
    int      enabled;
    int      local_slot;          /* 0..slot_count-1 (lobby / wire seat) */
    int      slot_count;          /* 2..NES_NETPLAY_MAX_SLOTS */
    uint32_t occupied_mask;       /* 0 = every seat in [0, slot_count) */
    int      spectator;           /* 1 = watch: simulate, contribute nothing */
    int      spectator_wire_slot; /* relay namespace slot (>= slot_count) */
    int      input_player;        /* local device index sampled; 0 = P1 */
    int      input_delay;         /* D, frames */
    int      input_prediction;    /* P; 0 = engine default */
    int      rollback;            /* 1 = rollback (default), 0 = lockstep */
    uint32_t session_id;
    char     bind_hostport[64];
    char     peer_hostport[64];
    int      transport;           /* 0 auto, 1 ICE, 2 LAN / relay UDP */
    int      force_turn;
    int      force_input_relay;
    /* The session configuration image the HOST settled (widescreen, the
     * co-op mode, ...), canonical text from nes_netplay_session_*. Applied on
     * every peer before boot and confirmed in-band by the driver's mod-set
     * handshake. Empty = derive from this peer's settings (the host). */
    char     session_config[512];
} NesNetplayConfig;

void nes_netplay_config_defaults(NesNetplayConfig *cfg);
/* NES_NETPLAY=1, NES_NET_SLOT, NES_NET_SLOTS, NES_NET_BIND, NES_NET_PEER,
 * NES_NET_DELAY, NES_NET_PREDICTION, NES_NET_MODE=rollback|delay,
 * NES_NET_SESSION_ID, NES_NET_TRANSPORT=lan|ice, NES_NET_SPECTATOR,
 * NES_NET_SPECTATOR_SLOT, NES_NET_OCCUPIED, NES_NET_RELAY=1,
 * NES_NET_SESSION_CONFIG="<canonical text, ';' for newlines>". */
void nes_netplay_config_apply_env(NesNetplayConfig *cfg);
/* The launcher's consumed launch, handed to the runner. */
void nes_netplay_set_pending_config(const NesNetplayConfig *cfg);
int  nes_netplay_take_pending_config(NesNetplayConfig *cfg);
int  nes_netplay_pending(void);

/* The session configuration seal lives in nes_session_config.h. */
#include "nes_session_config.h"

/* ---- lifecycle -------------------------------------------------------- */
/* Start after runtime/game init and BEFORE guest code runs. Refuses what an
 * online session cannot honour (input scripts, recording, TCP overrides).
 * Returns 0 on success. */
int  nes_netplay_start(const NesNetplayConfig *cfg);
/* Pre-boot barrier: connect, then the host-authoritative SRAM transfer, so
 * the guest's RESET code already sees the host's cartridge RAM. Returns 0
 * when the match may boot, <0 (with a reason in nes_netplay_last_error) when
 * it must go back to the lobby. Pumps SDL events while waiting. */
int  nes_netplay_boot_barrier(void);
void nes_netplay_shutdown(void);
int  nes_netplay_active(void);
int  nes_netplay_is_running(void);
int  nes_netplay_rollback_active(void);
int  nes_netplay_local_slot(void);
int  nes_netplay_slot_count(void);
int  nes_netplay_is_spectator(void);
int  nes_netplay_is_host(void);
int  nes_netplay_input_player(void);
uint32_t nes_netplay_sim_tick(void);
/* The tick being run (live or replayed): the one whose rows were published. */
uint32_t nes_netplay_current_tick(void);
const char *nes_netplay_transport_name(void);

/* ---- the per-tick gate (main_runner.c) --------------------------------- */
/* Stage the local seat's pad byte for the next admit. */
void nes_netplay_stage_local(uint8_t pad);
/* Pump + admit. Returns NES_NETPLAY_ADMIT_*. */
int  nes_netplay_poll_admit(void);
/* After the admitted tick has run (called at the top of the next callback). */
void nes_netplay_frame_end(void);
/* Rows for the admitted tick: delivered through this hook by the driver.
 * rows[i] is seat i's pad byte; slots <= NES_NETPLAY_MAX_SLOTS. */
typedef void (*NesNetplayPublishFn)(const uint8_t *rows, int slots, int replay);
void nes_netplay_set_publish_hook(NesNetplayPublishFn fn);
/* Resim bracket (presentation / audio suppression) hooks, set by the runner. */
typedef void (*NesNetplayResimFn)(int begin);
void nes_netplay_set_resim_hook(NesNetplayResimFn fn);

/* ---- leaving ------------------------------------------------------------ */
void nes_netplay_request_return_to_lobby(const char *why);
int  nes_netplay_return_to_lobby_requested(const char **why);
/* The driver's refusal code (boot_digest_mismatch, mod_set_mismatch, ...)
 * or a session error ("peer_disconnected", "connect_timeout"). */
const char *nes_netplay_last_error(void);
/* Coordinated stop (SIGUSR1): 1 once drained and the process should exit. */
int  nes_netplay_quiesced(void);
/* Ask for the coordinated stop now (as SIGUSR1 does). Idempotent. */
void nes_netplay_request_quiesce(void);
int  nes_netplay_draining(void);

/* End-of-match counters, one line "NETPLAY_DRIVER ..." on stderr. */
void nes_netplay_log_summary(void);

#ifdef __cplusplus
}
#endif
#endif
