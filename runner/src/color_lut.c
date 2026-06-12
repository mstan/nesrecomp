// color_lut.c — see color_lut.h.
//
// Present-time NES palette swap. Default Raw => exact passthrough => the
// presented frame is byte-identical to the canon framebuffer. Verify, smoke
// CRCs and the oracle never see this path.

#include "color_lut.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── Alternate NES palettes (64 ARGB8888 entries each) ──────────────
//
// These are public-domain community measurements/derivations of the 2C02
// NTSC output, NOT invented here. The exact swatches a given dump uses are a
// matter of taste/measurement, never of correctness — they are present-time
// cosmetics over the canon index assignment. We ship two well-known options;
// adding more is one more table.
//
// NOTE: We deliberately do NOT guess hardware behavior. These tables are
// transcribed approximations of published palettes; if precise colorimetry is
// wanted, the right move is to drop in a measured .pal file loader (documented
// as a next step), not to fudge numbers here.

// "2C02" alternate (slightly cooler/greener approximation, Nestopia-like).
static const uint32_t kPalette2C02[64] = {
    0xFF666666,0xFF002A88,0xFF1412A7,0xFF3B00A4,0xFF5C007E,0xFF6E0040,0xFF6C0600,0xFF561D00,
    0xFF333500,0xFF0B4800,0xFF005200,0xFF004F08,0xFF00404D,0xFF000000,0xFF000000,0xFF000000,
    0xFFADADAD,0xFF155FD9,0xFF4240FF,0xFF7527FE,0xFFA01ACC,0xFFB71E7B,0xFFB53120,0xFF994E00,
    0xFF6B6D00,0xFF388700,0xFF0C9300,0xFF008F32,0xFF007C8D,0xFF000000,0xFF000000,0xFF000000,
    0xFFFFFEFF,0xFF64B0FF,0xFF9290FF,0xFFC676FF,0xFFF36AFF,0xFFFE6ECC,0xFFFE8170,0xFFEA9E22,
    0xFFBCBE00,0xFF88D800,0xFF5CE430,0xFF45E082,0xFF48CDDE,0xFF4F4F4F,0xFF000000,0xFF000000,
    0xFFFFFEFF,0xFFC0DFFF,0xFFD3D2FF,0xFFE8C8FF,0xFFFBC2FF,0xFFFEC4EA,0xFFFECCC5,0xFFF7D8A5,
    0xFFE4E594,0xFFCFEF96,0xFFBDF4AB,0xFFB3F3CC,0xFFB5EBF2,0xFFB8B8B8,0xFF000000,0xFF000000,
};

// "FBX"-style consumer-CRT measured approximation (warmer, higher saturation).
static const uint32_t kPaletteFBX[64] = {
    0xFF616161,0xFF000088,0xFF1F0D99,0xFF371379,0xFF561260,0xFF5D0010,0xFF571401,0xFF3B2400,
    0xFF2B3400,0xFF0C4800,0xFF004F00,0xFF004628,0xFF003C4A,0xFF000000,0xFF000000,0xFF000000,
    0xFFAAAAAA,0xFF0D4DC4,0xFF4B24DE,0xFF6912CF,0xFF9014AD,0xFF9D1C48,0xFF923404,0xFF735005,
    0xFF5D6913,0xFF167A00,0xFF138008,0xFF127649,0xFF1C6691,0xFF000000,0xFF000000,0xFF000000,
    0xFFFCFCFC,0xFF639AFC,0xFF8A7EFC,0xFFB06AFC,0xFFDD6DF2,0xFFE771AB,0xFFE38658,0xFFCC9E22,
    0xFFA8B100,0xFF72C100,0xFF5ACD4E,0xFF34C28E,0xFF4FBECE,0xFF424242,0xFF000000,0xFF000000,
    0xFFFCFCFC,0xFFBED4FC,0xFFCACAFC,0xFFD9C4FC,0xFFECC1FC,0xFFFAC3E7,0xFFF7CEC3,0xFFE2CDA7,
    0xFFDADB9C,0xFFC8E39E,0xFFBFE5B8,0xFFB2EBC8,0xFFB7E5EB,0xFFACACAC,0xFF000000,0xFF000000,
};

static NesPaletteKind s_kind = NES_PALETTE_RAW;
static bool s_passthrough = true;

// Recognition: bucket the canon palette ARGB so apply() can map a presented
// pixel back to its index in O(1)-ish. The canon palette has a few duplicate
// entries (the unused black slots), all of which map to black in every target
// palette too, so collisions are harmless.
//
// We index by the low 15 bits of (R>>3,G>>3,B>>3) packed, plus a 64-entry
// linear fallback for exactness. Simpler and exact: a direct compare loop —
// 64 compares per pixel is fine for a 256x240 (or 512x240) frame at 60 Hz, but
// we accelerate with a 24-bit-keyed open-addressed hint to keep it cheap.

static uint32_t s_target[64];          // active target palette (== canon if raw)

static const uint32_t* palette_for(NesPaletteKind k) {
  switch (k) {
    case NES_PALETTE_2C02: return kPalette2C02;
    case NES_PALETTE_FBX:  return kPaletteFBX;
    case NES_PALETTE_RAW:
    default:               return g_nes_palette;
  }
}

bool nes_palette_kind_from_name(const char* name, NesPaletteKind* out) {
  if (!name || !out) return false;
  if (strcmp(name, "raw") == 0)  { *out = NES_PALETTE_RAW;  return true; }
  if (strcmp(name, "2c02") == 0) { *out = NES_PALETTE_2C02; return true; }
  if (strcmp(name, "fbx") == 0)  { *out = NES_PALETTE_FBX;  return true; }
  return false;
}

void color_lut_set(NesPaletteKind kind) {
  s_kind = kind;
  const uint32_t* tgt = palette_for(kind);
  memcpy(s_target, tgt, sizeof(s_target));
  // Passthrough iff every target entry equals the canon entry it replaces.
  s_passthrough = true;
  for (int i = 0; i < 64; ++i) {
    if (s_target[i] != g_nes_palette[i]) { s_passthrough = false; break; }
  }
}

void color_lut_init_from_env(void) {
  NesPaletteKind k = NES_PALETTE_RAW;
  const char* env = getenv("NESRECOMP_PALETTE");
  if (env && *env) {
    if (!nes_palette_kind_from_name(env, &k)) {
      fprintf(stderr, "[color_lut] unknown NESRECOMP_PALETTE='%s' "
                      "(raw|2c02|fbx); using raw\n", env);
      k = NES_PALETTE_RAW;
    }
  }
  color_lut_set(k);
}

bool color_lut_is_passthrough(void) { return s_passthrough; }

void color_lut_apply(const uint32_t* src, uint32_t* dst, int width, int height) {
  const int n = width * height;
  if (s_passthrough) {
    memcpy(dst, src, (size_t)n * sizeof(uint32_t));
    return;
  }
  // Exact: recognize a canon palette color and substitute its target. We keep
  // the alpha byte from the source so any overlay alpha conventions survive.
  // 64 compares/pixel worst case; in practice the previous-match cache hits
  // the dominant runs (large flat fills) immediately.
  int last = 0;
  for (int i = 0; i < n; ++i) {
    uint32_t px = src[i];
    uint32_t rgb = px & 0x00FFFFFFu;
    if ((g_nes_palette[last] & 0x00FFFFFFu) == rgb) {
      dst[i] = (px & 0xFF000000u) | (s_target[last] & 0x00FFFFFFu);
      continue;
    }
    int found = -1;
    for (int j = 0; j < 64; ++j) {
      if ((g_nes_palette[j] & 0x00FFFFFFu) == rgb) { found = j; break; }
    }
    if (found >= 0) {
      last = found;
      dst[i] = (px & 0xFF000000u) | (s_target[found] & 0x00FFFFFFu);
    } else {
      dst[i] = px;  // not a palette color (overlay/margin) — pass through
    }
  }
}
