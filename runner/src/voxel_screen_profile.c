#include "voxel_screen_profile.h"

#include "nes_runtime.h"
#include "voxel_renderer.h"

#include <stdio.h>
#include <string.h>

#define SCREEN_WIDTH 256
#define SCREEN_HEIGHT 240
#define TILE_SIZE 8

static int clamp_int(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static float ease_value(float current, float target) {
    float delta = target - current;
    if (delta > -0.05f && delta < 0.05f) return target;
    return current + delta * 0.25f;
}

static void build_sprite_mask(NesVoxelScreenState *state) {
    int sprite_height = (g_ppuctrl & 0x20) ? 16 : 8;
    memset(state->sprite_mask, 0, sizeof(state->sprite_mask));
    for (int i = 0; i < 64; i++) {
        int x = g_ppu_oam[i * 4 + 3];
        int y = g_ppu_oam[i * 4] + 1;
        if (g_ppu_oam[i * 4] >= 0xEF) continue;
        for (int py = 0; py < sprite_height; py++) {
            int sy = y + py;
            if (sy < 0 || sy >= SCREEN_HEIGHT) continue;
            for (int px = 0; px < 8; px++) {
                int sx = x + px;
                if (sx >= 0 && sx < SCREEN_WIDTH)
                    state->sprite_mask[sy * SCREEN_WIDTH + sx] = 1;
            }
        }
    }
}

static void sample_grid(NesVoxelScreenState *state,
                        const NesVoxelScreenProfile *profile,
                        const uint32_t *framebuffer) {
    uint32_t background = g_nes_palette[g_ppu_pal[0] & 0x3F];
    int rows = profile->source_height / TILE_SIZE;
    int source_x = g_widescreen_left;

    build_sprite_mask(state);
    for (int ty = 0; ty < rows; ty++) {
        for (int tx = 0; tx < 32; tx++) {
            NesVoxelScreenSample sample;
            int index = ty * 32 + tx;
            memset(&sample, 0, sizeof(sample));
            sample.tile_x = tx;
            sample.tile_y = ty;
            sample.screen_y = profile->source_y + ty * TILE_SIZE;
            sample.background = background;
            state->tiles[index] = (uint8_t)index;

            for (int py = 0; py < TILE_SIZE; py++) {
                int sy = sample.screen_y + py;
                for (int px = 0; px < TILE_SIZE; px++) {
                    int sx = state->grid_offset_x +
                             tx * TILE_SIZE + px;
                    uint32_t color;
                    unsigned r, g, b, sum;
                    if (sx < 0 || sx >= SCREEN_WIDTH) continue;
                    if (state->sprite_mask[sy * SCREEN_WIDTH + sx])
                        continue;
                    color = framebuffer[sy * g_render_width + source_x + sx];
                    sample.usable_pixels++;
                    if (color == background) continue;
                    sample.non_background_pixels++;
                    r = (color >> 16) & 0xFFu;
                    g = (color >> 8) & 0xFFu;
                    b = color & 0xFFu;
                    sum = r + g + b;
                    if (sum < 150u) sample.dark_pixels++;
                    if (sum > 570u) sample.bright_pixels++;
                    if (r > g + 18u && g > b + 8u)
                        sample.warm_pixels++;
                    if (g > r + 16u && g > b + 16u)
                        sample.green_pixels++;
                    if (b > r + 16u && b > g + 8u)
                        sample.blue_pixels++;
                }
            }
            state->heights[index] =
                ty < profile->blank_source_rows || !profile->height
                    ? 0.0f
                    : profile->height(&sample, profile->user);
        }
    }
}

static float screen_tile_height(uint8_t tile, int x, int y, void *user) {
    NesVoxelScreenState *state = (NesVoxelScreenState *)user;
    (void)tile;
    return state->heights[y * 32 + x];
}

static uint32_t common_unmasked_color(const NesVoxelScreenState *state,
                                      const NesVoxelScreenProfile *profile,
                                      int tile_x, int tile_y) {
    uint32_t colors[64];
    uint8_t counts[64];
    int unique = 0;
    int common = -1;
    int source_x = g_widescreen_left;
    int base_y = profile->source_y + tile_y * TILE_SIZE;
    memset(counts, 0, sizeof(counts));
    for (int py = 0; py < TILE_SIZE; py++) {
        for (int px = 0; px < TILE_SIZE; px++) {
            int sx = state->grid_offset_x +
                     tile_x * TILE_SIZE + px;
            int sy = base_y + py;
            uint32_t color;
            int found = -1;
            if (sx < 0 || sx >= SCREEN_WIDTH) continue;
            if (state->sprite_mask[sy * SCREEN_WIDTH + sx]) continue;
            color = state->flat_framebuffer[
                sy * g_render_width + source_x + sx];
            for (int i = 0; i < unique; i++) {
                if (colors[i] == color) {
                    found = i;
                    break;
                }
            }
            if (found < 0) {
                found = unique++;
                colors[found] = color;
            }
            counts[found]++;
            if (common < 0 || counts[found] > counts[common])
                common = found;
        }
    }
    return common >= 0
        ? colors[common]
        : g_nes_palette[g_ppu_pal[0] & 0x3F];
}

static void screen_tile_pixels(uint32_t *pixels, int stride, uint8_t tile,
                               int tile_x, int tile_y, void *user) {
    NesVoxelScreenState *state = (NesVoxelScreenState *)user;
    const NesVoxelScreenProfile *profile = state->active_profile;
    uint32_t replacement =
        common_unmasked_color(state, profile, tile_x, tile_y);
    int source_x = g_widescreen_left;
    int base_y = profile->source_y + tile_y * TILE_SIZE;
    (void)tile;
    for (int py = 0; py < TILE_SIZE; py++) {
        for (int px = 0; px < TILE_SIZE; px++) {
            int sx = state->grid_offset_x +
                     tile_x * TILE_SIZE + px;
            int sy = base_y + py;
            if (sx < 0 || sx >= SCREEN_WIDTH ||
                tile_y < profile->blank_source_rows ||
                state->sprite_mask[sy * SCREEN_WIDTH + sx]) {
                pixels[py * stride + px] = replacement;
            } else {
                pixels[py * stride + px] = state->flat_framebuffer[
                    sy * g_render_width + source_x + sx];
            }
        }
    }
}

static float screen_sprite_shadow(int min_x, int min_y,
                                  int max_x, int max_y, void *user) {
    (void)user;
    return max_x - min_x >= 8 && max_y - min_y >= 12 ? 1.0f : 0.0f;
}

void nes_voxel_screen_set_enabled(NesVoxelScreenState *state, int enabled) {
    if (!state) return;
    state->enabled = enabled != 0;
    if (state->enabled && !state->view_enabled)
        state->view_enabled = 1;
}

void nes_voxel_screen_configure(NesVoxelScreenState *state,
                                int pitch, int yaw, int roll,
                                int zoom_percent,
                                int sprite_scale_percent) {
    if (!state) return;
    state->pitch = state->default_pitch = clamp_int(pitch, 5, 85);
    state->yaw = state->default_yaw = clamp_int(yaw, -180, 180);
    state->roll = state->default_roll = clamp_int(roll, -45, 45);
    state->zoom_percent = state->default_zoom_percent =
        clamp_int(zoom_percent, 50, 200);
    state->sprite_scale_percent = state->default_sprite_scale_percent =
        clamp_int(sprite_scale_percent, 75, 250);
    state->render_pitch = (float)state->pitch;
    state->render_yaw = (float)state->yaw;
    state->render_roll = (float)state->roll;
    state->render_zoom_percent = (float)state->zoom_percent;
    state->render_sprite_scale_percent =
        (float)state->sprite_scale_percent;
    state->configured = 1;
}

void nes_voxel_screen_init(NesVoxelScreenState *state,
                           const NesVoxelScreenProfile *profile) {
    if (!state || !profile) return;
    if (!state->configured) {
        nes_voxel_screen_configure(
            state, profile->default_pitch, profile->default_yaw,
            profile->default_roll, profile->default_zoom_percent,
            profile->default_sprite_scale_percent);
    }
    if (!state->enabled) return;
    g_widescreen_left = profile->output_margin;
    g_widescreen_right = profile->output_margin;
    g_render_width = SCREEN_WIDTH + profile->output_margin * 2;
    g_ws_eff_left = 0;
    g_ws_eff_right = 0;
    printf("[Voxel] %s enabled: pitch=%d yaw=%d roll=%d zoom=%d%% "
           "sprites=%d%%\n",
           profile->name ? profile->name : "screen diorama",
           state->pitch, state->yaw, state->roll, state->zoom_percent,
           state->sprite_scale_percent);
}

void nes_voxel_screen_handle_event(NesVoxelScreenState *state,
                                   const SDL_Event *event) {
    int changed = 0;
    SDL_Scancode key;
    if (!state || !state->enabled || !event ||
        event->type != SDL_KEYDOWN)
        return;
    key = event->key.keysym.scancode;
    if (event->key.repeat &&
        (key == SDL_SCANCODE_KP_0 || key == SDL_SCANCODE_KP_5))
        return;
    switch (key) {
        case SDL_SCANCODE_KP_0:
            state->view_enabled = !state->view_enabled; changed = 1; break;
        case SDL_SCANCODE_KP_8:
            state->pitch = clamp_int(state->pitch + 5, 5, 85);
            changed = 1; break;
        case SDL_SCANCODE_KP_2:
            state->pitch = clamp_int(state->pitch - 5, 5, 85);
            changed = 1; break;
        case SDL_SCANCODE_KP_4:
            state->yaw = clamp_int(state->yaw - 5, -180, 180);
            changed = 1; break;
        case SDL_SCANCODE_KP_6:
            state->yaw = clamp_int(state->yaw + 5, -180, 180);
            changed = 1; break;
        case SDL_SCANCODE_KP_7:
            state->roll = clamp_int(state->roll - 5, -45, 45);
            changed = 1; break;
        case SDL_SCANCODE_KP_9:
            state->roll = clamp_int(state->roll + 5, -45, 45);
            changed = 1; break;
        case SDL_SCANCODE_KP_PLUS:
            state->zoom_percent =
                clamp_int(state->zoom_percent + 5, 50, 200);
            changed = 1; break;
        case SDL_SCANCODE_KP_MINUS:
            state->zoom_percent =
                clamp_int(state->zoom_percent - 5, 50, 200);
            changed = 1; break;
        case SDL_SCANCODE_KP_1:
            state->sprite_scale_percent =
                clamp_int(state->sprite_scale_percent - 10, 75, 250);
            changed = 1; break;
        case SDL_SCANCODE_KP_3:
            state->sprite_scale_percent =
                clamp_int(state->sprite_scale_percent + 10, 75, 250);
            changed = 1; break;
        case SDL_SCANCODE_KP_5:
            state->pitch = state->default_pitch;
            state->yaw = state->default_yaw;
            state->roll = state->default_roll;
            state->zoom_percent = state->default_zoom_percent;
            state->sprite_scale_percent =
                state->default_sprite_scale_percent;
            state->view_enabled = 1;
            changed = 1;
            break;
        default:
            break;
    }
    if (changed) {
        printf("[Voxel] %s pitch=%d yaw=%d roll=%d zoom=%d%% "
               "sprites=%d%%\n",
               state->view_enabled ? "on" : "off",
               state->pitch, state->yaw, state->roll,
               state->zoom_percent, state->sprite_scale_percent);
    }
}

void nes_voxel_screen_update(NesVoxelScreenState *state,
                             const NesVoxelScreenProfile *profile) {
    if (!state || !profile || !state->enabled) return;
    state->render_pitch =
        ease_value(state->render_pitch, (float)state->pitch);
    state->render_yaw =
        ease_value(state->render_yaw, (float)state->yaw);
    state->render_roll =
        ease_value(state->render_roll, (float)state->roll);
    state->render_zoom_percent =
        ease_value(state->render_zoom_percent,
                   (float)state->zoom_percent);
    state->render_sprite_scale_percent =
        ease_value(state->render_sprite_scale_percent,
                   (float)state->sprite_scale_percent);
    /* The native 256px image stays centered and pillarboxed. The compositor
     * owns the margins only while it is actually rendering a scene. */
    g_ws_eff_left = 0;
    g_ws_eff_right = 0;
}

void nes_voxel_screen_post_render(NesVoxelScreenState *state,
                                  const NesVoxelScreenProfile *profile,
                                  uint32_t *framebuffer) {
    NesVoxelScene scene;
    NesVoxelScreenCamera camera;
    int rows;
    if (!state || !profile || !framebuffer ||
        !state->enabled || !state->view_enabled)
        return;
    if (profile->source_y < 0 || profile->source_height <= 0 ||
        profile->source_y + profile->source_height > SCREEN_HEIGHT ||
        profile->source_height % TILE_SIZE != 0)
        return;
    if (profile->visible &&
        !profile->visible(framebuffer, g_render_width, profile->user))
        return;
    rows = profile->source_height / TILE_SIZE;
    if (rows * 32 > NES_VOXEL_SCREEN_MAX_TILES) return;

    state->flat_framebuffer = framebuffer;
    state->active_profile = profile;
    state->grid_offset_x =
        profile->grid_offset_x ? profile->grid_offset_x(profile->user) : 0;
    state->grid_offset_x =
        clamp_int(state->grid_offset_x, -31, 31);
    sample_grid(state, profile, framebuffer);
    memset(&scene, 0, sizeof(scene));
    memset(&camera, 0, sizeof(camera));
    scene.framebuffer = framebuffer;
    scene.output_width = g_render_width;
    scene.output_height = SCREEN_HEIGHT;
    scene.source_x = g_widescreen_left;
    scene.source_y = profile->source_y;
    scene.source_width = SCREEN_WIDTH;
    scene.source_height = profile->source_height;
    scene.tiles = state->tiles;
    scene.tile_columns = 32;
    scene.tile_rows = rows;
    scene.tile_stride = 32;
    scene.tile_size = TILE_SIZE;
    scene.tile_height = screen_tile_height;
    scene.terrain_layout = profile->terrain_layout;
    scene.side_group_tiles = profile->side_group_tiles;
    scene.terrain_offset_x = (float)state->grid_offset_x;
    scene.tile_pixels = screen_tile_pixels;
    scene.user = state;
    scene.elevation_degrees = state->render_pitch;
    scene.yaw_degrees = state->render_yaw;
    scene.roll_degrees = state->render_roll;
    scene.camera_distance =
        300.0f * 100.0f / state->render_zoom_percent;
    if (profile->camera) {
        profile->camera(&camera, state->render_pitch, state->render_yaw,
                        state->render_roll, state->render_zoom_percent,
                        profile->user);
        if (camera.enabled) {
            scene.use_camera_pose = 1;
            scene.camera_eye_x = camera.eye_x;
            scene.camera_eye_y = camera.eye_y;
            scene.camera_eye_z = camera.eye_z;
            scene.camera_look_at_x = camera.look_at_x;
            scene.camera_look_at_y = camera.look_at_y;
            scene.camera_look_at_z = camera.look_at_z;
            scene.camera_focal_scale = camera.focal_scale;
            scene.camera_center_y = camera.center_y;
        }
    }
    scene.sprite_scale =
        state->render_sprite_scale_percent / 100.0f;
    scene.sprite_face_camera_pitch = 1;
    scene.sprite_constant_screen_size =
        profile->terrain_layout == NES_VOXEL_LAYOUT_SIDE ? 0 : 1;
    scene.clip_sprites_to_source = 1;
    scene.sprite_depth_bias = 1.0f;
    scene.sprite_shadow = screen_sprite_shadow;
    scene.sprite_visible = profile->sprite_visible;
    scene.sprite_shadow_scale = 0.60f;
    scene.sprite_shadow_opacity = 0.32f;
    scene.draw_oam_sprites = 1;
    scene.preserve_top_rows = profile->preserve_top_rows;
    scene.extend_preserved_rows = profile->preserve_top_rows > 0;
    scene.preserved_rows_fill =
        g_nes_palette[g_ppu_pal[0] & 0x3F];
    scene.sky_top = profile->sky_top;
    scene.sky_bottom = profile->sky_bottom;
    nes_voxel_render(&scene);
    state->flat_framebuffer = NULL;
    state->active_profile = NULL;
}
