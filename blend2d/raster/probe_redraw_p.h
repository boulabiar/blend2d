// This file is part of Blend2D project <https://blend2d.com>
//
// See blend2d.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

// Experimental fixed-document redraw hooks for the native benchmark.
#ifndef BLEND2D_RASTER_PROBE_REDRAW_P_H_INCLUDED
#define BLEND2D_RASTER_PROBE_REDRAW_P_H_INCLUDED

#include <blend2d/core/context.h>

namespace bl::RasterEngine {

struct ProbePackedSpan {
  uint32_t vertex_offset;
  uint32_t vertex_count;
  uint32_t control_offset;
  uint32_t control_size;
};

struct ProbePackedPart {
  ProbePackedSpan a, b, c;
};

struct ProbePackedView {
  const BLPoint* vertices;
  const uint8_t* controls;
  const ProbePackedPart* parts;
  size_t count;
};

} // {bl::RasterEngine}

// The immutable arrays must live through flush and all recorded replays.
extern "C" BLResult bl_probe_stroke_packed(BLContextCore* self, const bl::RasterEngine::ProbePackedView* view,
                                          uint32_t first, uint32_t count, const BLObjectCore* style) noexcept;

extern "C" BLResult bl_probe_geometry_borrowed(BLContextCore* self, const BLPathCore* path,
                                               uint32_t stroke, const BLObjectCore* style) noexcept;

extern "C" BLResult bl_probe_record_begin(BLContextCore* self) noexcept;
extern "C" BLResult bl_probe_record_end(BLContextCore* self) noexcept;
extern "C" BLResult bl_probe_record_replay(BLContextCore* self) noexcept;
extern "C" BLResult bl_probe_set_band_range(BLContextCore* self, uint32_t y0, uint32_t y1) noexcept;
extern "C" BLResult bl_probe_set_async_path_limit(BLContextCore* self, uint32_t byte_limit) noexcept;

#endif // BLEND2D_RASTER_PROBE_REDRAW_P_H_INCLUDED
