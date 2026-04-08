// This file is part of Blend2D project <https://blend2d.com>
//
// See blend2d.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#ifndef BLEND2D_PIXELOPS_GRADIENTFILL_P_H_INCLUDED
#define BLEND2D_PIXELOPS_GRADIENTFILL_P_H_INCLUDED

#include <blend2d/core/api-internal_p.h>
#include <blend2d/pipeline/pipedefs_p.h>

//! \cond INTERNAL
//! \addtogroup blend2d_internal
//! \{

namespace bl::PixelOps::GradientFill {

//! Fill a rectangular region with a linear gradient (pad extend mode, PRGB32).
//!
//! Uses the precomputed LUT from `gradient->lut.data`. The fill writes directly
//! to the destination buffer, bypassing the JIT pipeline.
//!
//! Parameters:
//!   dst_data   - pointer to the first pixel of the fill region in the destination
//!   dst_stride - destination stride in bytes
//!   x0, y0     - origin of the fill region (used to compute gradient offset)
//!   w, h       - width and height of the fill region
//!   gradient   - precomputed gradient data (LUT + linear parameters)
typedef void (BL_CDECL* FillLinearFunc)(
    uint8_t* dst_data, intptr_t dst_stride,
    uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
    const Pipeline::FetchData::Gradient* gradient) noexcept;

//! Scalar reference implementation.
BL_HIDDEN void BL_CDECL fill_linear_pad_prgb32(
    uint8_t* dst_data, intptr_t dst_stride,
    uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
    const Pipeline::FetchData::Gradient* gradient) noexcept;

#ifdef BL_BUILD_OPT_AVX2
BL_HIDDEN void BL_CDECL fill_linear_pad_prgb32_avx2(
    uint8_t* dst_data, intptr_t dst_stride,
    uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
    const Pipeline::FetchData::Gradient* gradient) noexcept;
#endif

} // {bl::PixelOps::GradientFill}

//! \}
//! \endcond

#endif // BLEND2D_PIXELOPS_GRADIENTFILL_P_H_INCLUDED
