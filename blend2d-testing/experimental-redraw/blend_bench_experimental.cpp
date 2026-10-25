// Optional benchmark backend. Native paths/paints are retained, as Rasterizer
// retains its scene.
#include "review_draw.h"
#include <blend2d/blend2d.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <memory>
#include "blend_cache.h"
static void check(BLResult r) {
  if (r) {
    fprintf(stderr, "Blend2D error %u\n", r);
    std::abort();
  }
}
static BLMatrix2D matrix(const float *m) {
  return BLMatrix2D(m[0], m[1], m[2], m[3], m[4], m[5]);
}
struct StrokeCache {
  struct Part {BLPath a,b,c;};
  std::vector<Part> parts;
  std::vector<BlProbeStrokePart> references;
  BlProbeStrokeView view{};
};
struct Draw {
  BLPath path;
  BLVar paint;
  BLMatrix2D transform;
  BLRect clip;
  double width, opacity;
  uint32_t flags;
  uint32_t updates = 0;
  union {const StrokeCache* cached_stroke = nullptr;struct {uint32_t first,count;} packed_stroke;};
};
struct TileGroup;
struct Bench {
  std::vector<Draw> draws;
  BLImage image;
  BLContext context;
  TileGroup* tiles = nullptr;
  bool restricted = false;
  BLRect base_clip;
  bool metrics = std::getenv("BLEND_PROBE_METRICS") != nullptr;
  bool prepared = std::getenv("BLEND_PROBE_PREPARED") && std::strcmp(std::getenv("BLEND_PROBE_PREPARED"),"1")==0;
  unsigned solid_mode = std::getenv("BLEND_PROBE_SOLID_MODE") ? unsigned(std::strtoul(std::getenv("BLEND_PROBE_SOLID_MODE"),nullptr,10)) : 0;
  unsigned borrow_mode = std::getenv("BLEND_PROBE_BORROW") ? unsigned(std::strtoul(std::getenv("BLEND_PROBE_BORROW"),nullptr,10)) : 0;
  unsigned cache_mode = std::getenv("BLEND_PROBE_PRESTROKE") ? unsigned(std::strtoul(std::getenv("BLEND_PROBE_PRESTROKE"),nullptr,10)) : 0;
  bool record_requested = std::getenv("BLEND_PROBE_RECORD") && std::strcmp(std::getenv("BLEND_PROBE_RECORD"),"1")==0;
  bool record_ready=false;
  bool asynchronous = false, reported_cache = false;
  std::vector<std::unique_ptr<StrokeCache>> stroke_caches;
  size_t cached_count=0, cache_source_entries=0, cache_outline_entries=0;
  std::vector<BLPoint> packed_vertices;
  std::vector<uint8_t> packed_commands;
  std::vector<BlProbePackedPart> packed_parts;
  BlProbePackedView packed_view{};
  const BlProbePackedView* render_packed = &packed_view;
  size_t last_stroke = SIZE_MAX, last_fill = SIZE_MAX;
  unsigned long long render_count = 0, render_cpu_ns = 0;
};
static void tile_begin(Bench*, unsigned, unsigned);
static void tile_add(Bench*, const Draw&);
static void tile_render(Bench*, bool);
static void tile_free(Bench*);
static unsigned long long thread_cpu_ns() {
#ifdef __linux__
  timespec t{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return static_cast<unsigned long long>(t.tv_sec) * 1000000000ull + t.tv_nsec;
#else
  return 0;
#endif
}
static void configure_async_limit(BLContext& context) {
  if(const char* limit=std::getenv("BLEND_PROBE_ASYNC_LIMIT")) {
    if(!bl_probe_set_async_path_limit){std::fprintf(stderr,"Async threshold override needs the campaign library.\n");std::abort();}
    check(bl_probe_set_async_path_limit(&context,uint32_t(std::strtoul(limit,nullptr,10))));
  }
}
extern "C" void *bb_new_config(unsigned w, unsigned h, unsigned threads,
                               unsigned queue_limit) {
  auto b = new Bench;
  b->asynchronous = threads != 0;
  check(b->image.create(w, h, BL_FORMAT_PRGB32));
  const char* tile_count = std::getenv("BLEND_PROBE_TILES");
  if (threads > 1 && h >= 64 && tile_count && std::strtoul(tile_count, nullptr, 10) > 1) {
    tile_begin(b, threads, queue_limit);
    return b;
  }
  BLContextCreateInfo info{};
  info.thread_count = threads;
  info.command_queue_limit = queue_limit;
  check(b->context.begin(b->image, &info));
  if(threads)configure_async_limit(b->context);
  if (threads && b->context.thread_count() != threads) {
    fprintf(stderr, "Requested %u threads, got %u\n", threads,
            b->context.thread_count());
    std::abort();
  }
  return b;
}
extern "C" void *bb_new(unsigned w, unsigned h, unsigned threads) {
  return bb_new_config(w, h, threads, 0);
}
enum Update : uint32_t { Clip=1, Transform=2, Alpha=4, Width=8, Caps=16, Join=32, Rule=64 };
static void append_draw(Bench& bench, Draw draw) {
  if (!bench.prepared) { bench.draws.push_back(std::move(draw)); return; }
  const Draw* previous = bench.draws.empty() ? nullptr : &bench.draws.back();
  bool clip = !previous || !(draw.clip == previous->clip);
  if (clip) draw.updates |= Clip;
  if (clip || !previous || !draw.transform.equals(previous->transform)) draw.updates |= Transform;
  if (!previous || draw.opacity != previous->opacity) draw.updates |= Alpha;
  if (draw.width) {
    const Draw* stroke = bench.last_stroke == SIZE_MAX ? nullptr : &bench.draws[bench.last_stroke];
    if (!stroke || draw.width != stroke->width) draw.updates |= Width;
    if (!stroke || ((draw.flags ^ stroke->flags) & ((1<<2)|(1<<3)))) draw.updates |= Caps;
    if (!stroke || ((draw.flags ^ stroke->flags) & (1<<4))) draw.updates |= Join;
    bench.last_stroke = bench.draws.size();
  } else {
    const Draw* fill = bench.last_fill == SIZE_MAX ? nullptr : &bench.draws[bench.last_fill];
    if (!fill || ((draw.flags ^ fill->flags) & (1<<1))) draw.updates |= Rule;
    bench.last_fill = bench.draws.size();
  }
  bench.draws.push_back(std::move(draw));
}
static BLResult BL_CDECL capture_stroke(BLPathCore* a,BLPathCore* b,BLPathCore* c,size_t,size_t,void* data) noexcept {
  auto& cache=*static_cast<StrokeCache*>(data);
  StrokeCache::Part part;
  BLResult result=part.a.add_path(a->dcast());if(result)return result;
  result=part.b.add_path(b->dcast());if(result)return result;
  result=part.c.add_path(c->dcast());if(result)return result;
  cache.parts.push_back(std::move(part));
  return bl_path_clear(a);
}
static BLResult BL_CDECL capture_packed_stroke(BLPathCore* a,BLPathCore* b,BLPathCore* c,size_t,size_t,void* data) noexcept {
  auto& bench=*static_cast<Bench*>(data);BlProbePackedPart part;
  auto append=[&](const BLPath& path,BlProbePackedSpan& span){
    if(bench.packed_commands.size()+path.size()>UINT32_MAX)std::abort();
    span={uint32_t(bench.packed_commands.size()),uint32_t(path.size())};
    bench.packed_commands.insert(bench.packed_commands.end(),path.command_data(),path.command_data()+path.size());
    bench.packed_vertices.insert(bench.packed_vertices.end(),path.vertex_data(),path.vertex_data()+path.size());
  };
  append(a->dcast(),part.a);append(b->dcast(),part.b);append(c->dcast(),part.c);
  bench.packed_parts.push_back(part);return bl_path_clear(a);
}
static void prepare_stroke(Bench& bench,Draw& draw) {
  if(!bench.cache_mode || !bench.asynchronous || !draw.width)return;
  bool linear=true;for(size_t i=0;i<draw.path.size();i++){auto cmd=draw.path.command_data()[i];if(cmd!=BL_PATH_CMD_MOVE && cmd!=BL_PATH_CMD_ON && cmd!=BL_PATH_CMD_CLOSE){linear=false;break;}}
  bool line=draw.path.size()==2 && draw.path.command_data()[0]==BL_PATH_CMD_MOVE && draw.path.command_data()[1]==BL_PATH_CMD_ON;
  if((bench.cache_mode==1 && !line)||(bench.cache_mode==2 && !linear))return;
  BLStrokeOptions options;options.width=draw.width;options.miter_limit=3.863703305;
  options.set_caps(draw.flags&(1<<2)?BL_STROKE_CAP_ROUND:draw.flags&(1<<3)?BL_STROKE_CAP_SQUARE:BL_STROKE_CAP_BUTT);
  options.join=uint8_t(draw.flags&(1<<4)?BL_STROKE_JOIN_ROUND:BL_STROKE_JOIN_MITER_CLIP);
  size_t original_entries=draw.path.size(),outline_entries=0;
  if(bench.cache_mode>=5){
    BLPath a,b,c;size_t start=bench.packed_parts.size(),entries=bench.packed_vertices.size();
    check(bl_path_stroke_to_sink(&draw.path,nullptr,&options,&bl_default_approximation_options,&a,&b,&c,capture_packed_stroke,&bench));
    draw.packed_stroke={uint32_t(start),uint32_t(bench.packed_parts.size()-start)};draw.flags|=(1u<<31);
    outline_entries=bench.packed_vertices.size()-entries;
    bench.packed_view={bench.packed_vertices.data(),bench.packed_commands.data(),bench.packed_parts.data(),bench.packed_parts.size()};
  } else if(bench.cache_mode==4){
    auto cache=std::make_unique<StrokeCache>();BLPath a,b,c;
    check(bl_path_stroke_to_sink(&draw.path,nullptr,&options,&bl_default_approximation_options,&a,&b,&c,capture_stroke,cache.get()));
    cache->references.reserve(cache->parts.size());
    for(auto& part:cache->parts){cache->references.push_back({&part.a,&part.b,&part.c});outline_entries+=part.a.size()+part.b.size()+part.c.size();}
    cache->view={cache->references.data(),cache->references.size()};
    draw.cached_stroke=cache.get();bench.stroke_caches.push_back(std::move(cache));
  }else{
    BLPath outline;check(outline.add_stroked_path(draw.path,options,bl_default_approximation_options));
    outline_entries=outline.size();draw.path=std::move(outline);draw.width=0;draw.flags&=~(1u<<1);
  }
  bench.cached_count++;bench.cache_source_entries+=original_entries;bench.cache_outline_entries+=outline_entries;
}
static void report_cache(Bench& bench) {
  if(!bench.reported_cache && bench.cache_mode){std::fprintf(stderr,"STROKE_CACHE mode=%u paths=%zu source_entries=%zu outline_entries=%zu\n",bench.cache_mode,bench.cached_count,bench.cache_source_entries,bench.cache_outline_entries);bench.reported_cache=true;}
}
extern "C" int bb_add(void *ptr, const ReviewDrawInfo *d, const float *m,
                      const float *clip) {
  // No arbitrary path clipping in Blend2D's public context API. Reject whole
  // documents requiring it.
  if (d->clipTypeCount || d->blendMode || d->paintType > 3)
    return 1;
  Draw v;
  v.transform = matrix(m);
  v.flags = d->flags;
  v.clip = BLRect(clip[0], clip[1], std::max(0.f, clip[2] - clip[0]),
                  std::max(0.f, clip[3] - clip[1]));
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
    v.paint =
        BLVar(BLRgba32(d->color[0], d->color[1], d->color[2], d->color[3]));
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
      std::memcpy((char *)data.pixel_data + y * data.stride,
                  d->imagePixels + y * d->imageWidth * 4, d->imageWidth * 4);
    BLPattern pat(image, BL_EXTEND_MODE_PAD);
    check(pat.set_transform(
        BLMatrix2D((d->bounds[2] - d->bounds[0]) / d->imageWidth, 0, 0,
                   -(d->bounds[3] - d->bounds[1]) / d->imageHeight,
                   d->bounds[0], d->bounds[3])));
    v.paint = pat;
  }
  auto* bench = static_cast<Bench*>(ptr);
  prepare_stroke(*bench,v);
  if (bench->tiles) tile_add(bench, v);
  else append_draw(*bench, std::move(v));
  return 0;
}
template<unsigned Mode,bool Stroke>
static void submit_path(BLContext& context,const Draw& draw,const Draw* previous) {
  // Exact type equality avoids the broken is_rgba32() predicate in the baseline revision.
  if constexpr (Mode!=0)if(draw.paint.type()==BL_OBJECT_TYPE_RGBA32){
    BLRgba32 color=draw.paint._d.rgba32;
    if constexpr (Mode==2){
      if(!previous || previous->paint.type()!=BL_OBJECT_TYPE_RGBA32 || previous->paint._d.rgba32!=color){
        if constexpr(Stroke)check(context.set_stroke_style(color));else check(context.set_fill_style(color));
      }
      if constexpr(Stroke)check(context.stroke_path(draw.path));else check(context.fill_path(draw.path));
    }else{if constexpr(Stroke)check(context.stroke_path(draw.path,color));else check(context.fill_path(draw.path,color));}
    return;
  }
  if constexpr(Stroke)check(context.stroke_path(draw.path,draw.paint));else check(context.fill_path(draw.path,draw.paint));
}
template<bool Cached, bool Prepared = false, unsigned SolidMode = 0> static void render(void *ptr) {
  auto &b = *static_cast<Bench *>(ptr);
  const auto cpu_start = b.metrics ? thread_cpu_ns() : 0;
  auto &c = b.context;
  check(c.reset_transform());
  check(c.restore_clipping());
  if (b.restricted) check(c.clip_to_rect(b.base_clip));
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
    if (Prepared ? bool(d.updates & Transform) : (!Cached || clip_changed || !previous || !d.transform.equals(previous->transform)))
      check(c.set_transform(d.transform));
    if (Prepared ? bool(d.updates & Alpha) : (!Cached || !previous || d.opacity != previous->opacity))
      check(c.set_global_alpha(d.opacity));
    if (d.width) {
      if (Prepared ? bool(d.updates & Width) : (!Cached || !previous_stroke || d.width != previous_stroke->width))
        check(c.set_stroke_width(d.width));
      if (Prepared ? bool(d.updates & Caps) : (!Cached || !previous_stroke || ((d.flags ^ previous_stroke->flags) & ((1<<2)|(1<<3)))))
        check(c.set_stroke_caps(d.flags & (1 << 2) ? BL_STROKE_CAP_ROUND : d.flags & (1 << 3) ? BL_STROKE_CAP_SQUARE : BL_STROKE_CAP_BUTT));
      if (Prepared ? bool(d.updates & Join) : (!Cached || !previous_stroke || ((d.flags ^ previous_stroke->flags) & (1<<4))))
        check(c.set_stroke_join(d.flags & (1 << 4) ? BL_STROKE_JOIN_ROUND : BL_STROKE_JOIN_MITER_CLIP));
      if (d.flags & (1u<<31)) {
        if(!bl_probe_stroke_packed){std::fprintf(stderr,"Packed stroke cache requires the campaign experimental library.\n");std::abort();}
        check(bl_probe_stroke_packed(&c,b.cache_mode>=6?nullptr:&d.path,b.render_packed,d.packed_stroke.first,d.packed_stroke.count,&d.paint));
      } else if (d.cached_stroke) {
        if(!bl_probe_stroke_cached){std::fprintf(stderr,"Signed stroke cache requires the campaign experimental library.\n");std::abort();}
        check(bl_probe_stroke_cached(&c,&d.path,&d.cached_stroke->view,&d.paint));
      } else if(b.asynchronous && b.borrow_mode) {
        if(!bl_probe_geometry_borrowed)std::abort();
        check(bl_probe_geometry_borrowed(&c,&d.path,1,&d.paint));
      } else submit_path<SolidMode,true>(c,d,previous_stroke);
      previous_stroke = &d;
    } else {
      if (Prepared ? bool(d.updates & Rule) : (!Cached || !previous_fill || ((d.flags ^ previous_fill->flags) & (1<<1))))
        check(c.set_fill_rule(d.flags & (1 << 1) ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO));
      if(b.asynchronous && b.borrow_mode>=2 && d.path.size()<32768) {
        if(!bl_probe_geometry_borrowed)std::abort();
        check(bl_probe_geometry_borrowed(&c,&d.path,0,&d.paint));
      } else submit_path<SolidMode,false>(c,d,previous_fill);
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
static void render_cached_unrecorded(Bench* bench) {
  if(bench->prepared){
    if(bench->solid_mode==1)render<true,true,1>(bench);
    else if(bench->solid_mode==2)render<true,true,2>(bench);
    else render<true,true>(bench);
  }else{
    if(bench->solid_mode==1)render<true,false,1>(bench);
    else if(bench->solid_mode==2)render<true,false,2>(bench);
    else render<true>(bench);
  }
}
static void render_cached_dispatch(Bench* bench) {
  if(bench->record_requested && bench->asynchronous){
    if(!bl_probe_record_begin||!bl_probe_record_end||!bl_probe_record_replay){std::fprintf(stderr,"Recorded command replay requires the record experimental library.\n");std::abort();}
    if(bench->record_ready){check(bl_probe_record_replay(&bench->context));if(bench->context.accumulated_error_flags())std::abort();return;}
    check(bl_probe_record_begin(&bench->context));render_cached_unrecorded(bench);check(bl_probe_record_end(&bench->context));bench->record_ready=true;return;
  }
  render_cached_unrecorded(bench);
}
#include "blend_tiles.inc"
extern "C" void bb_render(void *ptr) {
  auto* b = static_cast<Bench*>(ptr);
  report_cache(*b);
  if (b->tiles) tile_render(b, false); else render<false>(ptr);
}
extern "C" void bb_render_cached(void *ptr) {
  auto* b = static_cast<Bench*>(ptr);
  report_cache(*b);
  if (b->tiles) { tile_render(b, true); return; }
  render_cached_dispatch(b);
}
extern "C" void bb_prepare(void* ptr) {
  auto* bench=static_cast<Bench*>(ptr);
  if(bench->record_requested && bench->asynchronous)bb_render_cached(ptr);
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
  if (b->tiles) tile_free(b);
  if (b->metrics)
    std::fprintf(stderr, "RENDER_METRICS frames=%llu main_cpu_ms=%.3f\n",
                 b->render_count, b->render_cpu_ns / 1e6);
  delete b;
}
