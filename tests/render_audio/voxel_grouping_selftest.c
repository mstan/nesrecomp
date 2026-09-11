/* Exercise real OAM component construction and presentation-only filters. */
#include "voxel_renderer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t g_chr_ram[0x2000], g_ppu_oam[256], g_ppu_pal[32], g_ppuctrl;
uint32_t g_nes_palette[64];
static uint32_t pixels[256 * 240], baseline[256 * 240];
static uint8_t tiles[32 * 30];
static int group_count, sizes[64], masks[64], connection_mode, hide_zero;
static float flat_tile(uint8_t tile, int col, int row, void *user) {
    (void)tile; (void)col; (void)row; (void)user;
    return 0.0f;
}

static int connect_pieces(int a, int b, void *user) {
    (void)user;
    if (connection_mode < 0 && ((a == 0) != (b == 0))) return -1;
    return connection_mode > 0 ? 1 : 0;
}
static int inspect_group(const int *members, int count,
                         int x0, int y0, int x1, int y1, void *user) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)user;
    int mask = 0;
    for (int i = 0; i < count; i++) mask |= 1 << members[i];
    masks[group_count] = mask; sizes[group_count++] = count;
    return !hide_zero || !(mask & 1);
}
static void render(NesVoxelScene *s) {
    memset(pixels, 0, sizeof pixels); group_count = 0;
    if (!nes_voxel_render(s)) exit(6);
}
int main(void) {
    NesVoxelScene scene = {0};
    scene.framebuffer = pixels; scene.output_width = scene.source_width = 256;
    scene.output_height = scene.source_height = 240;
    scene.tiles = tiles; scene.tile_columns = scene.tile_stride = 32;
    scene.tile_rows = 30; scene.tile_size = 8;
    scene.tile_height = flat_tile;
    scene.draw_oam_sprites = 1; scene.elevation_degrees = 50;
    memset(g_ppu_oam, 0xff, sizeof g_ppu_oam);
    memset(g_chr_ram, 0xff, 8); g_ppu_pal[17] = 1;
    g_nes_palette[0] = 0xff000000; g_nes_palette[1] = 0xff00ff00;
    for (int i = 0; i < 3; i++) {
        g_ppu_oam[i*4] = 119; g_ppu_oam[i*4+1] = g_ppu_oam[i*4+2] = 0;
        g_ppu_oam[i*4+3] = (uint8_t)(112 + i * 8);
    }
    render(&scene); memcpy(baseline, pixels, sizeof baseline);
    scene.sprite_connect = connect_pieces;
    scene.sprite_members_visible = inspect_group;
    render(&scene);
    if (group_count != 1 || sizes[0] != 3 || masks[0] != 7 ||
        memcmp(baseline, pixels, sizeof pixels)) return 1;
    connection_mode = -1; render(&scene);
    if (group_count != 2 || masks[0] != 6 || masks[1] != 1) return 2;
    hide_zero = 1; render(&scene);
    if (!memcmp(baseline, pixels, sizeof pixels)) return 3;
    if (g_ppu_oam[0] != 119 || g_ppu_oam[3] != 112) return 4;
    hide_zero = 0; connection_mode = 1; scene.sprite_group_max_width = 16;
    render(&scene);
    if (group_count != 2 || sizes[0] != 2 || sizes[1] != 1) return 5;
    puts("PASS: default pixels, grouping veto, member visibility, size bounds, OAM unchanged");
    return 0;
}
