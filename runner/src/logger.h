/*
 * logger.h — Centralized logger: only prints on first occurrence or value change
 */
#pragma once
#include <stdint.h>

/* Track a named uint32 value. Prints only when value changes. */
void log_on_change(const char *label, uint32_t value);

/* Reset all tracked values (call at start of each frame for per-frame labels) */
void log_reset_frame(void);
