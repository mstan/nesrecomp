/*
 * nestopia_oracle_cmds.c — Generic Nestopia oracle TCP debug commands
 *
 * Commands: emu_ppu_state, emu_screenshot, framebuf_diff, read_emu_ppu, read_emu_oam
 * All commands are guarded by nestopia_bridge_is_loaded() — they return 0
 * (unhandled) if Nestopia is not initialized.
 */
#ifdef ENABLE_NESTOPIA_ORACLE

#include "nestopia_oracle_cmds.h"
#include "nestopia_bridge.h"
#include "debug_server.h"
#include "nes_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by main_runner.c */
extern void     runner_save_argb_png(const char *path, const uint32_t *argb, int w, int h);
extern uint32_t *runner_get_framebuffer(void);

static void handle_emu_ppu_state(int id, const char *json) {
    (void)json;
    NestopiaPpuInternals pi;
    nestopia_bridge_get_ppu_internals(&pi);
    int mirror = nestopia_bridge_get_mirroring();
    debug_server_send_fmt(
        "{\"id\":%d,\"ok\":true,"
        "\"t\":\"0x%04X\",\"v\":\"0x%04X\",\"w\":%d,\"fine_x\":%d,"
        "\"status\":\"0x%02X\",\"oam_addr\":%d,\"scanline\":%d,"
        "\"scroll_x_from_t\":%d,\"scroll_y_from_t\":%d,"
        "\"scroll_x_from_v\":%d,\"scroll_y_from_v\":%d,"
        "\"mirroring\":%d}",
        id, pi.t, pi.v, pi.w, pi.fine_x,
        pi.status, pi.oam_addr, pi.scanline,
        pi.scroll_x_from_t, pi.scroll_y_from_t,
        pi.scroll_x_from_v, pi.scroll_y_from_v,
        mirror);
}

static void handle_emu_screenshot(int id, const char *json) {
    static uint32_t emu_fb[256 * 240];
    nestopia_bridge_get_framebuf_argb(emu_fb);
    char path[256] = {0};
    const char *pp = strstr(json, "\"path\"");
    if (pp) {
        pp = strchr(pp, ':');
        if (pp) {
            pp++;
            while (*pp == ' ' || *pp == '"') pp++;
            int i = 0;
            while (*pp && *pp != '"' && i < 254) path[i++] = *pp++;
            path[i] = '\0';
        }
    }
    if (!path[0]) snprintf(path, sizeof(path), "emu_shot_%04llu.png", (unsigned long long)g_frame_count);
    runner_save_argb_png(path, emu_fb, 256, 240);
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\"}", id, path);
}

static void handle_framebuf_diff(int id, const char *json) {
    (void)json;
    static uint32_t emu_fb[256 * 240];
    nestopia_bridge_get_framebuf_argb(emu_fb);
    uint32_t *nat_fb = runner_get_framebuffer();
    int diff_count = 0;
    int first_x = -1, first_y = -1;
    char diffs[4096];
    int pos = 0;
    pos += snprintf(diffs + pos, sizeof(diffs) - pos, "[");
    for (int i = 0; i < 256 * 240 && pos < (int)sizeof(diffs) - 120; i++) {
        uint32_t n = nat_fb[i] & 0x00FFFFFF;
        uint32_t e = emu_fb[i] & 0x00FFFFFF;
        if (n != e) {
            if (diff_count > 0 && diff_count < 20)
                pos += snprintf(diffs + pos, sizeof(diffs) - pos, ",");
            if (diff_count < 20) {
                int x = i % 256, y = i / 256;
                if (first_x < 0) { first_x = x; first_y = y; }
                pos += snprintf(diffs + pos, sizeof(diffs) - pos,
                    "{\"x\":%d,\"y\":%d,\"nat\":\"#%06X\",\"emu\":\"#%06X\"}",
                    x, y, n, e);
            }
            diff_count++;
        }
    }
    pos += snprintf(diffs + pos, sizeof(diffs) - pos, "]");
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"total_diff\":%d,"
        "\"first_x\":%d,\"first_y\":%d,\"samples\":%s}",
        id, diff_count, first_x, first_y, diffs);
}

static void handle_read_emu_ppu(int id, const char *json) {
    char addr_str[32] = {0};
    const char *p = strstr(json, "\"addr\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') p++;
            int i = 0;
            while (*p && *p != '"' && *p != ',' && *p != '}' && i < 31)
                addr_str[i++] = *p++;
            addr_str[i] = '\0';
        }
    }
    if (!addr_str[0]) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"missing addr\"}", id);
        return;
    }
    uint32_t addr = (uint32_t)strtoul(addr_str, NULL, 0);

    int len = 1;
    p = strstr(json, "\"len\"");
    if (p) {
        p = strchr(p, ':');
        if (p) len = atoi(p + 1);
    }
    if (len < 1) len = 1;
    if (len > 256) len = 256;

    static uint8_t emu_nt[0x1000];
    static uint8_t emu_pal[0x20];
    static uint8_t emu_chr[0x2000];
    nestopia_bridge_get_nametable(emu_nt, sizeof(emu_nt));
    nestopia_bridge_get_palette(emu_pal);
    nestopia_bridge_get_chr_ram(emu_chr, sizeof(emu_chr));

    char hex[513];
    for (int i = 0; i < len; i++) {
        uint32_t a = addr + i;
        uint8_t v = 0;
        if (a < 0x2000)
            v = emu_chr[a];
        else if (a < 0x3000)
            v = emu_nt[a - 0x2000];
        else if (a >= 0x3F00 && a < 0x3F20)
            v = emu_pal[a - 0x3F00];
        snprintf(hex + i * 2, 3, "%02x", v);
    }

    debug_server_send_fmt(
        "{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"len\":%d,\"hex\":\"%s\"}",
        id, addr, len, hex);
}

static void handle_read_emu_oam(int id, const char *json) {
    (void)json;
    static uint8_t emu_oam[0x100];
    nestopia_bridge_get_oam(emu_oam);
    char hex[513];
    for (int i = 0; i < 256; i++)
        snprintf(hex + i * 2, 3, "%02x", emu_oam[i]);
    debug_server_send_fmt(
        "{\"id\":%d,\"ok\":true,\"len\":256,\"hex\":\"%s\"}",
        id, hex);
}

int nestopia_oracle_handle_cmd(const char *cmd, int id, const char *json) {
    if (!nestopia_bridge_is_loaded()) return 0;

    if (strcmp(cmd, "emu_ppu_state") == 0)  { handle_emu_ppu_state(id, json);  return 1; }
    if (strcmp(cmd, "emu_screenshot") == 0) { handle_emu_screenshot(id, json); return 1; }
    if (strcmp(cmd, "framebuf_diff") == 0)  { handle_framebuf_diff(id, json);  return 1; }
    if (strcmp(cmd, "read_emu_ppu") == 0)   { handle_read_emu_ppu(id, json);   return 1; }
    if (strcmp(cmd, "read_emu_oam") == 0)   { handle_read_emu_oam(id, json);   return 1; }
    return 0;
}

#endif /* ENABLE_NESTOPIA_ORACLE */
