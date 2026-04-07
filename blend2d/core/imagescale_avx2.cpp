// This file is part of Blend2D project <https://blend2d.com>
//
// See blend2d.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#include <blend2d/core/api-build_p.h>
#ifdef BL_TARGET_OPT_AVX2

#include <blend2d/core/imagescale_p.h>
#include <blend2d/core/format_p.h>
#include <blend2d/core/rgba_p.h>
#include <blend2d/simd/simd_p.h>
#include <blend2d/support/memops_p.h>

namespace bl {

// bl::ImageScale - Vert PRGB32 - AVX2
// ====================================
//
// The vertical scaler blends `count` source rows into one output row using
// fixed-point weights. The key insight is that the weight for a given kernel
// row is the SAME for every x position, so we can process 8 pixels (32 bytes)
// at a time across the width.
//
// Strategy: flip the loop order vs the scalar code. The outer loop iterates
// over x (8 pixels at a time), the inner loop over kernel rows. For each
// kernel row we broadcast the weight, load 8 contiguous pixels, widen u8->u16,
// and multiply-accumulate into 16-bit accumulators.
//
// Precision: channel (0-255) x weight (max 256) = max 65,280, plus bias 128
// = 65,408. This fits in u16 (max 65,535), so 16-bit accumulation is safe
// for the bound (non-unbound) case where all weights are non-negative.

void BL_CDECL image_scale_vert_prgb32_avx2(const ImageScaleContext::Data* d, uint8_t* dst_line, intptr_t dst_stride, const uint8_t* src_line, intptr_t src_stride) noexcept {
  using namespace SIMD;

  uint32_t dw = uint32_t(d->dst_size[0]);
  uint32_t dh = uint32_t(d->dst_size[1]);
  uint32_t kernel_size = uint32_t(d->kernel_size[ImageScaleContext::kDirVert]);

  const ImageScaleContext::Record* record_list = d->record_list[ImageScaleContext::kDirVert];
  const int32_t* weight_list = d->weight_list[ImageScaleContext::kDirVert];

  // Rounding bias: 0x0080 per u16 channel.
  Vec16xU16 bias = make256_u16(uint16_t(0x0080));

  if (!d->is_unbound[ImageScaleContext::kDirVert]) {
    for (uint32_t y = 0; y < dh; y++) {
      const uint8_t* src_data = src_line + intptr_t(record_list->pos) * src_stride;
      uint8_t* dp = dst_line;

      uint32_t count = record_list->count;

      // --- AVX2 main loop: 8 pixels (32 bytes) per iteration ---
      uint32_t x = 0;
      for (; x + 8 <= dw; x += 8) {
        Vec16xU16 acc_lo = bias;  // channels for pixels 0-3 (16 x u16)
        Vec16xU16 acc_hi = bias;  // channels for pixels 4-7 (16 x u16)

        const uint8_t* sp = src_data + x * 4;
        const int32_t* wp = weight_list;

        for (uint32_t i = 0; i < count; i++) {
          // Broadcast the weight for this kernel row to all 16 u16 lanes.
          Vec16xU16 w = make256_u16(uint16_t(wp[i]));

          // Load 8 contiguous ARGB32 pixels, widen each half u8->u16.
          Vec16xU16 lo = loadu_128_u8_u16<Vec16xU16>(sp);
          Vec16xU16 hi = loadu_128_u8_u16<Vec16xU16>(sp + 16);

          // Multiply-accumulate: acc += channel * weight.
          acc_lo = add_i16(acc_lo, mul_u16(lo, w));
          acc_hi = add_i16(acc_hi, mul_u16(hi, w));

          sp += src_stride;
        }

        // Normalize: shift right by 8 to convert fixed-point to u8 range.
        acc_lo = srli_u16<8>(acc_lo);
        acc_hi = srli_u16<8>(acc_hi);

        // Pack u16 -> u8 with unsigned saturation.
        // packs_128 operates within 128-bit lanes, producing interleaved order.
        Vec32xU8 result = vec_u8(packs_128_i16_u8(vec_i16(acc_lo), vec_i16(acc_hi)));

        // Fix the lane interleaving: permute qwords to [0,2,1,3] order.
        result = vec_u8(permute_i64<3, 1, 2, 0>(vec_i64(result)));

        storeu(dp + x * 4, result);
      }

      // --- Scalar tail for remaining pixels ---
      for (; x < dw; x++) {
        const uint8_t* sp = src_data + x * 4;
        const int32_t* wp = weight_list;

        uint32_t cr_cb = 0x00800080;
        uint32_t ca_cg = 0x00800080;

        for (uint32_t i = 0; i < count; i++) {
          uint32_t p0 = MemOps::readU32a(sp);
          uint32_t w0 = unsigned(wp[i]);

          ca_cg += ((p0 >> 8) & 0x00FF00FF) * w0;
          cr_cb += ((p0     ) & 0x00FF00FF) * w0;

          sp += src_stride;
        }

        MemOps::writeU32a(dp + x * 4, (ca_cg & 0xFF00FF00) + ((cr_cb & 0xFF00FF00) >> 8));
      }

      record_list += 1;
      weight_list += kernel_size;
      dst_line += dst_stride;
    }
  }
  else {
    // Unbound case: weights can be negative (Lanczos), need i32 accumulators.
    for (uint32_t y = 0; y < dh; y++) {
      const uint8_t* src_data = src_line + intptr_t(record_list->pos) * src_stride;
      uint8_t* dp = dst_line;

      uint32_t count = record_list->count;
      uint32_t x = 0;

      Vec8xI32 i32_bias = make256_i32(0x80);

      for (; x + 4 <= dw; x += 4) {
        Vec8xI32 acc0 = i32_bias;  // pixels 0-1
        Vec8xI32 acc1 = i32_bias;  // pixels 2-3

        const uint8_t* sp = src_data + x * 4;
        const int32_t* wp = weight_list;

        for (uint32_t i = 0; i < count; i++) {
          Vec8xI32 w = make256_i32(wp[i]);

          // Load 4 pixels (16 bytes), widen u8 -> u16 -> u32.
          // movw_u16_u32 takes the low 128 bits and widens to 256-bit.
          // For the high half, swap the 128-bit lanes first.
          Vec16xU16 pix16 = loadu_128_u8_u16<Vec16xU16>(sp);

          Vec8xI32 pix32_lo = vec_i32(movw_u16_u32(pix16));
          Vec8xI32 pix32_hi = vec_i32(movw_u16_u32(vec_u16(permute_i64<1, 0, 3, 2>(vec_i64(pix16)))));

          // Signed multiply-accumulate in i32.
          acc0 = add_i32(acc0, mul_i32(pix32_lo, w));
          acc1 = add_i32(acc1, mul_i32(pix32_hi, w));

          sp += src_stride;
        }

        // Shift right by 8 (arithmetic for signed).
        acc0 = srai_i32<8>(acc0);
        acc1 = srai_i32<8>(acc1);

        // Pack i32 -> i16 -> u8 with saturation and fix lane order.
        Vec8xI32 packed32 = packs_128_i32_i16(acc0, acc1);
        packed32 = vec_i32(permute_i64<3, 1, 2, 0>(vec_i64(packed32)));
        Vec32xU8 packed8 = vec_u8(packs_128_i16_u8(vec_i16(packed32), vec_i16(packed32)));

        // Store 4 pixels (16 bytes from low 128 bits).
        storeu_128(dp + x * 4, packed8);
      }

      // Scalar tail.
      for (; x < dw; x++) {
        const uint8_t* sp = src_data + x * 4;
        const int32_t* wp = weight_list;

        int32_t ca = 0x80, cr = 0x80, cg = 0x80, cb = 0x80;

        for (uint32_t i = 0; i < count; i++) {
          uint32_t p0 = MemOps::readU32a(sp);
          int32_t w0 = wp[i];

          ca += int32_t((p0 >> 24)        ) * w0;
          cr += int32_t((p0 >> 16) & 0xFFu) * w0;
          cg += int32_t((p0 >>  8) & 0xFFu) * w0;
          cb += int32_t((p0      ) & 0xFFu) * w0;

          sp += src_stride;
        }

        ca = bl_clamp<int32_t>(ca >> 8, 0, 255);
        cr = bl_clamp<int32_t>(cr >> 8, 0, ca);
        cg = bl_clamp<int32_t>(cg >> 8, 0, ca);
        cb = bl_clamp<int32_t>(cb >> 8, 0, ca);

        MemOps::writeU32a(dp + x * 4, RgbaInternal::packRgba32(uint32_t(cr), uint32_t(cg), uint32_t(cb), uint32_t(ca)));
      }

      record_list += 1;
      weight_list += kernel_size;
      dst_line += dst_stride;
    }
  }
}

} // {bl}

#endif // BL_TARGET_OPT_AVX2
