// Native-only integration test: links Blend2D, no Rasterizer parser required.
#include "blend_bench_experimental.cpp"
#include <array>
struct Variant {
  const char *name;
  unsigned threads, tiles, cache;
  int limit;
  bool prepared;
  unsigned borrow = 0;
  bool record = false;
  unsigned cut = 0;
};
static unsigned random_u(unsigned &s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
static void number_env(const char *key, unsigned value) {
  auto text = std::to_string(value);
  setenv(key, text.c_str(), 1);
}
static std::vector<unsigned char> test_render(int size, unsigned seed, Variant variant) {
  unsetenv("BLEND_PROBE_TILE_CUT");
  if (variant.cut)
    number_env("BLEND_PROBE_TILE_CUT", variant.cut);
  unsetenv("BLEND_PROBE_RECORD");
  if (variant.record)
    setenv("BLEND_PROBE_RECORD", "1", 1);
  unsetenv("BLEND_PROBE_PREPARED");
  unsetenv("BLEND_PROBE_ASYNC_LIMIT");
  number_env("BLEND_PROBE_BORROW", variant.borrow);
  number_env("BLEND_PROBE_TILES", variant.tiles);
  number_env("BLEND_PROBE_PRESTROKE", variant.cache);
  if (variant.prepared)
    setenv("BLEND_PROBE_PREPARED", "1", 1);
  if (variant.limit >= 0)
    number_env("BLEND_PROBE_ASYNC_LIMIT", unsigned(variant.limit));
  auto *bench = static_cast<Bench *>(bb_new_config(size, size, variant.threads, 32768));
  for (int i = 0; i < 500; i++) {
    Draw d;
    d.flags = 0;
    double x = int(random_u(seed) % (size + 64)) - 32, y = int(random_u(seed) % (size + 64)) - 32;
    check(d.path.move_to(x, y));
    if (i % 5 == 0)
      check(d.path.line_to(x + 9.5, y + 17.25));
    else if (i % 5 == 1) {
      check(d.path.line_to(x + 12.5, y - 3.25));
      check(d.path.line_to(x + 17.75, y + 18.5));
      check(d.path.line_to(x - 4, y + 10.25));
      check(d.path.close());
    } else {
      check(d.path.cubic_to(x + 24, y - 12, x - 10, y + 28, x + 15.25, y + 20));
      if (i % 3)
        check(d.path.quad_to(x - 5, y + 26, x, y));
      else
        check(d.path.conic_to(x - 5, y + 26, x, y, 0.75));
      if (i % 2)
        check(d.path.close());
    }
    d.transform = i % 4 ? BLMatrix2D::make_identity() : BLMatrix2D(0.8, 0.125, -0.075, 1.05, 0.25, -0.5);
    d.clip = i % 7 ? BLRect(0, 0, size, size) : BLRect(6.25, 12.5, size - 18.75, size - 37.25);
    if (i % 29 == 0)
      d.clip.h = 0;
    d.opacity = (i % 7 + 1) / 7.;
    d.width = i % 3 ? 0. : 0.25 + (i % 8);
    if (i % 2)
      d.flags |= 1 << 1;
    if (i % 3 == 0)
      d.flags |= 1 << 2;
    else if (i % 3 == 1)
      d.flags |= 1 << 3;
    if (i % 5)
      d.flags |= 1 << 4;
    if (i % 5 == 0) {
      BLGradient g(BLLinearGradientValues(x, y, x + 24, y + 24));
      check(g.add_stop(0, BLRgba32(0x3377aaff)));
      check(g.add_stop(1, BLRgba32(0xddff2211)));
      d.paint = g;
    } else if (i % 11 == 0) {
      BLImage image;
      check(image.create(2, 2, BL_FORMAT_PRGB32));
      BLImageData pixels;
      check(image.get_data(&pixels));
      for (int yy = 0; yy < 2; yy++)
        for (int xx = 0; xx < 2; xx++)
          reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(pixels.pixel_data) + yy * pixels.stride)[xx] =
              0xff003300u + uint32_t((xx + yy) * 0x00662244u);
      BLPattern pattern(image, BL_EXTEND_MODE_REFLECT);
      d.paint = pattern;
    } else
      d.paint = BLVar(BLRgba32(random_u(seed) | 0x80000000u));
    prepare_stroke(*bench, d);
    if (bench->tiles)
      tile_add(bench, d);
    else
      append_draw(*bench, std::move(d));
  }
  bb_prepare(bench);
  bb_render_cached(bench);
  std::vector<unsigned char> first(size_t(size) * size * 4);
  bb_pixels(bench, first.data());
  // Retained state/cache data must remain valid across redraws and batch reuse.
  bb_render_cached(bench);
  std::vector<unsigned char> second(first.size());
  bb_pixels(bench, second.data());
  if (first != second) {
    std::fprintf(stderr, "Repeat changed pixels: %s size=%d\n", variant.name, size);
    std::abort();
  }
  bb_free(bench);
  return first;
}
// A batch that cannot be recorded must still render every draw call, including
// the one that fills the queue. Only bl_probe_record_end() reports the failure.
static void test_unrecordable_batch() {
  auto draw = [](BLContext &context) {
    check(context.clear_all());
    for (int i = 0; i < 700; i++) {
      double x = (i % 30) * 8.0 + 1.3, y = (i / 30) * 8.0 + 0.7;
      BLPath path;
      for (int k = 0; k < 6; k++) {
        path.move_to(x + k, y);
        path.line_to(x + k + 0.6, y + 1.5);
        path.line_to(x + k + 0.2, y + 6.5);
        path.close();
      }
      check(context.fill_path(path, BLRgba32(0xFF000000u | (unsigned(i) * 2654435761u >> 8))));
    }
  };
  BLContextCreateInfo info{};
  info.thread_count = 4;
  info.command_queue_limit = 256;
  BLImage expected(256, 256, BL_FORMAT_PRGB32), actual(256, 256, BL_FORMAT_PRGB32);
  {
    BLContext context(expected, info);
    draw(context);
    check(context.end());
  }
  BLContext context(actual, info);
  check(bl_probe_record_begin(&context));
  // Inline analytic edges cannot be replayed.
  check(bl_probe_set_async_path_limit(&context, 1u << 20));
  draw(context);
  check(context.flush(BL_CONTEXT_FLUSH_SYNC));
  if (bl_probe_record_end(&context) != BL_ERROR_NOT_IMPLEMENTED ||
      bl_probe_record_replay(&context) != BL_ERROR_NOT_IMPLEMENTED)
    std::abort();
  check(context.end());
  BLImageData a, b;
  check(expected.get_data(&a));
  check(actual.get_data(&b));
  if (std::memcmp(a.pixel_data, b.pixel_data, size_t(a.stride) * size_t(a.size.h)) != 0) {
    std::fprintf(stderr, "Unrecordable batch changed pixels\n");
    std::abort();
  }
}
int main(int argc, char** argv) {
  Variant reference = {"sync", 0, 0, 0, -1, false};
  if (argc == 2 && std::strcmp(argv[1], "--lifetime") == 0) {
    auto expected = test_render(128, 1, reference);
    Variant lifetime_variants[] = {
        {"record", 4, 0, 0, -1, true, 0, true},
        {"packed-record", 4, 0, 6, -1, true, 0, true},
        {"borrowed", 4, 0, 0, -1, true, 2, false}};
    for (int repeat = 0; repeat < 3; repeat++) {
      for (Variant variant : lifetime_variants) {
        if (test_render(128, 1, variant) != expected)
          return 1;
      }
    }
    check(bl_runtime_cleanup(BL_RUNTIME_CLEANUP_EVERYTHING));
    // Cleanup requests detached worker shutdown. Allow pthread TLS destructors
    // to finish before an external leak checker inspects process exit.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
  }
  Variant variants[] = {{"cached4", 4, 0, 0, -1, false},
                        {"cached16", 16, 0, 0, -1, false},
                        {"prepared", 16, 0, 0, -1, true},
                        {"async0", 16, 0, 0, 0, true},
                        {"packed", 16, 0, 6, -1, true},
                        {"bands2", 16, 2, 0, -1, true},
                        {"bands4", 16, 4, 0, -1, true},
                        {"packed-bands2", 16, 2, 6, -1, true},
                        {"borrow1", 16, 0, 0, -1, true, 1},
                        {"borrow2", 16, 0, 0, -1, true, 2},
                        {"borrow2-bands2", 16, 2, 0, -1, true, 2},
                        {"record", 16, 0, 0, -1, true, 0, true},
                        {"record-bands2", 16, 2, 0, -1, true, 0, true},
                        {"record-packed", 16, 0, 6, -1, true, 0, true},
                        {"record-packed-bands2", 16, 2, 6, -1, true, 0, true},
                        {"record-packed-cut", 16, 2, 6, -1, true, 0, true, 1280}};
  test_unrecordable_batch();
  for (int size : {128, 257, 321, 2048})
    for (unsigned seed : {1u, 987654321u}) {
      auto image = test_render(size, seed, reference);
      for (auto variant : variants) {
        auto actual = test_render(size, seed, variant);
        if (actual != image) {
          size_t differences = 0;
          for (size_t i = 0; i < image.size(); i++)
            differences += image[i] != actual[i];
          std::fprintf(stderr, "Mismatch %s size=%d seed=%u channels=%zu\n", variant.name, size, seed, differences);
          return 1;
        }
      }
      std::printf("Exact replay: size=%d seed=%u, all variants; two redraws each\n", size, seed);
    }
  return 0;
}
