/* Cycle host binding to the shared NES rollback/session facade. */
#pragma once
#include "nes_netplay.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void nes_runner_register_session_keys(void);
void cyc_net_prepare(NesNetplayConfig *cfg);
int cyc_net_start(const NesNetplayConfig *cfg, const char *rom);
int cyc_net_boot(void);
int cyc_net_admit(uint8_t local);
void cyc_net_input(uint8_t buttons[2]);
void cyc_net_finish(void);
bool cyc_net_replaying(void);
bool cyc_net_leaving(void);
bool cyc_net_guest(void);
void cyc_net_shutdown(void);
/* Cartridge storage wire image, including Datach; no host path is serialized. */
const uint8_t *cyc_net_sram(size_t *len);
bool cyc_net_sram_load(const void *data, size_t len);
