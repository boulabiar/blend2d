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
  bl_unused(src1, hs);

  const __m256i coeff = _mm256_set1_epi16(0x0103); // byte pairs: {3, 1}
  const __m256i bias = _mm256_set1_epi16(2);

  uint32_t i = 0;

  // Main loop: 32 bytes per iteration.
  for (; i + 32 <= w; i += 32) {
    __m256i s0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src0 + i));
    __m256i s1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src1 + i));

    // Interleave bytes into (src0, src1) pairs within each 128-bit lane.
    __m256i lo = _mm256_unpacklo_epi8(s0, s1);
    __m256i hi = _mm256_unpackhi_epi8(s0, s1);

    // Fused u8*i8 multiply-add: result[k] = 3*s0[k] + 1*s1[k] as i16.
    __m256i r_lo = _mm256_maddubs_epi16(lo, coeff);
    __m256i r_hi = _mm256_maddubs_epi16(hi, coeff);

    // Add rounding bias and shift right by 2.
    r_lo = _mm256_srli_epi16(_mm256_add_epi16(r_lo, bias), 2);
    r_hi = _mm256_srli_epi16(_mm256_add_epi16(r_hi, bias), 2);

    // Pack back to u8 with unsigned saturation. packus operates within
    // 128-bit lanes, matching the lane layout from unpacklo/hi above.
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_packus_epi16(r_lo, r_hi));
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
  bl_unused(hs, src1);

  if (w == 1) {
    dst[0] = dst[1] = src0[0];
    return dst;
  }

  const __m256i bias = _mm256_set1_epi16(2);

  // Handle first pixel (no left neighbor: left = self).
  dst[0] = src0[0];
  dst[1] = uint8_t((src0[0] * 3 + src0[1] + 2) >> 2);

  // Main loop: process 16 input bytes → 32 output bytes per iteration.
  // Loop range: i ∈ [1, w-1) for the interior pixels.
  uint32_t i = 1;
  for (; i + 16 <= w - 1; i += 16) {
    // Three overlapping loads provide center, left (-1), and right (+1) neighbors.
    __m256i c = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src0 + i)));
    __m256i l = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src0 + i - 1)));
    __m256i r = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src0 + i + 1)));

    // 3 * center = (center << 1) + center.
    __m256i c3 = _mm256_add_epi16(_mm256_slli_epi16(c, 1), c);

    // even = (3*center + left + 2) >> 2
    __m256i even = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(c3, l), bias), 2);
    // odd  = (3*center + right + 2) >> 2
    __m256i odd = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(c3, r), bias), 2);

    // Interleave (even, odd) as u16 pairs, then pack to u8.
    // unpacklo/hi operate within 128-bit lanes; packus does the same.
    // This produces the correct output order: [E0,O0, E1,O1, ...].
    __m256i pairs_lo = _mm256_unpacklo_epi16(even, odd);
    __m256i pairs_hi = _mm256_unpackhi_epi16(even, odd);
    __m256i result = _mm256_packus_epi16(pairs_lo, pairs_hi);

    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i * 2), result);
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
  bl_unused(hs);

  if (w == 1) {
    dst[0] = dst[1] = uint8_t((3 * src0[0] + src1[0] + 2) >> 2);
    return dst;
  }

  const __m256i bias = _mm256_set1_epi16(8);

  // Handle first output pixel. t1 = 3*src0[0] + src1[0].
  uint32_t t1_scalar = 3 * src0[0] + src1[0];
  dst[0] = uint8_t((t1_scalar + 2) >> 2);

  // Main loop: process 16 input positions → 32 output bytes per iteration.
  // Loop range: i ∈ [1, w) for interior + last position.
  uint32_t i = 1;
  for (; i + 16 <= w; i += 16) {
    // Load and widen src0/src1 at current and previous positions.
    __m256i s0c = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src0 + i)));
    __m256i s1c = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src1 + i)));
    __m256i s0p = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src0 + i - 1)));
    __m256i s1p = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src1 + i - 1)));

    // Vertical blend: t = 3*s0 + s1.
    __m256i t_cur = _mm256_add_epi16(_mm256_add_epi16(_mm256_slli_epi16(s0c, 1), s0c), s1c);
    __m256i t_prev = _mm256_add_epi16(_mm256_add_epi16(_mm256_slli_epi16(s0p, 1), s0p), s1p);

    // Horizontal blend: 3*t + t_neighbor.
    __m256i t_cur3 = _mm256_add_epi16(_mm256_slli_epi16(t_cur, 1), t_cur);
    __m256i t_prev3 = _mm256_add_epi16(_mm256_slli_epi16(t_prev, 1), t_prev);

    // dst[i*2-1] = (3*t_prev + t_cur + 8) >> 4
    __m256i val_odd = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(t_prev3, t_cur), bias), 4);
    // dst[i*2]   = (3*t_cur + t_prev + 8) >> 4
    __m256i val_even = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(t_cur3, t_prev), bias), 4);

    // Interleave (odd, even) — odd goes to dst[i*2-1], even to dst[i*2].
    __m256i pairs_lo = _mm256_unpacklo_epi16(val_odd, val_even);
    __m256i pairs_hi = _mm256_unpackhi_epi16(val_odd, val_even);
    __m256i result = _mm256_packus_epi16(pairs_lo, pairs_hi);

    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i * 2 - 1), result);
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
