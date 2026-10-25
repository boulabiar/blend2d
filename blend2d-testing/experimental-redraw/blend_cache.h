#pragma once
#include <blend2d/blend2d.h>
#include <cstddef>
struct BlProbeStrokePart {const BLPathCore* a;const BLPathCore* b;const BLPathCore* c;};
struct BlProbeStrokeView {const BlProbeStrokePart* parts;size_t count;};
#if defined(__GNUC__)
extern "C" BLResult bl_probe_stroke_cached(BLContextCore*,const BLPathCore*,const void*,const void*) noexcept __attribute__((weak));
#else
static constexpr BLResult (*bl_probe_stroke_cached)(BLContextCore*,const BLPathCore*,const void*,const void*) noexcept = nullptr;
#endif
struct BlProbePackedSpan {uint32_t offset,count;};
struct BlProbePackedPart {BlProbePackedSpan a,b,c;};
struct BlProbePackedView {const BLPoint* vertices;const uint8_t* commands;const BlProbePackedPart* parts;size_t count;};
#if defined(__GNUC__)
extern "C" BLResult bl_probe_stroke_packed(BLContextCore*,const BLPathCore*,const void*,uint32_t,uint32_t,const void*) noexcept __attribute__((weak));
#else
static constexpr BLResult (*bl_probe_stroke_packed)(BLContextCore*,const BLPathCore*,const void*,uint32_t,uint32_t,const void*) noexcept = nullptr;
#endif

#if defined(__GNUC__)
extern "C" BLResult bl_probe_set_async_path_limit(BLContextCore*,uint32_t) noexcept __attribute__((weak));
#else
static constexpr BLResult (*bl_probe_set_async_path_limit)(BLContextCore*,uint32_t) noexcept = nullptr;
#endif
#if defined(__GNUC__)
extern "C" BLResult bl_probe_geometry_borrowed(BLContextCore*,const BLPathCore*,uint32_t,const void*) noexcept __attribute__((weak));
#else
static constexpr BLResult (*bl_probe_geometry_borrowed)(BLContextCore*,const BLPathCore*,uint32_t,const void*) noexcept = nullptr;
#endif
#if defined(__GNUC__)
extern "C" BLResult bl_probe_record_begin(BLContextCore*) noexcept __attribute__((weak));
extern "C" BLResult bl_probe_record_end(BLContextCore*) noexcept __attribute__((weak));
extern "C" BLResult bl_probe_record_replay(BLContextCore*) noexcept __attribute__((weak));
#else
static constexpr BLResult (*bl_probe_record_begin)(BLContextCore*) noexcept = nullptr;
static constexpr BLResult (*bl_probe_record_end)(BLContextCore*) noexcept = nullptr;
static constexpr BLResult (*bl_probe_record_replay)(BLContextCore*) noexcept = nullptr;
#endif
