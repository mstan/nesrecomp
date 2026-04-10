/*
 * nestopia_oracle_cmds.h — Generic Nestopia oracle TCP commands for NES recomp
 * Provides: emu_ppu_state, emu_screenshot, framebuf_diff, read_emu_ppu, read_emu_oam
 */
#pragma once

/* Returns 1 if cmd was handled, 0 if not.
 * Call from debug_server dispatch chain when ENABLE_NESTOPIA_ORACLE is defined. */
int nestopia_oracle_handle_cmd(const char *cmd, int id, const char *json);
