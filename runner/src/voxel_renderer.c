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
#include <string.h>

#define VOXEL_MAX_WIDTH  512
#define VOXEL_MAX_HEIGHT 240
#define VOXEL_PI 3.14159265358979323846f

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
} Texture;

typedef struct RenderContext {
    const NesVoxelScene *scene;
    Vec3 eye, right, up, forward;
    float focal;
    float center_x, center_y;
} RenderContext;

static uint32_t s_source[VOXEL_MAX_WIDTH * VOXEL_MAX_HEIGHT];
static float s_depth[VOXEL_MAX_WIDTH * VOXEL_MAX_HEIGHT];
static uint8_t s_contaminated[32 * 30];
static int16_t s_representative[256];

extern const uint32_t g_nes_palette[64];

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
    unsigned r = (color >> 16) & 0xFF;
    unsigned g = (color >> 8) & 0xFF;
    unsigned b = color & 0xFF;
    r = (unsigned)(r * shade);
    g = (unsigned)(g * shade);
    b = (unsigned)(b * shade);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
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
            if (inv_depth <= s_depth[pos]) continue;

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
            s->framebuffer[pos] = shade_color(color, texture->shade);
            s_depth[pos] = inv_depth;
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
    return texture;
}

static void render_terrain(const RenderContext *ctx) {
    const NesVoxelScene *s = ctx->scene;
    float ts = (float)s->tile_size;
    for (int ty = 0; ty < s->tile_rows; ty++) {
        for (int tx = 0; tx < s->tile_columns; tx++) {
            float x0 = tx * ts, x1 = x0 + ts;
            float z0 = ty * ts, z1 = z0 + ts;
            float h = scene_height(s, tx, ty);
            float north = scene_height(s, tx, ty - 1);
            float south = scene_height(s, tx, ty + 1);
            float west = scene_height(s, tx - 1, ty);
            float east = scene_height(s, tx + 1, ty);
            Texture top = tile_texture(s, tx, ty, h < 0.0f ? 0.86f : 1.0f);
            Texture side_a = tile_texture(s, tx, ty, 0.62f);
            Texture side_b = tile_texture(s, tx, ty, 0.76f);

            draw_quad(ctx, vec3(x0, h, z0), vec3(x1, h, z0),
                      vec3(x1, h, z1), vec3(x0, h, z1), &top);
            if (h > north)
                draw_quad(ctx, vec3(x1, h, z0), vec3(x0, h, z0),
                          vec3(x0, north, z0), vec3(x1, north, z0), &side_a);
            if (h > south)
                draw_quad(ctx, vec3(x0, h, z1), vec3(x1, h, z1),
                          vec3(x1, south, z1), vec3(x0, south, z1), &side_b);
            if (h > west)
                draw_quad(ctx, vec3(x0, h, z0), vec3(x0, h, z1),
                          vec3(x0, west, z1), vec3(x0, west, z0), &side_a);
            if (h > east)
                draw_quad(ctx, vec3(x1, h, z1), vec3(x1, h, z0),
                          vec3(x1, east, z0), vec3(x1, east, z1), &side_b);
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

static void render_sprites(const RenderContext *ctx) {
    const NesVoxelScene *s = ctx->scene;
    int sprite_height = (g_ppuctrl & 0x20) ? 16 : 8;
    Vec3 horizontal_right = vec3_normalize(vec3(ctx->right.x, 0.0f, ctx->right.z));
    uint32_t pixels[8 * 16];
    Texture texture = { pixels, 8, sprite_height, 8, 1.0f, 1 };

    for (int i = 63; i >= 0; i--) {
        int sy = g_ppu_oam[i * 4] + 1;
        int sx = g_ppu_oam[i * 4 + 3];
        float foot_z, ground, center_x;
        Vec3 center, left_bottom, right_bottom, right_top, left_top;
        if (g_ppu_oam[i * 4] >= 0xEF) continue;
        if (sy + sprite_height <= s->source_y ||
            sy >= s->source_y + s->source_height)
            continue;

        foot_z = (float)(sy + sprite_height - s->source_y);
        if (foot_z < 0.0f || foot_z >= s->source_height) continue;
        center_x = sx + 4.0f;
        ground = scene_height(s, sx / s->tile_size,
                              (int)foot_z / s->tile_size);
        if (ground < 0.0f) ground = 0.0f;
        center = vec3(center_x, ground + 0.2f, foot_z);
        left_bottom = vec3_sub(center, vec3_scale(horizontal_right, 4.0f));
        right_bottom = vec3(center.x + horizontal_right.x * 4.0f,
                            center.y, center.z + horizontal_right.z * 4.0f);
        right_top = vec3(right_bottom.x, right_bottom.y + sprite_height,
                         right_bottom.z);
        left_top = vec3(left_bottom.x, left_bottom.y + sprite_height,
                        left_bottom.z);
        decode_sprite(pixels, i, sprite_height);
        draw_quad(ctx, left_top, right_top, right_bottom, left_bottom, &texture);
    }
}

static int validate_scene(const NesVoxelScene *s) {
    if (!s || !s->framebuffer || !s->tiles || !s->tile_height) return 0;
    if (s->output_width <= 0 || s->output_width > VOXEL_MAX_WIDTH ||
        s->output_height <= 0 || s->output_height > VOXEL_MAX_HEIGHT)
        return 0;
    if (s->tile_columns <= 0 || s->tile_columns > 32 ||
        s->tile_rows <= 0 || s->tile_rows > 30 ||
        s->tile_size <= 0)
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
    float elevation, yaw, horizontal_distance, distance;
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

    target = vec3(s->source_width * 0.5f, 2.0f,
                  s->source_height * 0.5f);
    elevation = s->elevation_degrees * VOXEL_PI / 180.0f;
    yaw = -18.0f * VOXEL_PI / 180.0f;
    distance = 285.0f;
    horizontal_distance = cosf(elevation) * distance;
    ctx.eye = vec3(target.x + sinf(yaw) * horizontal_distance,
                   target.y + sinf(elevation) * distance,
                   target.z + cosf(yaw) * horizontal_distance);
    ctx.forward = vec3_normalize(vec3_sub(target, ctx.eye));
    ctx.right = vec3_normalize(vec3_cross(ctx.forward, vec3(0.0f, 1.0f, 0.0f)));
    ctx.up = vec3_normalize(vec3_cross(ctx.right, ctx.forward));
    ctx.focal = s->output_width * 0.92f;
    ctx.center_x = s->output_width * 0.5f;
    ctx.center_y = s->output_height * 0.59f;
    ctx.scene = s;

    render_terrain(&ctx);
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
