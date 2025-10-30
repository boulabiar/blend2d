#include "bl_lottie.h"

#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>

BLMatrix2D lottie_matrix_multiply(const BLMatrix2D& a, const BLMatrix2D& b) noexcept {
  BLMatrix2D result;
  result.m00 = a.m00 * b.m00 + a.m10 * b.m01;
  result.m01 = a.m01 * b.m00 + a.m11 * b.m01;
  result.m10 = a.m00 * b.m10 + a.m10 * b.m11;
  result.m11 = a.m01 * b.m10 + a.m11 * b.m11;
  result.m20 = a.m00 * b.m20 + a.m10 * b.m21 + a.m20;
  result.m21 = a.m01 * b.m20 + a.m11 * b.m21 + a.m21;
  return result;
}

namespace {

constexpr double kPi = 3.14159265358979323846;

double to_double(const QJsonValue& value, double fallback) noexcept {
  if (value.isDouble())
    return value.toDouble();
  if (value.isArray()) {
    const QJsonArray arr = value.toArray();
    if (!arr.isEmpty())
      return arr.first().toDouble(fallback);
  }
  return fallback;
}

LottieVec2 to_vec2(const QJsonValue& value, const LottieVec2& fallback) noexcept {
  if (!value.isArray())
    return fallback;

  const QJsonArray arr = value.toArray();
  if (arr.size() < 2)
    return fallback;

  LottieVec2 result {};
  result.x = arr.at(0).toDouble(fallback.x);
  result.y = arr.at(1).toDouble(fallback.y);
  return result;
}

LottieColor to_color(const QJsonValue& value, const LottieColor& fallback) noexcept {
  if (!value.isArray())
    return fallback;

  const QJsonArray arr = value.toArray();
  if (arr.size() < 3)
    return fallback;

  LottieColor result {};
  result.r = arr.at(0).toDouble(fallback.r);
  result.g = arr.at(1).toDouble(fallback.g);
  result.b = arr.at(2).toDouble(fallback.b);
  result.a = arr.size() > 3 ? arr.at(3).toDouble(fallback.a) : fallback.a;
  return result;
}

LottieColor parse_hex_color(const QString& value) noexcept {
  LottieColor color {};
  color.a = 1.0;

  auto parse_component = [](const QString& str, int start) -> int {
    bool ok = false;
    const int result = str.mid(start, 2).toInt(&ok, 16);
    return ok ? result : -1;
  };

  if (value.size() == 7 && value.startsWith(QLatin1Char('#'))) {
    const int r = parse_component(value, 1);
    const int g = parse_component(value, 3);
    const int b = parse_component(value, 5);
    if (r >= 0 && g >= 0 && b >= 0) {
      color.r = double(r) / 255.0;
      color.g = double(g) / 255.0;
      color.b = double(b) / 255.0;
    }
    return color;
  }

  if (value.size() == 9 && value.startsWith(QLatin1Char('#'))) {
    const int a = parse_component(value, 1);
    const int r = parse_component(value, 3);
    const int g = parse_component(value, 5);
    const int b = parse_component(value, 7);
    if (a >= 0 && r >= 0 && g >= 0 && b >= 0) {
      color.a = double(a) / 255.0;
      color.r = double(r) / 255.0;
      color.g = double(g) / 255.0;
      color.b = double(b) / 255.0;
    }
    return color;
  }

  return color;
}

void parse_animated_double(const QJsonValue& value, LottieAnimatedValue<double>& dst, double fallback) {
  dst.animated = false;
  dst.value = fallback;
  dst.keyframes.clear();

  if (!value.isObject()) {
    dst.value = to_double(value, fallback);
    return;
  }

  const QJsonObject obj = value.toObject();
  const int animated = obj.value(QLatin1String("a")).toInt();
  if (!animated) {
    dst.value = obj.value(QLatin1String("k")).toDouble(fallback);
    return;
  }

  const QJsonArray frames = obj.value(QLatin1String("k")).toArray();
  dst.animated = true;
  dst.keyframes.reserve(frames.size());
  double prev_end = fallback;
  bool prev_end_valid = false;
  for (const QJsonValue& entry : frames) {
    if (!entry.isObject())
      continue;
    const QJsonObject key = entry.toObject();
    const double time = key.value(QLatin1String("t")).toDouble();
    double number = fallback;
    const QJsonValue sValue = key.value(QLatin1String("s"));
    if (sValue.isArray()) {
      const QJsonArray arr = sValue.toArray();
      if (!arr.isEmpty())
        number = arr.first().toDouble(fallback);
    }
    else if (!sValue.isUndefined() && !sValue.isNull() && sValue.isDouble()) {
      number = sValue.toDouble(fallback);
    }
    else if (prev_end_valid) {
      number = prev_end;
    }

    dst.keyframes.push_back({time, number});

    const QJsonValue eValue = key.value(QLatin1String("e"));
    if (eValue.isArray()) {
      const QJsonArray arr = eValue.toArray();
      if (!arr.isEmpty()) {
        prev_end = arr.first().toDouble(number);
        prev_end_valid = true;
      }
    }
    else if (eValue.isDouble()) {
      prev_end = eValue.toDouble(number);
      prev_end_valid = true;
    }
    else {
      prev_end = number;
      prev_end_valid = true;
    }
  }

  if (dst.keyframes.empty()) {
    dst.animated = false;
    dst.value = fallback;
  }
  else {
    dst.value = dst.keyframes.front().value;
  }
}

void parse_animated_vec2(const QJsonValue& value, LottieAnimatedValue<LottieVec2>& dst, const LottieVec2& fallback) {
  dst.animated = false;
  dst.value = fallback;
  dst.keyframes.clear();

  if (!value.isObject()) {
    dst.value = to_vec2(value, fallback);
    return;
  }

  const QJsonObject obj = value.toObject();
  const int animated = obj.value(QLatin1String("a")).toInt();
  if (!animated) {
    dst.value = to_vec2(obj.value(QLatin1String("k")), fallback);
    return;
  }

  const QJsonArray frames = obj.value(QLatin1String("k")).toArray();
  dst.animated = true;
  dst.keyframes.reserve(frames.size());
  LottieVec2 prev_end = fallback;
  bool prev_end_valid = false;
  for (const QJsonValue& entry : frames) {
    if (!entry.isObject())
      continue;
    const QJsonObject key = entry.toObject();
    const double time = key.value(QLatin1String("t")).toDouble();
    LottieVec2 vec = fallback;
    const QJsonValue sval = key.value(QLatin1String("s"));
    if ((sval.isUndefined() || sval.isNull()) && prev_end_valid)
      vec = prev_end;
    else
      vec = to_vec2(sval, fallback);
    dst.keyframes.push_back({time, vec});

    const QJsonValue eval = key.value(QLatin1String("e"));
    if (!eval.isUndefined() && !eval.isNull()) {
      prev_end = to_vec2(eval, vec);
      prev_end_valid = true;
    }
    else {
      prev_end = vec;
      prev_end_valid = true;
    }
  }

  if (dst.keyframes.empty()) {
    dst.animated = false;
    dst.value = fallback;
  }
  else {
    dst.value = dst.keyframes.front().value;
  }
}

void parse_animated_color(const QJsonValue& value, LottieAnimatedValue<LottieColor>& dst, const LottieColor& fallback) {
  dst.animated = false;
  dst.value = fallback;
  dst.keyframes.clear();

  if (!value.isObject()) {
    dst.value = to_color(value, fallback);
    return;
  }

  const QJsonObject obj = value.toObject();
  const int animated = obj.value(QLatin1String("a")).toInt();
  if (!animated) {
    dst.value = to_color(obj.value(QLatin1String("k")), fallback);
    return;
  }

  const QJsonArray frames = obj.value(QLatin1String("k")).toArray();
  dst.animated = true;
  dst.keyframes.reserve(frames.size());
  LottieColor prev_end = fallback;
  bool prev_end_valid = false;
  for (const QJsonValue& entry : frames) {
    if (!entry.isObject())
      continue;
    const QJsonObject key = entry.toObject();
    const double time = key.value(QLatin1String("t")).toDouble();
    LottieColor color = fallback;
    const QJsonValue sValue = key.value(QLatin1String("s"));
    if ((sValue.isUndefined() || sValue.isNull()) && prev_end_valid)
      color = prev_end;
    else
      color = to_color(sValue, fallback);
    dst.keyframes.push_back({time, color});

    const QJsonValue eValue = key.value(QLatin1String("e"));
    if (!eValue.isUndefined() && !eValue.isNull()) {
      prev_end = to_color(eValue, color);
      prev_end_valid = true;
    }
    else {
      prev_end = color;
      prev_end_valid = true;
    }
  }

  if (dst.keyframes.empty()) {
    dst.animated = false;
    dst.value = fallback;
  }
  else {
    dst.value = dst.keyframes.front().value;
  }
}

void parse_transform_object(const QJsonObject& obj, LottieTransform& transform) {
  parse_animated_vec2(obj.value(QLatin1String("a")), transform.anchor, LottieVec2{0.0, 0.0});
  parse_animated_vec2(obj.value(QLatin1String("p")), transform.position, LottieVec2{0.0, 0.0});
  parse_animated_vec2(obj.value(QLatin1String("s")), transform.scale, LottieVec2{100.0, 100.0});
  parse_animated_double(obj.value(QLatin1String("r")), transform.rotation, 0.0);
  parse_animated_double(obj.value(QLatin1String("o")), transform.opacity, 100.0);
  parse_animated_double(obj.value(QLatin1String("sk")), transform.skew, 0.0);
  parse_animated_double(obj.value(QLatin1String("sa")), transform.skew_axis, 0.0);
}

bool parse_shape_data(const QJsonObject& data, LottieShapePath::ShapeData& dst) {
  const QJsonArray vertices = data.value(QLatin1String("v")).toArray();
  const QJsonArray in_tangents = data.value(QLatin1String("i")).toArray();
  const QJsonArray out_tangents = data.value(QLatin1String("o")).toArray();
  const bool closed = data.value(QLatin1String("c")).toBool(false);

  const int count = vertices.size();
  if (count == 0)
    return false;

  if (in_tangents.size() != count || out_tangents.size() != count)
    return false;

  auto point_from = [](const QJsonArray& arr) -> LottieVec2 {
    LottieVec2 v {};
    if (arr.size() >= 2) {
      v.x = arr.at(0).toDouble();
      v.y = arr.at(1).toDouble();
    }
    return v;
  };

  dst.vertices.resize(count);
  dst.in_tangents.resize(count);
  dst.out_tangents.resize(count);

  for (int i = 0; i < count; i++) {
    dst.vertices[i] = point_from(vertices.at(i).toArray());
    dst.in_tangents[i] = point_from(in_tangents.at(i).toArray());
    dst.out_tangents[i] = point_from(out_tangents.at(i).toArray());
  }

  dst.closed = closed;
  return true;
}

BLPath build_path_from_shape(const LottieShapePath::ShapeData& shape) {
  BLPath path;
  const size_t count = shape.vertices.size();
  if (count == 0)
    return path;

  path.move_to(shape.vertices[0].x, shape.vertices[0].y);
  for (size_t i = 0; i + 1 < count; i++) {
    const LottieVec2& p0 = shape.vertices[i];
    const LottieVec2& p1 = shape.vertices[i + 1];
    const LottieVec2& o0 = shape.out_tangents[i];
    const LottieVec2& i1 = shape.in_tangents[i + 1];

    path.cubic_to(p0.x + o0.x,
                  p0.y + o0.y,
                  p1.x + i1.x,
                  p1.y + i1.y,
                  p1.x,
                  p1.y);
  }

  if (shape.closed && count > 1) {
    const LottieVec2& last = shape.vertices.back();
    const LottieVec2& o_last = shape.out_tangents.back();
    const LottieVec2& first = shape.vertices.front();
    const LottieVec2& i_first = shape.in_tangents.front();

    path.cubic_to(last.x + o_last.x,
                  last.y + o_last.y,
                  first.x + i_first.x,
                  first.y + i_first.y,
                  first.x,
                  first.y);

    path.close();
  }

  return path;
}

namespace {

struct TrimSegment {
  BLPoint p0 {};
  BLPoint p1 {};
  double length {};
};

struct TrimSubpath {
  BLPoint start {};
  bool closed {};
  double length {};
  std::vector<TrimSegment> segments;
};

struct FlattenedPath {
  std::vector<TrimSubpath> subpaths;
  double total_length {};
};

constexpr double kTrimEpsilon = 1e-9;

static double segment_distance(const BLPoint& a, const BLPoint& b) noexcept {
  return std::hypot(a.x - b.x, a.y - b.y);
}

static BLPoint lerp_point(const BLPoint& a, const BLPoint& b, double t) noexcept {
  return BLPoint(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

static int curve_subdivisions(double length_estimate) noexcept {
  int steps = int(std::ceil(length_estimate / 5.0));
  if (steps < 1) steps = 1;
  if (steps > 64) steps = 64;
  return steps;
}

static double append_segment(std::vector<TrimSegment>& segments, const BLPoint& p0, const BLPoint& p1) {
  const double len = segment_distance(p0, p1);
  if (len <= kTrimEpsilon)
    return 0.0;
  segments.push_back(TrimSegment{p0, p1, len});
  return len;
}

static void flatten_quadratic(const BLPoint& p0, const BLPoint& c0, const BLPoint& p1, TrimSubpath& subpath) {
  const double estimate = segment_distance(p0, c0) + segment_distance(c0, p1);
  const int steps = curve_subdivisions(estimate);
  BLPoint prev = p0;
  for (int i = 1; i <= steps; i++) {
    const double t = double(i) / double(steps);
    const double it = 1.0 - t;
    const BLPoint point{
      it * it * p0.x + 2.0 * it * t * c0.x + t * t * p1.x,
      it * it * p0.y + 2.0 * it * t * c0.y + t * t * p1.y
    };
    subpath.length += append_segment(subpath.segments, prev, point);
    prev = point;
  }
}

static void flatten_cubic(const BLPoint& p0, const BLPoint& c0, const BLPoint& c1, const BLPoint& p1, TrimSubpath& subpath) {
  const double estimate = segment_distance(p0, c0) + segment_distance(c0, c1) + segment_distance(c1, p1);
  const int steps = curve_subdivisions(estimate);
  BLPoint prev = p0;
  for (int i = 1; i <= steps; i++) {
    const double t = double(i) / double(steps);
    const double it = 1.0 - t;
    const BLPoint point{
      it * it * it * p0.x +
      3.0 * it * it * t * c0.x +
      3.0 * it * t * t * c1.x +
      t * t * t * p1.x,
      it * it * it * p0.y +
      3.0 * it * it * t * c0.y +
      3.0 * it * t * t * c1.y +
      t * t * t * p1.y
    };
    subpath.length += append_segment(subpath.segments, prev, point);
    prev = point;
  }
}

static FlattenedPath flatten_path(const BLPath& path) {
  FlattenedPath result;
  BLPathView view = path.view();
  const uint8_t* cmds = view.command_data;
  const BLPoint* vtx = view.vertex_data;
  const size_t size = view.size;

  TrimSubpath current;
  bool have_current = false;
  BLPoint current_point {};
  BLPoint start_point {};
  size_t i = 0;

  auto flush_current = [&]() {
    if (!have_current)
      return;
    if (!current.segments.empty()) {
      result.total_length += current.length;
      result.subpaths.push_back(std::move(current));
      current = TrimSubpath{};
    }
    have_current = false;
  };

  while (i < size) {
    const uint8_t cmd = cmds[i];
    switch (cmd) {
      case BL_PATH_CMD_MOVE: {
        flush_current();
        start_point = vtx[i];
        current_point = start_point;
        current.start = start_point;
        current.closed = false;
        current.length = 0.0;
        have_current = true;
        i += 1;
        break;
      }

      case BL_PATH_CMD_ON: {
        if (!have_current) {
          start_point = vtx[i];
          current_point = start_point;
          current.start = start_point;
          current.closed = false;
          current.length = 0.0;
          have_current = true;
        }
        const BLPoint end = vtx[i];
        current.length += append_segment(current.segments, current_point, end);
        current_point = end;
        i += 1;
        break;
      }

      case BL_PATH_CMD_QUAD: {
        if (!have_current) {
          start_point = current_point = BLPoint(0, 0);
          current.closed = false;
          current.length = 0.0;
          have_current = true;
        }
        const BLPoint c0 = vtx[i];
        const BLPoint end = vtx[i + 1];
        flatten_quadratic(current_point, c0, end, current);
        current_point = end;
        i += 2;
        break;
      }

      case BL_PATH_CMD_CUBIC: {
        if (!have_current) {
          start_point = current_point = BLPoint(0, 0);
          current.closed = false;
          current.length = 0.0;
          have_current = true;
        }
        const BLPoint c0 = vtx[i];
        const BLPoint c1 = vtx[i + 1];
        const BLPoint end = vtx[i + 2];
        flatten_cubic(current_point, c0, c1, end, current);
        current_point = end;
        i += 3;
        break;
      }

      case BL_PATH_CMD_CLOSE: {
        if (have_current) {
          current.length += append_segment(current.segments, current_point, start_point);
          current_point = start_point;
          current.closed = true;
        }
        i += 1;
        break;
      }

      default:
        // Unsupported commands (conic/weight) are ignored.
        i += 1;
        break;
    }
  }

  flush_current();
  return result;
}

static bool points_close(const BLPoint& a, const BLPoint& b) noexcept {
  return segment_distance(a, b) <= 1e-6;
}

static void append_trimmed_range(const std::vector<TrimSegment>& segments,
                                 double start_distance,
                                 double end_distance,
                                 BLPath& out) {
  if (end_distance - start_distance <= kTrimEpsilon)
    return;

  double cursor = 0.0;
  bool has_point = false;
  BLPoint last {};

  for (const TrimSegment& seg : segments) {
    const double seg_start = cursor;
    const double seg_end = cursor + seg.length;
    const double range_start = std::max(start_distance, seg_start);
    const double range_end = std::min(end_distance, seg_end);
    cursor = seg_end;

    if (range_end - range_start <= kTrimEpsilon)
      continue;

    const double local_start = (range_start - seg_start) / seg.length;
    const double local_end = (range_end - seg_start) / seg.length;
    const BLPoint p_start = lerp_point(seg.p0, seg.p1, local_start);
    const BLPoint p_end = lerp_point(seg.p0, seg.p1, local_end);

    if (!has_point) {
      out.move_to(p_start.x, p_start.y);
      has_point = true;
    }
    else if (!points_close(last, p_start)) {
      out.line_to(p_start.x, p_start.y);
    }

    out.line_to(p_end.x, p_end.y);
    last = p_end;
  }
}

static void append_trimmed_subpath(const TrimSubpath& subpath,
                                   double start_frac,
                                   double end_frac,
                                   double length,
                                   BLPath& out) {
  if (length <= kTrimEpsilon)
    return;

  const double start_distance = start_frac * length;
  const double end_distance = end_frac * length;

  if (end_distance - start_distance <= kTrimEpsilon)
    return;

  append_trimmed_range(subpath.segments, start_distance, end_distance, out);
}

static void append_trimmed_with_wrap(const TrimSubpath& subpath,
                                     double start_frac,
                                     double end_frac,
                                     double length,
                                     BLPath& out) {
  if (end_frac <= 1.0) {
    append_trimmed_subpath(subpath, start_frac, end_frac, length, out);
    return;
  }

  append_trimmed_subpath(subpath, start_frac, 1.0, length, out);
  append_trimmed_subpath(subpath, 0.0, end_frac - 1.0, length, out);
}

static void append_trimmed_with_wrap(const std::vector<TrimSegment>& segments,
                                     double start_frac,
                                     double end_frac,
                                     double length,
                                     BLPath& out) {
  if (length <= kTrimEpsilon)
    return;

  const double start_distance = start_frac * length;
  const double end_distance = end_frac * length;

  if (end_distance - start_distance <= kTrimEpsilon)
    return;

  if (end_distance <= length) {
    append_trimmed_range(segments, start_distance, end_distance, out);
    return;
  }

  append_trimmed_range(segments, start_distance, length, out);
  append_trimmed_range(segments, 0.0, end_distance - length, out);
}

static double normalize_fraction(double value) noexcept {
  double result = std::fmod(value, 1.0);
  if (result < 0.0)
    result += 1.0;
  return result;
}

static double normalize_degrees(double value) noexcept {
  double result = std::fmod(value, 360.0);
  if (result < 0.0)
    result += 360.0;
  return result;
}

static bool apply_trim_to_path(const BLPath& source,
                               const LottieTrimPath& trim,
                               double frame,
                               BLPath& out) {
  const double start_value = trim.start.evaluate(frame);
  const double end_value = trim.end.evaluate(frame);
  const double offset_value = trim.offset.evaluate(frame);

  const double clamped_start = std::clamp(start_value, 0.0, 100.0);
  const double clamped_end = std::clamp(end_value, 0.0, 100.0);
  double range = clamped_end - clamped_start;
  if (range < 0.0)
    range += 100.0;

  if (range >= 99.999)
    return false;

  const double offset_fraction = normalize_fraction(normalize_degrees(offset_value) / 360.0);
  const double start_fraction = normalize_fraction(clamped_start / 100.0 + offset_fraction);
  const double end_fraction = start_fraction + range / 100.0;

  const FlattenedPath flattened = flatten_path(source);
  if (flattened.total_length <= kTrimEpsilon || flattened.subpaths.empty()) {
    out.clear();
    return true;
  }

  out.clear();
  out.reserve(source.size());

  if (trim.mode == 2) {
    for (const TrimSubpath& subpath : flattened.subpaths) {
      const double sub_length = subpath.length;
      if (sub_length <= kTrimEpsilon)
        continue;
      append_trimmed_with_wrap(subpath, start_fraction, end_fraction, sub_length, out);
    }
    return true;
  }

  std::vector<TrimSegment> aggregate;
  aggregate.reserve(source.size());
  for (const TrimSubpath& subpath : flattened.subpaths) {
    for (const TrimSegment& seg : subpath.segments)
      aggregate.push_back(seg);
  }

  if (aggregate.empty()) {
    out.clear();
    return true;
  }

  const double total_length = flattened.total_length;
  append_trimmed_with_wrap(aggregate, start_fraction, end_fraction, total_length, out);
  return true;
}

} // namespace

std::unique_ptr<LottieShapePath> parse_shape_path(const QJsonObject& obj) {
  const QJsonObject ks = obj.value(QLatin1String("ks")).toObject();

  auto extract_path_object = [](const QJsonValue& value) -> QJsonObject {
    if (value.isObject())
      return value.toObject();

    if (value.isArray()) {
      const QJsonArray arr = value.toArray();
      if (!arr.isEmpty() && arr.first().isObject())
        return arr.first().toObject();
    }

    return QJsonObject();
  };

  auto path = std::make_unique<LottieShapePath>();
  const int animated = ks.value(QLatin1String("a")).toInt();

  if (!animated) {
    const QJsonObject data = extract_path_object(ks.value(QLatin1String("k")));
    if (data.isEmpty())
      return nullptr;

    if (!parse_shape_data(data, path->shape))
      return nullptr;

    path->animated = false;
    path->cache_valid = false;
    return path;
  }

  const QJsonArray frames = ks.value(QLatin1String("k")).toArray();
  for (const QJsonValue& entry : frames) {
    if (!entry.isObject())
      continue;
    const QJsonObject key = entry.toObject();
    const QJsonObject data = extract_path_object(key.value(QLatin1String("s")));
    if (data.isEmpty())
      continue;

    LottieShapePath::ShapeData shape_data;
    if (!parse_shape_data(data, shape_data))
      continue;

    LottieShapePath::Keyframe kf;
    kf.time = key.value(QLatin1String("t")).toDouble(path->keyframes.empty() ? 0.0 : path->keyframes.back().time);
    kf.hold = key.value(QLatin1String("h")).toInt() == 1;
    kf.shape = std::move(shape_data);
    path->keyframes.push_back(std::move(kf));
  }

  if (path->keyframes.empty()) {
    const QJsonObject data = extract_path_object(ks.value(QLatin1String("k")));
    if (data.isEmpty())
      return nullptr;

    if (!parse_shape_data(data, path->shape))
      return nullptr;

    path->animated = false;
    path->cache_valid = false;
    return path;
  }

  std::sort(path->keyframes.begin(), path->keyframes.end(), [](const LottieShapePath::Keyframe& a, const LottieShapePath::Keyframe& b) noexcept {
    return a.time < b.time;
  });

  path->animated = true;
  path->shape = path->keyframes.front().shape;
  path->cache_valid = false;
  return path;
}

bool parse_mask(const QJsonObject& obj, LottieMask& dst) {
  const QString mode = obj.value(QLatin1String("mode")).toString();
  if (mode == QLatin1String("s"))
    dst.mode = LottieMask::kSubtract;
  else if (mode == QLatin1String("i"))
    dst.mode = LottieMask::kIntersect;
  else if (mode == QLatin1String("a") || mode == QLatin1String("f") || mode.isEmpty())
    dst.mode = LottieMask::kAdd;
  else
    dst.mode = LottieMask::kUnknown;

  dst.inverted = obj.value(QLatin1String("inv")).toBool(false);
  parse_animated_double(obj.value(QLatin1String("o")), dst.opacity, 100.0);

  const QJsonObject pt = obj.value(QLatin1String("pt")).toObject();
  if (pt.isEmpty())
    return false;

  QJsonObject wrapper;
  wrapper.insert(QLatin1String("ks"), pt);
  std::unique_ptr<LottieShapePath> path = parse_shape_path(wrapper);
  if (!path)
    return false;

  dst.path = std::move(*path);
  return true;
}

std::unique_ptr<LottieShapePath> parse_polystar(const QJsonObject& obj) {
  auto star = std::make_unique<LottiePolystar>();

  star->star_type = obj.value(QLatin1String("sy")).toInt(1);
  const int dir_value = obj.value(QLatin1String("d")).toInt(1);
  star->direction = dir_value == 3 ? -1 : 1;

  parse_animated_double(obj.value(QLatin1String("pt")), star->points, 5.0);
  parse_animated_vec2(obj.value(QLatin1String("p")), star->position, LottieVec2{0.0, 0.0});
  parse_animated_double(obj.value(QLatin1String("r")), star->rotation, 0.0);
  parse_animated_double(obj.value(QLatin1String("or")), star->outer_radius, 0.0);
  parse_animated_double(obj.value(QLatin1String("os")), star->outer_roundness, 0.0);

  if (star->star_type == 1) {
    parse_animated_double(obj.value(QLatin1String("ir")), star->inner_radius, 0.0);
    parse_animated_double(obj.value(QLatin1String("is")), star->inner_roundness, 0.0);
  }
  else {
    star->inner_radius.animated = false;
    star->inner_radius.value = 0.0;
    star->inner_roundness.animated = false;
    star->inner_roundness.value = 0.0;
  }

  star->animated = star->points.animated ||
                   star->position.animated ||
                   star->rotation.animated ||
                   star->outer_radius.animated ||
                   star->outer_roundness.animated ||
                   star->inner_radius.animated ||
                   star->inner_roundness.animated;
  star->cache_valid = false;
  return star;
}

std::unique_ptr<LottieShapePath> parse_rectangle(const QJsonObject& obj) {
  const QJsonObject posObj = obj.value(QLatin1String("p")).toObject();
  const QJsonObject sizeObj = obj.value(QLatin1String("s")).toObject();
  const QJsonObject radiusObj = obj.value(QLatin1String("r")).toObject();

  if (posObj.value(QLatin1String("a")).toInt() != 0)
    return nullptr;
  if (sizeObj.value(QLatin1String("a")).toInt() != 0)
    return nullptr;
  if (radiusObj.value(QLatin1String("a")).toInt() != 0)
    return nullptr;

  const QJsonArray posArr = posObj.value(QLatin1String("k")).toArray();
  const QJsonArray sizeArr = sizeObj.value(QLatin1String("k")).toArray();
  if (posArr.size() < 2 || sizeArr.size() < 2)
    return nullptr;

  const double px = posArr.at(0).toDouble();
  const double py = posArr.at(1).toDouble();
  const double sx = sizeArr.at(0).toDouble();
  const double sy = sizeArr.at(1).toDouble();
  const double radius = radiusObj.value(QLatin1String("k")).toDouble(0.0);

  const double x = px - sx * 0.5;
  const double y = py - sy * 0.5;

  auto path = std::make_unique<LottieShapePath>();
  const BLGeometryDirection direction = obj.value(QLatin1String("d")).toInt(1) == 1 ? BL_GEOMETRY_DIRECTION_CW : BL_GEOMETRY_DIRECTION_CCW;

  BLPath built;
  if (radius <= 0.0) {
    built.add_rect(x, y, sx, sy, direction);
  }
  else {
    const double clamped = std::min(radius, std::min(std::abs(sx), std::abs(sy)) * 0.5);
    BLRoundRect rr(x, y, sx, sy, clamped, clamped);
    built.add_round_rect(rr, direction);
  }

  path->animated = false;
  path->cached_path = std::move(built);
  path->cache_valid = true;
  return path;
}

std::unique_ptr<LottieShapePath> parse_ellipse(const QJsonObject& obj) {
  const QJsonObject posObj = obj.value(QLatin1String("p")).toObject();
  const QJsonObject sizeObj = obj.value(QLatin1String("s")).toObject();

  if (posObj.value(QLatin1String("a")).toInt() != 0)
    return nullptr;
  if (sizeObj.value(QLatin1String("a")).toInt() != 0)
    return nullptr;

  const QJsonArray posArr = posObj.value(QLatin1String("k")).toArray();
  const QJsonArray sizeArr = sizeObj.value(QLatin1String("k")).toArray();
  if (posArr.size() < 2 || sizeArr.size() < 2)
    return nullptr;

  const double cx = posArr.at(0).toDouble();
  const double cy = posArr.at(1).toDouble();
  const double rx = sizeArr.at(0).toDouble() * 0.5;
  const double ry = sizeArr.at(1).toDouble() * 0.5;

  auto path = std::make_unique<LottieShapePath>();
  const BLGeometryDirection direction = obj.value(QLatin1String("d")).toInt(1) == 1 ? BL_GEOMETRY_DIRECTION_CW : BL_GEOMETRY_DIRECTION_CCW;

  BLPath built;
  built.add_ellipse(BLEllipse(cx, cy, rx, ry), direction);

  path->animated = false;
  path->cached_path = std::move(built);
  path->cache_valid = true;
  return path;
}

std::unique_ptr<LottieGradientFill> parse_gradient_fill(const QJsonObject& obj) {
  auto fill = std::make_unique<LottieGradientFill>();
  fill->gradient_type = obj.value(QLatin1String("t")).toInt(1);
  fill->fill_rule = obj.value(QLatin1String("r")).toInt(1);

  parse_animated_vec2(obj.value(QLatin1String("s")), fill->start, LottieVec2{0.0, 0.0});
  parse_animated_vec2(obj.value(QLatin1String("e")), fill->end, LottieVec2{0.0, 0.0});
  parse_animated_double(obj.value(QLatin1String("o")), fill->opacity, 100.0);

  const QJsonObject grad = obj.value(QLatin1String("g")).toObject();
  const int stop_count = grad.value(QLatin1String("p")).toInt();
  const QJsonValue stops_value = grad.value(QLatin1String("k"));

  auto extract_gradient_values = [](const QJsonValue& value) -> QJsonArray {
    if (value.isObject()) {
      const QJsonObject obj = value.toObject();
      if (obj.value(QLatin1String("a")).toInt() == 0)
        return obj.value(QLatin1String("k")).toArray();

      const QJsonArray frames = obj.value(QLatin1String("k")).toArray();
      for (const QJsonValue& frameValue : frames) {
        if (!frameValue.isObject())
          continue;
        const QJsonObject frameObj = frameValue.toObject();
        const QJsonArray components = frameObj.value(QLatin1String("s")).toArray();
        if (!components.isEmpty() && components.first().isArray())
          return components.first().toArray();
        if (!components.isEmpty())
          return components;
      }
      return QJsonArray();
    }

    if (value.isArray())
      return value.toArray();

    return QJsonArray();
  };

  const QJsonArray values = extract_gradient_values(stops_value);
  if (values.isEmpty())
    return nullptr;

  const qsizetype value_count = values.size();
  const int derived_stop_count = int(value_count / 4);
  int color_stop_count = stop_count > 0 ? std::min(stop_count, derived_stop_count) : derived_stop_count;

  if (color_stop_count < 0)
    color_stop_count = 0;

  std::vector<LottieGradientStop> stops;
  stops.reserve(size_t(color_stop_count));

  for (int i = 0; i < color_stop_count; i++) {
    int base = i * 4;
    double offset = values.at(base).toDouble();
    if (offset < 0.0) offset = 0.0;
    if (offset > 1.0) offset = 1.0;
    LottieColor color;
    color.r = values.at(base + 1).toDouble();
    color.g = values.at(base + 2).toDouble();
    color.b = values.at(base + 3).toDouble();
    color.a = 1.0;
    stops.push_back(LottieGradientStop{offset, color});
  }

  std::vector<std::pair<double, double>> alphaStops;
  for (int i = color_stop_count * 4; i + 1 < values.size(); i += 2) {
    double offset = values.at(i).toDouble();
    if (offset < 0.0) offset = 0.0;
    if (offset > 1.0) offset = 1.0;
    double alpha = values.at(i + 1).toDouble();
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    alphaStops.emplace_back(offset, alpha);
  }

  auto alpha_at = [&](double offset) noexcept -> double {
    if (alphaStops.empty())
      return 1.0;
    if (offset <= alphaStops.front().first)
      return alphaStops.front().second;
    if (offset >= alphaStops.back().first)
      return alphaStops.back().second;

    for (size_t i = 0; i + 1 < alphaStops.size(); i++) {
      double p0 = alphaStops[i].first;
      double p1 = alphaStops[i + 1].first;
      double a0 = alphaStops[i].second;
      double a1 = alphaStops[i + 1].second;
      if (offset >= p0 && offset <= p1) {
        double denom = p1 - p0;
        double t = denom != 0.0 ? (offset - p0) / denom : 0.0;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        return a0 + (a1 - a0) * t;
      }
    }
    return alphaStops.back().second;
  };

  for (auto& stop : stops)
    stop.color.a = alpha_at(stop.offset);

  if (stops.empty())
    return nullptr;

  std::sort(stops.begin(), stops.end(), [](const LottieGradientStop& a, const LottieGradientStop& b) noexcept {
    return a.offset < b.offset;
  });

  fill->stops = std::move(stops);
  return fill;
}

std::unique_ptr<LottieTrimPath> parse_trim_path(const QJsonObject& obj) {
  auto trim = std::make_unique<LottieTrimPath>();
  parse_animated_double(obj.value(QLatin1String("s")), trim->start, 0.0);
  parse_animated_double(obj.value(QLatin1String("e")), trim->end, 100.0);
  parse_animated_double(obj.value(QLatin1String("o")), trim->offset, 0.0);
  int mode = obj.value(QLatin1String("m")).toInt(1);
  if (mode != 1 && mode != 2)
    mode = 1;
  trim->mode = mode;
  trim->enabled = !obj.value(QLatin1String("hd")).toBool(false);
  return trim;
}

std::unique_ptr<LottieFill> parse_fill(const QJsonObject& obj) {
  auto fill = std::make_unique<LottieFill>();
  parse_animated_color(obj.value(QLatin1String("c")), fill->color, LottieColor{0.0, 0.0, 0.0, 1.0});
  parse_animated_double(obj.value(QLatin1String("o")), fill->opacity, 100.0);
  fill->fill_rule = obj.value(QLatin1String("r")).toInt(1);
  return fill;
}

std::unique_ptr<LottieStroke> parse_stroke(const QJsonObject& obj) {
  auto stroke = std::make_unique<LottieStroke>();
  parse_animated_color(obj.value(QLatin1String("c")), stroke->color, LottieColor{0.0, 0.0, 0.0, 1.0});
  parse_animated_double(obj.value(QLatin1String("o")), stroke->opacity, 100.0);
  parse_animated_double(obj.value(QLatin1String("w")), stroke->width, 1.0);

  stroke->cap = obj.value(QLatin1String("lc")).toInt(1);
  stroke->join = obj.value(QLatin1String("lj")).toInt(1);
  stroke->miter_limit = obj.value(QLatin1String("ml")).toDouble(4.0);
  return stroke;
}

std::unique_ptr<LottieGroup> parse_group(const QJsonObject& obj);

std::unique_ptr<LottieNode> parse_shape_item(const QJsonObject& obj) {
  const QString type = obj.value(QLatin1String("ty")).toString();

  if (type == QLatin1String("gr"))
    return parse_group(obj);

  if (type == QLatin1String("sh"))
    return parse_shape_path(obj);

  if (type == QLatin1String("rc"))
    return parse_rectangle(obj);

  if (type == QLatin1String("el"))
    return parse_ellipse(obj);

  if (type == QLatin1String("sr"))
    return parse_polystar(obj);

  if (type == QLatin1String("gf"))
    return parse_gradient_fill(obj);

  if (type == QLatin1String("fl"))
    return parse_fill(obj);

  if (type == QLatin1String("st"))
    return parse_stroke(obj);

  return nullptr;
}

std::unique_ptr<LottieGroup> parse_group(const QJsonObject& obj) {
  auto group = std::make_unique<LottieGroup>();
  const QJsonArray items = obj.value(QLatin1String("it")).toArray();

  for (const QJsonValue& itemValue : items) {
    if (!itemValue.isObject())
      continue;
    const QJsonObject itemObj = itemValue.toObject();
    const QString type = itemObj.value(QLatin1String("ty")).toString();
    if (type == QLatin1String("tr")) {
      parse_transform_object(itemObj, group->transform);
      continue;
    }

    if (type == QLatin1String("tm")) {
      std::unique_ptr<LottieTrimPath> trim = parse_trim_path(itemObj);
      if (trim)
        group->trims.push_back(std::move(trim));
      continue;
    }

    std::unique_ptr<LottieNode> node = parse_shape_item(itemObj);
    if (node)
      group->children.push_back(std::move(node));
  }

  return group;
}

BLRgba32 make_rgba32(const LottieColor& color, double alpha_scale) noexcept {
  auto clamp_unit = [](double v) noexcept {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
  };

  const double alpha = clamp_unit(color.a * alpha_scale);
  const double r = clamp_unit(color.r);
  const double g = clamp_unit(color.g);
  const double b = clamp_unit(color.b);

  const uint32_t ri = uint32_t(std::round(r * 255.0));
  const uint32_t gi = uint32_t(std::round(g * 255.0));
  const uint32_t bi = uint32_t(std::round(b * 255.0));
  const uint32_t ai = uint32_t(std::round(alpha * 255.0));

  return BLRgba32(ri, gi, bi, ai);
}

BLStrokeCap map_stroke_cap(int cap) noexcept {
  switch (cap) {
    case 2: return BL_STROKE_CAP_ROUND;
    case 3: return BL_STROKE_CAP_SQUARE;
    default: return BL_STROKE_CAP_BUTT;
  }
}

BLStrokeJoin map_stroke_join(int join) noexcept {
  switch (join) {
    case 2: return BL_STROKE_JOIN_ROUND;
    case 3: return BL_STROKE_JOIN_BEVEL;
    default: return BL_STROKE_JOIN_MITER_CLIP;
  }
}

void render_group(const LottieGroup& group,
                  BLContext& ctx,
                  double frame,
                  const BLMatrix2D& parent_matrix,
                  double opacity,
                  const std::vector<const LottieTrimPath*>& inherited_trims);

void render_group(const LottieGroup& group,
                  BLContext& ctx,
                  double frame,
                  const BLMatrix2D& parent_matrix,
                  double opacity,
                  const std::vector<const LottieTrimPath*>& inherited_trims) {
  const double local_opacity = opacity * group.transform.opacity_at(frame);
  if (local_opacity <= 0.0)
    return;

  const BLMatrix2D matrix = lottie_matrix_multiply(parent_matrix, group.transform.matrix(frame));
  std::vector<const LottieShapePath*> path_stack;
  path_stack.reserve(8);
  std::vector<const LottieTrimPath*> trim_stack = inherited_trims;
  trim_stack.reserve(trim_stack.size() + group.trims.size());
  for (const std::unique_ptr<LottieTrimPath>& trim : group.trims) {
    if (trim && trim->enabled)
      trim_stack.push_back(trim.get());
  }
  bool path_consumed = false;

  struct DrawCommand {
    enum Type { kGroup, kFill, kStroke, kGradientFill } type;

    const LottieGroup* group {};
    BLMatrix2D matrix {};
    double opacity {};
    std::vector<const LottieTrimPath*> trims;

    BLPath path {};
    BLFillRule fill_rule {BL_FILL_RULE_NON_ZERO};
    BLRgba32 color {};
    BLGradient gradient;

    double stroke_width {};
    BLStrokeCap stroke_cap {BL_STROKE_CAP_BUTT};
    BLStrokeJoin stroke_join {BL_STROKE_JOIN_MITER_CLIP};
    double miter_limit {};
  };

  std::vector<DrawCommand> commands;
  commands.reserve(group.children.size());

  auto build_combined_path = [&](BLPath& out) {
    out.clear();
    for (const LottieShapePath* path : path_stack) {
      if (!path)
        continue;
      const BLPath& source = path->path_at(frame);
      if (source.is_empty())
        continue;

      const BLPath* current = &source;
      BLPath temp_a;
      BLPath temp_b;
      bool use_a = true;

      for (const LottieTrimPath* trim : trim_stack) {
        if (!trim || !trim->enabled)
          continue;
        BLPath& dst = use_a ? temp_a : temp_b;
        dst.clear();
        if (apply_trim_to_path(*current, *trim, frame, dst)) {
          if (dst.is_empty()) {
            current = nullptr;
            break;
          }
          current = &dst;
          use_a = !use_a;
        }
      }

      if (!current || current->is_empty())
        continue;

      BLPath transformed(*current);
      transformed.transform(matrix);
      out.add_path(transformed);
    }
  };

  auto emit_group_command = [&](const LottieGroup& child_group) {
    DrawCommand cmd;
    cmd.type = DrawCommand::kGroup;
    cmd.group = &child_group;
    cmd.matrix = matrix;
    cmd.opacity = local_opacity;
    cmd.trims = trim_stack;
    commands.push_back(std::move(cmd));
  };

  auto emit_fill_command = [&](const LottieFill& fill, const BLPath& combined, BLFillRule rule, BLRgba32 rgba) {
    DrawCommand cmd;
    cmd.type = DrawCommand::kFill;
    cmd.path = combined;
    cmd.fill_rule = rule;
    cmd.color = rgba;
    commands.push_back(std::move(cmd));
  };

  auto emit_stroke_command = [&](const LottieStroke& stroke, const BLPath& combined, BLRgba32 rgba, double width, BLStrokeCap cap, BLStrokeJoin join, double miter_limit) {
    DrawCommand cmd;
    cmd.type = DrawCommand::kStroke;
    cmd.path = combined;
    cmd.color = rgba;
    cmd.stroke_width = width;
    cmd.stroke_cap = cap;
    cmd.stroke_join = join;
    cmd.miter_limit = miter_limit;
    commands.push_back(std::move(cmd));
  };

  auto emit_gradient_fill_command = [&](const BLPath& combined, BLFillRule rule, BLGradient&& gradient) {
    DrawCommand cmd;
    cmd.type = DrawCommand::kGradientFill;
    cmd.path = combined;
    cmd.fill_rule = rule;
    cmd.gradient = std::move(gradient);
    commands.push_back(std::move(cmd));
  };

  for (const auto& child : group.children) {
    switch (child->type) {
      case LottieNode::kGroup: {
        if (!path_stack.empty()) {
          path_stack.clear();
          path_consumed = false;
        }
        emit_group_command(static_cast<const LottieGroup&>(*child));
        break;
      }

      case LottieNode::kPath:
        if (path_consumed && !path_stack.empty()) {
          path_stack.clear();
          path_consumed = false;
        }
        path_stack.push_back(static_cast<const LottieShapePath*>(child.get()));
        break;

      case LottieNode::kFill: {
        const LottieFill& fill = static_cast<const LottieFill&>(*child);
        const double style_opacity = fill.opacity.evaluate(frame) * 0.01;
        const double final_opacity = local_opacity * style_opacity;
        if (final_opacity > 0.0 && !path_stack.empty()) {
          const LottieColor color = fill.color.evaluate(frame);
          const BLRgba32 rgba = make_rgba32(color, final_opacity);
          const BLFillRule rule = fill.fill_rule == 2 ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;

          BLPath combined;
          build_combined_path(combined);
          if (!combined.is_empty())
            emit_fill_command(fill, combined, rule, rgba);
        }
        path_consumed = true;
        break;
      }

      case LottieNode::kGradientFill: {
        const LottieGradientFill& gradient = static_cast<const LottieGradientFill&>(*child);
        const double style_opacity = gradient.opacity.evaluate(frame) * 0.01;
        const double final_opacity = local_opacity * style_opacity;
        if (final_opacity > 0.0 && !path_stack.empty() && !gradient.stops.empty()) {
          const LottieVec2 start = gradient.start.evaluate(frame);
          const LottieVec2 end = gradient.end.evaluate(frame);

          const BLPoint p0 = matrix.map_point(start.x, start.y);
          const BLPoint p1 = matrix.map_point(end.x, end.y);

          BLGradient bl_gradient;
          if (gradient.gradient_type == 2) {
            double radius = std::hypot(p1.x - p0.x, p1.y - p0.y);
            if (radius <= 0.0)
              radius = 0.0001;
            bl_gradient = BLGradient(BLRadialGradientValues(p0.x, p0.y, 0.0, p1.x, p1.y, radius));
          }
          else {
            bl_gradient = BLGradient(BLLinearGradientValues(p0.x, p0.y, p1.x, p1.y));
          }

          for (const LottieGradientStop& stop : gradient.stops) {
            const BLRgba32 rgba = make_rgba32(stop.color, final_opacity);
            bl_gradient.add_stop(stop.offset, rgba);
          }

          const BLFillRule rule = gradient.fill_rule == 2 ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;
          BLPath combined;
          build_combined_path(combined);
          if (!combined.is_empty())
            emit_gradient_fill_command(combined, rule, std::move(bl_gradient));
        }
        path_consumed = true;
        break;
      }

      case LottieNode::kStroke: {
        const LottieStroke& stroke = static_cast<const LottieStroke&>(*child);
        const double style_opacity = stroke.opacity.evaluate(frame) * 0.01;
        const double final_opacity = local_opacity * style_opacity;
        if (final_opacity > 0.0 && !path_stack.empty()) {
          const double width = stroke.width.evaluate(frame);
          const BLStrokeCap cap = map_stroke_cap(stroke.cap);
          const BLStrokeJoin join = map_stroke_join(stroke.join);
          const double miter_limit = stroke.miter_limit;

          const LottieColor color = stroke.color.evaluate(frame);
          const BLRgba32 rgba = make_rgba32(color, final_opacity);

          BLPath combined;
          build_combined_path(combined);
          if (!combined.is_empty())
            emit_stroke_command(stroke, combined, rgba, width, cap, join, miter_limit);
        }
        path_consumed = true;
        break;
      }
    }
  }

  for (auto it = commands.rbegin(); it != commands.rend(); ++it) {
    const DrawCommand& cmd = *it;
    switch (cmd.type) {
      case DrawCommand::kGroup:
        render_group(*cmd.group, ctx, frame, cmd.matrix, cmd.opacity, cmd.trims);
        break;

      case DrawCommand::kFill:
        ctx.set_fill_rule(cmd.fill_rule);
        ctx.fill_path(cmd.path, cmd.color);
        break;

      case DrawCommand::kGradientFill:
        ctx.set_fill_rule(cmd.fill_rule);
        ctx.fill_path(cmd.path, cmd.gradient);
        break;

      case DrawCommand::kStroke:
        ctx.set_stroke_width(cmd.stroke_width);
        ctx.set_stroke_caps(cmd.stroke_cap);
        ctx.set_stroke_join(cmd.stroke_join);
        ctx.set_stroke_miter_limit(cmd.miter_limit);
        ctx.stroke_path(cmd.path, cmd.color);
        break;
    }
  }
}

} // namespace

void LottieComposition::render_layer_content(const LottieLayer& layer,
                                             BLContext& ctx,
                                             double frame,
                                             const BLMatrix2D& layer_matrix,
                                             double opacity) const {
  if (opacity <= 0.0)
    return;

  const std::vector<const LottieTrimPath*> empty_trims;

  auto paint_layer = [&](BLContext& dst_ctx, double local_opacity) -> bool {
    if (local_opacity <= 0.0)
      return false;

    if (layer.root) {
      render_group(*layer.root, dst_ctx, frame, layer_matrix, local_opacity, empty_trims);
      return true;
    }

    if (layer.is_solid && layer.solid_width > 0.0 && layer.solid_height > 0.0) {
      dst_ctx.save();
      dst_ctx.apply_transform(layer_matrix);
      const BLRgba32 color = make_rgba32(layer.solid_color, local_opacity);
      if (color.a() != 0)
        dst_ctx.fill_rect(BLRect(0.0, 0.0, layer.solid_width, layer.solid_height), color);
      dst_ctx.restore();
      return true;
    }

    if (layer.image_index >= 0 && size_t(layer.image_index) < _images.size()) {
      const LottieImageAsset& image = _images[layer.image_index];
      if (!image.image || image.width <= 0.0 || image.height <= 0.0)
        return false;

      dst_ctx.save();
      dst_ctx.apply_transform(layer_matrix);
      const double previous_alpha = dst_ctx.global_alpha();
      dst_ctx.set_global_alpha(previous_alpha * local_opacity);
      dst_ctx.blit_image(BLRect(0.0, 0.0, image.width, image.height), image.image);
      dst_ctx.set_global_alpha(previous_alpha);
      dst_ctx.restore();
      return true;
    }

    if (layer.precomp_index >= 0 && size_t(layer.precomp_index) < _precomps.size()) {
      const LottiePrecomposition& precomp = _precomps[layer.precomp_index];
      render_layer_array(precomp.layers, dst_ctx, frame, layer_matrix, local_opacity);
      return true;
    }

    return false;
  };

  if (!layer.masks.empty()) {
    const BLSize target_size = ctx.target_size();
    const int canvas_width = std::max(1, int(std::ceil(target_size.w)));
    const int canvas_height = std::max(1, int(std::ceil(target_size.h)));

    BLImage content;
    if (content.create(canvas_width, canvas_height, BL_FORMAT_PRGB32) != BL_SUCCESS) {
      paint_layer(ctx, opacity);
      return;
    }

    bool painted = false;
    {
      BLContext content_ctx(content);
      content_ctx.clear_all();
      painted = paint_layer(content_ctx, opacity);
    }

    if (!painted) {
      return;
    }

    BLImage coverage;
    bool mask_applied = false;
    if (coverage.create(canvas_width, canvas_height, BL_FORMAT_PRGB32) == BL_SUCCESS) {
      BLContext mask_ctx(coverage);
      mask_ctx.clear_all();

      const bool has_positive_add = std::any_of(layer.masks.begin(), layer.masks.end(), [](const LottieMask& mask) {
        return mask.mode == LottieMask::kAdd && !mask.inverted;
      });

      bool coverage_initialized = false;
      if (!has_positive_add) {
        mask_ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
        mask_ctx.fill_all(BLRgba32(255, 255, 255, 255));
        coverage_initialized = true;
      }

      for (const LottieMask& mask : layer.masks) {
        if (mask.mode == LottieMask::kUnknown)
          continue;

        double mask_opacity = std::clamp(mask.opacity.evaluate(frame) * 0.01, 0.0, 1.0);
        if (mask_opacity <= 0.0)
          continue;

        const BLPath& mask_path_source = mask.path.path_at(frame);
        if (mask_path_source.is_empty())
          continue;

        BLPath mask_path(mask_path_source);
        mask_path.transform(layer_matrix);

        const uint32_t alpha_byte = uint32_t(std::round(mask_opacity * 255.0));
        mask_ctx.set_fill_style(BLRgba32(255, 255, 255, alpha_byte));

        switch (mask.mode) {
          case LottieMask::kAdd: {
            if (mask.inverted && !coverage_initialized) {
              mask_ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
              mask_ctx.fill_all(BLRgba32(255, 255, 255, 255));
              coverage_initialized = true;
            }
            mask_ctx.set_comp_op(mask.inverted
                                  ? BL_COMP_OP_DST_OUT
                                  : (coverage_initialized ? BL_COMP_OP_SRC_OVER : BL_COMP_OP_SRC_COPY));
            coverage_initialized = true;
            break;
          }

          case LottieMask::kSubtract: {
            if (!coverage_initialized) {
              mask_ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
              mask_ctx.fill_all(BLRgba32(255, 255, 255, 255));
              coverage_initialized = true;
            }
            mask_ctx.set_comp_op(mask.inverted
                                  ? (coverage_initialized ? BL_COMP_OP_SRC_OVER : BL_COMP_OP_SRC_COPY)
                                  : BL_COMP_OP_DST_OUT);
            break;
          }

          case LottieMask::kIntersect: {
            if (!coverage_initialized) {
              mask_ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
              mask_ctx.fill_all(BLRgba32(255, 255, 255, 255));
              coverage_initialized = true;
            }
            mask_ctx.set_comp_op(mask.inverted ? BL_COMP_OP_DST_OUT : BL_COMP_OP_DST_IN);
            break;
          }

          default:
            continue;
        }

        mask_ctx.fill_path(mask_path);
        mask_applied = true;
      }
    }

    if (mask_applied) {
      BLContext content_ctx(content);
      content_ctx.set_comp_op(BL_COMP_OP_DST_IN);
      content_ctx.blit_image(BLPoint(0, 0), coverage);
    }

    ctx.blit_image(BLPoint(0, 0), content);
    return;
  }

  paint_layer(ctx, opacity);
}

double lottie_lerp(const double a, const double b, const double t) noexcept {
  return a + (b - a) * t;
}

LottieVec2 lottie_lerp(const LottieVec2& a, const LottieVec2& b, double t) noexcept {
  return LottieVec2{lottie_lerp(a.x, b.x, t), lottie_lerp(a.y, b.y, t)};
}

LottieColor lottie_lerp(const LottieColor& a, const LottieColor& b, double t) noexcept {
  return LottieColor{
    lottie_lerp(a.r, b.r, t),
    lottie_lerp(a.g, b.g, t),
    lottie_lerp(a.b, b.b, t),
    lottie_lerp(a.a, b.a, t)
  };
}

LottieTransform::LottieTransform() {
  anchor.value = LottieVec2{0.0, 0.0};
  position.value = LottieVec2{0.0, 0.0};
  scale.value = LottieVec2{100.0, 100.0};
  rotation.value = 0.0;
  skew.value = 0.0;
  skew_axis.value = 0.0;
  opacity.value = 100.0;
}

LottieShapePath::LottieShapePath()
  : LottieNode(LottieNode::kPath) {}

const BLPath& LottieShapePath::path_at(double frame) const {
  auto refresh_cache = [&](const ShapeData& data) -> const BLPath& {
    cached_path = build_path_from_shape(data);
    cached_frame = frame;
    cache_valid = true;
    return cached_path;
  };

  if (!animated) {
    if (cache_valid)
      return cached_path;
    if (!shape.vertices.empty())
      return refresh_cache(shape);
    return cached_path;
  }

  if (cache_valid && cached_frame == frame)
    return cached_path;

  const ShapeData* shape_ptr = nullptr;

  if (frame <= keyframes.front().time) {
    shape_ptr = &keyframes.front().shape;
  }
  else if (frame >= keyframes.back().time) {
    shape_ptr = &keyframes.back().shape;
  }
  else {
    for (size_t i = 0; i + 1 < keyframes.size(); i++) {
      const Keyframe& k0 = keyframes[i];
      const Keyframe& k1 = keyframes[i + 1];
      if (frame < k1.time) {
        if (k0.hold || k0.shape.vertices.size() != k1.shape.vertices.size()) {
          shape_ptr = &k0.shape;
        }
        else {
          const double denom = k1.time - k0.time;
          double t = denom != 0.0 ? (frame - k0.time) / denom : 0.0;
          if (t < 0.0) t = 0.0;
          if (t > 1.0) t = 1.0;

          const size_t count = k0.shape.vertices.size();
          interpolated_shape.vertices.resize(count);
          interpolated_shape.in_tangents.resize(count);
          interpolated_shape.out_tangents.resize(count);
          interpolated_shape.closed = k0.shape.closed;

          for (size_t j = 0; j < count; j++) {
            interpolated_shape.vertices[j] = lottie_lerp(k0.shape.vertices[j], k1.shape.vertices[j], t);
            interpolated_shape.in_tangents[j] = lottie_lerp(k0.shape.in_tangents[j], k1.shape.in_tangents[j], t);
            interpolated_shape.out_tangents[j] = lottie_lerp(k0.shape.out_tangents[j], k1.shape.out_tangents[j], t);
          }

          shape_ptr = &interpolated_shape;
        }
        break;
      }
    }
  }

  if (!shape_ptr)
    shape_ptr = &keyframes.back().shape;

  return refresh_cache(*shape_ptr);
}

LottiePolystar::LottiePolystar()
  : LottieShapePath() {
  points.value = 5.0;
  position.value = LottieVec2{0.0, 0.0};
  rotation.value = 0.0;
  outer_radius.value = 0.0;
  outer_roundness.value = 0.0;
  inner_radius.value = 0.0;
  inner_roundness.value = 0.0;
  animated = true;
  cache_valid = false;
}

const BLPath& LottiePolystar::path_at(double frame) const {
  if (cache_valid && cached_frame == frame)
    return cached_path;

  cached_path.clear();

  constexpr double kDegToRad = kPi / 180.0;
  constexpr double kPolystarMagic = 0.47829 / 0.28;
  constexpr double kPolygonMagic = 0.25;

  const double evaluated_points = std::max(points.evaluate(frame), 0.0);
  const double evaluated_outer = std::abs(outer_radius.evaluate(frame));
  const double evaluated_inner = std::abs(inner_radius.evaluate(frame));
  const double evaluated_rotation = rotation.evaluate(frame);
  const LottieVec2 center = position.evaluate(frame);
  const int direction_sign = direction >= 0 ? 1 : -1;

  if (evaluated_outer <= 0.0 || evaluated_points < 1.0) {
    cached_frame = frame;
    cache_valid = true;
    return cached_path;
  }

  if (star_type == 1) {
    const double pts_count = std::max(evaluated_points, 1.0);
    const double outer_round = std::clamp(outer_roundness.evaluate(frame) * 0.01, 0.0, 1.0);
    const double inner_round = std::clamp(inner_roundness.evaluate(frame) * 0.01, 0.0, 1.0);
    const double direction_value = double(direction_sign);

    const double angle_per_point = (2.0 * kPi) / pts_count;
    const double half_angle_per_point = angle_per_point * 0.5;
    const double partial_amount = pts_count - std::floor(pts_count);

    double angle = (evaluated_rotation - 90.0) * kDegToRad;
    double partial_radius = 0.0;
    bool has_roundness = (outer_round > 0.0 || inner_round > 0.0);

    if (partial_amount != 0.0)
      angle += half_angle_per_point * (1.0 - partial_amount) * direction_value;

    double x = 0.0;
    double y = 0.0;
    if (partial_amount != 0.0) {
      partial_radius = evaluated_inner + partial_amount * (evaluated_outer - evaluated_inner);
      x = partial_radius * std::cos(angle);
      y = partial_radius * std::sin(angle);
      angle += angle_per_point * partial_amount * 0.5 * direction_value;
    }
    else {
      x = evaluated_outer * std::cos(angle);
      y = evaluated_outer * std::sin(angle);
      angle += half_angle_per_point * direction_value;
    }

    cached_path.move_to(center.x + x, center.y + y);

    const size_t num_points = size_t(std::ceil(pts_count) * 2.0);
    if (num_points == 0) {
      cached_path.close();
      cached_frame = frame;
      cache_valid = true;
      return cached_path;
    }

    bool long_segment = false;
    for (size_t i = 0; i < num_points; ++i) {
      double radius = long_segment ? evaluated_outer : evaluated_inner;
      double delta_theta = half_angle_per_point;

      if (partial_radius != 0.0 && i == num_points - 2)
        delta_theta = angle_per_point * partial_amount * 0.5;
      if (partial_radius != 0.0 && i == num_points - 1)
        radius = partial_radius;

      const double previous_x = x;
      const double previous_y = y;
      x = radius * std::cos(angle);
      y = radius * std::sin(angle);

      if (has_roundness) {
        const double cp1_theta = std::atan2(previous_y, previous_x) - (kPi * 0.5) * direction_value;
        const double cp2_theta = std::atan2(y, x) - (kPi * 0.5) * direction_value;

        const double cp1_dir_x = std::cos(cp1_theta);
        const double cp1_dir_y = std::sin(cp1_theta);
        const double cp2_dir_x = std::cos(cp2_theta);
        const double cp2_dir_y = std::sin(cp2_theta);

        const double cp1_round = long_segment ? inner_round : outer_round;
        const double cp2_round = long_segment ? outer_round : inner_round;
        const double cp1_radius = long_segment ? evaluated_inner : evaluated_outer;
        const double cp2_radius = long_segment ? evaluated_outer : evaluated_inner;

        double cp1x = cp1_radius * cp1_round * kPolystarMagic * cp1_dir_x / pts_count;
        double cp1y = cp1_radius * cp1_round * kPolystarMagic * cp1_dir_y / pts_count;
        double cp2x = cp2_radius * cp2_round * kPolystarMagic * cp2_dir_x / pts_count;
        double cp2y = cp2_radius * cp2_round * kPolystarMagic * cp2_dir_y / pts_count;

        if (partial_amount != 0.0 && (i == 0 || i == num_points - 1)) {
          cp1x *= partial_amount;
          cp1y *= partial_amount;
          cp2x *= partial_amount;
          cp2y *= partial_amount;
        }

        cached_path.cubic_to(center.x + previous_x - cp1x,
                             center.y + previous_y - cp1y,
                             center.x + x + cp2x,
                             center.y + y + cp2y,
                             center.x + x,
                             center.y + y);
      }
      else {
        cached_path.line_to(center.x + x, center.y + y);
      }

      angle += delta_theta * direction_value;
      long_segment = !long_segment;
    }

    cached_path.close();
    cached_frame = frame;
    cache_valid = true;
    return cached_path;
  }

  const size_t polygon_count = size_t(std::max(2.0, std::floor(evaluated_points)));
  if (polygon_count < 2) {
    cached_frame = frame;
    cache_valid = true;
    return cached_path;
  }

  const double outer_round = std::clamp(outer_roundness.evaluate(frame) * 0.01, 0.0, 1.0);
  const bool has_roundness = outer_round > 0.0;
  const double direction_value = double(direction_sign);
  const double angle_per_point = (2.0 * kPi) / double(polygon_count);

  double angle = (evaluated_rotation - 90.0) * kDegToRad;
  double x = evaluated_outer * std::cos(angle);
  double y = evaluated_outer * std::sin(angle);
  cached_path.move_to(center.x + x, center.y + y);

  angle += angle_per_point * direction_value;
  const double coeff = angle_per_point * evaluated_outer * outer_round * kPolygonMagic;

  for (size_t i = 0; i < polygon_count; ++i) {
    const double previous_x = x;
    const double previous_y = y;
    x = evaluated_outer * std::cos(angle);
    y = evaluated_outer * std::sin(angle);

    if (has_roundness) {
      const double cp1_theta = std::atan2(previous_y, previous_x) - (kPi * 0.5) * direction_value;
      const double cp2_theta = std::atan2(y, x) - (kPi * 0.5) * direction_value;

      const double cp1x = coeff * std::cos(cp1_theta);
      const double cp1y = coeff * std::sin(cp1_theta);
      const double cp2x = coeff * std::cos(cp2_theta);
      const double cp2y = coeff * std::sin(cp2_theta);

      cached_path.cubic_to(center.x + previous_x - cp1x,
                           center.y + previous_y - cp1y,
                           center.x + x + cp2x,
                           center.y + y + cp2y,
                           center.x + x,
                           center.y + y);
    }
    else {
      cached_path.line_to(center.x + x, center.y + y);
    }

    angle += angle_per_point * direction_value;
  }

  cached_path.close();
  cached_frame = frame;
  cache_valid = true;
  return cached_path;
}

LottieMask::LottieMask() = default;

LottieFill::LottieFill()
  : LottieNode(LottieNode::kFill) {}

LottieGradientFill::LottieGradientFill()
  : LottieNode(LottieNode::kGradientFill) {}

LottieTrimPath::LottieTrimPath()
  : LottieNode(LottieNode::kTrim) {}

LottieStroke::LottieStroke()
  : LottieNode(LottieNode::kStroke) {}

LottieGroup::LottieGroup()
  : LottieNode(LottieNode::kGroup) {}

BLMatrix2D LottieTransform::matrix(double frame) const {
  const LottieVec2 pos = position.evaluate(frame);
  const LottieVec2 scl = scale.evaluate(frame);
  const LottieVec2 anc = anchor.evaluate(frame);
  const double angle = rotation.evaluate(frame) * (kPi / 180.0);
  const double skew_angle = skew.evaluate(frame) * (kPi / 180.0);
  const double skew_axis_angle = (this->skew_axis.evaluate(frame) + 90.0) * (kPi / 180.0);

  BLMatrix2D result = BLMatrix2D::make_identity();
  result = lottie_matrix_multiply(result, BLMatrix2D::make_translation(pos.x, pos.y));
  if (angle != 0.0)
    result = lottie_matrix_multiply(result, BLMatrix2D::make_rotation(angle));
  if (skew_angle != 0.0) {
    const double tan_skew = std::tan(skew_angle);
    const BLMatrix2D rot = BLMatrix2D::make_rotation(skew_axis_angle);
    const BLMatrix2D rot_inv = BLMatrix2D::make_rotation(-skew_axis_angle);
    const BLMatrix2D shear(1.0, 0.0, tan_skew, 1.0, 0.0, 0.0);
    result = lottie_matrix_multiply(result, rot);
    result = lottie_matrix_multiply(result, shear);
    result = lottie_matrix_multiply(result, rot_inv);
  }
  result = lottie_matrix_multiply(result, BLMatrix2D::make_scaling(scl.x * 0.01, scl.y * 0.01));
  if (anc.x != 0.0 || anc.y != 0.0)
    result = lottie_matrix_multiply(result, BLMatrix2D::make_translation(-anc.x, -anc.y));
  return result;
}

double LottieTransform::opacity_at(double frame) const {
  double value = opacity.evaluate(frame) * 0.01;
  if (value < 0.0) value = 0.0;
  if (value > 1.0) value = 1.0;
  return value;
}

bool LottieComposition::load_from_file(const QString& path, QString* error_message) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (error_message)
      *error_message = QString::fromLatin1("Failed to open Lottie file: %1").arg(path);
    return false;
  }

  const QByteArray data = file.readAll();
  QJsonParseError parse_error {};
  const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
  if (doc.isNull()) {
    if (error_message)
      *error_message = QString::fromLatin1("Failed to parse Lottie JSON: %1").arg(parse_error.errorString());
    return false;
  }

  const QJsonObject root = doc.object();
  _width = root.value(QLatin1String("w")).toDouble(0.0);
  _height = root.value(QLatin1String("h")).toDouble(0.0);
  _frame_rate = root.value(QLatin1String("fr")).toDouble(60.0);
  _in_point = root.value(QLatin1String("ip")).toDouble(0.0);
  _out_point = root.value(QLatin1String("op")).toDouble(_in_point);
  _name = root.value(QLatin1String("nm")).toString();
  _layers.clear();
  _images.clear();
  _precomps.clear();

  QHash<QString, int> image_index_map;

  const QJsonArray assets = root.value(QLatin1String("assets")).toArray();
  const QDir file_dir = QFileInfo(path).dir();
  _images.reserve(assets.size());

  auto load_image_asset = [&](const QJsonObject& asset_obj) -> void {
    const QString id = asset_obj.value(QLatin1String("id")).toString();
    if (id.isEmpty())
      return;

    const QString file_name = asset_obj.value(QLatin1String("p")).toString();
    if (file_name.isEmpty())
      return;

    const int embed = asset_obj.value(QLatin1String("e")).toInt();
    BLImage image;
    bool loaded = false;

    if (embed == 1 || file_name.startsWith(QLatin1String("data:"))) {
      QString data_str = file_name;
      const int comma_pos = data_str.indexOf(QLatin1Char(','));
      if (comma_pos >= 0)
        data_str = data_str.mid(comma_pos + 1);
      const QByteArray decoded = QByteArray::fromBase64(data_str.toUtf8());
      if (!decoded.isEmpty())
        loaded = image.read_from_data(decoded.constData(), size_t(decoded.size())) == BL_SUCCESS;
    }
    else {
      const QString base_path = asset_obj.value(QLatin1String("u")).toString();
      const QString absolute_path = file_dir.absoluteFilePath(base_path + file_name);
      const QByteArray encoded = QFile::encodeName(absolute_path);
      loaded = image.read_from_file(encoded.constData()) == BL_SUCCESS;
    }

    if (!loaded || !image)
      return;

    LottieImageAsset asset {};
    asset.id = id;
    asset.image = std::move(image);
    asset.width = asset_obj.value(QLatin1String("w")).toDouble(double(asset.image.width()));
    asset.height = asset_obj.value(QLatin1String("h")).toDouble(double(asset.image.height()));
    image_index_map.insert(id, _images.size());
    _images.push_back(std::move(asset));
  };

  for (const QJsonValue& assetValue : assets) {
    if (!assetValue.isObject())
      continue;
    const QJsonObject assetObj = assetValue.toObject();
    if (assetObj.contains(QLatin1String("p")))
      load_image_asset(assetObj);
  }

  auto finalize_parent_relationships = [](std::vector<LottieLayer>& layers) {
    QHash<int, int> index_map;
    index_map.reserve(int(layers.size()));
    for (int i = 0; i < int(layers.size()); i++) {
      const int layer_index = layers[size_t(i)].index;
      index_map.insert(layer_index, i);
    }

    for (LottieLayer& layer : layers) {
      if (layer.parent_index < 0)
        continue;
      const auto it = index_map.constFind(layer.parent_index);
      if (it != index_map.constEnd())
        layer.parent = *it;
    }
  };

  auto assign_track_mattes = [](std::vector<LottieLayer>& layers) {
    int pending = -1;
    int pending_mode = 0;
    for (size_t i = 0; i < layers.size(); ++i) {
      LottieLayer& layer = layers[i];
      if (layer.is_matte_source) {
        pending = int(i);
        pending_mode = layer.matte_source_mode;
        layer.hidden = true;
        continue;
      }
      if (layer.track_matte_mode > 0) {
        if (pending >= 0) {
          layer.matte_source = pending;
          layer.matte_mode = layer.track_matte_mode > 0 ? layer.track_matte_mode : pending_mode;
          layers[size_t(pending)].hidden = true;
          pending = -1;
          pending_mode = 0;
        }
        else {
          layer.matte_source = -1;
          layer.matte_mode = 0;
          layer.track_matte_mode = 0;
        }
      }
    }
  };

  auto parse_layer_array = [&](const QJsonArray& layer_array, std::vector<LottieLayer>& target) {
    target.clear();
    target.reserve(layer_array.size());

    for (const QJsonValue& layerValue : layer_array) {
      if (!layerValue.isObject())
        continue;
      const QJsonObject layerObj = layerValue.toObject();

      LottieLayer layer {};
      layer.type = layerObj.value(QLatin1String("ty")).toInt();
      layer.index = layerObj.value(QLatin1String("ind")).toInt();
      layer.parent_index = layerObj.contains(QLatin1String("parent"))
        ? layerObj.value(QLatin1String("parent")).toInt()
        : -1;
      layer.name = layerObj.value(QLatin1String("nm")).toString();
      layer.ref_id = layerObj.value(QLatin1String("refId")).toString();
      layer.track_matte_mode = layerObj.value(QLatin1String("tt")).toInt();
      layer.matte_source_mode = layerObj.value(QLatin1String("td")).toInt();
      layer.is_matte_source = layer.matte_source_mode != 0;
      layer.hidden = false;
      layer.matte_source = -1;
      layer.matte_mode = 0;
      layer.in_point = layerObj.value(QLatin1String("ip")).toDouble(_in_point);
      layer.out_point = layerObj.value(QLatin1String("op")).toDouble(_out_point);
      parse_transform_object(layerObj.value(QLatin1String("ks")).toObject(), layer.transform);

      const QJsonArray masks_array = layerObj.value(QLatin1String("masksProperties")).toArray();
      if (!masks_array.isEmpty()) {
        layer.masks.reserve(masks_array.size());
        for (const QJsonValue& mask_value : masks_array) {
          if (!mask_value.isObject())
            continue;
          LottieMask mask {};
          if (parse_mask(mask_value.toObject(), mask))
            layer.masks.push_back(std::move(mask));
        }
      }

      if (layer.type == 1) {
        layer.solid_width = layerObj.value(QLatin1String("sw")).toDouble(0.0);
        layer.solid_height = layerObj.value(QLatin1String("sh")).toDouble(0.0);
        if (layer.solid_width > 0.0 && layer.solid_height > 0.0) {
          layer.is_solid = true;
          layer.solid_color = parse_hex_color(layerObj.value(QLatin1String("sc")).toString());
        }
      }

      if (layer.type == 4) {
        const QJsonArray shapes = layerObj.value(QLatin1String("shapes")).toArray();
        if (!shapes.isEmpty()) {
          auto root_group = std::make_unique<LottieGroup>();
          for (const QJsonValue& shapeValue : shapes) {
            if (!shapeValue.isObject())
              continue;
            std::unique_ptr<LottieNode> node = parse_shape_item(shapeValue.toObject());
            if (node)
              root_group->children.push_back(std::move(node));
          }
          if (!root_group->children.empty())
            layer.root = std::move(root_group);
        }
      }

      target.push_back(std::move(layer));
    }

    finalize_parent_relationships(target);
    assign_track_mattes(target);
  };

  auto resolve_layer_resources = [&](std::vector<LottieLayer>& layers, const QHash<QString, int>& local_image_map, const QHash<QString, int>& local_precomp_map) {
    for (LottieLayer& layer : layers) {
      layer.image_index = -1;
      layer.precomp_index = -1;

      if (layer.type == 2 && !layer.ref_id.isEmpty()) {
        const auto it = local_image_map.constFind(layer.ref_id);
        if (it != local_image_map.constEnd())
          layer.image_index = *it;
      }
      else if (layer.type == 0 && !layer.ref_id.isEmpty()) {
        const auto it = local_precomp_map.constFind(layer.ref_id);
        if (it != local_precomp_map.constEnd())
          layer.precomp_index = *it;
      }
    }
  };

  QHash<QString, int> precomp_index_map;

  for (const QJsonValue& assetValue : assets) {
    if (!assetValue.isObject())
      continue;
    const QJsonObject assetObj = assetValue.toObject();
    if (!assetObj.contains(QLatin1String("layers")))
      continue;

    LottiePrecomposition precomp {};
    precomp.id = assetObj.value(QLatin1String("id")).toString();
    precomp.width = assetObj.value(QLatin1String("w")).toDouble(0.0);
    precomp.height = assetObj.value(QLatin1String("h")).toDouble(0.0);
    parse_layer_array(assetObj.value(QLatin1String("layers")).toArray(), precomp.layers);
    _precomps.push_back(std::move(precomp));
  }

  for (int i = 0; i < int(_precomps.size()); i++)
    precomp_index_map.insert(_precomps[size_t(i)].id, i);

  for (LottiePrecomposition& precomp : _precomps) {
    resolve_layer_resources(precomp.layers, image_index_map, precomp_index_map);
    assign_track_mattes(precomp.layers);
  }

  const QJsonArray layers = root.value(QLatin1String("layers")).toArray();
  parse_layer_array(layers, _layers);
  resolve_layer_resources(_layers, image_index_map, precomp_index_map);
  assign_track_mattes(_layers);

  bool has_renderable_layer = false;
  for (const LottieLayer& layer : _layers) {
    if ((layer.root && !layer.root->children.empty()) ||
        (layer.type == 2 && layer.image_index >= 0 && layer.image_index < int(_images.size())) ||
        (layer.type == 0 && layer.precomp_index >= 0 && layer.precomp_index < int(_precomps.size())) ||
        (layer.is_solid && layer.solid_width > 0.0 && layer.solid_height > 0.0)) {
      has_renderable_layer = true;
      break;
    }
  }

  if (!has_renderable_layer) {
    if (error_message)
      *error_message = QString::fromLatin1("No supported shape layers found in %1").arg(path);
    return false;
  }

  return true;
}

void LottieComposition::render(BLContext& ctx, double frame, const BLMatrix2D& root_matrix, double opacity) const {
  render_layer_array(_layers, ctx, frame, root_matrix, opacity);
}

void LottieComposition::render_layer_array(const std::vector<LottieLayer>& layers,
                                           BLContext& ctx,
                                           double frame,
                                           const BLMatrix2D& root_matrix,
                                           double opacity) const {
  if (layers.empty())
    return;

  const BLSize target_size = ctx.target_size();
  const int canvas_width = std::max(1, int(std::ceil(target_size.w)));
  const int canvas_height = std::max(1, int(std::ceil(target_size.h)));

  std::vector<BLMatrix2D> matrix_cache(layers.size());
  std::vector<uint8_t> matrix_valid(layers.size(), 0);
  std::function<BLMatrix2D(size_t)> resolve_matrix = [&](size_t index) -> BLMatrix2D {
    if (matrix_valid[index])
      return matrix_cache[index];

    BLMatrix2D mat = layers[index].transform.matrix(frame);
    const int parent = layers[index].parent;
    if (parent >= 0)
      mat = lottie_matrix_multiply(resolve_matrix(size_t(parent)), mat);

    matrix_cache[index] = mat;
    matrix_valid[index] = 1;
    return mat;
  };

  std::vector<double> opacity_cache(layers.size());
  std::vector<uint8_t> opacity_valid(layers.size(), 0);
  std::function<double(size_t)> resolve_opacity = [&](size_t index) -> double {
    if (opacity_valid[index])
      return opacity_cache[index];

    double value = layers[index].transform.opacity_at(frame);
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;

    opacity_cache[index] = value;
    opacity_valid[index] = 1;
    return value;
  };

  for (size_t i = layers.size(); i-- > 0;) {
    const LottieLayer& layer = layers[i];
    const bool has_vector = layer.root != nullptr;
    const bool has_solid = layer.is_solid && layer.solid_width > 0.0 && layer.solid_height > 0.0;
    const bool has_image = layer.image_index >= 0 && size_t(layer.image_index) < _images.size();
    const bool has_precomp = layer.precomp_index >= 0 && size_t(layer.precomp_index) < _precomps.size();
    const bool has_matte_target = layer.matte_source >= 0 && layer.matte_mode > 0;
    if (!has_vector && !has_solid && !has_image && !has_precomp && !has_matte_target)
      continue;

    if (layer.hidden && !has_matte_target)
      continue;

    if (frame < layer.in_point || frame >= layer.out_point)
      continue;

    const double layer_local_opacity = resolve_opacity(i);
    const double layer_opacity = opacity * layer_local_opacity;
    if (layer_opacity <= 0.0 && !has_matte_target)
      continue;

    const BLMatrix2D layer_matrix_local = resolve_matrix(i);
    const BLMatrix2D layer_matrix = lottie_matrix_multiply(root_matrix, layer_matrix_local);

    if (has_matte_target) {
      const int matte_index = layer.matte_source;
      if (matte_index < 0 || size_t(matte_index) >= layers.size())
        continue;

      const LottieLayer& matte_layer = layers[size_t(matte_index)];
      const BLMatrix2D matte_matrix_local = resolve_matrix(size_t(matte_index));
      const BLMatrix2D matte_matrix = lottie_matrix_multiply(root_matrix, matte_matrix_local);
      const double matte_opacity = opacity * resolve_opacity(size_t(matte_index));

      auto render_to_image = [&](const LottieLayer& srcLayer,
                                 const BLMatrix2D& matrix,
                                 double op) -> BLImage {
        BLImage img;
        if (img.create(canvas_width, canvas_height, BL_FORMAT_PRGB32) != BL_SUCCESS)
          return img;
        {
          BLContext imgCtx(img);
          imgCtx.clear_all();
          render_layer_content(srcLayer, imgCtx, frame, matrix, op);
        }
        return img;
      };

      const BLImage matte_image = render_to_image(matte_layer, matte_matrix, matte_opacity);
      BLImage content_image = render_to_image(layer, layer_matrix, layer_opacity);

      if (matte_image && content_image) {
        if (layer.matte_mode == 1 || layer.matte_mode == 2) {
          BLContext blendCtx(content_image);
          blendCtx.set_comp_op(layer.matte_mode == 1 ? BL_COMP_OP_DST_IN : BL_COMP_OP_DST_OUT);
          blendCtx.blit_image(BLPoint(0, 0), matte_image);
        }
        ctx.blit_image(BLPoint(0, 0), content_image);
      }
      continue;
    }

    render_layer_content(layer, ctx, frame, layer_matrix, layer_opacity);
  }
}
