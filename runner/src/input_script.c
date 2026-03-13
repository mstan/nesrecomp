/*
 * input_script.c — NES input script playback and recording
 */
#include "input_script.h"
#include "savestate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int g_turbo = 0;

/* ---- Button name table ---- */
static const struct { const char *name; uint8_t mask; } s_buttons[] = {
    {"A",      0x80}, {"B",      0x40},
    {"SELECT", 0x20}, {"START",  0x10},
    {"UP",     0x08}, {"DOWN",   0x04},
    {"LEFT",   0x02}, {"RIGHT",  0x01},
};
#define NUM_BUTTONS (sizeof(s_buttons)/sizeof(s_buttons[0]))

static uint8_t parse_button(const char *name) {
    for (int i = 0; i < (int)NUM_BUTTONS; i++)
        if (strcmp(name, s_buttons[i].name) == 0) return s_buttons[i].mask;
    fprintf(stderr, "[Script] Unknown button: %s\n", name);
    return 0;
}

/* ---- Command types ---- */
typedef enum {
    CMD_WAIT, CMD_HOLD, CMD_RELEASE,
    CMD_TURBO_ON, CMD_TURBO_OFF,
    CMD_SCREENSHOT, CMD_LOG, CMD_EXIT,
    CMD_WAIT_RAM8, CMD_ASSERT_RAM8,
    CMD_SAVE_STATE, CMD_LOAD_STATE,
} CmdType;

typedef struct {
    CmdType type;
    int     iarg;          /* WAIT: frames; EXIT: code; WAIT_RAM8/ASSERT_RAM8: addr */
    uint8_t barg;          /* HOLD/RELEASE: button mask; WAIT/ASSERT_RAM8: expected value */
    char    sarg[128];     /* SCREENSHOT: filename; LOG/ASSERT: message */
} Cmd;

#define MAX_CMDS 4096
static Cmd    s_cmds[MAX_CMDS];
static int    s_cmd_count  = 0;
static int    s_cmd_cursor = 0;
static int    s_wait_left  = 0;
static int    s_exit_code  = -1;
static int    s_loaded     = 0;
static uint8_t s_buttons_held = 0;
static char   s_shot_pending[128] = {0};
static int    s_auto_shot_num = 1;

/* For WAIT_RAM8 timeout */
static uint64_t s_wait_ram_start_frame = 0;
#define WAIT_RAM_TIMEOUT_FRAMES (30 * 60)  /* 30 seconds at 60fps */

static void trim(char *s) {
    char *p = s + strlen(s) - 1;
    while (p >= s && isspace((unsigned char)*p)) *p-- = '\0';
    while (*s && isspace((unsigned char)*s)) memmove(s, s+1, strlen(s));
}

int script_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[Script] Cannot open: %s\n", path); return 0; }

    char line[256];
    s_cmd_count = 0;
    while (fgets(line, sizeof(line), f) && s_cmd_count < MAX_CMDS) {
        /* Strip comment */
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        trim(line);
        if (!line[0]) continue;

        Cmd c = {0};
        char tok[64], arg1[128], arg2[128];
        int n = sscanf(line, "%63s %127s %127s", tok, arg1, arg2);
        if (n < 1) continue;

        /* Uppercase tok */
        for (char *p = tok; *p; p++) *p = (char)toupper((unsigned char)*p);

        if (strcmp(tok, "WAIT") == 0 && n >= 2) {
            c.type = CMD_WAIT; c.iarg = atoi(arg1);
        } else if (strcmp(tok, "HOLD") == 0 && n >= 2) {
            for (char *p = arg1; *p; p++) *p = (char)toupper((unsigned char)*p);
            c.type = CMD_HOLD; c.barg = parse_button(arg1);
        } else if (strcmp(tok, "RELEASE") == 0 && n >= 2) {
            for (char *p = arg1; *p; p++) *p = (char)toupper((unsigned char)*p);
            c.type = CMD_RELEASE; c.barg = parse_button(arg1);
        } else if (strcmp(tok, "TURBO") == 0 && n >= 2) {
            for (char *p = arg1; *p; p++) *p = (char)toupper((unsigned char)*p);
            c.type = (strcmp(arg1, "ON") == 0) ? CMD_TURBO_ON : CMD_TURBO_OFF;
        } else if (strcmp(tok, "SCREENSHOT") == 0) {
            c.type = CMD_SCREENSHOT;
            if (n >= 2) strncpy(c.sarg, arg1, sizeof(c.sarg)-1);
        } else if (strcmp(tok, "LOG") == 0) {
            c.type = CMD_LOG;
            /* Capture rest of line after "LOG " */
            const char *rest = strstr(line, " ");
            if (rest) strncpy(c.sarg, rest+1, sizeof(c.sarg)-1);
        } else if (strcmp(tok, "EXIT") == 0) {
            c.type = CMD_EXIT; c.iarg = (n >= 2) ? atoi(arg1) : 0;
        } else if (strcmp(tok, "WAIT_RAM8") == 0 && n >= 3) {
            c.type = CMD_WAIT_RAM8;
            c.iarg = (int)strtol(arg1, NULL, 16) & 0x7FF;
            c.barg = (uint8_t)strtol(arg2, NULL, 16);
        } else if (strcmp(tok, "ASSERT_RAM8") == 0 && n >= 3) {
            c.type = CMD_ASSERT_RAM8;
            c.iarg = (int)strtol(arg1, NULL, 16) & 0x7FF;
            c.barg = (uint8_t)strtol(arg2, NULL, 16);
            if (n >= 4) {
                /* message is everything after addr and value */
                const char *p = line;
                for (int skip = 0; skip < 3 && *p; p++)
                    if (*p == ' ') { skip++; while(*p == ' ') p++; break; }
                strncpy(c.sarg, p, sizeof(c.sarg)-1);
            }
        } else if (strcmp(tok, "SAVE_STATE") == 0 && n >= 2) {
            c.type = CMD_SAVE_STATE;
            strncpy(c.sarg, arg1, sizeof(c.sarg)-1);
        } else if (strcmp(tok, "LOAD_STATE") == 0 && n >= 2) {
            c.type = CMD_LOAD_STATE;
            strncpy(c.sarg, arg1, sizeof(c.sarg)-1);
        } else {
            fprintf(stderr, "[Script] Unknown command: %s\n", tok);
            continue;
        }
        s_cmds[s_cmd_count++] = c;
    }
    fclose(f);
    printf("[Script] Loaded %d commands from %s\n", s_cmd_count, path);
    s_loaded = 1;
    s_cmd_cursor = 0;
    s_wait_left  = 0;
    s_exit_code  = -1;
    s_buttons_held = 0;
    return 1;
}

void script_tick(uint64_t frame, const uint8_t *ram) {
    if (!s_loaded || s_exit_code >= 0) return;

    /* Process commands until we need to block */
    while (s_cmd_cursor < s_cmd_count) {
        Cmd *c = &s_cmds[s_cmd_cursor];

        if (c->type == CMD_WAIT) {
            if (s_wait_left == 0) {
                if (c->iarg == 0) { s_cmd_cursor++; continue; }
                s_wait_left = c->iarg;
            }
            s_wait_left--;
            if (s_wait_left > 0) return;
            s_wait_left = 0;
            s_cmd_cursor++;
            continue;
        }

        if (c->type == CMD_WAIT_RAM8) {
            if (s_wait_ram_start_frame == 0) s_wait_ram_start_frame = frame;
            uint8_t actual = ram[c->iarg & 0x7FF];
            if (actual == c->barg) {
                printf("[Script] WAIT_RAM8 $%03X==%02X satisfied at frame %llu\n",
                       c->iarg, c->barg, (unsigned long long)frame);
                s_wait_ram_start_frame = 0;
                s_cmd_cursor++;
                continue;
            }
            if (frame - s_wait_ram_start_frame > WAIT_RAM_TIMEOUT_FRAMES) {
                fprintf(stderr, "[Script] WAIT_RAM8 $%03X==%02X TIMEOUT (got %02X)\n",
                        c->iarg, c->barg, actual);
                s_wait_ram_start_frame = 0;
                s_cmd_cursor++;
            }
            return;
        }

        /* All other commands execute immediately */
        switch (c->type) {
            case CMD_HOLD:
                s_buttons_held |= c->barg;
                printf("[Script] HOLD %02X (held=%02X)\n", c->barg, s_buttons_held);
                break;
            case CMD_RELEASE:
                s_buttons_held &= ~c->barg;
                printf("[Script] RELEASE %02X (held=%02X)\n", c->barg, s_buttons_held);
                break;
            case CMD_TURBO_ON:
                g_turbo = 1;
                printf("[Script] TURBO ON\n");
                break;
            case CMD_TURBO_OFF:
                g_turbo = 0;
                printf("[Script] TURBO OFF\n");
                break;
            case CMD_SCREENSHOT: {
                char name[128];
                if (c->sarg[0])
                    strncpy(name, c->sarg, sizeof(name)-1);
                else
                    snprintf(name, sizeof(name), "nes_script_%03d.png", s_auto_shot_num++);
                snprintf(s_shot_pending, sizeof(s_shot_pending), "%s", name);
                printf("[Script] SCREENSHOT -> %s\n", s_shot_pending);
                break;
            }
            case CMD_LOG:
                printf("[Script] %s\n", c->sarg);
                break;
            case CMD_SAVE_STATE:
                printf("[Script] SAVE_STATE %s at frame %llu\n",
                       c->sarg, (unsigned long long)frame);
                savestate_save(c->sarg);
                break;
            case CMD_LOAD_STATE:
                printf("[Script] LOAD_STATE %s at frame %llu\n",
                       c->sarg, (unsigned long long)frame);
                savestate_load(c->sarg);
                break;
            case CMD_EXIT:
                printf("[Script] EXIT %d at frame %llu\n",
                       c->iarg, (unsigned long long)frame);
                s_exit_code = c->iarg;
                return;
            case CMD_ASSERT_RAM8: {
                uint8_t actual = ram[c->iarg & 0x7FF];
                if (actual != c->barg)
                    fprintf(stderr, "[Script] ASSERT FAIL: $%03X expected %02X got %02X %s\n",
                            c->iarg, c->barg, actual, c->sarg);
                else
                    printf("[Script] ASSERT OK: $%03X==%02X %s\n",
                           c->iarg, c->barg, c->sarg);
                break;
            }
            default: break;
        }
        s_cmd_cursor++;
    }

    /* Reached end of script with no EXIT — treat as EXIT 0 */
    if (s_cmd_cursor >= s_cmd_count) {
        printf("[Script] Script complete at frame %llu\n", (unsigned long long)frame);
        s_exit_code = 0;
    }
}

int script_get_buttons(void) {
    if (!s_loaded) return -1;
    return (int)(uint8_t)s_buttons_held;
}

int script_check_exit(void) {
    return s_exit_code;
}

int script_wants_screenshot(char *buf, int buflen) {
    if (!s_shot_pending[0]) return 0;
    snprintf(buf, buflen, "C:/temp/%s", s_shot_pending);
    s_shot_pending[0] = '\0';
    return 1;
}

/* ---- Recording ---- */
static FILE    *s_rec_file      = NULL;
static uint8_t  s_rec_prev_btn  = 0;
static int      s_rec_prev_turbo = 0;
static uint64_t s_rec_last_frame = 0;
static int      s_rec_opened    = 0;

void record_open(const char *path) {
    s_rec_file = fopen(path, "w");
    if (!s_rec_file) { fprintf(stderr, "[Record] Cannot open: %s\n", path); return; }
    fprintf(s_rec_file, "# NES input recording\n");
    fflush(s_rec_file);
    s_rec_opened = 1;
    printf("[Record] Recording to %s\n", path);
}

void record_close(void) {
    if (!s_rec_file) return;
    fprintf(s_rec_file, "EXIT 0\n");
    fflush(s_rec_file);
    fclose(s_rec_file);
    s_rec_file = NULL;
}

void record_loadstate(uint64_t frame, const char *path) {
    if (!s_rec_file) return;
    uint64_t delta = frame - s_rec_last_frame;
    fprintf(s_rec_file, "WAIT %llu\n", (unsigned long long)delta);
    fprintf(s_rec_file, "LOAD_STATE %s\n", path);
    fflush(s_rec_file);
    s_rec_last_frame = frame;
}

/* Call immediately after savestate_load() to re-sync the recording's frame
 * baseline to the restored frame count, preventing uint64_t underflow on
 * the next record_tick() delta calculation. */
void record_sync_frame(uint64_t frame) {
    s_rec_last_frame = frame;
}

void record_tick(uint64_t frame, uint8_t buttons, int turbo) {
    if (!s_rec_file) return;

    uint8_t changed_btn   = buttons ^ s_rec_prev_btn;
    int     changed_turbo = (turbo != s_rec_prev_turbo);

    if (!changed_btn && !changed_turbo) return;

    /* On first change (or after a gap) emit a WAIT */
    uint64_t delta = frame - s_rec_last_frame;
    if (delta > 0 || !s_rec_opened) {
        fprintf(s_rec_file, "WAIT %llu\n", (unsigned long long)delta);
        s_rec_opened = 0;
    }
    s_rec_last_frame = frame;

    if (changed_turbo) {
        fprintf(s_rec_file, "TURBO %s\n", turbo ? "ON" : "OFF");
        s_rec_prev_turbo = turbo;
    }

    for (int i = 0; i < (int)NUM_BUTTONS; i++) {
        if (!(changed_btn & s_buttons[i].mask)) continue;
        if (buttons & s_buttons[i].mask)
            fprintf(s_rec_file, "HOLD %s\n", s_buttons[i].name);
        else
            fprintf(s_rec_file, "RELEASE %s\n", s_buttons[i].name);
    }
    s_rec_prev_btn = buttons;
    fflush(s_rec_file);
}
