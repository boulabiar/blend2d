// This file is part of Blend2D project <https://blend2d.com>
//
// See blend2d.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#include <blend2d/core/api-build_test_p.h>
#if defined(BL_TEST)

#include <blend2d/core/context.h>
#include <blend2d/core/gradient.h>
#include <blend2d/core/image.h>
#include <blend2d/core/random.h>
#include <blend2d/core/runtime_p.h>
#include <blend2d/pipeline/pipedefs_p.h>
#include <blend2d/pixelops/funcs_p.h>
#include <blend2d/pixelops/gradientfill_p.h>

namespace bl::PixelOps::GradientFill::Tests {

// Compare the AVX2 gradient fill output against the scalar reference.
// Both use the same precomputed LUT, so output must be bit-exact.
static void test_fill_matches_scalar(const char* impl_name) {
  INFO("Testing linear gradient fill [%s] vs scalar reference", impl_name);

  // Build a 3-stop gradient LUT using Blend2D's own interpolation.
  static constexpr uint32_t kLutSize = 256;
  uint32_t lut[kLutSize];

  BLGradientStop stops[3];
  stops[0].offset = 0.0;
  stops[0].rgba = BLRgba64(0xFFFF, 0, 0, 0xFFFF);   // red
  stops[1].offset = 0.5;
  stops[1].rgba = BLRgba64(0, 0, 0xFFFF, 0xFFFF);    // blue
  stops[2].offset = 1.0;
  stops[2].rgba = BLRgba64(0, 0xFFFF, 0, 0xFFFF);    // green

  PixelOps::funcs.interpolate_prgb32(lut, kLutSize, stops, 3);

  // Test multiple image widths and gradient configurations.
  static const uint32_t test_widths[] = {32, 64, 128, 256, 512, 1024};
  static const uint32_t kHeight = 32;

  for (uint32_t wi = 0; wi < BL_ARRAY_SIZE(test_widths); wi++) {
    uint32_t w = test_widths[wi];

    Pipeline::FetchData::Gradient gradient;
    memset(&gradient, 0, sizeof(gradient));

    gradient.lut.data = lut;
    gradient.lut.size = kLutSize;
    gradient.linear.dt.u64 = ((uint64_t)kLutSize << 32) / w;
    gradient.linear.dy.u64 = 0;
    gradient.linear.pt[0].u64 = gradient.linear.dt.u64 / 2;  // pixel-center offset
    gradient.linear.maxi = kLutSize - 1;
    gradient.linear.rori = 0;

    // Allocate destination buffers.
    intptr_t stride = intptr_t(w) * 4;
    uint8_t* dst_ref = static_cast<uint8_t*>(malloc(kHeight * stride));
    uint8_t* dst_opt = static_cast<uint8_t*>(malloc(kHeight * stride));

    EXPECT_NOT_NULL(dst_ref);
    EXPECT_NOT_NULL(dst_opt);

    memset(dst_ref, 0xCC, kHeight * stride);
    memset(dst_opt, 0xCC, kHeight * stride);

    // Run scalar reference.
    fill_linear_pad_prgb32(dst_ref, stride, 0, 0, w, kHeight, &gradient);

    // Run optimized (whatever is dispatched via funcs).
    PixelOps::funcs.fill_linear_pad_prgb32(dst_opt, stride, 0, 0, w, kHeight, &gradient);

    // Compare pixel-by-pixel.
    EXPECT_TRUE(memcmp(dst_ref, dst_opt, kHeight * stride) == 0)
      .message("Linear gradient fill [%s] mismatch: w=%u h=%u", impl_name, w, kHeight);

    free(dst_ref);
    free(dst_opt);
  }
}

UNIT(pixelops_gradient_fill, BL_TEST_GROUP_IMAGE_PIXEL_OPS) {
#ifdef BL_BUILD_OPT_AVX2
  if (bl_runtime_has_avx2(&bl_runtime_context)) {
    test_fill_matches_scalar("AVX2");
  }
#endif
}

} // {bl::PixelOps::GradientFill::Tests}

#endif // BL_TEST
