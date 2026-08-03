/*
 * voxel_renderer.h -- optional presentation-only 2.5D/3D scene compositor
 *
 * Games provide a tile grid and a height callback.  The renderer projects the
 * existing NES pixels onto tile prisms and can turn current OAM sprites into
 * camera-facing cards.  It does not mutate emulated state.
 */
#pragma once

#include <stdint.h>

typedef float (*NesVoxelTileHeightFn)(uint8_t tile, int tile_x, int tile_y,
                                      void *user);
typedef int (*NesVoxelSpriteOverlayFn)(int min_x, int min_y,
                                       int max_x, int max_y, void *user);

typedef struct NesVoxelScene {
    uint32_t *framebuffer;
    int output_width;
    int output_height;

    /* Rectangle in the flat framebuffer represented by the tile grid. */
    int source_x;
    int source_y;
    int source_width;
    int source_height;

    const uint8_t *tiles;
    int tile_columns;
    int tile_rows;
    int tile_stride;
    int column_major;
    int tile_size;

    NesVoxelTileHeightFn tile_height;
    void *user;

    /* Camera controls. Yaw orbits around the vertical axis; roll rotates the
     * camera plane. distance <= 0 uses the renderer default. */
    float elevation_degrees;
    float yaw_degrees;
    float roll_degrees;
    float camera_distance;

    /* OAM metasprites are assembled into coherent camera-facing cards.
     * Values <= 0 use 1.0. */
    float sprite_scale;
    /* Pitch-facing cards preserve pixel-art proportions under a high camera
     * instead of vertically foreshortening. depth_bias pulls cards slightly
     * camera-ward to keep feet from z-fighting with their ground tile. */
    int sprite_face_camera_pitch;
    float sprite_depth_bias;
    /* Optional game policy for actors that must remain readable when a low
     * camera puts foreground terrain between the actor and the camera. */
    NesVoxelSpriteOverlayFn sprite_overlay;
    int draw_oam_sprites;
    int preserve_top_rows;
    int extend_preserved_rows;
    uint32_t preserved_rows_fill;

    uint32_t sky_top;
    uint32_t sky_bottom;
} NesVoxelScene;

/* Returns 1 when a scene was rendered, 0 when the descriptor was invalid. */
int nes_voxel_render(const NesVoxelScene *scene);
