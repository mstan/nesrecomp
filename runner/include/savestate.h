#pragma once

/* Save current emulator state to a binary file.
 * Returns 1 on success, 0 on failure. */
int savestate_save(const char *path);

/* Load emulator state from a binary file.
 * Returns 1 on success, 0 on failure. */
int savestate_load(const char *path);
