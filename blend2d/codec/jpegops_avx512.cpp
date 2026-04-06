// This file is part of Blend2D project <https://blend2d.com>
//
// See blend2d.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

// The JPEG codec is based on stb_image <https://github.com/nothings/stb>
// released into PUBLIC DOMAIN. Blend2D's JPEG codec can be distributed
// under Blend2D's ZLIB license or under STB's PUBLIC DOMAIN as well.

#include <blend2d/core/api-build_p.h>
#ifdef BL_TARGET_OPT_AVX512

#include <blend2d/codec/jpegops_p.h>
#include <blend2d/simd/simd_p.h>
#include <blend2d/support/memops_p.h>

namespace bl::Jpeg {

// bl::Jpeg - Upsample 1x2 - AVX-512
// ==================================
//
// Vertical 3:1 blend: dst[i] = (3*src0[i] + src1[i] + 2) >> 2
//
// Same vpmaddubsw strategy as AVX2, doubled to 64 bytes per iteration.
// The tail is handled via AVX-512BW masked load/store, eliminating the
// scalar fallback entirely.

uint8_t* BL_CDECL upsample_1x2_avx512(uint8_t* dst, uint8_t* src0, uint8_t* src1, uint32_t w, uint32_t hs) noexcept {
  using namespace SIMD;
  bl_unused(src1, hs);

  const Vec64xU8 coeff = vec_u8(make512_u16(uint16_t(0x0103))); // byte pairs: {3, 1}
  const Vec32xU16 bias = make512_u16(uint16_t(2));

  uint32_t i = 0;

  // Main loop: 64 bytes per iteration.
  for (; i + 64 <= w; i += 64) {
    Vec64xU8 s0 = loadu<Vec64xU8>(src0 + i);
    Vec64xU8 s1 = loadu<Vec64xU8>(src1 + i);

    Vec64xU8 lo = interleave_lo_u8(s0, s1);
    Vec64xU8 hi = interleave_hi_u8(s0, s1);

    Vec32xU16 r_lo = vec_u16(maddws_u8xi8_i16(lo, coeff));
    Vec32xU16 r_hi = vec_u16(maddws_u8xi8_i16(hi, coeff));

    r_lo = srli_u16<2>(add_i16(r_lo, bias));
    r_hi = srli_u16<2>(add_i16(r_hi, bias));

    storeu(dst + i, vec_u8(packs_128_i16_u8(vec_i16(r_lo), vec_i16(r_hi))));
  }

  // Masked tail: process remaining bytes without scalar fallback.
  if (i < w) {
    uint32_t rem = w - i;
    __mmask64 k = (rem >= 64) ? (__mmask64)-1 : ((__mmask64)1 << rem) - 1;

    Vec64xU8 s0 = maskz_loadu_u8<Vec64xU8>(k, src0 + i);
    Vec64xU8 s1 = maskz_loadu_u8<Vec64xU8>(k, src1 + i);

    Vec64xU8 lo = interleave_lo_u8(s0, s1);
    Vec64xU8 hi = interleave_hi_u8(s0, s1);

    Vec32xU16 r_lo = vec_u16(maddws_u8xi8_i16(lo, coeff));
    Vec32xU16 r_hi = vec_u16(maddws_u8xi8_i16(hi, coeff));

    r_lo = srli_u16<2>(add_i16(r_lo, bias));
    r_hi = srli_u16<2>(add_i16(r_hi, bias));

    Vec64xU8 result = vec_u8(packs_128_i16_u8(vec_i16(r_lo), vec_i16(r_hi)));
    mask_storeu_u8(dst + i, k, result);
  }

  return dst;
}

// bl::Jpeg - Upsample 2x1 - AVX-512
// ==================================
//
// Horizontal 1-to-2 expansion with 3:1 interpolation.
// Processes 32 input bytes -> 64 output bytes per iteration using 512-bit
// vpmovzxbw (widen 32 u8 -> 32 u16 in one ZMM register).

uint8_t* BL_CDECL upsample_2x1_avx512(uint8_t* dst, uint8_t* src0, uint8_t* src1, uint32_t w, uint32_t hs) noexcept {
  using namespace SIMD;
  bl_unused(hs, src1);

  if (w == 1) {
    dst[0] = dst[1] = src0[0];
    return dst;
  }

  const Vec32xU16 bias = make512_u16(uint16_t(2));

  // Handle first pixel (no left neighbor).
  dst[0] = src0[0];
  dst[1] = uint8_t((src0[0] * 3 + src0[1] + 2) >> 2);

  // Main loop: 32 input bytes -> 64 output bytes per iteration.
  uint32_t i = 1;
  for (; i + 32 <= w - 1; i += 32) {
    Vec32xU16 c = loadu_256_u8_u16<Vec32xU16>(src0 + i);
    Vec32xU16 l = loadu_256_u8_u16<Vec32xU16>(src0 + i - 1);
    Vec32xU16 r = loadu_256_u8_u16<Vec32xU16>(src0 + i + 1);

    Vec32xU16 c3 = add_i16(slli_u16<1>(c), c);

    Vec32xU16 even = srli_u16<2>(add_i16(add_i16(c3, l), bias));
    Vec32xU16 odd = srli_u16<2>(add_i16(add_i16(c3, r), bias));

    Vec32xU16 pairs_lo = vec_u16(interleave_lo_u16(even, odd));
    Vec32xU16 pairs_hi = vec_u16(interleave_hi_u16(even, odd));
    Vec64xU8 result = vec_u8(packs_128_i16_u8(vec_i16(pairs_lo), vec_i16(pairs_hi)));

    storeu(dst + i * 2, result);
  }

  // Scalar tail for remaining interior pixels.
  for (; i < w - 1; i++) {
    uint32_t n = 3 * src0[i] + 2;
    dst[i * 2 + 0] = uint8_t((n + src0[i - 1]) >> 2);
    dst[i * 2 + 1] = uint8_t((n + src0[i + 1]) >> 2);
  }

  // Handle last pixel (no right neighbor).
  dst[i * 2 + 0] = uint8_t((src0[w - 2] * 3 + src0[w - 1] + 2) >> 2);
  dst[i * 2 + 1] = src0[w - 1];

  return dst;
}

// bl::Jpeg - Upsample 2x2 - AVX-512
// ==================================
//
// Separable 2D 3:1 filter (vertical then horizontal).
// Processes 32 input positions -> 64 output bytes per iteration.

uint8_t* BL_CDECL upsample_2x2_avx512(uint8_t* dst, uint8_t* src0, uint8_t* src1, uint32_t w, uint32_t hs) noexcept {
  using namespace SIMD;
  bl_unused(hs);

  if (w == 1) {
    dst[0] = dst[1] = uint8_t((3 * src0[0] + src1[0] + 2) >> 2);
    return dst;
  }

  const Vec32xU16 bias = make512_u16(uint16_t(8));

  // Handle first output pixel.
  uint32_t t1_scalar = 3 * src0[0] + src1[0];
  dst[0] = uint8_t((t1_scalar + 2) >> 2);

  // Main loop: 32 input positions -> 64 output bytes per iteration.
  uint32_t i = 1;
  for (; i + 32 <= w; i += 32) {
    Vec32xU16 s0c = loadu_256_u8_u16<Vec32xU16>(src0 + i);
    Vec32xU16 s1c = loadu_256_u8_u16<Vec32xU16>(src1 + i);
    Vec32xU16 s0p = loadu_256_u8_u16<Vec32xU16>(src0 + i - 1);
    Vec32xU16 s1p = loadu_256_u8_u16<Vec32xU16>(src1 + i - 1);

    // Vertical blend: t = 3*s0 + s1.
    Vec32xU16 t_cur = add_i16(add_i16(slli_u16<1>(s0c), s0c), s1c);
    Vec32xU16 t_prev = add_i16(add_i16(slli_u16<1>(s0p), s0p), s1p);

    // Horizontal blend: 3*t + t_neighbor.
    Vec32xU16 t_cur3 = add_i16(slli_u16<1>(t_cur), t_cur);
    Vec32xU16 t_prev3 = add_i16(slli_u16<1>(t_prev), t_prev);

    Vec32xU16 val_odd = srli_u16<4>(add_i16(add_i16(t_prev3, t_cur), bias));
    Vec32xU16 val_even = srli_u16<4>(add_i16(add_i16(t_cur3, t_prev), bias));

    Vec32xU16 pairs_lo = vec_u16(interleave_lo_u16(val_odd, val_even));
    Vec32xU16 pairs_hi = vec_u16(interleave_hi_u16(val_odd, val_even));
    Vec64xU8 result = vec_u8(packs_128_i16_u8(vec_i16(pairs_lo), vec_i16(pairs_hi)));

    storeu(dst + i * 2 - 1, result);
  }

  // Scalar tail.
  {
    uint32_t t0;
    uint32_t t1_val;

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

#endif // BL_TARGET_OPT_AVX512
