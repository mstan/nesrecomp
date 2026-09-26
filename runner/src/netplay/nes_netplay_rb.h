#ifndef NES_NETPLAY_RB_H
#define NES_NETPLAY_RB_H
/*
 * nes_netplay_rb.h -- the NES binding of recomp-net's rollback episode driver
 * (recomp_net/rb_driver.h, RNetRbHost). Shape copied from snesrecomp
 * runner/src/netplay/snes_netplay_rb.c; the difference is the replay mode:
 * INCREMENTAL, one replayed tick per outer frame callback (docs/NETPLAY.md,
 * "Replay model").
 */
#include <stdint.h>
#include "recomp_net/rb_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NesNetplayRbBindings {
    RNetSession **session;
    int *local_slot;
    int *slot_count;
    int *input_delay;
    int *input_prediction;
    uint32_t occupied_mask;
    int  force_turn;
    /* Rows for one tick (seat i -> rows[i]), replay = 1 inside a resim. */
    void (*publish)(uint32_t tick, const uint8_t *rows, int slots, int replay);
    void (*resim)(int begin);
    void (*refused)(const char *code);
} NesNetplayRbBindings;

void nes_netplay_rb_bind(const NesNetplayRbBindings *b);
int  nes_netplay_rb_start(void);
void nes_netplay_rb_shutdown(void);
void nes_netplay_rb_stage_local(uint8_t pad);
/* RNET_RB_ADMIT_* */
int  nes_netplay_rb_poll_admit(void);
void nes_netplay_rb_finish_frame(void);

void nes_netplay_rb_set_identity(uint32_t build_fp, uint32_t content_fp);
void nes_netplay_rb_set_modset(const char *text, RNetRbModSetCheckFn check,
                               RNetRbModSetAdoptFn adopt);

uint32_t nes_netplay_rb_sim_tick(void);
uint32_t nes_netplay_rb_episode_count(void);
uint32_t nes_netplay_rb_invent_count(void);
uint64_t nes_netplay_rb_resim_ticks(void);
uint32_t nes_netplay_rb_desync_count(void);
uint32_t nes_netplay_rb_replays_changed(void);
int  nes_netplay_rb_in_resim(void);
const char *nes_netplay_rb_refusal(void);
int  nes_netplay_rb_last_fork(uint32_t *tick, const char **partition);
int  nes_netplay_rb_fork_digests(uint32_t *mine, uint32_t *theirs);
int  nes_netplay_rb_draining(void);
int  nes_netplay_rb_quiesced(void);
void nes_netplay_rb_request_quiesce(void);
void nes_netplay_rb_log_summary(void);
/* One tick's host cost (admit -> end of the frame's work, excluding pacing). */
void nes_netplay_rb_note_tick_cost(int replay, double us);

#ifdef __cplusplus
}
#endif
#endif
