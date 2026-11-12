/* Immutable presentation data for game-owned, credited Proto Man packages.
 * No guest RAM, cartridge data, renderer state or simulation policy lives here.
 * A title compiles the .c explicitly and supplies the verified source PNGs. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CYC_PROTO_IDLE0, CYC_PROTO_IDLE1, CYC_PROTO_IDLE2, CYC_PROTO_IDLE3,
    CYC_PROTO_JUMP_GUARD, CYC_PROTO_HURT,
    CYC_PROTO_RUN0, CYC_PROTO_RUN1, CYC_PROTO_RUN2, CYC_PROTO_JUMP_FIRE,
    CYC_PROTO_FIRE_RUN0, CYC_PROTO_FIRE_RUN1, CYC_PROTO_FIRE_RUN2,
    CYC_PROTO_FIRE_IDLE0, CYC_PROTO_FIRE_IDLE1, CYC_PROTO_JUMP_OPEN,
    CYC_PROTO_LADDER0, CYC_PROTO_LADDER1, CYC_PROTO_LADDER_FIRE0, CYC_PROTO_LADDER_FIRE1,
    CYC_PROTO_SLIDE, CYC_PROTO_TELEPORT, CYC_PROTO_TELEPORT_PARTIAL, CYC_PROTO_TELEPORT_FULL,
    CYC_PROTO_LADDER_TOP0, CYC_PROTO_LADDER_TOP1,
    CYC_PROTO_THROW_IDLE, CYC_PROTO_THROW_JUMP,
    CYC_PROTO_THROW_RUN0, CYC_PROTO_THROW_RUN1, CYC_PROTO_THROW_RUN2,
    CYC_PROTO_STEP0, CYC_PROTO_STEP1, CYC_PROTO_POSE_COUNT
} CycProtoPoseId;

typedef struct {
    uint8_t width, height;
    int8_t origin_x, top_y;
    /* Row stride 32. Zero is transparent; 1..6 index atlas.colors[].
     * top_y is an OAM-coordinate offset: physical NES OAM Y displays at Y+1.
     * For an OAM-anchored body (x,y), draw at x-origin_x,y+1+top_y.
     * A title defines its native body anchor and mirrors around it. */
    uint8_t pixels[32 * 32];
} CycProtoPose;

typedef struct {
    uint32_t colors[6];
    CycProtoPose poses[CYC_PROTO_POSE_COUNT];
    uint32_t portrait[16 * 16]; /* ARGB; zero is transparent. */
} CycProtoAtlas;

bool cyc_protoman_atlas_load(CycProtoAtlas *out,
    const uint8_t *sheet, size_t sheet_size,
    const uint8_t *open_jump, size_t jump_size,
    char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
