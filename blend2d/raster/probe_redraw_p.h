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

#endif // BLEND2D_RASTER_PROBE_REDRAW_P_H_INCLUDED
