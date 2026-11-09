/* Exercise the profile's source-pixel contract and legacy positional callers. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../../src/voxel_screen_profile.c"

int g_render_width = 256, g_widescreen_left;
int g_widescreen_right, g_ws_eff_left, g_ws_eff_right;
uint32_t g_nes_palette[64];
uint8_t g_ppu_pal[32], g_ppu_oam[256], g_ppuctrl;
NesConfig g_nes_config;
void nes_video_request_margins(int left, int right) { (void)left; (void)right; }
int keybinds_camera_action_for_scancode(SDL_Scancode code) { (void)code; return -1; }
int controller_instance_is_player(SDL_JoystickID id, int player) { (void)id; (void)player; return 0; }
static int render_calls, shadow_enabled;
int nes_voxel_render(const NesVoxelScene *scene) {
    render_calls++;
    shadow_enabled = scene->sprite_shadow != NULL;
    return 1;
}

static int grid(void *user) { (void)user; return 3; }
static float flat_height(const NesVoxelScreenSample *sample, void *user) {
    (void)sample; (void)user; return 0;
}
static NesVoxelScreenProfile positional = {
    "positional", 0, 240, 0, 0, 0,
    35, 0, 0, 100, 100, 0, 0, NULL, NULL, NULL, NULL, NULL,
    NES_VOXEL_LAYOUT_SIDE, 4, grid, 16, NULL, NULL
};

int main(void) {
    assert(positional.terrain_layout == NES_VOXEL_LAYOUT_SIDE);
    assert(positional.side_group_tiles == 4 && positional.grid_offset_x == grid);
    assert(positional.sprite_group_max_width == 16);
    assert(!positional.suppress_sprite_shadows && !positional.keep_sprite_pixels);
    static uint32_t source[256 * 240];
    static NesVoxelScreenState state;
    uint32_t pixels[64];
    for (int i = 0; i < 256 * 240; ++i) source[i] = 0xff339955;
    g_nes_palette[0] = 0xff112233;
    state.active_profile = &positional;
    state.flat_framebuffer = source;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) state.sprite_mask[y * 256 + x] = 1;
    screen_tile_pixels(pixels, 8, 0, 0, 0, &state);
    for (int i = 0; i < 64; ++i) assert(pixels[i] == 0xff112233);
    positional.keep_sprite_pixels = 1;
    screen_tile_pixels(pixels, 8, 0, 0, 0, &state);
    for (int i = 0; i < 64; ++i) assert(pixels[i] == 0xff339955);
    positional.keep_sprite_pixels = 0;
    state.sprite_mask[0] = 0;
    screen_tile_pixels(pixels, 8, 0, 0, 0, &state);
    for (int i = 0; i < 64; ++i) assert(pixels[i] == 0xff339955);
    state.enabled = state.view_enabled = 1;
    positional.height = flat_height;
    memset(g_ppu_oam, 0xff, sizeof(g_ppu_oam));
    nes_voxel_screen_post_render(&state, &positional, source);
    assert(render_calls == 1 && shadow_enabled);
    positional.suppress_sprite_shadows = 1;
    nes_voxel_screen_post_render(&state, &positional, source);
    assert(render_calls == 2 && !shadow_enabled);
    puts("voxel profile compatibility and pixel policy passed");
    return 0;
}
