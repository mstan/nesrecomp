/*
 * voxel_screen_profile.h -- reusable screen-grid adapter for the optional
 * voxel renderer.
 *
 * Side-scrolling games often do not retain a convenient world-tile array.
 * This adapter samples the final 8x8 screen grid, masks reconstructed OAM,
 * and delegates the semantic "flat decoration or solid geometry?" decision
 * to a small game-local callback.
 */
#pragma once

#include <stdint.h>
#include <SDL.h>
#include "voxel_renderer.h"

#define NES_VOXEL_SCREEN_MAX_TILES (32 * 30)

typedef struct NesVoxelScreenSample {
    int tile_x;
    int tile_y;
    int screen_y;
    int usable_pixels;
    int non_background_pixels;
    int dark_pixels;
    int bright_pixels;
    int warm_pixels;
    int green_pixels;
    int blue_pixels;
    uint32_t background;
} NesVoxelScreenSample;

typedef int (*NesVoxelScreenVisibleFn)(const uint32_t *framebuffer,
                                       int stride, void *user);
typedef float (*NesVoxelScreenHeightFn)(
    const NesVoxelScreenSample *sample, void *user);
typedef struct NesVoxelScreenCamera {
    int enabled;
    float eye_x;
    float eye_y;
    float eye_z;
    float look_at_x;
    float look_at_y;
    float look_at_z;
    float focal_scale;
    float center_y;
} NesVoxelScreenCamera;
typedef void (*NesVoxelScreenCameraFn)(
    NesVoxelScreenCamera *camera, float pitch, float yaw, float roll,
    float zoom_percent, void *user);
typedef int (*NesVoxelScreenSpriteVisibleFn)(
    int min_x, int min_y, int max_x, int max_y, void *user);

typedef struct NesVoxelScreenProfile {
    const char *name;
    int source_y;
    int source_height;
    int preserve_top_rows;
    int blank_source_rows;
    int output_margin;

    int default_pitch;
    int default_yaw;
    int default_roll;
    int default_zoom_percent;
    int default_sprite_scale_percent;

    uint32_t sky_top;
    uint32_t sky_bottom;
    NesVoxelScreenVisibleFn visible;
    NesVoxelScreenHeightFn height;
    void *user;
    /* Optional dynamic camera and OAM-component filter. With both NULL the
     * adapter follows its existing orbit-camera behavior exactly. */
    NesVoxelScreenCameraFn camera;
    NesVoxelScreenSpriteVisibleFn sprite_visible;
    int terrain_layout;
    int side_group_tiles;
} NesVoxelScreenProfile;

typedef struct NesVoxelScreenState {
    uint8_t tiles[NES_VOXEL_SCREEN_MAX_TILES];
    float heights[NES_VOXEL_SCREEN_MAX_TILES];
    uint8_t sprite_mask[256 * 240];
    const uint32_t *flat_framebuffer;
    const NesVoxelScreenProfile *active_profile;

    int enabled;
    int view_enabled;
    int configured;
    int pitch;
    int yaw;
    int roll;
    int zoom_percent;
    int sprite_scale_percent;
    int default_pitch;
    int default_yaw;
    int default_roll;
    int default_zoom_percent;
    int default_sprite_scale_percent;
    float render_pitch;
    float render_yaw;
    float render_roll;
    float render_zoom_percent;
    float render_sprite_scale_percent;
} NesVoxelScreenState;

void nes_voxel_screen_set_enabled(NesVoxelScreenState *state, int enabled);
void nes_voxel_screen_configure(NesVoxelScreenState *state,
                                int pitch, int yaw, int roll,
                                int zoom_percent,
                                int sprite_scale_percent);
void nes_voxel_screen_init(NesVoxelScreenState *state,
                           const NesVoxelScreenProfile *profile);
void nes_voxel_screen_handle_event(NesVoxelScreenState *state,
                                   const SDL_Event *event);
void nes_voxel_screen_update(NesVoxelScreenState *state,
                             const NesVoxelScreenProfile *profile);
void nes_voxel_screen_post_render(NesVoxelScreenState *state,
                                  const NesVoxelScreenProfile *profile,
                                  uint32_t *framebuffer);
