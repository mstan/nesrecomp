#ifndef NES_NETPLAY_H
#define NES_NETPLAY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NES_NETPLAY_PAD_BYTES 5
typedef struct NesNetplayConfig {
    int enabled, local_slot, input_player, input_delay;
    uint32_t session_id;
    char bind_hostport[64], peer_hostport[64];
    int transport; /* 0 auto, 1 ICE, 2 LAN */
} NesNetplayConfig;
void nes_netplay_config_defaults(NesNetplayConfig*);
void nes_netplay_apply_env(NesNetplayConfig*);
void nes_netplay_set_pending_config(const NesNetplayConfig*);
int nes_netplay_take_pending_config(NesNetplayConfig*);
int nes_netplay_start(const NesNetplayConfig*);
void nes_netplay_shutdown(void);
int nes_netplay_active(void);
int nes_netplay_is_running(void);
int nes_netplay_local_slot(void);
int nes_netplay_input_player(void);
uint32_t nes_netplay_sim_tick(void);
void nes_netplay_stage_local(uint8_t);
int nes_netplay_needs_local_sample(void);
int nes_netplay_poll_admit(void);
void nes_netplay_finish_frame(void);
uint8_t nes_netplay_published_buttons(int);
void nes_netplay_wait_recv(int);
int nes_netplay_input_desync(uint32_t*,uint32_t*,uint32_t*);
int nes_netplay_state_desync(uint32_t*,uint32_t*,uint32_t*);
int nes_netplay_peer_disconnected(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
