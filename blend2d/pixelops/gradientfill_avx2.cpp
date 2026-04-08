// This file is part of Blend2D project <https://blend2d.com>
//
// See blend2d.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#include <blend2d/core/api-build_p.h>
#ifdef BL_TARGET_OPT_AVX2

#include <blend2d/simd/simd_p.h>
#include <blend2d/pipeline/pipedefs_p.h>
#include <blend2d/pixelops/gradientfill_p.h>
#include <blend2d/support/memops_p.h>

namespace bl::PixelOps::GradientFill {

// bl::PixelOps - Linear Gradient Fill - AVX2
// ==========================================
//
// Fills a rectangular region with a linear gradient using the precomputed
// LUT. Instead of per-pixel gather (as the JIT pipeline does), this uses
// vpermd to select LUT entries from a register-resident window.
//
// For a 32-pixel tile row, the gradient typically spans <= 8 distinct LUT
// entries. One vpermd selects 8 output pixels from 8 loaded LUT entries
// in a single instruction.
//
// Three code paths based on the LUT index range within each row:
//   range < 8:  1x vpermd from 1 YMM (common case, branchless inner loop)
//   range < 16: 2x vpermd + blend (moderate gradients)
//   range >= 16: per-group LUT load (steep gradients)

void BL_CDECL fill_linear_pad_prgb32_avx2(
    uint8_t* dst_data, intptr_t dst_stride,
    uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
    const Pipeline::FetchData::Gradient* gradient) noexcept {

  using namespace SIMD;

  const uint32_t* lut = static_cast<const uint32_t*>(gradient->lut.data);
  uint32_t maxi = gradient->linear.maxi;

  // Gradient parameters in 32.32 fixed-point.
  uint64_t dt = gradient->linear.dt.u64;   // per-pixel step in x
  uint64_t dy = gradient->linear.dy.u64;   // per-row step in y

  // Starting position for pixel (x0, y0).
  uint64_t pt_start = gradient->linear.pt[0].u64
                     + uint64_t(x0) * dt
                     + uint64_t(y0) * dy;

  // Precompute index offsets for 8-pixel groups.
  uint32_t offsets8[8];
  for (int i = 0; i < 8; i++)
    offsets8[i] = uint32_t((uint64_t(i) * dt) >> 32);

  Vec8xU32 v_offsets = loadu<Vec8xU32>(offsets8);
  Vec8xU32 v_step8 = make256_u32(offsets8[7] + uint32_t((dt) >> 32) - offsets8[0] + (offsets8[0])); // = offset[8]
  // Simpler: step8 = index at pixel 8 - index at pixel 0
  uint32_t step8 = uint32_t((8 * dt) >> 32);
  v_step8 = make256_u32(step8);

  Vec8xU32 v_maxi = make256_u32(maxi);

  // Determine per-group range (constant for entire fill).
  uint32_t group_range = offsets8[7]; // range within any 8-pixel group

  uint64_t pt_row = pt_start;
  uint32_t* dst = reinterpret_cast<uint32_t*>(dst_data);
  intptr_t dst_stride_px = dst_stride / 4;

  for (uint32_t y = 0; y < h; y++) {
    uint32_t row_base = uint32_t(pt_row >> 32);

    // Compute the total range of LUT indices for this row.
    uint32_t row_min = row_base > maxi ? maxi : row_base;
    uint32_t row_end = row_base + uint32_t(((uint64_t)(w - 1) * dt) >> 32);
    if (row_end > maxi) row_end = maxi;
    uint32_t row_range = row_end - row_min;

    Vec8xU32 v_base = add_i32(make256_u32(row_base), v_offsets);

    if (row_range < 8) {
      // Entire row fits in 1 YMM of LUT data.
      Vec8xU32 lut_reg = loadu<Vec8xU32>(lut + row_min);
      Vec8xU32 v_rel = sub_i32(min_u32(v_base, v_maxi), make256_u32(row_min));
      Vec8xU32 v_rel_max = make256_u32(maxi - row_min);

      for (uint32_t x = 0; x + 8 <= w; x += 8) {
        Vec8xU32 clamped = min_u32(v_rel, v_rel_max);
        storeu(dst + y * dst_stride_px + x,
               vec_u8(permute_u32_var(lut_reg, clamped)));
        v_rel = add_i32(v_rel, v_step8);
      }

      // Scalar tail.
      uint64_t pt = pt_row + uint64_t(w & ~7u) * dt;
      for (uint32_t x = w & ~7u; x < w; x++) {
        uint32_t idx = uint32_t(pt >> 32);
        if (idx > maxi) idx = maxi;
        dst[y * dst_stride_px + x] = lut[idx];
        pt += dt;
      }
    }
    else if (row_range < 16) {
      // Row fits in 2 YMMs.
      Vec8xU32 lut0 = loadu<Vec8xU32>(lut + row_min);
      Vec8xU32 lut1 = loadu<Vec8xU32>(lut + row_min + 8);
      Vec8xU32 v_row_min = make256_u32(row_min);
      Vec8xU32 v_rel = sub_i32(min_u32(v_base, v_maxi), v_row_min);
      Vec8xU32 v_rel_max = make256_u32(maxi - row_min);
      Vec8xI32 v_seven = make256_i32(7);
      Vec8xU32 v_eight = make256_u32(8);

      for (uint32_t x = 0; x + 8 <= w; x += 8) {
        Vec8xU32 clamped = min_u32(v_rel, v_rel_max);
        Vec8xU32 r0 = permute_u32_var(lut0, clamped);
        Vec8xU32 r1 = permute_u32_var(lut1, sub_i32(clamped, v_eight));
        Vec8xI32 mask = cmp_gt_i32(vec_i32(clamped), v_seven);
        storeu(dst + y * dst_stride_px + x,
               vec_u8(blendv_u8(vec_u8(r0), vec_u8(r1), vec_u8(mask))));
        v_rel = add_i32(v_rel, v_step8);
      }

      uint64_t pt = pt_row + uint64_t(w & ~7u) * dt;
      for (uint32_t x = w & ~7u; x < w; x++) {
        uint32_t idx = uint32_t(pt >> 32);
        if (idx > maxi) idx = maxi;
        dst[y * dst_stride_px + x] = lut[idx];
        pt += dt;
      }
    }
    else {
      // Wide range: load LUT window per 8-pixel group.
      for (uint32_t x = 0; x + 8 <= w; x += 8) {
        Vec8xU32 v_idx = min_u32(v_base, v_maxi);

        if (group_range < 8) {
          uint32_t min_idx = row_base + uint32_t(((uint64_t)x * dt) >> 32);
          if (min_idx > maxi) min_idx = maxi;

          Vec8xU32 lut_chunk = loadu<Vec8xU32>(lut + min_idx);
          Vec8xU32 v_rel = sub_i32(v_idx, make256_u32(min_idx));
          storeu(dst + y * dst_stride_px + x,
                 vec_u8(permute_u32_var(lut_chunk, v_rel)));
        }
        else {
          // Scalar fallback for very steep gradients.
          uint32_t indices[8];
          storeu(indices, v_idx);
          uint32_t pixels[8];
          for (int i = 0; i < 8; i++)
            pixels[i] = lut[indices[i]];
          storeu(dst + y * dst_stride_px + x, loadu<Vec32xU8>(pixels));
        }

        v_base = add_i32(v_base, v_step8);
      }

      uint64_t pt = pt_row + uint64_t(w & ~7u) * dt;
      for (uint32_t x = w & ~7u; x < w; x++) {
        uint32_t idx = uint32_t(pt >> 32);
        if (idx > maxi) idx = maxi;
        dst[y * dst_stride_px + x] = lut[idx];
        pt += dt;
      }
    }

    pt_row += dy;
  }
}

} // {bl::PixelOps::GradientFill}

#endif // BL_TARGET_OPT_AVX2
