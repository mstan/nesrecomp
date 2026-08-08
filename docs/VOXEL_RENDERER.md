# Voxel / Diorama Renderer

NESRecomp includes an optional presentation-only software 3D compositor. A
game can call `nes_voxel_render()` from `game_post_render()` to project a live
NES tile grid as textured prisms and current OAM sprites as camera-facing
cards.

The compositor is deliberately separate from emulation:

- it reads the final ARGB8888 framebuffer, tile IDs, CHR, palette, and OAM;
- it writes only the presentation framebuffer;
- it does not change CPU, PPU, mapper, save-state, or game timing state;
- a game that never calls it has unchanged rendering behavior.

## Game profile

Include `voxel_renderer.h`, fill a `NesVoxelScene`, and provide a height
callback:

```c
static float tile_height(uint8_t tile, int x, int y, void *user) {
    (void)x; (void)y; (void)user;
    return tile_is_wall(tile) ? 16.0f : 0.0f;
}

void game_post_render(uint32_t *framebuffer) {
    NesVoxelScene scene = {
        .framebuffer = framebuffer,
        .output_width = g_render_width,
        .output_height = 240,
        .source_x = g_widescreen_left,
        .source_y = 64,
        .source_width = 256,
        .source_height = 176,
        .tiles = game_tile_grid,
        .tile_columns = 32,
        .tile_rows = 22,
        .tile_stride = 22,
        .column_major = 1,
        .tile_size = 8,
        .tile_height = tile_height,
        .elevation_degrees = 35.0f,
        .draw_oam_sprites = 1,
        .preserve_top_rows = 64,
        .extend_preserved_rows = 1,
        .preserved_rows_fill = 0xFF000000u,
    };
    nes_voxel_render(&scene);
}
```

Positive height raises a tile, zero is ground, and negative height makes a
recess. Adjacent height differences receive shaded vertical faces. Tile tops
sample the game's live framebuffer, while sprite cards are decoded from the
live CHR/OAM/palette state. A per-pixel depth buffer handles terrain and sprite
occlusion.

`extend_preserved_rows` fills the full output width behind a preserved HUD
before copying the original centered pixels. This lets widescreen game
profiles extend a solid status-bar field without stretching its pixel art.

## Immediate-mode mesh API

For a caller that wants to place an arbitrary textured mesh instead of (or
alongside, in separate sessions -- see below) a tile grid, `voxel_renderer.h`
also exposes the same projection/rasterization as a small immediate-mode API:

```c
static const NesVoxelMeshVertex cube[] = { /* ... 8 verts, u/v per face ... */ };

void game_post_render(uint32_t *framebuffer) {
    NesVoxelCamera camera = {
        .eye_x = 40.0f, .eye_y = 60.0f, .eye_z = -160.0f,
        .look_at_x = 0.0f, .look_at_y = 12.0f, .look_at_z = 0.0f,
        .focal_scale = 0.6f, .center_y = 0.5f,
    };
    if (!nes_voxel_mesh_begin(framebuffer, g_render_width, 240, &camera))
        return;
    nes_voxel_mesh_bind_texture(face_pixels, 8, 8, 8, 1.0f, 0);
    nes_voxel_mesh_triangle(v0, v1, v2);
    nes_voxel_mesh_triangle(v0, v2, v3);
    nes_voxel_mesh_end();
}
```

There is no tile grid or OAM here, so world space means whatever the caller's
vertex data and camera pose agree on -- the one fixed rule is right-handed
with +Y up. A caller replacing a screen-space sprite typically picks world X
= screen column and places its object's ground plane at world Y = 0, with
depth (world Z) centered on 0 purely to give the mesh volume.

`nes_voxel_mesh_begin()` clears the shared depth buffer for its own session;
it does not depth-test against a `nes_voxel_render()` scene from earlier in
the same `game_post_render()` call. Combining a tile/sprite scene and a mesh
in one depth pass is not implemented -- no current caller needs it.

The renderer chooses an uncontaminated occurrence of a repeated tile when an
OAM sprite overlaps its source pixels. This avoids stamping the original flat
sprite into the terrain texture before drawing the upright sprite card.

## Screen-grid adapter

Side-scrolling games often stream nametables without retaining a convenient
world-tile array. `voxel_screen_profile.h` provides an optional adapter for
those games. It divides a selected part of the final native framebuffer into
8x8 cells, masks pixels covered by live OAM, and reports color/material
statistics through `NesVoxelScreenSample`.

The game supplies two semantic callbacks:

- `visible` keeps title, menu, dialogue, and other authored 2D screens native.
- `height` decides whether a sampled cell is flat decoration, shallow relief,
  or solid collision/architecture geometry.

The adapter owns the repeated presentation mechanics: widescreen allocation,
camera easing and numpad controls, OAM metasprite cards, native-room clipping,
contact shadows, HUD preservation, and clean tile reconstruction underneath
sprites. It remains dormant until the game explicitly enables and calls it.
This keeps game-specific material policy out of the engine while avoiding a
copy of the camera/OAM plumbing in every side-scrolling profile.

## Opt-in cards and shadows

Zero-initialize `NesVoxelScene` and enable only the policies a game needs.
All of the following are opt-in and leave existing profiles unchanged:

- `tile_billboard` can replace a grouped background decoration with one
  alpha-tested camera-facing card. Return `1` at the group's top-left tile,
  `-1` for its remaining member tiles, and `0` for ordinary terrain. The
  callback supplies the group width and height in tiles.
- `tile_billboard_scale`, `tile_billboard_shadow_scale`, and
  `tile_billboard_shadow_opacity` control those generated decoration cards.
  An opacity of zero disables their contact shadows.
- `sprite_constant_screen_size` keeps OAM cards pixel-stable under
  perspective. `clip_sprites_to_source` prevents OAM outside the native room
  rectangle from reappearing in 3D.
- `sprite_max_height` lets a game cap a particular assembled metasprite when
  transition-only OAM pieces would otherwise create an elongated card.
- `sprite_visible` can suppress a complete assembled metasprite. This is
  useful for first-person profiles that hide the player body but retain
  enemies, projectiles, and effects.
- `sprite_ground`, `sprite_shadow`, and `sprite_overlay` remain per-game
  policy callbacks. Shadow scale and opacity are configured independently.

`use_camera_pose` switches from the default orbit camera to an explicit
eye/look-at pose. `camera_focal_scale` and normalized `camera_center_y` tune
that view without changing the default projection. The screen-grid adapter
exposes the same path through its optional dynamic `camera` callback; profiles
that leave it unset continue to use the existing eased orbit camera.

`NES_VOXEL_LAYOUT_SIDE` is an additional opt-in geometry layout for
side-scrollers. It rotates the sampled screen plane upright: screen X remains
world X, screen Y becomes world height, and the semantic height callback
controls block depth. Empty cells become sky instead of a horizontal painted
floor. The default `NES_VOXEL_LAYOUT_FLOOR` path is unchanged.

`side_group_tiles` optionally combines adjacent sampled cells into one upright
block and assembles their source pixels into one face texture. For example, a
value of `2` turns SMB's four 8×8 cells into a real 16×16×16 metatile cube.
Upright-layout OAM cards use world scale rather than constant screen size, so
actors preserve their size relative to grouped blocks and grow with proximity.
For pixel-scrolling games, the screen adapter's optional `grid_offset_x`
callback aligns those groups to the live world/metatile origin; partial cells
outside the native viewport are clipped rather than contaminating a group.

The engine owns assembly, alpha testing, depth sorting, and contact-shadow
rendering. A game profile owns tile/object identification because CHR and OAM
layouts are game-specific. Zelda, for example, identifies its 2x2 tree
metatiles as billboard groups without placing Zelda tile IDs in nesrecomp.

## Current scope

This is a compact native counterpart to the render-pipeline ideas in
[Dramatic Shape Voxel Mod](https://github.com/DramaticShape/DramaticShapeVoxelMod)
for [Gen1Recomp](https://github.com/bryanthaboi/gen1recomp). It is not a port
of that LÖVE/Lua implementation.

The current renderer provides the reusable foundation: perspective camera,
textured geometry, depth buffering, background and OAM billboards, contact
shadows, and HUD preservation. Per-game authored shape profiles,
post-processing/tilt-shift, free camera, and GPU acceleration are natural
follow-up layers.
