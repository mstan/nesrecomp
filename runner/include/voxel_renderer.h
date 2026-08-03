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

    /* Camera elevation in degrees.  A small fixed yaw exposes side faces. */
    float elevation_degrees;
    int draw_oam_sprites;
    int preserve_top_rows;
    int extend_preserved_rows;
    uint32_t preserved_rows_fill;

    uint32_t sky_top;
    uint32_t sky_bottom;
} NesVoxelScene;

/* Returns 1 when a scene was rendered, 0 when the descriptor was invalid. */
int nes_voxel_render(const NesVoxelScene *scene);
