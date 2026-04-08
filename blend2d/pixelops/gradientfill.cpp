// This file is part of Blend2D project <https://blend2d.com>
//
// See blend2d.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#include <blend2d/core/api-build_p.h>
#include <blend2d/pixelops/gradientfill_p.h>
#include <blend2d/support/memops_p.h>

namespace bl::PixelOps::GradientFill {

// bl::PixelOps - Linear Gradient Fill - Scalar Reference
// ======================================================
//
// Fills a rectangular region with a linear gradient using the precomputed LUT.
// Per-pixel: compute 32.32 fixed-point index, clamp to [0, maxi], fetch from LUT.

void BL_CDECL fill_linear_pad_prgb32(
    uint8_t* dst_data, intptr_t dst_stride,
    uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
    const Pipeline::FetchData::Gradient* gradient) noexcept {

  const uint32_t* lut = static_cast<const uint32_t*>(gradient->lut.data);
  uint32_t maxi = gradient->linear.maxi;

  uint64_t dt = gradient->linear.dt.u64;
  uint64_t dy = gradient->linear.dy.u64;
  uint64_t pt_start = gradient->linear.pt[0].u64
                     + uint64_t(x0) * dt
                     + uint64_t(y0) * dy;

  for (uint32_t y = 0; y < h; y++) {
    uint32_t* row = reinterpret_cast<uint32_t*>(dst_data + y * dst_stride);
    uint64_t pt = pt_start + uint64_t(y) * dy;

    for (uint32_t x = 0; x < w; x++) {
      uint32_t idx = uint32_t(pt >> 32);
      if (idx > maxi) idx = maxi;
      row[x] = lut[idx];
      pt += dt;
    }
  }
}

} // {bl::PixelOps::GradientFill}
