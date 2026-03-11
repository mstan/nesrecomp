/*
 * logger.c — Centralized logger
 * Suppresses duplicate output. Only prints first occurrence or on value change.
 */
#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define MAX_TRACKED 64

typedef struct {
    char     label[32];
    uint32_t last_value;
    int      ever_seen;
} TrackedValue;

static TrackedValue s_tracked[MAX_TRACKED];
static int          s_tracked_count = 0;

void log_on_change(const char *label, uint32_t value) {
    /* Find existing entry */
    for (int i = 0; i < s_tracked_count; i++) {
        if (strncmp(s_tracked[i].label, label, 31) == 0) {
            if (!s_tracked[i].ever_seen || s_tracked[i].last_value != value) {
                printf("[LOG] %s = 0x%X\n", label, value);
                s_tracked[i].last_value = value;
                s_tracked[i].ever_seen = 1;
            }
            return;
        }
    }
    /* New label */
    if (s_tracked_count < MAX_TRACKED) {
        strncpy(s_tracked[s_tracked_count].label, label, 31);
        s_tracked[s_tracked_count].last_value = value;
        s_tracked[s_tracked_count].ever_seen = 1;
        s_tracked_count++;
        printf("[LOG] %s = 0x%X\n", label, value);
    }
}

void log_reset_frame(void) {
    /* Do nothing by default — labels persist across frames.
     * Override per-session if needed. */
}
