/*
 * games/faxanadu/extras.c — Faxanadu-specific runner hooks
 *
 * Implements game_extras.h for Faxanadu.
 * The only game-specific feature is --password: auto-injects a mantra
 * into the RAM buffer on the password entry screen.
 */
#include "game_extras.h"
#include "nes_runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- Password state ---- */
static const char *s_password          = NULL;
static int         s_password_injected = 0;

/* Returns the mantra table index (0-63) for a character, or -1 if invalid.
 * Bank12 $8764 table order: A-Z (0-25), a-z (26-51), 0-9 (52-61), ',' (62), '?' (63) */
static int password_char_to_index(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return 26 + (ch - 'a');
    if (ch >= '0' && ch <= '9') return 52 + (ch - '0');
    if (ch == ',') return 62;
    if (ch == '?') return 63;
    return -1;
}

/* Inject password into the mantra entry RAM buffer.
 * Detection (Ghidra bank12 analysis):
 *   $0665 == len  — max-length register set to our password length
 *   $0666 == 0    — no characters entered yet (fresh screen)
 *   $0600 == 0xFF — first slot is empty sentinel
 * Writes character table indices to $0600+i, sets $0664 = $0666 = len. */
static void maybe_inject_password(void) {
    if (!s_password || s_password_injected) return;

    /* Strip spaces (sometimes shown as separators online) */
    static char s_pw_stripped[25];
    int slen = 0;
    for (const char *p = s_password; *p && slen < 24; p++)
        if (*p != ' ') s_pw_stripped[slen++] = *p;
    s_pw_stripped[slen] = '\0';
    s_password = s_pw_stripped;

    int len = (int)strlen(s_password);
    if (len == 0 || len > 24) return;

    uint8_t max_len = g_ram[0x665];
    if (max_len == 0)         return;   /* screen not initialized yet */
    if (g_ram[0x666] != 0)   return;   /* something already entered */
    if (g_ram[0x600] != 0xFF) return;  /* first slot not empty */

    for (int i = 0; i < len; i++) {
        int idx = password_char_to_index(s_password[i]);
        if (idx < 0) {
            fprintf(stderr, "[Password] Unknown char '%c' at pos %d — aborted\n",
                    s_password[i], i);
            return;
        }
        g_ram[0x600 + i] = (uint8_t)idx;
    }
    for (int i = len; i < (int)max_len; i++)
        g_ram[0x600 + i] = 0xFF;   /* fill remaining slots with empty sentinel */

    g_ram[0x664] = (uint8_t)len;   /* cursor: positioned after last entered char */
    g_ram[0x666] = (uint8_t)len;   /* characters-entered count */

    /* Queue PPU tile writes so the characters appear on screen.
     * $0500 DMA queue format: [count(1 byte), addr_hi, addr_lo, tile...]
     * Queue write-ptr at $0020. Tiles are ASCII values (bank12 $8764 maps index→ASCII).
     * Row 0 (positions 0-15):  PPU $2129+pos
     * Row 1 (positions 16-31): PPU $2149+(pos-16) */
    for (int i = 0; i < len; i++) {
        uint8_t ppu_lo = (i < 16) ? (0x28 + i) : (0x48 + (i - 16));
        uint8_t tile   = (uint8_t)s_password[i];   /* ASCII = PPU tile number */
        uint8_t wp     = g_ram[0x20];
        g_ram[0x500 + wp++] = 0x01;   /* 1 tile */
        g_ram[0x500 + wp++] = 0x21;   /* PPU addr hi */
        g_ram[0x500 + wp++] = ppu_lo;
        g_ram[0x500 + wp++] = tile;
        g_ram[0x20] = wp;
    }

    s_password_injected = 1;
    printf("[Password] Injected \"%s\" (%d chars)\n", s_password, len);
}

/* ---- game_extras.h implementation ---- */

const char *game_get_name(void) { return "Faxanadu"; }

void game_on_init(void) {}

void game_on_frame(uint64_t frame_count) {
    (void)frame_count;
    maybe_inject_password();
}

int game_handle_arg(const char *key, const char *val) {
    if (strcmp(key, "--password") == 0 && val) {
        s_password = val;
        printf("[Password] Will auto-fill mantra: \"%s\"\n", val);
        return 1;
    }
    return 0;
}

const char *game_arg_usage(void) {
    return "  --password STRING   Auto-fill Faxanadu mantra on password screen\n";
}
