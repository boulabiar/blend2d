// This file is part of Blend2D project <https://blend2d.com>
//
// See blend2d.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

// The JPEG codec is based on stb_image <https://github.com/nothings/stb>
// released into PUBLIC DOMAIN. Blend2D's JPEG codec can be distributed
// under Blend2D's ZLIB license or under STB's PUBLIC DOMAIN as well.

#include <blend2d/core/api-build_p.h>
#ifdef BL_TARGET_OPT_AVX2

#include <blend2d/codec/jpegops_p.h>
#include <blend2d/simd/simd_p.h>
#include <blend2d/support/memops_p.h>

namespace bl::Jpeg {

// bl::Jpeg - Upsample 1x2 - AVX2
// ===============================
//
// Vertical 3:1 blend: dst[i] = (3*src0[i] + src1[i] + 2) >> 2
//
// Uses vpmaddubsw to fuse the weighted sum into a single instruction per
// 16 output bytes. We interleave (src0[i], src1[i]) as byte pairs and
// multiply by coefficients (3, 1), producing the exact u16 result
// 3*src0[i] + src1[i]. Add rounding bias (2), shift right by 2, packus
// back to u8.

uint8_t* BL_CDECL upsample_1x2_avx2(uint8_t* dst, uint8_t* src0, uint8_t* src1, uint32_t w, uint32_t hs) noexcept {
  using namespace SIMD;
  bl_unused(src1, hs);

  const Vec32xU8 coeff = vec_u8(make256_u16(uint16_t(0x0103))); // byte pairs: {3, 1}
  const Vec16xU16 bias = make256_u16(uint16_t(2));

  uint32_t i = 0;

  // Main loop: 32 bytes per iteration.
  for (; i + 32 <= w; i += 32) {
    Vec32xU8 s0 = loadu<Vec32xU8>(src0 + i);
    Vec32xU8 s1 = loadu<Vec32xU8>(src1 + i);

    // Interleave bytes into (src0, src1) pairs within each 128-bit lane.
    Vec32xU8 lo = interleave_lo_u8(s0, s1);
    Vec32xU8 hi = interleave_hi_u8(s0, s1);

    // Fused u8*i8 multiply-add: result[k] = 3*s0[k] + 1*s1[k] as i16.
    Vec16xU16 r_lo = vec_u16(maddws_u8xi8_i16(lo, coeff));
    Vec16xU16 r_hi = vec_u16(maddws_u8xi8_i16(hi, coeff));

    // Add rounding bias and shift right by 2.
    r_lo = srli_u16<2>(add_i16(r_lo, bias));
    r_hi = srli_u16<2>(add_i16(r_hi, bias));

    // Pack back to u8 with unsigned saturation. packus operates within
    // 128-bit lanes, matching the lane layout from unpacklo/hi above.
    storeu(dst + i, vec_u8(packs_128_i16_u8(vec_i16(r_lo), vec_i16(r_hi))));
  }

  // Scalar tail.
  for (; i < w; i++)
    dst[i] = uint8_t((3 * src0[i] + src1[i] + 2) >> 2);

  return dst;
}

// bl::Jpeg - Upsample 2x1 - AVX2
// ===============================
//
// Horizontal 1-to-2 expansion with 3:1 interpolation:
//   dst[i*2  ] = (3*src[i] + src[i-1] + 2) >> 2
//   dst[i*2+1] = (3*src[i] + src[i+1] + 2) >> 2
//
// Uses overlapping 128-bit loads to get center, left, and right neighbors,
// widens to u16 via vpmovzxbw, computes both output values in parallel,
// then interleaves and packs to produce 32 output bytes per iteration.

uint8_t* BL_CDECL upsample_2x1_avx2(uint8_t* dst, uint8_t* src0, uint8_t* src1, uint32_t w, uint32_t hs) noexcept {
  using namespace SIMD;
  bl_unused(hs, src1);

  if (w == 1) {
    dst[0] = dst[1] = src0[0];
    return dst;
  }

  const Vec16xU16 bias = make256_u16(uint16_t(2));

  // Handle first pixel (no left neighbor: left = self).
  dst[0] = src0[0];
  dst[1] = uint8_t((src0[0] * 3 + src0[1] + 2) >> 2);

  // Main loop: process 16 input bytes -> 32 output bytes per iteration.
  // Loop range: i in [1, w-1) for the interior pixels.
  uint32_t i = 1;
  for (; i + 16 <= w - 1; i += 16) {
    // Three overlapping loads provide center, left (-1), and right (+1) neighbors.
    Vec16xU16 c = loadu_128_u8_u16<Vec16xU16>(src0 + i);
    Vec16xU16 l = loadu_128_u8_u16<Vec16xU16>(src0 + i - 1);
    Vec16xU16 r = loadu_128_u8_u16<Vec16xU16>(src0 + i + 1);

    // 3 * center = (center << 1) + center.
    Vec16xU16 c3 = add_i16(slli_u16<1>(c), c);

    // even = (3*center + left + 2) >> 2
    Vec16xU16 even = srli_u16<2>(add_i16(add_i16(c3, l), bias));
    // odd  = (3*center + right + 2) >> 2
    Vec16xU16 odd = srli_u16<2>(add_i16(add_i16(c3, r), bias));

    // Interleave (even, odd) as u16 pairs, then pack to u8.
    // unpacklo/hi operate within 128-bit lanes; packus does the same.
    // This produces the correct output order: [E0,O0, E1,O1, ...].
    Vec16xU16 pairs_lo = vec_u16(interleave_lo_u16(even, odd));
    Vec16xU16 pairs_hi = vec_u16(interleave_hi_u16(even, odd));
    Vec32xU8 result = vec_u8(packs_128_i16_u8(vec_i16(pairs_lo), vec_i16(pairs_hi)));

    storeu(dst + i * 2, result);
  }

  // Scalar tail for remaining interior pixels.
  for (; i < w - 1; i++) {
    uint32_t n = 3 * src0[i] + 2;
    dst[i * 2 + 0] = uint8_t((n + src0[i - 1]) >> 2);
    dst[i * 2 + 1] = uint8_t((n + src0[i + 1]) >> 2);
  }

  // Handle last pixel (no right neighbor: right = self).
  dst[i * 2 + 0] = uint8_t((src0[w - 2] * 3 + src0[w - 1] + 2) >> 2);
  dst[i * 2 + 1] = src0[w - 1];

  return dst;
}

// bl::Jpeg - Upsample 2x2 - AVX2
// ===============================
//
// Separable 2D 3:1 filter (vertical then horizontal):
//   t[i]       = 3*src0[i] + src1[i]                     (u16, max 1020)
//   dst[i*2-1] = (3*t[i-1] + t[i]   + 8) >> 4           (weights 9:3:3:1 / 16)
//   dst[i*2]   = (3*t[i]   + t[i-1] + 8) >> 4
//
// Computes both t_cur and t_prev by loading from (src + i) and (src + i - 1),
// then applies the horizontal 3:1 filter on the u16 t-values. All arithmetic
// stays in u16 (max intermediate: 3*1020 + 1020 + 8 = 4088, well within u16).

uint8_t* BL_CDECL upsample_2x2_avx2(uint8_t* dst, uint8_t* src0, uint8_t* src1, uint32_t w, uint32_t hs) noexcept {
  using namespace SIMD;
  bl_unused(hs);

  if (w == 1) {
    dst[0] = dst[1] = uint8_t((3 * src0[0] + src1[0] + 2) >> 2);
    return dst;
  }

  const Vec16xU16 bias = make256_u16(uint16_t(8));

  // Handle first output pixel. t1 = 3*src0[0] + src1[0].
  uint32_t t1_scalar = 3 * src0[0] + src1[0];
  dst[0] = uint8_t((t1_scalar + 2) >> 2);

  // Main loop: process 16 input positions -> 32 output bytes per iteration.
  // Loop range: i in [1, w) for interior + last position.
  uint32_t i = 1;
  for (; i + 16 <= w; i += 16) {
    // Load and widen src0/src1 at current and previous positions.
    Vec16xU16 s0c = loadu_128_u8_u16<Vec16xU16>(src0 + i);
    Vec16xU16 s1c = loadu_128_u8_u16<Vec16xU16>(src1 + i);
    Vec16xU16 s0p = loadu_128_u8_u16<Vec16xU16>(src0 + i - 1);
    Vec16xU16 s1p = loadu_128_u8_u16<Vec16xU16>(src1 + i - 1);

    // Vertical blend: t = 3*s0 + s1.
    Vec16xU16 t_cur = add_i16(add_i16(slli_u16<1>(s0c), s0c), s1c);
    Vec16xU16 t_prev = add_i16(add_i16(slli_u16<1>(s0p), s0p), s1p);

    // Horizontal blend: 3*t + t_neighbor.
    Vec16xU16 t_cur3 = add_i16(slli_u16<1>(t_cur), t_cur);
    Vec16xU16 t_prev3 = add_i16(slli_u16<1>(t_prev), t_prev);

    // dst[i*2-1] = (3*t_prev + t_cur + 8) >> 4
    Vec16xU16 val_odd = srli_u16<4>(add_i16(add_i16(t_prev3, t_cur), bias));
    // dst[i*2]   = (3*t_cur + t_prev + 8) >> 4
    Vec16xU16 val_even = srli_u16<4>(add_i16(add_i16(t_cur3, t_prev), bias));

    // Interleave (odd, even) — odd goes to dst[i*2-1], even to dst[i*2].
    Vec16xU16 pairs_lo = vec_u16(interleave_lo_u16(val_odd, val_even));
    Vec16xU16 pairs_hi = vec_u16(interleave_hi_u16(val_odd, val_even));
    Vec32xU8 result = vec_u8(packs_128_i16_u8(vec_i16(pairs_lo), vec_i16(pairs_hi)));

    storeu(dst + i * 2 - 1, result);
  }

  // Scalar tail.
  {
    uint32_t t0;
    uint32_t t1_val;

    // Recompute t_prev for the first scalar iteration.
    if (i == 1)
      t1_val = t1_scalar;
    else
      t1_val = 3 * src0[i - 1] + src1[i - 1];

    for (; i < w; i++) {
      t0 = t1_val;
      t1_val = 3 * src0[i] + src1[i];

      dst[i * 2 - 1] = uint8_t((3 * t0 + t1_val + 8) >> 4);
      dst[i * 2] = uint8_t((3 * t1_val + t0 + 8) >> 4);
    }
  }

  // Handle last output byte.
  dst[w * 2 - 1] = uint8_t((3 * src0[w - 1] + src1[w - 1] + 2) >> 2);

  return dst;
}

} // {bl::Jpeg}

#endif // BL_TARGET_OPT_AVX2
