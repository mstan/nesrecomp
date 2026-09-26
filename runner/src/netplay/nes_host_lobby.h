#ifndef NES_HOST_LOBBY_H
#define NES_HOST_LOBBY_H
/* NES adapter over recomp-ui's shared netplay backend; see nes_host_lobby.c. */
#include "recomp_launcher.h"
#include "nes_netplay.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Init once (idempotent) and return the launcher callback table. rom_path
 * (may be NULL) is hashed for the content fingerprint. */
const RecompLauncherCNetplayCallbacks *nes_host_lobby_init(const char *game_name,
                                                           const char *rom_path);
void nes_host_lobby_shutdown(void);
/* A launch -> the runner's session config (session image from the caps). */
int  nes_host_lobby_config_from_launch(const RecompLauncherCNetplayLaunch *l,
                                       NesNetplayConfig *c);
/* Soft return: surface the match's error code in the room and rebuild it for
 * a rematch (sets gi->resume_netplay_room when gi != NULL). */
void nes_host_lobby_returned(RecompLauncherCGameInfo *gi, const char *error_code);

/* Headless room (NES_LOBBY_SELFTEST=host|guest, _LOBBY, _NAME, _SEATS,
 * RNET_LOBBY_URL): round 1 connects and creates / finds and joins; every
 * round readies, the host starts once every seat is filled and ready. */
int  nes_host_lobby_selftest_role(void);
int  nes_host_lobby_selftest_room(int round, NesNetplayConfig *cfg);
void nes_host_lobby_selftest_report(int round);
void nes_host_lobby_selftest_linger(unsigned ms);

#ifdef __cplusplus
}
#endif
#endif
