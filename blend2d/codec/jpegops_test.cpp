// This file is part of Blend2D project <https://blend2d.com>
//
// See blend2d.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#include <blend2d/core/api-build_test_p.h>
#if defined(BL_TEST)

#include <blend2d/core/random.h>
#include <blend2d/core/runtime_p.h>
#include <blend2d/codec/jpegops_p.h>
#include <blend2d/support/memops_p.h>

namespace bl::Jpeg::Tests {

// bl::Jpeg - Upsample - Tests
// ===========================

static void test_upsample_1x2(
  uint8_t* (BL_CDECL* ref_fn)(uint8_t*, uint8_t*, uint8_t*, uint32_t, uint32_t),
  uint8_t* (BL_CDECL* opt_fn)(uint8_t*, uint8_t*, uint8_t*, uint32_t, uint32_t),
  const char* impl_name
) noexcept {
  INFO("Testing upsample_1x2 [%s]", impl_name);

  BLRandom rnd(0x1234ABCD5678EF01u);

  constexpr uint32_t kMaxWidth = 260;
  uint8_t src0[kMaxWidth];
  uint8_t src1[kMaxWidth];
  uint8_t dst_ref[kMaxWidth];
  uint8_t dst_opt[kMaxWidth];

  static const uint32_t test_widths[] = { 1, 2, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65, 128, 255, 256 };

  for (uint32_t wi = 0; wi < BL_ARRAY_SIZE(test_widths); wi++) {
    uint32_t w = test_widths[wi];

    for (uint32_t trial = 0; trial < 100; trial++) {
      for (uint32_t j = 0; j < w; j++) {
        src0[j] = uint8_t(rnd.next_uint32() >> 24u);
        src1[j] = uint8_t(rnd.next_uint32() >> 24u);
      }

      memset(dst_ref, 0xCC, sizeof(dst_ref));
      memset(dst_opt, 0xCC, sizeof(dst_opt));

      ref_fn(dst_ref, src0, src1, w, 0);
      opt_fn(dst_opt, src0, src1, w, 0);

      EXPECT_TRUE(memcmp(dst_ref, dst_opt, w) == 0)
        .message("upsample_1x2 [%s] mismatch: w=%u trial=%u", impl_name, w, trial);
    }
  }
}

static void test_upsample_2x1(
  uint8_t* (BL_CDECL* ref_fn)(uint8_t*, uint8_t*, uint8_t*, uint32_t, uint32_t),
  uint8_t* (BL_CDECL* opt_fn)(uint8_t*, uint8_t*, uint8_t*, uint32_t, uint32_t),
  const char* impl_name
) noexcept {
  INFO("Testing upsample_2x1 [%s]", impl_name);

  BLRandom rnd(0xABCD1234EF015678u);

  constexpr uint32_t kMaxWidth = 260;
  uint8_t src0[kMaxWidth];
  uint8_t dst_ref[kMaxWidth * 2];
  uint8_t dst_opt[kMaxWidth * 2];

  static const uint32_t test_widths[] = { 1, 2, 7, 8, 15, 16, 17, 31, 32, 33, 64, 128, 256 };

  for (uint32_t wi = 0; wi < BL_ARRAY_SIZE(test_widths); wi++) {
    uint32_t w = test_widths[wi];

    for (uint32_t trial = 0; trial < 100; trial++) {
      for (uint32_t j = 0; j < w; j++)
        src0[j] = uint8_t(rnd.next_uint32() >> 24u);

      memset(dst_ref, 0xCC, sizeof(dst_ref));
      memset(dst_opt, 0xCC, sizeof(dst_opt));

      ref_fn(dst_ref, src0, nullptr, w, 0);
      opt_fn(dst_opt, src0, nullptr, w, 0);

      uint32_t ow = w * 2;
      EXPECT_TRUE(memcmp(dst_ref, dst_opt, ow) == 0)
        .message("upsample_2x1 [%s] mismatch: w=%u trial=%u", impl_name, w, trial);
    }
  }
}

static void test_upsample_2x2(
  uint8_t* (BL_CDECL* ref_fn)(uint8_t*, uint8_t*, uint8_t*, uint32_t, uint32_t),
  uint8_t* (BL_CDECL* opt_fn)(uint8_t*, uint8_t*, uint8_t*, uint32_t, uint32_t),
  const char* impl_name
) noexcept {
  INFO("Testing upsample_2x2 [%s]", impl_name);

  BLRandom rnd(0xEF015678ABCD1234u);

  constexpr uint32_t kMaxWidth = 260;
  uint8_t src0[kMaxWidth];
  uint8_t src1[kMaxWidth];
  uint8_t dst_ref[kMaxWidth * 2];
  uint8_t dst_opt[kMaxWidth * 2];

  static const uint32_t test_widths[] = { 1, 2, 7, 8, 15, 16, 17, 31, 32, 33, 64, 128, 256 };

  for (uint32_t wi = 0; wi < BL_ARRAY_SIZE(test_widths); wi++) {
    uint32_t w = test_widths[wi];

    for (uint32_t trial = 0; trial < 100; trial++) {
      for (uint32_t j = 0; j < w; j++) {
        src0[j] = uint8_t(rnd.next_uint32() >> 24u);
        src1[j] = uint8_t(rnd.next_uint32() >> 24u);
      }

      memset(dst_ref, 0xCC, sizeof(dst_ref));
      memset(dst_opt, 0xCC, sizeof(dst_opt));

      ref_fn(dst_ref, src0, src1, w, 0);
      opt_fn(dst_opt, src0, src1, w, 0);

      uint32_t ow = w * 2;
      EXPECT_TRUE(memcmp(dst_ref, dst_opt, ow) == 0)
        .message("upsample_2x2 [%s] mismatch: w=%u trial=%u", impl_name, w, trial);
    }
  }
}

UNIT(codec_jpeg_upsample, BL_TEST_GROUP_IMAGE_CODEC_OPS) {
#ifdef BL_BUILD_OPT_AVX2
  if (bl_runtime_has_avx2(&bl_runtime_context)) {
    test_upsample_1x2(upsample_1x2, upsample_1x2_avx2, "AVX2");
    test_upsample_2x1(upsample_2x1, upsample_2x1_avx2, "AVX2");
    test_upsample_2x2(upsample_2x2, upsample_2x2_avx2, "AVX2");
  }
#endif

#ifdef BL_BUILD_OPT_AVX512
  if (bl_runtime_has_avx512(&bl_runtime_context)) {
    test_upsample_1x2(upsample_1x2, upsample_1x2_avx512, "AVX-512");
    test_upsample_2x1(upsample_2x1, upsample_2x1_avx512, "AVX-512");
    test_upsample_2x2(upsample_2x2, upsample_2x2_avx512, "AVX-512");
  }
#endif
}

} // {bl::Jpeg::Tests}

#endif // BL_TEST
