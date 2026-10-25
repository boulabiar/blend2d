// Plain input record used by the optional native redraw fixture.
// Its fields mirror the benchmark input ABI; no importer or external renderer is linked.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const float *points;  size_t pointCount;        // Floats, x y pairs
    const uint8_t *types;  size_t typeCount;        // Ra::Geometry::Type per point slot
    const float *clipPoints;  size_t clipPointCount;
    const uint8_t *clipTypes;  size_t clipTypeCount;
    float ctm[6];  float width;  uint32_t flags, blendMode;
    float clip[4];      // The draw's clip bounds, in scene space, or huge
    float bounds[4];    // The path's bounds
    uint32_t paintType;  uint8_t color[4];      // r, g, b, a
    const uint8_t *stops;  const float *locations;  size_t stopCount;  float paintCtm[6];   // Gradients, stops BGRA
    const uint8_t *imagePixels;  uint32_t imageWidth, imageHeight;                          // Images, BGRA premultiplied
} ReviewDrawInfo;
