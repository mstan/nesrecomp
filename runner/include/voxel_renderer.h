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
typedef void (*NesVoxelTilePixelsFn)(uint32_t *pixels, int pixel_stride,
                                     uint8_t tile, int tile_x, int tile_y,
                                     void *user);
typedef int (*NesVoxelTileBillboardFn)(uint8_t tile, int tile_x, int tile_y,
                                      int *tile_columns, int *tile_rows,
                                      void *user);
typedef int (*NesVoxelSpriteOverlayFn)(int min_x, int min_y,
                                       int max_x, int max_y, void *user);
typedef int (*NesVoxelSpriteVisibleFn)(int min_x, int min_y,
                                       int max_x, int max_y, void *user);
typedef float (*NesVoxelSpriteGroundFn)(int min_x, int min_y,
                                        int max_x, int max_y,
                                        float sampled_ground, void *user);
typedef float (*NesVoxelSpriteShadowFn)(int min_x, int min_y,
                                        int max_x, int max_y, void *user);
typedef int (*NesVoxelSpriteMaxHeightFn)(const int *oam_indices,
                                         int oam_count, void *user);

enum {
    NES_VOXEL_LAYOUT_FLOOR = 0,
    /* Rotate the sampled screen plane upright: screen X remains world X,
     * screen Y becomes world height, and positive tile height becomes depth.
     * This is useful for player-forward views of side-scrolling games. */
    NES_VOXEL_LAYOUT_SIDE = 1
};

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
    int terrain_layout;
    /* Upright layouts can group adjacent source cells into one block. A
     * value of 2 turns four 8x8 SMB cells into one 16x16x16 metatile cube. */
    int side_group_tiles;
    float terrain_offset_x;
    /* Optional source for generated or offscreen room textures. When absent,
     * the compositor samples the already-rendered flat framebuffer. */
    NesVoxelTilePixelsFn tile_pixels;
    /* Optional background decoration policy. Return 1 at a grouped
     * decoration's top-left tile, -1 for its remaining member tiles, and 0
     * for ordinary terrain. The renderer replaces member footprints with
     * nearby ground and assembles the original tiles into one alpha-tested,
     * camera-facing card. */
    NesVoxelTileBillboardFn tile_billboard;
    float tile_billboard_scale;
    /* Optional contact shadow beneath every generated tile billboard.
     * Opacity <= 0 disables it. */
    float tile_billboard_shadow_scale;
    float tile_billboard_shadow_opacity;
    void *user;

    /* Camera controls. Yaw orbits around the vertical axis; roll rotates the
     * camera plane. distance <= 0 uses the renderer default. */
    float elevation_degrees;
    float yaw_degrees;
    float roll_degrees;
    float camera_distance;
    /* Optional world-space camera target. This lets a game present adjacent
     * cached rooms during its native scrolling transition. */
    int use_camera_target;
    float camera_target_x;
    float camera_target_z;
    /* Optional explicit eye/look-at pose. This is intended for first-person
     * or rail-camera profiles; the ordinary orbit camera remains the default.
     * focal_scale is relative to output width and center_y is normalized. */
    int use_camera_pose;
    float camera_eye_x;
    float camera_eye_y;
    float camera_eye_z;
    float camera_look_at_x;
    float camera_look_at_y;
    float camera_look_at_z;
    float camera_focal_scale;
    float camera_center_y;

    /* OAM metasprites are assembled into coherent camera-facing cards.
     * Values <= 0 use 1.0. */
    float sprite_scale;
    /* Maximum assembled metasprite width in source pixels. Values <= 0 keep
     * the renderer's historical 32-pixel limit. */
    int sprite_group_max_width;
    /* Pitch-facing cards preserve pixel-art proportions under a high camera
     * instead of vertically foreshortening. depth_bias pulls cards slightly
     * camera-ward to keep feet from z-fighting with their ground tile. */
    int sprite_face_camera_pitch;
    /* Preserve the configured sprite dimensions in output pixels while the
     * card's foot remains perspective-anchored to the world. */
    int sprite_constant_screen_size;
    /* Clip reconstructed OAM cards to the native rectangle represented by
     * this scene. This prevents pieces hidden beyond a room boundary from
     * reappearing as a full or elongated 3D card. */
    int clip_sprites_to_source;
    float sprite_depth_bias;
    float sprite_world_offset_x;
    float sprite_world_offset_z;
    /* Optional game policy for actor ground placement. */
    NesVoxelSpriteGroundFn sprite_ground;
    /* Optional cap for assembled cards such as a player sprite whose
     * transition-only OAM pieces must not form one elongated metasprite. */
    NesVoxelSpriteMaxHeightFn sprite_max_height;
    /* Optional component filter, evaluated after OAM pieces are assembled
     * into one metasprite. A first-person profile can use this to suppress
     * the player body while retaining enemies and effects. */
    NesVoxelSpriteVisibleFn sprite_visible;
    /* Optional game policy for pixel-art contact shadows. The callback returns
     * an opacity multiplier; zero suppresses the shadow for flat effects and
     * other OAM pieces that are not standing actors. */
    NesVoxelSpriteShadowFn sprite_shadow;
    float sprite_shadow_scale;
    float sprite_shadow_opacity;
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

/*
 * ---------------------------------------------------------------------
 * Immediate-mode mesh API
 *
 * A thin wrapper around the same projection/rasterization used above
 * (project_vertex/draw_triangle), for callers that want to place an
 * arbitrary textured mesh in the scene instead of a tile grid and OAM
 * cards. Intended for a game_post_render() that draws a hand-authored
 * model -- e.g. a presentation-only 3D actor standing in for a 2D sprite
 * -- with no engine knowledge of what the mesh represents.
 *
 * Coordinate convention (independent of NesVoxelScene's tile grid, since
 * a mesh-only caller has no tile grid at all): the caller defines what
 * world X/Y/Z mean. The one fixed rule is right-handed with +Y up, matching
 * every Vec3 already used by NesVoxelScene's terrain/sprite code. A caller
 * placing a mesh at a screen position typically chooses world X = screen
 * column, world Y = height in pixels above a caller-chosen ground
 * reference (positive up), world Z = depth around that same reference
 * (0 at the object, camera at POSITIVE Z looking toward negative Z) --
 * this is a convention of the caller's camera and vertex data, not
 * something this API enforces.
 *
 * Handedness trap: the basis is right = cross(forward, world-up), so a
 * camera looking along +Z gets right = (-1,0,0) and the whole scene
 * renders mirrored about the view center (measured, not theoretical).
 * Overlay-style cameras that want screen-aligned world X must sit at
 * +Z and look toward -Z, as above.
 *
 * One session per call to nes_voxel_mesh_begin(); call it again (or call
 * nes_voxel_render() again) to start a new session. The depth buffer is
 * cleared at the start of the session, independent of any nes_voxel_render()
 * scene rendered earlier in the same game_post_render() -- meshes do not
 * currently depth-test against a tile/sprite scene from the same frame.
 * A combined terrain-plus-mesh session sharing one depth pass is future
 * work, not needed by any caller today.
 */
typedef struct NesVoxelCamera {
    float eye_x, eye_y, eye_z;
    float look_at_x, look_at_y, look_at_z;
    /* Relative to output width, as NesVoxelScene::camera_focal_scale.
     * <= 0.05 uses the same 0.92 default. */
    float focal_scale;
    /* Normalized, as NesVoxelScene::camera_center_y. Outside (0.05, 0.95)
     * uses the same 0.59 default. */
    float center_y;
} NesVoxelCamera;

typedef struct NesVoxelMeshVertex {
    float x, y, z;  /* world space; see the coordinate convention above */
    float u, v;     /* texel coordinates into the currently bound texture */
} NesVoxelMeshVertex;

/* Begin a mesh session into framebuffer (output_width x output_height,
 * ARGB8888), clears the shared depth buffer, and builds the camera basis
 * from an explicit eye/look-at pose (no orbit/yaw/elevation parameters --
 * a caller wanting an orbiting camera computes eye_x/y/z itself).
 * Returns 0 (does nothing else) when framebuffer/camera is NULL or the
 * dimensions exceed the renderer's fixed-size buffers. */
int nes_voxel_mesh_begin(uint32_t *framebuffer, int output_width,
                         int output_height, const NesVoxelCamera *camera);

/* Bind a flat ARGB8888 texture for triangles submitted after this call.
 * shade multiplies RGB (as Texture::shade); alpha_test nonzero skips fully
 * transparent texels (as OAM sprite cards do). No-op outside a session. */
void nes_voxel_mesh_bind_texture(const uint32_t *pixels, int width,
                                 int height, int stride, float shade,
                                 int alpha_test);

/* Bind one immediate mesh card with bilinear ARGB sampling. Existing callers
 * retain nearest sampling; use this only for source effects whose RDP path
 * explicitly enables BILERP. */
void nes_voxel_mesh_bind_texture_bilinear(const uint32_t *pixels, int width,
                                          int height, int stride, float shade,
                                          int alpha_test);

/* Bilinear translucent overlay with no depth test/write. Alpha comparison is
 * applied after filtering, matching texture-rectangle XLU particles. */
void nes_voxel_mesh_bind_texture_bilinear_overlay(
    const uint32_t *pixels, int width, int height, int stride, float shade,
    int alpha_cutoff);

/* Apply a straight-alpha RGB overlay to subsequently bound immediate mesh
 * textures. rgba==0 disables it. This is for material-color animation, not
 * a replacement particle system. */
void nes_voxel_mesh_set_color_overlay(uint32_t rgba);

/* Submit one triangle in the currently bound texture. No-op outside a
 * session or before any texture is bound. */
void nes_voxel_mesh_triangle(NesVoxelMeshVertex a, NesVoxelMeshVertex b,
                             NesVoxelMeshVertex c);

/* End the session. Draw calls are immediate (already landed in the
 * framebuffer), so this only clears the active flag; reserved so a future
 * batched implementation has a defined flush point without an ABI break. */
void nes_voxel_mesh_end(void);
