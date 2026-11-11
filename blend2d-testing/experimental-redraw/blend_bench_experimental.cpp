// Optional benchmark backend. Native paths/paints are retained, as Rasterizer
// retains its scene.
#include "review_draw.h"
#include <blend2d/blend2d.h>
#include <blend2d/raster/probe_redraw_p.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <memory>
using bl::RasterEngine::ProbePackedPart;
using bl::RasterEngine::ProbePackedSpan;
using bl::RasterEngine::ProbePackedView;
static void check(BLResult r) {
  if (r) {
    fprintf(stderr, "Blend2D error %u\n", r);
    std::abort();
  }
}
static BLMatrix2D matrix(const float *m) {
  return BLMatrix2D(m[0], m[1], m[2], m[3], m[4], m[5]);
}
struct Draw {
  BLPath path;
  BLVar paint;
  BLMatrix2D transform;
  BLRect clip;
  double width, opacity;
  uint32_t flags;
  uint32_t updates = 0;
  uint32_t packed_first = 0, packed_count = 0;
  bool packed = false;
};
struct TileGroup;
struct Bench {
  std::vector<Draw> draws;
  BLImage image;
  BLContext context;
  TileGroup *tiles = nullptr;
  BLRect base_clip;
  bool metrics = std::getenv("BLEND_PROBE_METRICS") != nullptr;
  bool prepared = std::getenv("BLEND_PROBE_PREPARED") && std::strcmp(std::getenv("BLEND_PROBE_PREPARED"), "1") == 0;
  unsigned borrow_mode =
      std::getenv("BLEND_PROBE_BORROW") ? unsigned(std::strtoul(std::getenv("BLEND_PROBE_BORROW"), nullptr, 10)) : 0;
  unsigned cache_mode = std::getenv("BLEND_PROBE_PRESTROKE")
                            ? unsigned(std::strtoul(std::getenv("BLEND_PROBE_PRESTROKE"), nullptr, 10))
                            : 0;
  bool record_requested = std::getenv("BLEND_PROBE_RECORD") && std::strcmp(std::getenv("BLEND_PROBE_RECORD"), "1") == 0;
  bool record_ready = false;
  bool asynchronous = false, reported_cache = false;
  size_t cached_count = 0, cache_source_entries = 0, cache_outline_entries = 0;
  std::vector<BLPoint> packed_vertices;
  std::vector<uint8_t> packed_controls;
  std::vector<ProbePackedPart> packed_parts;
  ProbePackedView packed_view{};
  const ProbePackedView *render_packed = &packed_view;
  size_t last_stroke = SIZE_MAX, last_fill = SIZE_MAX;
  unsigned long long render_count = 0, render_cpu_ns = 0;
};
static void tile_begin(Bench *, unsigned, unsigned);
static void tile_add(Bench *, const Draw &);
static void tile_render(Bench *, bool);
static void tile_free(Bench *);
static unsigned long long thread_cpu_ns() {
#ifdef __linux__
  timespec t{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return static_cast<unsigned long long>(t.tv_sec) * 1000000000ull + t.tv_nsec;
#else
  return 0;
#endif
}
static void configure_async_limit(BLContext &context) {
  if (const char *limit = std::getenv("BLEND_PROBE_ASYNC_LIMIT")) {
    check(bl_probe_set_async_path_limit(&context, uint32_t(std::strtoul(limit, nullptr, 10))));
  }
}
extern "C" void *bb_new_config(unsigned w, unsigned h, unsigned threads, unsigned queue_limit) {
  auto b = new Bench;
  b->asynchronous = threads != 0;
  check(b->image.create(w, h, BL_FORMAT_PRGB32));
  const char *tile_count = std::getenv("BLEND_PROBE_TILES");
  if (threads > 1 && h >= 64 && tile_count && std::strtoul(tile_count, nullptr, 10) > 1) {
    tile_begin(b, threads, queue_limit);
    return b;
  }
  BLContextCreateInfo info{};
  info.thread_count = threads;
  info.command_queue_limit = queue_limit;
  check(b->context.begin(b->image, &info));
  if (threads)
    configure_async_limit(b->context);
  if (threads && b->context.thread_count() != threads) {
    fprintf(stderr, "Requested %u threads, got %u\n", threads, b->context.thread_count());
    std::abort();
  }
  return b;
}
extern "C" void *bb_new(unsigned w, unsigned h, unsigned threads) {
  return bb_new_config(w, h, threads, 0);
}
enum Update : uint32_t { Clip = 1, Transform = 2, Alpha = 4, Width = 8, Caps = 16, Join = 32, Rule = 64 };
static void append_draw(Bench &bench, Draw draw) {
  if (!bench.prepared) {
    bench.draws.push_back(std::move(draw));
    return;
  }
  const Draw *previous = bench.draws.empty() ? nullptr : &bench.draws.back();
  bool clip = !previous || !(draw.clip == previous->clip);
  if (clip)
    draw.updates |= Clip;
  if (clip || !previous || !draw.transform.equals(previous->transform))
    draw.updates |= Transform;
  if (!previous || draw.opacity != previous->opacity)
    draw.updates |= Alpha;
  if (draw.width) {
    const Draw *stroke = bench.last_stroke == SIZE_MAX ? nullptr : &bench.draws[bench.last_stroke];
    if (!stroke || draw.width != stroke->width)
      draw.updates |= Width;
    if (!stroke || ((draw.flags ^ stroke->flags) & ((1 << 2) | (1 << 3))))
      draw.updates |= Caps;
    if (!stroke || ((draw.flags ^ stroke->flags) & (1 << 4)))
      draw.updates |= Join;
    bench.last_stroke = bench.draws.size();
  } else {
    const Draw *fill = bench.last_fill == SIZE_MAX ? nullptr : &bench.draws[bench.last_fill];
    if (!fill || ((draw.flags ^ fill->flags) & (1 << 1)))
      draw.updates |= Rule;
    bench.last_fill = bench.draws.size();
  }
  bench.draws.push_back(std::move(draw));
}
static BLResult BL_CDECL capture_packed_stroke(BLPathCore *a, BLPathCore *b, BLPathCore *c, const BLPathRegion *,
                                               void *data) noexcept {
  auto &bench = *static_cast<Bench *>(data);
  ProbePackedPart part;
  auto append = [&](const BLPath &path, ProbePackedSpan &span) {
    BLPathView view = path.view();
    if (view.vertex_count > UINT32_MAX - bench.packed_vertices.size() ||
        view.control_data_size > UINT32_MAX - bench.packed_controls.size())
      std::abort();
    span = {uint32_t(bench.packed_vertices.size()), uint32_t(view.vertex_count), uint32_t(bench.packed_controls.size()),
            uint32_t(view.control_data_size)};
    if (view.vertex_count) {
      bench.packed_vertices.insert(bench.packed_vertices.end(), view.vertex_data, view.vertex_data + view.vertex_count);
    }
    if (view.control_data_size) {
      bench.packed_controls.insert(bench.packed_controls.end(), view.control_data_rev - view.control_data_size,
                                   view.control_data_rev);
    }
  };
  append(a->dcast(), part.a);
  append(b->dcast(), part.b);
  append(c->dcast(), part.c);
  bench.packed_parts.push_back(part);
  return bl_path_clear(a);
}

static void prepare_stroke(Bench &bench, Draw &draw) {
  if (!bench.cache_mode || !bench.asynchronous || !draw.width)
    return;

  BLStrokeOptions options;
  check(options.set_width(float(draw.width)));
  check(options.set_miter_limit(3.863703305f));
  check(options.set_caps(draw.flags & (1 << 2)   ? BL_STROKE_CAP_ROUND
                         : draw.flags & (1 << 3) ? BL_STROKE_CAP_SQUARE
                                                 : BL_STROKE_CAP_BUTT));
  check(options.set_join(draw.flags & (1 << 4) ? BL_STROKE_JOIN_ROUND : BL_STROKE_JOIN_MITER_CLIP));

  BLPath a, b, c;
  size_t first = bench.packed_parts.size();
  size_t vertices = bench.packed_vertices.size();
  check(bl_path_stroke_to_sink(&draw.path, &options, nullptr, &a, &b, &c, capture_packed_stroke, &bench));
  if (bench.packed_parts.size() > UINT32_MAX)
    std::abort();
  draw.packed_first = uint32_t(first);
  draw.packed_count = uint32_t(bench.packed_parts.size() - first);
  draw.packed = true;
  bench.packed_view = {bench.packed_vertices.data(), bench.packed_controls.data(), bench.packed_parts.data(),
                       bench.packed_parts.size()};
  bench.cached_count++;
  bench.cache_source_entries += draw.path.vertex_count();
  bench.cache_outline_entries += bench.packed_vertices.size() - vertices;
}

static void report_cache(Bench &bench) {
  if (bench.cache_mode && !bench.reported_cache) {
    std::fprintf(stderr, "STROKE_CACHE paths=%zu source_vertices=%zu outline_vertices=%zu control_bytes=%zu\n",
                 bench.cached_count, bench.cache_source_entries, bench.cache_outline_entries,
                 bench.packed_controls.size());
    bench.reported_cache = true;
  }
}

extern "C" int bb_add(void *ptr, const ReviewDrawInfo *d, const float *m, const float *clip) {
  // No arbitrary path clipping in Blend2D's public context API. Reject whole
  // documents requiring it.
  if (d->clipTypeCount || d->blendMode || d->paintType > 3)
    return 1;
  Draw v;
  v.transform = matrix(m);
  v.flags = d->flags;
  v.clip = BLRect(clip[0], clip[1], std::max(0.f, clip[2] - clip[0]), std::max(0.f, clip[3] - clip[1]));
  size_t p = 0;
  for (size_t i = 0; i < d->typeCount;) {
    const float *q = d->points + p;
    switch (d->types[i]) {
    case 0:
      check(v.path.move_to(q[0], q[1]));
      ++i;
      p += 2;
      break;
    case 1:
      check(v.path.line_to(q[0], q[1]));
      ++i;
      p += 2;
      break;
    case 2:
      check(v.path.quad_to(q[0], q[1], q[2], q[3]));
      i += 2;
      p += 4;
      break;
    case 3:
      check(v.path.cubic_to(q[0], q[1], q[2], q[3], q[4], q[5]));
      i += 3;
      p += 6;
      break;
    default:
      check(v.path.close());
      ++i;
      p += 2;
      break;
    }
  }
  double scale = std::sqrt(std::abs(double(m[0]) * m[3] - double(m[1]) * m[2]));
  if (!(scale > 0))
    return 2;
  double pixels = d->width > 0 ? d->width * scale : -d->width;
  v.width = d->width == 0 ? 0 : std::max(1.0, pixels) / scale;
  v.opacity = d->width != 0 ? std::min(1.0, pixels) : 1;
  if (pixels <= 1)
    v.flags &= ~(1u << 2);
  if (d->paintType == 0)
    v.paint = BLVar(BLRgba32(d->color[0], d->color[1], d->color[2], d->color[3]));
  else if (d->paintType <= 2) {
    BLGradient g;
    if (d->paintType == 1)
      check(g.create(BLLinearGradientValues(0, 0, 0, 1)));
    else
      check(g.create(BLRadialGradientValues(0, 0, 0, 0, 1)));
    for (size_t k = 0; k < d->stopCount; k++) {
      auto c = d->stops + 4 * k;
      check(g.add_stop(d->locations[k], BLRgba32(c[2], c[1], c[0], c[3])));
    }
    check(g.set_transform(matrix(d->paintCtm)));
    v.paint = g;
  } else {
    BLImage image;
    check(image.create(d->imageWidth, d->imageHeight, BL_FORMAT_PRGB32));
    BLImageData data;
    check(image.get_data(&data));
    for (unsigned y = 0; y < d->imageHeight; y++)
      std::memcpy((char *)data.pixel_data + y * data.stride, d->imagePixels + y * d->imageWidth * 4, d->imageWidth * 4);
    BLPattern pat(image, BL_EXTEND_MODE_PAD);
    check(pat.set_transform(BLMatrix2D((d->bounds[2] - d->bounds[0]) / d->imageWidth, 0, 0,
                                       -(d->bounds[3] - d->bounds[1]) / d->imageHeight, d->bounds[0], d->bounds[3])));
    v.paint = pat;
  }
  auto *bench = static_cast<Bench *>(ptr);
  prepare_stroke(*bench, v);
  if (bench->tiles)
    tile_add(bench, v);
  else
    append_draw(*bench, std::move(v));
  return 0;
}
template <bool Cached, bool Prepared = false> static void render(void *ptr) {
  auto &b = *static_cast<Bench *>(ptr);
  const auto cpu_start = b.metrics ? thread_cpu_ns() : 0;
  auto &c = b.context;
  check(c.reset_transform());
  check(c.restore_clipping());
  check(c.set_global_alpha(1));
  check(c.set_comp_op(BL_COMP_OP_SRC_COPY));
  check(c.fill_all(BLRgba32(0xffffffff)));
  check(c.set_comp_op(BL_COMP_OP_SRC_OVER));
  check(c.set_stroke_miter_limit(3.863703305));
  const Draw *previous = nullptr, *previous_stroke = nullptr, *previous_fill = nullptr;
  for (const auto &d : b.draws) {
    bool clip_changed = Prepared ? bool(d.updates & Clip) : (!Cached || !previous || !(d.clip == previous->clip));
    if (clip_changed) {
      check(c.reset_transform());
      check(c.restore_clipping());
      check(c.clip_to_rect(d.clip));
    }
    if (Prepared ? bool(d.updates & Transform)
                 : (!Cached || clip_changed || !previous || !d.transform.equals(previous->transform)))
      check(c.set_transform(d.transform));
    if (Prepared ? bool(d.updates & Alpha) : (!Cached || !previous || d.opacity != previous->opacity))
      check(c.set_global_alpha(d.opacity));
    if (d.width) {
      if (Prepared ? bool(d.updates & Width) : (!Cached || !previous_stroke || d.width != previous_stroke->width))
        check(c.set_stroke_width(d.width));
      if (Prepared ? bool(d.updates & Caps)
                   : (!Cached || !previous_stroke || ((d.flags ^ previous_stroke->flags) & ((1 << 2) | (1 << 3)))))
        check(c.set_stroke_caps(d.flags & (1 << 2)   ? BL_STROKE_CAP_ROUND
                                : d.flags & (1 << 3) ? BL_STROKE_CAP_SQUARE
                                                     : BL_STROKE_CAP_BUTT));
      if (Prepared ? bool(d.updates & Join)
                   : (!Cached || !previous_stroke || ((d.flags ^ previous_stroke->flags) & (1 << 4))))
        check(c.set_stroke_join(d.flags & (1 << 4) ? BL_STROKE_JOIN_ROUND : BL_STROKE_JOIN_MITER_CLIP));
      if (d.packed)
        check(bl_probe_stroke_packed(&c, b.render_packed, d.packed_first, d.packed_count, &d.paint));
      else if (b.asynchronous && b.borrow_mode)
        check(bl_probe_geometry_borrowed(&c, &d.path, 1, &d.paint));
      else
        check(c.stroke_path(d.path, d.paint));
      previous_stroke = &d;
    } else {
      if (Prepared ? bool(d.updates & Rule)
                   : (!Cached || !previous_fill || ((d.flags ^ previous_fill->flags) & (1 << 1))))
        check(c.set_fill_rule(d.flags & (1 << 1) ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO));
      if (b.asynchronous && b.borrow_mode >= 2 && d.path.vertex_count() < 32768)
        check(bl_probe_geometry_borrowed(&c, &d.path, 0, &d.paint));
      else
        check(c.fill_path(d.path, d.paint));
      previous_fill = &d;
    }
    previous = &d;
  }
  check(c.flush(BL_CONTEXT_FLUSH_SYNC));
  if (b.metrics) {
    b.render_cpu_ns += thread_cpu_ns() - cpu_start;
    b.render_count++;
  }
  if (c.accumulated_error_flags()) {
    fprintf(stderr, "Blend2D accumulated errors\n");
    std::abort();
  }
}
static void render_cached_unrecorded(Bench *bench) {
  if (bench->prepared)
    render<true, true>(bench);
  else
    render<true>(bench);
}

static void render_cached_dispatch(Bench *bench) {
  if (!bench->record_requested || !bench->asynchronous) {
    render_cached_unrecorded(bench);
    return;
  }
  if (bench->record_ready) {
    check(bl_probe_record_replay(&bench->context));
    if (bench->context.accumulated_error_flags())
      std::abort();
    return;
  }
  check(bl_probe_record_begin(&bench->context));
  if (const char *retain = std::getenv("BLEND_PROBE_RETAIN_EDGES"); retain && std::strcmp(retain, "1") == 0)
    check(bl_probe_record_retain_edges(&bench->context));
  render_cached_unrecorded(bench);
  check(bl_probe_record_end(&bench->context));
  bench->record_ready = true;
}

#include "blend_tiles.inc"
extern "C" void bb_render(void *ptr) {
  auto *b = static_cast<Bench *>(ptr);
  report_cache(*b);
  if (b->tiles)
    tile_render(b, false);
  else
    render<false>(ptr);
}
extern "C" void bb_render_cached(void *ptr) {
  auto *b = static_cast<Bench *>(ptr);
  report_cache(*b);
  if (b->tiles) {
    tile_render(b, true);
    return;
  }
  render_cached_dispatch(b);
}
extern "C" void bb_prepare(void *ptr) {
  auto *bench = static_cast<Bench *>(ptr);
  if (bench->record_requested && bench->asynchronous)
    bb_render_cached(ptr);
}
extern "C" void bb_pixels(void *ptr, unsigned char *out) {
  auto &b = *static_cast<Bench *>(ptr);
  BLImageData data;
  check(b.image.get_data(&data));
  for (int y = 0; y < b.image.height(); y++)
    for (int x = 0; x < b.image.width(); x++) {
      auto p = (unsigned char *)data.pixel_data + y * data.stride + 4 * x;
      *out++ = p[2];
      *out++ = p[1];
      *out++ = p[0];
      *out++ = p[3];
    }
}
extern "C" void bb_free(void *ptr) {
  auto b = static_cast<Bench *>(ptr);
  if (b->tiles)
    tile_free(b);
  if (b->metrics)
    std::fprintf(stderr, "RENDER_METRICS frames=%llu main_cpu_ms=%.3f\n", b->render_count, b->render_cpu_ns / 1e6);
  delete b;
}
