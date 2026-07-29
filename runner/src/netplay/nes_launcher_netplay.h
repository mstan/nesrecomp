#ifndef NES_LAUNCHER_NETPLAY_H
#define NES_LAUNCHER_NETPLAY_H

#ifdef RECOMP_LAUNCHER
#include "recomp_launcher.h"
const RecompLauncherCNetplayCallbacks *nes_launcher_netplay_callbacks(
    const char *game_name, const char *game_version);
void nes_launcher_netplay_seed_settings(RecompLauncherCSettings *settings);
void nes_launcher_netplay_persist_settings(const RecompLauncherCSettings *settings);
int nes_launcher_netplay_consume_launch(const RecompLauncherCSettings *settings,
                                        int *host_widescreen);
void nes_launcher_netplay_returned_to_lobby(void);
#endif

#endif
