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

The renderer chooses an uncontaminated occurrence of a repeated tile when an
OAM sprite overlaps its source pixels. This avoids stamping the original flat
sprite into the terrain texture before drawing the upright sprite card.

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
- `sprite_ground`, `sprite_shadow`, and `sprite_overlay` remain per-game
  policy callbacks. Shadow scale and opacity are configured independently.

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
