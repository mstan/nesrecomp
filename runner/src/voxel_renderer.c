/*
 * voxel_renderer.c -- small software 3D compositor for NESRecomp
 *
 * This intentionally uses the already-rendered ARGB8888 frame as its texture
 * atlas.  It therefore works with every SDL renderer backend and remains a
 * presentation layer: CPU, PPU, save-state, and game timing state are untouched.
 */
#include "voxel_renderer.h"
#include "nes_runtime.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define VOXEL_MAX_WIDTH  512
#define VOXEL_MAX_HEIGHT 240
#define VOXEL_MAX_TILE_COLUMNS 64
#define VOXEL_MAX_TILE_ROWS    60
#define VOXEL_MAX_TILES (VOXEL_MAX_TILE_COLUMNS * VOXEL_MAX_TILE_ROWS)
#define VOXEL_CUSTOM_TILE_SIZE 8
#define VOXEL_MAX_BILLBOARD_SIZE 32
#define VOXEL_PI 3.14159265358979323846f

enum {
    TILE_EDGE_NORTH,
    TILE_EDGE_SOUTH,
    TILE_EDGE_WEST,
    TILE_EDGE_EAST
};

typedef struct Vec3 {
    float x, y, z;
} Vec3;

typedef struct ProjectedVertex {
    float x, y;
    float inv_depth;
    float u, v;
} ProjectedVertex;

typedef struct Texture {
    const uint32_t *pixels;
    int width, height, stride;
    float shade;
    int alpha_test;
    int overlay;
} Texture;

typedef struct RenderContext {
    const NesVoxelScene *scene;
    Vec3 eye, right, up, forward;
    float focal;
    float center_x, center_y;
} RenderContext;

static uint32_t s_source[VOXEL_MAX_WIDTH * VOXEL_MAX_HEIGHT];
static float s_depth[VOXEL_MAX_WIDTH * VOXEL_MAX_HEIGHT];
static uint8_t s_contaminated[VOXEL_MAX_TILES];
static int16_t s_representative[256];
static uint32_t
    s_custom_tile_pixels[VOXEL_MAX_TILES *
                         VOXEL_CUSTOM_TILE_SIZE * VOXEL_CUSTOM_TILE_SIZE];

extern const uint32_t g_nes_palette[64];

static void draw_card_shadow(const RenderContext *ctx, float center_x,
                             float ground, float foot_z, float card_width,
                             float scale, float opacity);

static Vec3 vec3(float x, float y, float z) {
    Vec3 v = { x, y, z };
    return v;
}

static Vec3 vec3_sub(Vec3 a, Vec3 b) {
    return vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static Vec3 vec3_scale(Vec3 v, float s) {
    return vec3(v.x * s, v.y * s, v.z * s);
}

static float vec3_dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 vec3_cross(Vec3 a, Vec3 b) {
    return vec3(a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x);
}

static Vec3 vec3_normalize(Vec3 v) {
    float len = sqrtf(vec3_dot(v, v));
    return len > 0.00001f ? vec3_scale(v, 1.0f / len) : v;
}

static uint8_t scene_tile(const NesVoxelScene *s, int x, int y) {
    int index = s->column_major ? x * s->tile_stride + y
                                : y * s->tile_stride + x;
    return s->tiles[index];
}

static float scene_height(const NesVoxelScene *s, int x, int y) {
    if (x < 0 || y < 0 || x >= s->tile_columns || y >= s->tile_rows)
        return -6.0f;
    return s->tile_height(scene_tile(s, x, y), x, y, s->user);
}

static uint32_t shade_color(uint32_t color, float shade) {
    unsigned a = color >> 24;
    unsigned r = (color >> 16) & 0xFF;
    unsigned g = (color >> 8) & 0xFF;
    unsigned b = color & 0xFF;
    r = (unsigned)(r * shade);
    g = (unsigned)(g * shade);
    b = (unsigned)(b * shade);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t blend_over(uint32_t destination, uint32_t source) {
    unsigned a = source >> 24;
    unsigned inv = 255 - a;
    unsigned sr = (source >> 16) & 0xFF;
    unsigned sg = (source >> 8) & 0xFF;
    unsigned sb = source & 0xFF;
    unsigned dr = (destination >> 16) & 0xFF;
    unsigned dg = (destination >> 8) & 0xFF;
    unsigned db = destination & 0xFF;
    unsigned r = (sr * a + dr * inv + 127) / 255;
    unsigned g = (sg * a + dg * inv + 127) / 255;
    unsigned b = (sb * a + db * inv + 127) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static uint32_t lerp_color(uint32_t a, uint32_t b, float t) {
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    unsigned r = (unsigned)(ar + (br - ar) * t);
    unsigned g = (unsigned)(ag + (bg - ag) * t);
    unsigned bl = (unsigned)(ab + (bb - ab) * t);
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

static int project_vertex(const RenderContext *ctx, Vec3 world, float u, float v,
                          ProjectedVertex *out) {
    Vec3 rel = vec3_sub(world, ctx->eye);
    float depth = vec3_dot(rel, ctx->forward);
    if (depth < 1.0f) return 0;
    out->inv_depth = 1.0f / depth;
    out->x = ctx->center_x + vec3_dot(rel, ctx->right) * ctx->focal / depth;
    out->y = ctx->center_y - vec3_dot(rel, ctx->up) * ctx->focal / depth;
    out->u = u;
    out->v = v;
    return 1;
}

static float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void draw_triangle(const RenderContext *ctx,
                          const ProjectedVertex *a,
                          const ProjectedVertex *b,
                          const ProjectedVertex *c,
                          const Texture *texture) {
    const NesVoxelScene *s = ctx->scene;
    float area = edge(a->x, a->y, b->x, b->y, c->x, c->y);
    float min_xf, max_xf, min_yf, max_yf;
    int min_x, max_x, min_y, max_y;
    if (fabsf(area) < 0.0001f) return;

    min_xf = fminf(a->x, fminf(b->x, c->x));
    max_xf = fmaxf(a->x, fmaxf(b->x, c->x));
    min_yf = fminf(a->y, fminf(b->y, c->y));
    max_yf = fmaxf(a->y, fmaxf(b->y, c->y));
    min_x = (int)floorf(min_xf);
    max_x = (int)ceilf(max_xf);
    min_y = (int)floorf(min_yf);
    max_y = (int)ceilf(max_yf);
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= s->output_width) max_x = s->output_width - 1;
    if (max_y >= s->output_height) max_y = s->output_height - 1;

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = edge(b->x, b->y, c->x, c->y, px, py) / area;
            float w1 = edge(c->x, c->y, a->x, a->y, px, py) / area;
            float w2 = 1.0f - w0 - w1;
            float inv_depth, u, v;
            int tx, ty, pos;
            uint32_t color;
            if (w0 < -0.0001f || w1 < -0.0001f || w2 < -0.0001f) continue;

            inv_depth = w0 * a->inv_depth + w1 * b->inv_depth +
                        w2 * c->inv_depth;
            pos = y * s->output_width + x;
            if (!texture->overlay && inv_depth <= s_depth[pos]) continue;

            u = (w0 * a->u * a->inv_depth +
                 w1 * b->u * b->inv_depth +
                 w2 * c->u * c->inv_depth) / inv_depth;
            v = (w0 * a->v * a->inv_depth +
                 w1 * b->v * b->inv_depth +
                 w2 * c->v * c->inv_depth) / inv_depth;
            tx = (int)floorf(u);
            ty = (int)floorf(v);
            if (tx < 0) tx = 0;
            if (ty < 0) ty = 0;
            if (tx >= texture->width) tx = texture->width - 1;
            if (ty >= texture->height) ty = texture->height - 1;
            color = texture->pixels[ty * texture->stride + tx];
            if (texture->alpha_test && (color >> 24) == 0) continue;
            color = shade_color(color, texture->shade);
            s->framebuffer[pos] = (color >> 24) < 0xFF
                ? blend_over(s->framebuffer[pos], color)
                : color;
            if (!texture->overlay) s_depth[pos] = inv_depth;
        }
    }
}

static void draw_quad(const RenderContext *ctx, Vec3 a, Vec3 b, Vec3 c, Vec3 d,
                      const Texture *texture) {
    ProjectedVertex pa, pb, pc, pd;
    float umax = (float)texture->width - 0.001f;
    float vmax = (float)texture->height - 0.001f;
    if (!project_vertex(ctx, a, 0.0f, 0.0f, &pa) ||
        !project_vertex(ctx, b, umax, 0.0f, &pb) ||
        !project_vertex(ctx, c, umax, vmax, &pc) ||
        !project_vertex(ctx, d, 0.0f, vmax, &pd))
        return;
    draw_triangle(ctx, &pa, &pb, &pc, texture);
    draw_triangle(ctx, &pa, &pc, &pd, texture);
}

static int rects_overlap(int ax, int ay, int aw, int ah,
                         int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static void find_clean_tile_sources(const NesVoxelScene *s) {
    int sprite_height = (g_ppuctrl & 0x20) ? 16 : 8;
    int count = s->tile_columns * s->tile_rows;
    if (s->tile_pixels) {
        for (int ty = 0; ty < s->tile_rows; ty++) {
            for (int tx = 0; tx < s->tile_columns; tx++) {
                int index = ty * s->tile_columns + tx;
                s->tile_pixels(
                    s_custom_tile_pixels +
                        index * VOXEL_CUSTOM_TILE_SIZE * VOXEL_CUSTOM_TILE_SIZE,
                    VOXEL_CUSTOM_TILE_SIZE, scene_tile(s, tx, ty), tx, ty,
                    s->user);
            }
        }
        return;
    }
    memset(s_contaminated, 0, (size_t)count);
    for (int ty = 0; ty < s->tile_rows; ty++) {
        for (int tx = 0; tx < s->tile_columns; tx++) {
            int tile_x = tx * s->tile_size;
            int tile_y = s->source_y + ty * s->tile_size;
            int index = ty * s->tile_columns + tx;
            for (int i = 0; i < 64; i++) {
                int sy = g_ppu_oam[i * 4] + 1;
                int sx = g_ppu_oam[i * 4 + 3];
                if (g_ppu_oam[i * 4] >= 0xEF) continue;
                if (rects_overlap(tile_x, tile_y, s->tile_size, s->tile_size,
                                  sx, sy, 8, sprite_height)) {
                    s_contaminated[index] = 1;
                    break;
                }
            }
        }
    }

    for (int i = 0; i < 256; i++) s_representative[i] = -1;
    for (int ty = 0; ty < s->tile_rows; ty++) {
        for (int tx = 0; tx < s->tile_columns; tx++) {
            int index = ty * s->tile_columns + tx;
            uint8_t tile = scene_tile(s, tx, ty);
            if (!s_contaminated[index] && s_representative[tile] < 0)
                s_representative[tile] = (int16_t)index;
        }
    }
}

static Texture tile_texture(const NesVoxelScene *s, int tx, int ty, float shade) {
    int index = ty * s->tile_columns + tx;
    int source_index = index;
    uint8_t tile = scene_tile(s, tx, ty);
    int sx, sy;
    Texture texture;
    if (s->tile_pixels) {
        texture.pixels =
            s_custom_tile_pixels +
            index * VOXEL_CUSTOM_TILE_SIZE * VOXEL_CUSTOM_TILE_SIZE;
        texture.width = s->tile_size;
        texture.height = s->tile_size;
        texture.stride = VOXEL_CUSTOM_TILE_SIZE;
        texture.shade = shade;
        texture.alpha_test = 0;
        texture.overlay = 0;
        return texture;
    }
    if (s_contaminated[index] && s_representative[tile] >= 0)
        source_index = s_representative[tile];
    sx = (source_index % s->tile_columns) * s->tile_size + s->source_x;
    sy = (source_index / s->tile_columns) * s->tile_size + s->source_y;
    texture.pixels = s_source + sy * s->output_width + sx;
    texture.width = s->tile_size;
    texture.height = s->tile_size;
    texture.stride = s->output_width;
    texture.shade = shade;
    texture.alpha_test = 0;
    texture.overlay = 0;
    return texture;
}

static unsigned color_luma(uint32_t color) {
    unsigned r = (color >> 16) & 0xFF;
    unsigned g = (color >> 8) & 0xFF;
    unsigned b = color & 0xFF;
    return r * 54u + g * 183u + b * 19u;
}

static uint32_t tile_side_color(const Texture *top) {
    uint32_t colors[64];
    uint8_t counts[64];
    uint32_t background =
        g_nes_palette[g_ppu_pal[0] & 0x3F];
    int unique_count = 0;
    int common = 0;
    int candidate = -1;

    memset(counts, 0, sizeof(counts));
    for (int y = 0; y < top->height; y++) {
        for (int x = 0; x < top->width; x++) {
            uint32_t color = top->pixels[y * top->stride + x];
            int found = -1;
            for (int i = 0; i < unique_count; i++) {
                if (colors[i] == color) {
                    found = i;
                    break;
                }
            }
            if (found < 0 && unique_count < 64) {
                found = unique_count++;
                colors[found] = color;
            }
            if (found >= 0 && counts[found] < 255) counts[found]++;
        }
    }
    if (unique_count == 0) return 0xFF000000u;
    for (int i = 1; i < unique_count; i++) {
        if (counts[i] > counts[common]) common = i;
    }
    /* Prefer a dark material tone over the already-composited universal
     * background or black outline. This becomes the replacement color for
     * bright ground pixels that do not belong on a vertical face. */
    for (int i = 0; i < unique_count; i++) {
        unsigned luma = color_luma(colors[i]);
        if (colors[i] == background || luma < 4096u) continue;
        if (candidate < 0 || luma < color_luma(colors[candidate]) ||
            (luma == color_luma(colors[candidate]) &&
             counts[i] > counts[candidate]))
            candidate = i;
    }
    if (candidate < 0) {
        for (int i = 0; i < unique_count; i++) {
            if (colors[i] != background &&
                (candidate < 0 || counts[i] > counts[candidate]))
                candidate = i;
        }
    }
    return colors[candidate >= 0 ? candidate : common];
}

static Texture tile_side_texture(const Texture *top, int edge,
                                 uint32_t material, uint32_t *pixels,
                                 float shade) {
    Texture texture;
    unsigned material_luma = color_luma(material);
    unsigned bright_limit = material_luma + material_luma / 2u;
    for (int i = 0; i < top->width; i++) {
        int x = i;
        int y = 0;
        uint32_t color;
        if (edge == TILE_EDGE_SOUTH) {
            y = top->height - 1;
        } else if (edge == TILE_EDGE_WEST) {
            x = 0;
            y = i < top->height ? i : top->height - 1;
        } else if (edge == TILE_EDGE_EAST) {
            x = top->width - 1;
            y = i < top->height ? i : top->height - 1;
        }
        color = top->pixels[y * top->stride + x];
        pixels[i] = color_luma(color) > bright_limit ? material : color;
    }
    texture.pixels = pixels;
    texture.width = top->width;
    texture.height = 1;
    texture.stride = top->width;
    texture.shade = shade;
    texture.alpha_test = 0;
    texture.overlay = 0;
    return texture;
}

static int tile_billboard_info(const NesVoxelScene *s, int tx, int ty,
                               int *columns, int *rows) {
    int result;
    if (!s->tile_billboard) return 0;
    *columns = 1;
    *rows = 1;
    result = s->tile_billboard(
        scene_tile(s, tx, ty), tx, ty, columns, rows, s->user);
    if (result > 0 &&
        (*columns <= 0 || *rows <= 0 ||
         tx + *columns > s->tile_columns ||
         ty + *rows > s->tile_rows ||
         *columns * s->tile_size > VOXEL_MAX_BILLBOARD_SIZE ||
         *rows * s->tile_size > VOXEL_MAX_BILLBOARD_SIZE))
        return 0;
    return result;
}

static Texture billboard_ground_texture(const NesVoxelScene *s) {
    unsigned counts[256] = {0};
    int representative_x[256];
    int representative_y[256];
    int common = -1;
    memset(representative_x, -1, sizeof(representative_x));
    memset(representative_y, -1, sizeof(representative_y));
    for (int y = 0; y < s->tile_rows; y++) {
        for (int x = 0; x < s->tile_columns; x++) {
            int columns, rows;
            uint8_t tile;
            if (tile_billboard_info(s, x, y, &columns, &rows) != 0)
                continue;
            if (fabsf(scene_height(s, x, y)) > 0.01f)
                continue;
            tile = scene_tile(s, x, y);
            counts[tile]++;
            if (representative_x[tile] < 0) {
                representative_x[tile] = x;
                representative_y[tile] = y;
            }
            if (common < 0 || counts[tile] > counts[common])
                common = tile;
        }
    }
    /* The modal walkable tile is the room's base floor. Local nearest-tile
     * selection can accidentally copy a decorative but walkable shrub or
     * doorway into a removed tree footprint. */
    if (common >= 0)
        return tile_texture(
            s, representative_x[common], representative_y[common], 1.0f);
    return tile_texture(s, 0, 0, 1.0f);
}

static void render_terrain(const RenderContext *ctx) {
    const NesVoxelScene *s = ctx->scene;
    float ts = (float)s->tile_size;
    Texture billboard_ground =
        s->tile_billboard ? billboard_ground_texture(s)
                          : tile_texture(s, 0, 0, 1.0f);
    for (int ty = 0; ty < s->tile_rows; ty++) {
        for (int tx = 0; tx < s->tile_columns; tx++) {
            int billboard_columns, billboard_rows;
            float x0 = tx * ts, x1 = x0 + ts;
            float z0 = ty * ts, z1 = z0 + ts;
            float h = scene_height(s, tx, ty);
            float north = scene_height(s, tx, ty - 1);
            float south = scene_height(s, tx, ty + 1);
            float west = scene_height(s, tx - 1, ty);
            float east = scene_height(s, tx + 1, ty);
            Texture top = tile_texture(s, tx, ty, h < 0.0f ? 0.86f : 1.0f);

            if (tile_billboard_info(
                    s, tx, ty, &billboard_columns, &billboard_rows) != 0) {
                draw_quad(ctx, vec3(x0, 0.0f, z0), vec3(x1, 0.0f, z0),
                          vec3(x1, 0.0f, z1), vec3(x0, 0.0f, z1),
                          &billboard_ground);
                continue;
            }

            draw_quad(ctx, vec3(x0, h, z0), vec3(x1, h, z0),
                      vec3(x1, h, z1), vec3(x0, h, z1), &top);
            if (h > north || h > south || h > west || h > east) {
                uint32_t side_color = tile_side_color(&top);
                uint32_t north_pixels[VOXEL_MAX_WIDTH];
                uint32_t south_pixels[VOXEL_MAX_WIDTH];
                uint32_t west_pixels[VOXEL_MAX_WIDTH];
                uint32_t east_pixels[VOXEL_MAX_WIDTH];
                Texture north_side = tile_side_texture(
                    &top, TILE_EDGE_NORTH, side_color,
                    north_pixels, 0.62f);
                Texture south_side = tile_side_texture(
                    &top, TILE_EDGE_SOUTH, side_color,
                    south_pixels, 0.76f);
                Texture west_side = tile_side_texture(
                    &top, TILE_EDGE_WEST, side_color,
                    west_pixels, 0.62f);
                Texture east_side = tile_side_texture(
                    &top, TILE_EDGE_EAST, side_color,
                    east_pixels, 0.76f);
                if (h > north)
                    draw_quad(ctx, vec3(x1, h, z0), vec3(x0, h, z0),
                              vec3(x0, north, z0), vec3(x1, north, z0),
                              &north_side);
                if (h > south)
                    draw_quad(ctx, vec3(x0, h, z1), vec3(x1, h, z1),
                              vec3(x1, south, z1), vec3(x0, south, z1),
                              &south_side);
                if (h > west)
                    draw_quad(ctx, vec3(x0, h, z0), vec3(x0, h, z1),
                              vec3(x0, west, z1), vec3(x0, west, z0),
                              &west_side);
                if (h > east)
                    draw_quad(ctx, vec3(x1, h, z1), vec3(x1, h, z0),
                              vec3(x1, east, z0), vec3(x1, east, z1),
                              &east_side);
            }
        }
    }
}

static float side_group_depth(const NesVoxelScene *s,
                              int tx, int ty, int group) {
    float depth = 0.0f;
    for (int gy = 0; gy < group && ty + gy < s->tile_rows; gy++) {
        for (int gx = 0; gx < group && tx + gx < s->tile_columns; gx++) {
            float cell = scene_height(s, tx + gx, ty + gy);
            if (cell > depth) depth = cell;
        }
    }
    return depth;
}

static Texture side_group_texture(const NesVoxelScene *s,
                                  int tx, int ty, int group,
                                  uint32_t *pixels) {
    Texture result;
    int cells_x = group;
    int cells_y = group;
    int width, height;
    if (tx + cells_x > s->tile_columns) cells_x = s->tile_columns - tx;
    if (ty + cells_y > s->tile_rows) cells_y = s->tile_rows - ty;
    width = cells_x * s->tile_size;
    height = cells_y * s->tile_size;
    memset(pixels, 0, (size_t)width * height * sizeof(uint32_t));
    for (int gy = 0; gy < cells_y; gy++) {
        for (int gx = 0; gx < cells_x; gx++) {
            Texture cell = tile_texture(s, tx + gx, ty + gy, 1.0f);
            for (int py = 0; py < s->tile_size; py++) {
                for (int px = 0; px < s->tile_size; px++) {
                    pixels[(gy * s->tile_size + py) * width +
                           gx * s->tile_size + px] =
                        cell.pixels[py * cell.stride + px];
                }
            }
        }
    }
    result.pixels = pixels;
    result.width = width;
    result.height = height;
    result.stride = width;
    result.shade = 1.0f;
    result.alpha_test = 0;
    result.overlay = 0;
    return result;
}

static void render_side_terrain(const RenderContext *ctx) {
    const NesVoxelScene *s = ctx->scene;
    float ts = (float)s->tile_size;
    int group = s->side_group_tiles > 0 ? s->side_group_tiles : 1;
    if (group > 4) group = 4;
    for (int ty = 0; ty < s->tile_rows; ty += group) {
        for (int tx = 0; tx < s->tile_columns; tx += group) {
            float semantic_depth = side_group_depth(s, tx, ty, group);
            float depth, z0, z1;
            float x0, x1, y0, y1;
            float north, south, west, east;
            Texture face, dim_face, side_face;
            uint32_t group_pixels[32 * 32];
            if (semantic_depth <= 0.01f) continue;

            depth = group * ts;
            z0 = -depth * 0.5f;
            z1 = depth * 0.5f;
            x0 = tx * ts;
            x1 = x0 + group * ts;
            y1 = s->source_height - ty * ts;
            y0 = y1 - group * ts;
            north = side_group_depth(s, tx, ty - group, group);
            south = side_group_depth(s, tx, ty + group, group);
            west = side_group_depth(s, tx - group, ty, group);
            east = side_group_depth(s, tx + group, ty, group);
            face = side_group_texture(
                s, tx, ty, group, group_pixels);
            dim_face = face;
            dim_face.shade = 0.72f;
            side_face = face;
            side_face.shade = 0.84f;

            /* Faces perpendicular to level travel retain the complete source
             * tile, making blocks and question tiles readable from ahead. */
            if (west <= 0.01f)
                draw_quad(ctx, vec3(x0, y1, z1), vec3(x0, y1, z0),
                          vec3(x0, y0, z0), vec3(x0, y0, z1), &face);
            if (east <= 0.01f)
                draw_quad(ctx, vec3(x1, y1, z0), vec3(x1, y1, z1),
                          vec3(x1, y0, z1), vec3(x1, y0, z0), &face);

            if (north <= 0.01f)
                draw_quad(ctx, vec3(x0, y1, z0), vec3(x1, y1, z0),
                          vec3(x1, y1, z1), vec3(x0, y1, z1), &side_face);
            if (south <= 0.01f)
                draw_quad(ctx, vec3(x0, y0, z1), vec3(x1, y0, z1),
                          vec3(x1, y0, z0), vec3(x0, y0, z0), &dim_face);

            /* The front/back thickness faces keep isolated blocks legible
             * when the user turns their head off the level axis. */
            draw_quad(ctx, vec3(x0, y1, z0), vec3(x0, y0, z0),
                      vec3(x1, y0, z0), vec3(x1, y1, z0), &side_face);
            draw_quad(ctx, vec3(x1, y1, z1), vec3(x1, y0, z1),
                      vec3(x0, y0, z1), vec3(x0, y1, z1), &dim_face);
        }
    }
}

static void render_tile_billboards(const RenderContext *ctx) {
    const NesVoxelScene *s = ctx->scene;
    Vec3 horizontal_right =
        vec3_normalize(vec3(ctx->right.x, 0.0f, ctx->right.z));
    float ts = (float)s->tile_size;
    float scale =
        s->tile_billboard_scale > 0.0f ? s->tile_billboard_scale : 1.0f;
    Texture ground = billboard_ground_texture(s);
    uint32_t transparent =
        g_nes_palette[g_ppu_pal[0] & 0x3F];

    if (!s->tile_billboard) return;
    for (int ty = 0; ty < s->tile_rows; ty++) {
        for (int tx = 0; tx < s->tile_columns; tx++) {
            int columns, rows;
            int width, height;
            uint32_t pixels[VOXEL_MAX_BILLBOARD_SIZE *
                            VOXEL_MAX_BILLBOARD_SIZE];
            Texture texture;
            float card_width, card_height;
            float center_x, foot_z;
            Vec3 center, left_bottom, right_bottom, right_top, left_top;

            if (tile_billboard_info(s, tx, ty, &columns, &rows) <= 0)
                continue;
            width = columns * s->tile_size;
            height = rows * s->tile_size;
            memset(pixels, 0, sizeof(pixels));
            for (int group_y = 0; group_y < rows; group_y++) {
                for (int group_x = 0; group_x < columns; group_x++) {
                    Texture source =
                        tile_texture(s, tx + group_x, ty + group_y, 1.0f);
                    for (int py = 0; py < s->tile_size; py++) {
                        for (int px = 0; px < s->tile_size; px++) {
                            uint32_t color =
                                source.pixels[py * source.stride + px];
                            int matches_ground = 0;
                            for (int ground_y = 0;
                                 ground_y < ground.height && !matches_ground;
                                 ground_y++) {
                                for (int ground_x = 0;
                                     ground_x < ground.width; ground_x++) {
                                    if (color == ground.pixels[
                                            ground_y * ground.stride +
                                            ground_x]) {
                                        matches_ground = 1;
                                        break;
                                    }
                                }
                            }
                            if (color == transparent || matches_ground)
                                color &= 0x00FFFFFFu;
                            pixels[(group_y * s->tile_size + py) *
                                       VOXEL_MAX_BILLBOARD_SIZE +
                                   group_x * s->tile_size + px] = color;
                        }
                    }
                }
            }

            center_x = (tx + columns * 0.5f) * ts;
            foot_z = (ty + rows) * ts;
            center = vec3(center_x, 0.12f, foot_z);
            card_width = width * scale;
            card_height = height * scale;
            if (s->tile_billboard_shadow_opacity > 0.0f) {
                float shadow_scale =
                    s->tile_billboard_shadow_scale > 0.0f
                        ? s->tile_billboard_shadow_scale : 0.68f;
                draw_card_shadow(
                    ctx, center_x, 0.0f, foot_z, card_width, shadow_scale,
                    s->tile_billboard_shadow_opacity);
            }
            left_bottom = vec3_sub(
                center, vec3_scale(horizontal_right, card_width * 0.5f));
            right_bottom = vec3(
                center.x + horizontal_right.x * card_width * 0.5f,
                center.y,
                center.z + horizontal_right.z * card_width * 0.5f);
            right_top = vec3(
                right_bottom.x + ctx->up.x * card_height,
                right_bottom.y + ctx->up.y * card_height,
                right_bottom.z + ctx->up.z * card_height);
            left_top = vec3(
                left_bottom.x + ctx->up.x * card_height,
                left_bottom.y + ctx->up.y * card_height,
                left_bottom.z + ctx->up.z * card_height);
            texture.pixels = pixels;
            texture.width = width;
            texture.height = height;
            texture.stride = VOXEL_MAX_BILLBOARD_SIZE;
            texture.shade = 1.0f;
            texture.alpha_test = 1;
            texture.overlay = 0;
            draw_quad(ctx, left_top, right_top, right_bottom, left_bottom,
                      &texture);
        }
    }
}

static void decode_sprite(uint32_t *pixels, int sprite_index, int sprite_height) {
    uint8_t tile = g_ppu_oam[sprite_index * 4 + 1];
    uint8_t attr = g_ppu_oam[sprite_index * 4 + 2];
    int flip_h = (attr & 0x40) != 0;
    int flip_v = (attr & 0x80) != 0;
    int palette = (attr & 3) + 4;
    int chr_base = (g_ppuctrl & 0x08) ? 0x1000 : 0;
    int tile_base = tile;
    if (sprite_height == 16) {
        chr_base = (tile & 1) ? 0x1000 : 0;
        tile_base = tile & 0xFE;
    }

    memset(pixels, 0, (size_t)8 * sprite_height * sizeof(uint32_t));
    for (int y = 0; y < sprite_height; y++) {
        int source_y = flip_v ? sprite_height - 1 - y : y;
        int tile_number = tile_base + source_y / 8;
        int row = source_y & 7;
        int chr_offset = chr_base + tile_number * 16 + row;
        uint8_t lo = g_chr_ram[chr_offset];
        uint8_t hi = g_chr_ram[chr_offset + 8];
        for (int x = 0; x < 8; x++) {
            int source_x = flip_h ? 7 - x : x;
            int bit = 7 - source_x;
            int color_index = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
            if (color_index) {
                uint8_t nes_color =
                    g_ppu_pal[(palette * 4 + color_index) & 0x1F] & 0x3F;
                pixels[y * 8 + x] = g_nes_palette[nes_color];
            }
        }
    }
}

static int sprites_connect(int ax, int ay, int ah, int ai,
                           int bx, int by, int bh, int bi) {
    int near_x = ax <= bx + 9 && bx <= ax + 9;
    int near_y = ay <= by + bh + 1 && by <= ay + ah + 1;
    /* Consecutive OAM entries are how NES games normally describe one
     * metasprite. The index bound avoids gluing unrelated actors together
     * merely because they overlap during combat. */
    return abs(ai - bi) <= 4 && near_x && near_y;
}

static void draw_card_shadow(const RenderContext *ctx, float center_x,
                             float ground, float foot_z, float card_width,
                             float scale, float opacity) {
    uint32_t pixels[8 * 8];
    Texture texture;
    float half_width = card_width * scale * 0.5f;
    float half_depth = half_width * 0.34f;
    unsigned max_alpha;

    if (half_width <= 0.0f || opacity <= 0.0f) return;
    if (opacity > 1.0f) opacity = 1.0f;
    max_alpha = (unsigned)(opacity * 255.0f + 0.5f);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            float dx = ((x + 0.5f) - 4.0f) / 4.0f;
            float dz = ((y + 0.5f) - 4.0f) / 4.0f;
            float distance = dx * dx + dz * dz;
            float strength = distance < 1.0f ? 1.0f - distance : 0.0f;
            unsigned alpha = (unsigned)(max_alpha * strength);
            pixels[y * 8 + x] = alpha << 24;
        }
    }
    texture.pixels = pixels;
    texture.width = 8;
    texture.height = 8;
    texture.stride = 8;
    texture.shade = 1.0f;
    texture.alpha_test = 1;
    texture.overlay = 0;
    draw_quad(ctx,
              vec3(center_x - half_width, ground + 0.08f,
                   foot_z - half_depth),
              vec3(center_x + half_width, ground + 0.08f,
                   foot_z - half_depth),
              vec3(center_x + half_width, ground + 0.08f,
                   foot_z + half_depth),
              vec3(center_x - half_width, ground + 0.08f,
                   foot_z + half_depth),
              &texture);
}

static void render_sprites(const RenderContext *ctx) {
    const NesVoxelScene *s = ctx->scene;
    int sprite_height = (g_ppuctrl & 0x20) ? 16 : 8;
    Vec3 horizontal_right = vec3_normalize(vec3(ctx->right.x, 0.0f, ctx->right.z));
    uint8_t active[64] = {0};
    uint8_t used[64] = {0};
    float sprite_scale = s->sprite_scale > 0.0f ? s->sprite_scale : 1.0f;

    for (int i = 0; i < 64; i++) {
        int sy = g_ppu_oam[i * 4] + 1;
        active[i] = g_ppu_oam[i * 4] < 0xEF &&
                    sy + sprite_height > s->source_y &&
                    sy < s->source_y + s->source_height;
    }
    for (int i = 63; i >= 0; i--) {
        int members[16], member_count = 0;
        int min_x, min_y, max_x, max_y;
        int source_min_x, source_min_y;
        uint32_t pixels[32 * 32];
        uint32_t decoded[8 * 16];
        float foot_z, ground, center_x, card_width, card_height;
        float shadow_strength;
        Vec3 card_up;
        Vec3 center, left_bottom, right_bottom, right_top, left_top;
        Texture texture;
        if (!active[i] || used[i]) continue;

        members[member_count++] = i;
        used[i] = 1;
        min_x = g_ppu_oam[i * 4 + 3];
        min_y = g_ppu_oam[i * 4] + 1;
        max_x = min_x + 8;
        max_y = min_y + sprite_height;

        /* Grow one connected component of nearby, consecutive OAM pieces. */
        for (int cursor = 0; cursor < member_count; cursor++) {
            int a = members[cursor];
            int ax = g_ppu_oam[a * 4 + 3];
            int ay = g_ppu_oam[a * 4] + 1;
            for (int j = 63; j >= 0 && member_count < 16; j--) {
                int bx, by, next_min_x, next_min_y, next_max_x, next_max_y;
                if (!active[j] || used[j]) continue;
                bx = g_ppu_oam[j * 4 + 3];
                by = g_ppu_oam[j * 4] + 1;
                if (!sprites_connect(ax, ay, sprite_height, a,
                                     bx, by, sprite_height, j))
                    continue;
                next_min_x = bx < min_x ? bx : min_x;
                next_min_y = by < min_y ? by : min_y;
                next_max_x = bx + 8 > max_x ? bx + 8 : max_x;
                next_max_y = by + sprite_height > max_y
                    ? by + sprite_height : max_y;
                if (next_max_x - next_min_x > 32 ||
                    next_max_y - next_min_y > 32)
                    continue;
                min_x = next_min_x;
                min_y = next_min_y;
                max_x = next_max_x;
                max_y = next_max_y;
                used[j] = 1;
                members[member_count++] = j;
            }
        }

        if (s->sprite_visible &&
            !s->sprite_visible(min_x, min_y, max_x, max_y, s->user))
            continue;

        source_min_x = min_x;
        source_min_y = min_y;
        memset(pixels, 0, sizeof(pixels));
        /* Higher OAM priority (lower index) wins where pieces overlap. */
        for (int index = 63; index >= 0; index--) {
            int belongs = 0;
            for (int pass = 0; pass < member_count; pass++)
                if (members[pass] == index) belongs = 1;
            if (!belongs) continue;
            int sx = g_ppu_oam[index * 4 + 3] - min_x;
            int sy = g_ppu_oam[index * 4] + 1 - min_y;
            decode_sprite(decoded, index, sprite_height);
            for (int y = 0; y < sprite_height; y++)
                for (int x = 0; x < 8; x++)
                    if (decoded[y * 8 + x] >> 24)
                        pixels[(sy + y) * 32 + sx + x] =
                            decoded[y * 8 + x];
        }

        if (s->sprite_max_height) {
            int max_height =
                s->sprite_max_height(members, member_count, s->user);
            if (max_height > 0 && max_y - min_y > max_height)
                min_y = max_y - max_height;
        }
        if (s->clip_sprites_to_source) {
            if (min_x < 0) min_x = 0;
            if (min_y < s->source_y) min_y = s->source_y;
            if (max_x > s->source_width) max_x = s->source_width;
            if (max_y > s->source_y + s->source_height)
                max_y = s->source_y + s->source_height;
            if (min_x >= max_x || min_y >= max_y) continue;
        }

        center_x = (min_x + max_x) * 0.5f;
        if (s->terrain_layout == NES_VOXEL_LAYOUT_SIDE) {
            foot_z = 0.0f;
            ground = (float)(s->source_y + s->source_height - max_y);
            center = vec3(center_x + s->sprite_world_offset_x,
                          ground + 0.2f,
                          s->sprite_world_offset_z);
        } else {
            foot_z = (float)(max_y - s->source_y);
            if (foot_z < 0.0f || foot_z >= s->source_height) continue;
            ground = scene_height(
                s, (int)(center_x + s->sprite_world_offset_x) / s->tile_size,
                (int)(foot_z + s->sprite_world_offset_z) / s->tile_size);
            if (s->sprite_ground)
                ground = s->sprite_ground(min_x, min_y, max_x, max_y,
                                          ground, s->user);
            if (ground < 0.0f) ground = 0.0f;
            center = vec3(center_x + s->sprite_world_offset_x,
                          ground + 0.2f,
                          foot_z + s->sprite_world_offset_z);
        }
        if (s->sprite_depth_bias > 0.0f) {
            Vec3 camera_pull = vec3_normalize(vec3(
                ctx->eye.x - center.x, 0.0f, ctx->eye.z - center.z));
            center.x += camera_pull.x * s->sprite_depth_bias;
            center.z += camera_pull.z * s->sprite_depth_bias;
        }
        card_width = (max_x - min_x) * sprite_scale;
        card_height = (max_y - min_y) * sprite_scale;
        if (s->sprite_constant_screen_size) {
            float depth = vec3_dot(vec3_sub(center, ctx->eye), ctx->forward);
            float world_units_per_pixel = depth / ctx->focal;
            if (world_units_per_pixel > 0.0f) {
                card_width *= world_units_per_pixel;
                card_height *= world_units_per_pixel;
            }
        }
        shadow_strength = s->sprite_shadow
            ? s->sprite_shadow(min_x, min_y, max_x, max_y, s->user)
            : 0.0f;
        if (shadow_strength > 0.0f) {
            float shadow_scale = s->sprite_shadow_scale > 0.0f
                ? s->sprite_shadow_scale : 0.62f;
            float shadow_opacity = s->sprite_shadow_opacity > 0.0f
                ? s->sprite_shadow_opacity : 0.38f;
            draw_card_shadow(
                ctx, center_x + s->sprite_world_offset_x, ground,
                foot_z + s->sprite_world_offset_z, card_width, shadow_scale,
                shadow_opacity * shadow_strength);
        }
        card_up = s->sprite_face_camera_pitch
            ? ctx->up : vec3(0.0f, 1.0f, 0.0f);
        left_bottom = vec3_sub(
            center, vec3_scale(horizontal_right, card_width * 0.5f));
        right_bottom = vec3(
            center.x + horizontal_right.x * card_width * 0.5f,
            center.y,
            center.z + horizontal_right.z * card_width * 0.5f);
        right_top = vec3(
            right_bottom.x + card_up.x * card_height,
            right_bottom.y + card_up.y * card_height,
            right_bottom.z + card_up.z * card_height);
        left_top = vec3(
            left_bottom.x + card_up.x * card_height,
            left_bottom.y + card_up.y * card_height,
            left_bottom.z + card_up.z * card_height);
        texture.pixels =
            pixels + (min_y - source_min_y) * 32 + (min_x - source_min_x);
        texture.width = max_x - min_x;
        texture.height = max_y - min_y;
        texture.stride = 32;
        texture.shade = 1.0f;
        texture.alpha_test = 1;
        texture.overlay = s->sprite_overlay &&
            s->sprite_overlay(min_x, min_y, max_x, max_y, s->user);
        draw_quad(ctx, left_top, right_top, right_bottom, left_bottom, &texture);
    }
}

static int validate_scene(const NesVoxelScene *s) {
    if (!s || !s->framebuffer || !s->tiles || !s->tile_height) return 0;
    if (s->output_width <= 0 || s->output_width > VOXEL_MAX_WIDTH ||
        s->output_height <= 0 || s->output_height > VOXEL_MAX_HEIGHT)
        return 0;
    if (s->tile_columns <= 0 ||
        s->tile_columns > VOXEL_MAX_TILE_COLUMNS ||
        s->tile_rows <= 0 || s->tile_rows > VOXEL_MAX_TILE_ROWS ||
        s->tile_size <= 0)
        return 0;
    if (s->tile_pixels && s->tile_size > VOXEL_CUSTOM_TILE_SIZE)
        return 0;
    if (s->source_x < 0 || s->source_y < 0 ||
        s->source_x + s->source_width > s->output_width ||
        s->source_y + s->source_height > s->output_height)
        return 0;
    return 1;
}

int nes_voxel_render(const NesVoxelScene *s) {
    RenderContext ctx;
    Vec3 target;
    float elevation, yaw, roll, horizontal_distance, distance;
    uint32_t sky_top, sky_bottom;

    if (!validate_scene(s)) return 0;
    memcpy(s_source, s->framebuffer,
           (size_t)s->output_width * s->output_height * sizeof(uint32_t));
    memset(s_depth, 0,
           (size_t)s->output_width * s->output_height * sizeof(float));
    find_clean_tile_sources(s);

    sky_top = s->sky_top ? s->sky_top : 0xFF8CC8F0u;
    sky_bottom = s->sky_bottom ? s->sky_bottom : 0xFFE8F4D8u;
    for (int y = 0; y < s->output_height; y++) {
        float t = (float)y / (float)(s->output_height - 1);
        uint32_t color = lerp_color(sky_top, sky_bottom, t);
        for (int x = 0; x < s->output_width; x++)
            s->framebuffer[y * s->output_width + x] = color;
    }

    roll = s->roll_degrees * VOXEL_PI / 180.0f;
    if (s->use_camera_pose) {
        ctx.eye = vec3(s->camera_eye_x, s->camera_eye_y, s->camera_eye_z);
        target = vec3(s->camera_look_at_x, s->camera_look_at_y,
                      s->camera_look_at_z);
    } else {
        target = vec3(s->use_camera_target
                          ? s->camera_target_x
                          : s->source_width * 0.5f,
                      2.0f,
                      s->use_camera_target
                          ? s->camera_target_z
                          : s->source_height * 0.5f);
        elevation = s->elevation_degrees * VOXEL_PI / 180.0f;
        yaw = s->yaw_degrees * VOXEL_PI / 180.0f;
        distance = s->camera_distance > 1.0f
            ? s->camera_distance : 285.0f;
        horizontal_distance = cosf(elevation) * distance;
        ctx.eye = vec3(target.x + sinf(yaw) * horizontal_distance,
                       target.y + sinf(elevation) * distance,
                       target.z + cosf(yaw) * horizontal_distance);
    }
    ctx.forward = vec3_normalize(vec3_sub(target, ctx.eye));
    ctx.right = vec3_normalize(vec3_cross(ctx.forward, vec3(0.0f, 1.0f, 0.0f)));
    ctx.up = vec3_normalize(vec3_cross(ctx.right, ctx.forward));
    if (fabsf(roll) > 0.0001f) {
        Vec3 base_right = ctx.right;
        Vec3 base_up = ctx.up;
        ctx.right = vec3_normalize(vec3(
            base_right.x * cosf(roll) + base_up.x * sinf(roll),
            base_right.y * cosf(roll) + base_up.y * sinf(roll),
            base_right.z * cosf(roll) + base_up.z * sinf(roll)));
        ctx.up = vec3_normalize(vec3(
            base_up.x * cosf(roll) - base_right.x * sinf(roll),
            base_up.y * cosf(roll) - base_right.y * sinf(roll),
            base_up.z * cosf(roll) - base_right.z * sinf(roll)));
    }
    ctx.focal = s->output_width *
        (s->camera_focal_scale > 0.05f ? s->camera_focal_scale : 0.92f);
    ctx.center_x = s->output_width * 0.5f;
    ctx.center_y = s->output_height *
        (s->camera_center_y > 0.05f && s->camera_center_y < 0.95f
             ? s->camera_center_y : 0.59f);
    ctx.scene = s;

    if (s->terrain_layout == NES_VOXEL_LAYOUT_SIDE)
        render_side_terrain(&ctx);
    else {
        render_terrain(&ctx);
        render_tile_billboards(&ctx);
    }
    if (s->draw_oam_sprites) render_sprites(&ctx);

    if (s->preserve_top_rows > 0) {
        int rows = s->preserve_top_rows;
        if (rows > s->output_height) rows = s->output_height;
        for (int y = 0; y < rows; y++) {
            if (s->extend_preserved_rows) {
                for (int x = 0; x < s->output_width; x++)
                    s->framebuffer[y * s->output_width + x] =
                        s->preserved_rows_fill;
            }
            memcpy(s->framebuffer + y * s->output_width + s->source_x,
                   s_source + y * s->output_width + s->source_x,
                   (size_t)s->source_width * sizeof(uint32_t));
        }
    }
    return 1;
}
