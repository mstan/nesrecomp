#ifndef NESRECOMP_PPU_HLE_KERNEL_H
#define NESRECOMP_PPU_HLE_KERNEL_H

#include <stdbool.h>
#include <stdint.h>
/* x86-64 uses SSE2 and AArch64 uses NEON. Other targets, including baseline
 * 32-bit x86, keep the same interface with portable byte operations.
 * Architecture headers are included only on their matching targets. */
#if !defined(NESRECOMP_PPU_HLE_FORCE_PORTABLE) && \
    (defined(_M_X64) || defined(__x86_64__)) && !defined(_M_ARM64EC)
#define NESRECOMP_PPU_HLE_SSE2 1
#include <emmintrin.h>
#else
#define NESRECOMP_PPU_HLE_SSE2 0
#endif
#if NESRECOMP_PPU_HLE_SSE2 && defined(_MSC_VER)
#include <intrin.h>
#endif
#if !defined(NESRECOMP_PPU_HLE_FORCE_PORTABLE) && defined(__aarch64__) && defined(__ARM_NEON)
#define NESRECOMP_PPU_HLE_NEON 1
#include <arm_neon.h>
/* AArch64 has no PMOVMSKB. Pack eight high bits with byte-lane weights;
 * the horizontal sum cannot overflow because each weight occurs once. */
static inline unsigned ppu_hle_movemask8(uint8x8_t value)
{
    const uint8x8_t weights = {1, 2, 4, 8, 16, 32, 64, 128};
    return (unsigned)vaddv_u8(vmul_u8(vshr_n_u8(value, 7), weights));
}
#else
#define NESRECOMP_PPU_HLE_NEON 0
#endif

static inline unsigned ppu_hle_first_sprite(unsigned mask)
{
#if NESRECOMP_PPU_HLE_SSE2 && defined(_MSC_VER)
    unsigned long index;
    _BitScanForward(&index, mask);
    return (unsigned)index;
#elif defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctz(mask);
#else
    unsigned index = 0;
    while (!(mask & 1u)) { mask >>= 1; ++index; }
    return index;
#endif
}

/* Decode an entire eight-pixel planar row when fetched, rather than extracting
 * two planes for each output pixel. This immutable 512-byte table needs no
 * invalidation: fetched bytes, not a CHR address, select the result. */
static const uint16_t ppu_hle_spread8[256] = {
    0x0000, 0x0001, 0x0004, 0x0005, 0x0010, 0x0011, 0x0014, 0x0015,
    0x0040, 0x0041, 0x0044, 0x0045, 0x0050, 0x0051, 0x0054, 0x0055,
    0x0100, 0x0101, 0x0104, 0x0105, 0x0110, 0x0111, 0x0114, 0x0115,
    0x0140, 0x0141, 0x0144, 0x0145, 0x0150, 0x0151, 0x0154, 0x0155,
    0x0400, 0x0401, 0x0404, 0x0405, 0x0410, 0x0411, 0x0414, 0x0415,
    0x0440, 0x0441, 0x0444, 0x0445, 0x0450, 0x0451, 0x0454, 0x0455,
    0x0500, 0x0501, 0x0504, 0x0505, 0x0510, 0x0511, 0x0514, 0x0515,
    0x0540, 0x0541, 0x0544, 0x0545, 0x0550, 0x0551, 0x0554, 0x0555,
    0x1000, 0x1001, 0x1004, 0x1005, 0x1010, 0x1011, 0x1014, 0x1015,
    0x1040, 0x1041, 0x1044, 0x1045, 0x1050, 0x1051, 0x1054, 0x1055,
    0x1100, 0x1101, 0x1104, 0x1105, 0x1110, 0x1111, 0x1114, 0x1115,
    0x1140, 0x1141, 0x1144, 0x1145, 0x1150, 0x1151, 0x1154, 0x1155,
    0x1400, 0x1401, 0x1404, 0x1405, 0x1410, 0x1411, 0x1414, 0x1415,
    0x1440, 0x1441, 0x1444, 0x1445, 0x1450, 0x1451, 0x1454, 0x1455,
    0x1500, 0x1501, 0x1504, 0x1505, 0x1510, 0x1511, 0x1514, 0x1515,
    0x1540, 0x1541, 0x1544, 0x1545, 0x1550, 0x1551, 0x1554, 0x1555,
    0x4000, 0x4001, 0x4004, 0x4005, 0x4010, 0x4011, 0x4014, 0x4015,
    0x4040, 0x4041, 0x4044, 0x4045, 0x4050, 0x4051, 0x4054, 0x4055,
    0x4100, 0x4101, 0x4104, 0x4105, 0x4110, 0x4111, 0x4114, 0x4115,
    0x4140, 0x4141, 0x4144, 0x4145, 0x4150, 0x4151, 0x4154, 0x4155,
    0x4400, 0x4401, 0x4404, 0x4405, 0x4410, 0x4411, 0x4414, 0x4415,
    0x4440, 0x4441, 0x4444, 0x4445, 0x4450, 0x4451, 0x4454, 0x4455,
    0x4500, 0x4501, 0x4504, 0x4505, 0x4510, 0x4511, 0x4514, 0x4515,
    0x4540, 0x4541, 0x4544, 0x4545, 0x4550, 0x4551, 0x4554, 0x4555,
    0x5000, 0x5001, 0x5004, 0x5005, 0x5010, 0x5011, 0x5014, 0x5015,
    0x5040, 0x5041, 0x5044, 0x5045, 0x5050, 0x5051, 0x5054, 0x5055,
    0x5100, 0x5101, 0x5104, 0x5105, 0x5110, 0x5111, 0x5114, 0x5115,
    0x5140, 0x5141, 0x5144, 0x5145, 0x5150, 0x5151, 0x5154, 0x5155,
    0x5400, 0x5401, 0x5404, 0x5405, 0x5410, 0x5411, 0x5414, 0x5415,
    0x5440, 0x5441, 0x5444, 0x5445, 0x5450, 0x5451, 0x5454, 0x5455,
    0x5500, 0x5501, 0x5504, 0x5505, 0x5510, 0x5511, 0x5514, 0x5515,
    0x5540, 0x5541, 0x5544, 0x5545, 0x5550, 0x5551, 0x5554, 0x5555,
};

static inline uint16_t ppu_hle_row(uint8_t lo, uint8_t hi)
{
    return (uint16_t)(ppu_hle_spread8[lo] | (ppu_hle_spread8[hi] << 1));
}

/* Diagnostics compare logical planar values. Shipping pixel work and snapshot
 * restoration do not materialize a second model. */
static inline uint16_t ppu_hle_plane(uint32_t packed, unsigned plane)
{
    uint32_t v = (packed >> plane) & 0x55555555u;
    v = (v | (v >> 1)) & 0x33333333u;
    v = (v | (v >> 2)) & 0x0f0f0f0fu;
    v = (v | (v >> 4)) & 0x00ff00ffu;
    return (uint16_t)((v | (v >> 8)) & 0xffffu);
}

/* The eight sprite output units form one native batch. Counters and planes
 * remain the interface consumed by HD-pack hooks and diagnostics. */
static inline unsigned ppu_hle_sprite_mask(const uint8_t x[8],
                                           const uint8_t lo[8],
                                           const uint8_t hi[8], bool skipped)
{
#if NESRECOMP_PPU_HLE_SSE2
    __m128i vx = _mm_loadl_epi64((const __m128i *)x);
    __m128i ready = skipped ? _mm_set1_epi8(-1) : _mm_cmpeq_epi8(vx, _mm_setzero_si128());
    __m128i bits = _mm_or_si128(_mm_loadl_epi64((const __m128i *)lo),
                               _mm_loadl_epi64((const __m128i *)hi));
    return (unsigned)_mm_movemask_epi8(_mm_and_si128(bits, ready)) & 255u;
#elif NESRECOMP_PPU_HLE_NEON
    uint8x8_t vx = vld1_u8(x);
    uint8x8_t ready = skipped ? vdup_n_u8(255) : vceq_u8(vx, vdup_n_u8(0));
    uint8x8_t bits = vorr_u8(vld1_u8(lo), vld1_u8(hi));
    return ppu_hle_movemask8(vand_u8(bits, ready));
#else
    unsigned mask = 0;
    for (unsigned i = 0; i < 8; ++i)
        if ((skipped || !x[i]) && ((lo[i] | hi[i]) & 128)) mask |= 1u << i;
    return mask;
#endif
}

static inline unsigned ppu_hle_sprite_shift(uint8_t x[8], uint8_t lo[8],
                                            uint8_t hi[8], bool render, bool skipped)
{
#if NESRECOMP_PPU_HLE_SSE2
    __m128i vx = _mm_loadl_epi64((const __m128i *)x);
    __m128i ready = skipped ? _mm_set1_epi8(-1) : _mm_cmpeq_epi8(vx, _mm_setzero_si128());
    if (!skipped) _mm_storel_epi64((__m128i *)x, _mm_subs_epu8(vx, _mm_set1_epi8(1)));
    if (!render) return 0;
    __m128i vl = _mm_loadl_epi64((const __m128i *)lo);
    __m128i vh = _mm_loadl_epi64((const __m128i *)hi);
    vl = _mm_or_si128(_mm_andnot_si128(ready, vl), _mm_and_si128(ready, _mm_add_epi8(vl, vl)));
    vh = _mm_or_si128(_mm_andnot_si128(ready, vh), _mm_and_si128(ready, _mm_add_epi8(vh, vh)));
    _mm_storel_epi64((__m128i *)lo, vl);
    _mm_storel_epi64((__m128i *)hi, vh);
    return (unsigned)_mm_movemask_epi8(ready) & 255u;
#elif NESRECOMP_PPU_HLE_NEON
    uint8x8_t vx = vld1_u8(x);
    uint8x8_t ready = skipped ? vdup_n_u8(255) : vceq_u8(vx, vdup_n_u8(0));
    if (!skipped) vst1_u8(x, vqsub_u8(vx, vdup_n_u8(1)));
    if (!render) return 0;
    uint8x8_t vl = vld1_u8(lo), vh = vld1_u8(hi);
    vst1_u8(lo, vbsl_u8(ready, vshl_n_u8(vl, 1), vl));
    vst1_u8(hi, vbsl_u8(ready, vshl_n_u8(vh, 1), vh));
    return ppu_hle_movemask8(ready);
#else
    unsigned shifted = 0;
    for (unsigned i = 0; i < 8; ++i) {
        bool ready = skipped || !x[i];
        if (!skipped && x[i]) --x[i];
        if (render && ready) {
            lo[i] = (uint8_t)(lo[i] << 1);
            hi[i] = (uint8_t)(hi[i] << 1);
            shifted |= 1u << i;
        }
    }
    return shifted;
#endif
}

#endif
