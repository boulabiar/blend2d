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
    QJsonArray arr = value.toArray();
    if (!arr.isEmpty())
      return arr.first().toDouble(fallback);
  }
  return fallback;
}

LottieVec2 to_vec2(const QJsonValue& value, const LottieVec2& fallback) noexcept {
  if (!value.isArray())
    return fallback;

  QJsonArray arr = value.toArray();
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

  QJsonArray arr = value.toArray();
  if (arr.size() < 3)
    return fallback;

  LottieColor result {};
  result.r = arr.at(0).toDouble(fallback.r);
  result.g = arr.at(1).toDouble(fallback.g);
  result.b = arr.at(2).toDouble(fallback.b);
  result.a = arr.size() > 3 ? arr.at(3).toDouble(fallback.a) : fallback.a;
  return result;
}

void parse_animated_double(const QJsonValue& value, LottieAnimatedValue<double>& dst, double fallback) {
  dst.animated = false;
  dst.value = fallback;
  dst.keyframes.clear();

  if (!value.isObject()) {
    dst.value = to_double(value, fallback);
    return;
  }

  QJsonObject obj = value.toObject();
  int animated = obj.value(QLatin1String("a")).toInt();
  if (!animated) {
    dst.value = obj.value(QLatin1String("k")).toDouble(fallback);
    return;
  }

  QJsonArray frames = obj.value(QLatin1String("k")).toArray();
  dst.animated = true;
  dst.keyframes.reserve(frames.size());
  for (const QJsonValue& entry : frames) {
    if (!entry.isObject())
      continue;
    QJsonObject key = entry.toObject();
    double time = key.value(QLatin1String("t")).toDouble();
    double number = fallback;
    QJsonValue sValue = key.value(QLatin1String("s"));
    if (sValue.isArray()) {
      QJsonArray arr = sValue.toArray();
      if (!arr.isEmpty())
        number = arr.first().toDouble(fallback);
    }
    else {
      number = sValue.toDouble(fallback);
    }
    dst.keyframes.push_back({time, number});
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

  QJsonObject obj = value.toObject();
  int animated = obj.value(QLatin1String("a")).toInt();
  if (!animated) {
    dst.value = to_vec2(obj.value(QLatin1String("k")), fallback);
    return;
  }

  QJsonArray frames = obj.value(QLatin1String("k")).toArray();
  dst.animated = true;
  dst.keyframes.reserve(frames.size());
  for (const QJsonValue& entry : frames) {
    if (!entry.isObject())
      continue;
    QJsonObject key = entry.toObject();
    double time = key.value(QLatin1String("t")).toDouble();
    LottieVec2 vec = fallback;
    QJsonValue sval = key.value(QLatin1String("s"));
    vec = to_vec2(sval, fallback);
    dst.keyframes.push_back({time, vec});
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

  QJsonObject obj = value.toObject();
  int animated = obj.value(QLatin1String("a")).toInt();
  if (!animated) {
    dst.value = to_color(obj.value(QLatin1String("k")), fallback);
    return;
  }

  QJsonArray frames = obj.value(QLatin1String("k")).toArray();
  dst.animated = true;
  dst.keyframes.reserve(frames.size());
  for (const QJsonValue& entry : frames) {
    if (!entry.isObject())
      continue;
    QJsonObject key = entry.toObject();
    double time = key.value(QLatin1String("t")).toDouble();
    LottieColor color = to_color(key.value(QLatin1String("s")), fallback);
    dst.keyframes.push_back({time, color});
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
  QJsonArray vertices = data.value(QLatin1String("v")).toArray();
  QJsonArray in_tangents = data.value(QLatin1String("i")).toArray();
  QJsonArray out_tangents = data.value(QLatin1String("o")).toArray();
  bool closed = data.value(QLatin1String("c")).toBool(false);

  int count = vertices.size();
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
  size_t count = shape.vertices.size();
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

std::unique_ptr<LottieShapePath> parse_shape_path(const QJsonObject& obj) {
  QJsonObject ks = obj.value(QLatin1String("ks")).toObject();

  auto extract_path_object = [](const QJsonValue& value) -> QJsonObject {
    if (value.isObject())
      return value.toObject();

    if (value.isArray()) {
      QJsonArray arr = value.toArray();
      if (!arr.isEmpty() && arr.first().isObject())
        return arr.first().toObject();
    }

    return QJsonObject();
  };

  auto path = std::make_unique<LottieShapePath>();
  int animated = ks.value(QLatin1String("a")).toInt();

  if (!animated) {
    QJsonObject data = extract_path_object(ks.value(QLatin1String("k")));
    if (data.isEmpty())
      return nullptr;

    if (!parse_shape_data(data, path->shape))
      return nullptr;

    path->animated = false;
    path->cache_valid = false;
    return path;
  }

  QJsonArray frames = ks.value(QLatin1String("k")).toArray();
  for (const QJsonValue& entry : frames) {
    if (!entry.isObject())
      continue;
    QJsonObject key = entry.toObject();
    QJsonObject data = extract_path_object(key.value(QLatin1String("s")));
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
    QJsonObject data = extract_path_object(ks.value(QLatin1String("k")));
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

std::unique_ptr<LottieShapePath> parse_rectangle(const QJsonObject& obj) {
  QJsonObject posObj = obj.value(QLatin1String("p")).toObject();
  QJsonObject sizeObj = obj.value(QLatin1String("s")).toObject();
  QJsonObject radiusObj = obj.value(QLatin1String("r")).toObject();

  if (posObj.value(QLatin1String("a")).toInt() != 0)
    return nullptr;
  if (sizeObj.value(QLatin1String("a")).toInt() != 0)
    return nullptr;
  if (radiusObj.value(QLatin1String("a")).toInt() != 0)
    return nullptr;

  QJsonArray posArr = posObj.value(QLatin1String("k")).toArray();
  QJsonArray sizeArr = sizeObj.value(QLatin1String("k")).toArray();
  if (posArr.size() < 2 || sizeArr.size() < 2)
    return nullptr;

  double px = posArr.at(0).toDouble();
  double py = posArr.at(1).toDouble();
  double sx = sizeArr.at(0).toDouble();
  double sy = sizeArr.at(1).toDouble();
  double radius = radiusObj.value(QLatin1String("k")).toDouble(0.0);

  double x = px - sx * 0.5;
  double y = py - sy * 0.5;

  auto path = std::make_unique<LottieShapePath>();
  BLGeometryDirection direction = obj.value(QLatin1String("d")).toInt(1) == 1 ? BL_GEOMETRY_DIRECTION_CW : BL_GEOMETRY_DIRECTION_CCW;

  BLPath built;
  if (radius <= 0.0) {
    built.add_rect(x, y, sx, sy, direction);
  }
  else {
    double clamped = std::min(radius, std::min(std::abs(sx), std::abs(sy)) * 0.5);
    BLRoundRect rr(x, y, sx, sy, clamped, clamped);
    built.add_round_rect(rr, direction);
  }

  path->animated = false;
  path->cached_path = std::move(built);
  path->cache_valid = true;
  return path;
}

std::unique_ptr<LottieShapePath> parse_ellipse(const QJsonObject& obj) {
  QJsonObject posObj = obj.value(QLatin1String("p")).toObject();
  QJsonObject sizeObj = obj.value(QLatin1String("s")).toObject();

  if (posObj.value(QLatin1String("a")).toInt() != 0)
    return nullptr;
  if (sizeObj.value(QLatin1String("a")).toInt() != 0)
    return nullptr;

  QJsonArray posArr = posObj.value(QLatin1String("k")).toArray();
  QJsonArray sizeArr = sizeObj.value(QLatin1String("k")).toArray();
  if (posArr.size() < 2 || sizeArr.size() < 2)
    return nullptr;

  double cx = posArr.at(0).toDouble();
  double cy = posArr.at(1).toDouble();
  double rx = sizeArr.at(0).toDouble() * 0.5;
  double ry = sizeArr.at(1).toDouble() * 0.5;

  auto path = std::make_unique<LottieShapePath>();
  BLGeometryDirection direction = obj.value(QLatin1String("d")).toInt(1) == 1 ? BL_GEOMETRY_DIRECTION_CW : BL_GEOMETRY_DIRECTION_CCW;

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

  QJsonObject grad = obj.value(QLatin1String("g")).toObject();
  int stop_count = grad.value(QLatin1String("p")).toInt();
  QJsonValue stops_value = grad.value(QLatin1String("k"));

  auto extract_gradient_values = [](const QJsonValue& value) -> QJsonArray {
    if (value.isObject()) {
      QJsonObject obj = value.toObject();
      if (obj.value(QLatin1String("a")).toInt() == 0)
        return obj.value(QLatin1String("k")).toArray();

      QJsonArray frames = obj.value(QLatin1String("k")).toArray();
      for (const QJsonValue& frameValue : frames) {
        if (!frameValue.isObject())
          continue;
        QJsonObject frameObj = frameValue.toObject();
        QJsonArray components = frameObj.value(QLatin1String("s")).toArray();
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

  QJsonArray values = extract_gradient_values(stops_value);
  if (values.isEmpty())
    return nullptr;

  qsizetype value_count = values.size();
  int derived_stop_count = int(value_count / 4);
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
  QString type = obj.value(QLatin1String("ty")).toString();

  if (type == QLatin1String("gr"))
    return parse_group(obj);

  if (type == QLatin1String("sh"))
    return parse_shape_path(obj);

  if (type == QLatin1String("rc"))
    return parse_rectangle(obj);

  if (type == QLatin1String("el"))
    return parse_ellipse(obj);

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
  QJsonArray items = obj.value(QLatin1String("it")).toArray();

  for (const QJsonValue& itemValue : items) {
    if (!itemValue.isObject())
      continue;
    QJsonObject itemObj = itemValue.toObject();
    QString type = itemObj.value(QLatin1String("ty")).toString();
    if (type == QLatin1String("tr")) {
      parse_transform_object(itemObj, group->transform);
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

  double alpha = clamp_unit(color.a * alpha_scale);
  double r = clamp_unit(color.r);
  double g = clamp_unit(color.g);
  double b = clamp_unit(color.b);

  uint32_t ri = uint32_t(std::round(r * 255.0));
  uint32_t gi = uint32_t(std::round(g * 255.0));
  uint32_t bi = uint32_t(std::round(b * 255.0));
  uint32_t ai = uint32_t(std::round(alpha * 255.0));

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

void render_group(const LottieGroup& group, BLContext& ctx, double frame, const BLMatrix2D& parent_matrix, double opacity);

void render_group(const LottieGroup& group, BLContext& ctx, double frame, const BLMatrix2D& parent_matrix, double opacity) {
  double local_opacity = opacity * group.transform.opacity_at(frame);
  if (local_opacity <= 0.0)
    return;

  BLMatrix2D matrix = lottie_matrix_multiply(parent_matrix, group.transform.matrix(frame));
  std::vector<const LottieShapePath*> path_stack;
  path_stack.reserve(8);
  bool path_consumed = false;

  struct DrawCommand {
    enum Type { kGroup, kFill, kStroke, kGradientFill } type;

    const LottieGroup* group {};
    BLMatrix2D matrix {};
    double opacity {};

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

  auto emit_group_command = [&](const LottieGroup& child_group) {
    DrawCommand cmd;
    cmd.type = DrawCommand::kGroup;
    cmd.group = &child_group;
    cmd.matrix = matrix;
    cmd.opacity = local_opacity;
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
        double style_opacity = fill.opacity.evaluate(frame) * 0.01;
        double final_opacity = local_opacity * style_opacity;
        if (final_opacity > 0.0 && !path_stack.empty()) {
          LottieColor color = fill.color.evaluate(frame);
          BLRgba32 rgba = make_rgba32(color, final_opacity);
          BLFillRule rule = fill.fill_rule == 2 ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;

          BLPath combined;
          for (const LottieShapePath* path : path_stack) {
            if (!path)
              continue;
            const BLPath& source = path->path_at(frame);
            if (source.is_empty())
              continue;
            BLPath transformed(source);
            transformed.transform(matrix);
            combined.add_path(transformed);
          }

          if (!combined.is_empty())
            emit_fill_command(fill, combined, rule, rgba);
        }
        path_consumed = true;
        break;
      }

      case LottieNode::kGradientFill: {
        const LottieGradientFill& gradient = static_cast<const LottieGradientFill&>(*child);
        double style_opacity = gradient.opacity.evaluate(frame) * 0.01;
        double final_opacity = local_opacity * style_opacity;
        if (final_opacity > 0.0 && !path_stack.empty() && !gradient.stops.empty()) {
          LottieVec2 start = gradient.start.evaluate(frame);
          LottieVec2 end = gradient.end.evaluate(frame);

          BLPoint p0 = matrix.map_point(start.x, start.y);
          BLPoint p1 = matrix.map_point(end.x, end.y);

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
            BLRgba32 rgba = make_rgba32(stop.color, final_opacity);
            bl_gradient.add_stop(stop.offset, rgba);
          }

          BLFillRule rule = gradient.fill_rule == 2 ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;
          BLPath combined;
          for (const LottieShapePath* path : path_stack) {
            if (!path)
              continue;
            const BLPath& source = path->path_at(frame);
            if (source.is_empty())
              continue;
            BLPath transformed(source);
            transformed.transform(matrix);
            combined.add_path(transformed);
          }

          if (!combined.is_empty())
            emit_gradient_fill_command(combined, rule, std::move(bl_gradient));
        }
        path_consumed = true;
        break;
      }

      case LottieNode::kStroke: {
        const LottieStroke& stroke = static_cast<const LottieStroke&>(*child);
        double style_opacity = stroke.opacity.evaluate(frame) * 0.01;
        double final_opacity = local_opacity * style_opacity;
        if (final_opacity > 0.0 && !path_stack.empty()) {
          double width = stroke.width.evaluate(frame);
          BLStrokeCap cap = map_stroke_cap(stroke.cap);
          BLStrokeJoin join = map_stroke_join(stroke.join);
          double miter_limit = stroke.miter_limit;

          LottieColor color = stroke.color.evaluate(frame);
          BLRgba32 rgba = make_rgba32(color, final_opacity);

          BLPath combined;
          for (const LottieShapePath* path : path_stack) {
            if (!path)
              continue;
            const BLPath& source = path->path_at(frame);
            if (source.is_empty())
              continue;
            BLPath transformed(source);
            transformed.transform(matrix);
            combined.add_path(transformed);
          }

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
        render_group(*cmd.group, ctx, frame, cmd.matrix, cmd.opacity);
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

  if (layer.root) {
    render_group(*layer.root, ctx, frame, layer_matrix, opacity);
    return;
  }

  if (layer.image_index >= 0 && size_t(layer.image_index) < _images.size()) {
    const LottieImageAsset& image = _images[layer.image_index];
    if (!image.image || image.width <= 0.0 || image.height <= 0.0)
      return;

    ctx.save();
    ctx.apply_transform(layer_matrix);
    double previous_alpha = ctx.global_alpha();
    ctx.set_global_alpha(previous_alpha * opacity);
    ctx.blit_image(BLRect(0.0, 0.0, image.width, image.height), image.image);
    ctx.set_global_alpha(previous_alpha);
    ctx.restore();
    return;
  }

  if (layer.precomp_index >= 0 && size_t(layer.precomp_index) < _precomps.size()) {
    const LottiePrecomposition& precomp = _precomps[layer.precomp_index];
    render_layer_array(precomp.layers, ctx, frame, layer_matrix, opacity);
  }
}

double lottie_lerp(double a, double b, double t) noexcept {
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
          double denom = k1.time - k0.time;
          double t = denom != 0.0 ? (frame - k0.time) / denom : 0.0;
          if (t < 0.0) t = 0.0;
          if (t > 1.0) t = 1.0;

          size_t count = k0.shape.vertices.size();
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

LottieFill::LottieFill()
  : LottieNode(LottieNode::kFill) {}

LottieGradientFill::LottieGradientFill()
  : LottieNode(LottieNode::kGradientFill) {}

LottieStroke::LottieStroke()
  : LottieNode(LottieNode::kStroke) {}

LottieGroup::LottieGroup()
  : LottieNode(LottieNode::kGroup) {}

BLMatrix2D LottieTransform::matrix(double frame) const {
  LottieVec2 pos = position.evaluate(frame);
  LottieVec2 scl = scale.evaluate(frame);
  LottieVec2 anc = anchor.evaluate(frame);
  double angle = rotation.evaluate(frame) * (kPi / 180.0);
  double skew_angle = skew.evaluate(frame) * (kPi / 180.0);
  double skew_axis_angle = (this->skew_axis.evaluate(frame) + 90.0) * (kPi / 180.0);

  BLMatrix2D result = BLMatrix2D::make_identity();
  result = lottie_matrix_multiply(result, BLMatrix2D::make_translation(pos.x, pos.y));
  if (angle != 0.0)
    result = lottie_matrix_multiply(result, BLMatrix2D::make_rotation(angle));
  if (skew_angle != 0.0) {
    double tan_skew = std::tan(skew_angle);
    BLMatrix2D rot = BLMatrix2D::make_rotation(skew_axis_angle);
    BLMatrix2D rot_inv = BLMatrix2D::make_rotation(-skew_axis_angle);
    BLMatrix2D shear(1.0, 0.0, tan_skew, 1.0, 0.0, 0.0);
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

  QByteArray data = file.readAll();
  QJsonParseError parse_error {};
  QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
  if (doc.isNull()) {
    if (error_message)
      *error_message = QString::fromLatin1("Failed to parse Lottie JSON: %1").arg(parse_error.errorString());
    return false;
  }

  QJsonObject root = doc.object();
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

  QJsonArray assets = root.value(QLatin1String("assets")).toArray();
  QDir file_dir = QFileInfo(path).dir();
  _images.reserve(assets.size());

  auto load_image_asset = [&](const QJsonObject& asset_obj) -> void {
    QString id = asset_obj.value(QLatin1String("id")).toString();
    if (id.isEmpty())
      return;

    QString file_name = asset_obj.value(QLatin1String("p")).toString();
    if (file_name.isEmpty())
      return;

    int embed = asset_obj.value(QLatin1String("e")).toInt();
    BLImage image;
    bool loaded = false;

    if (embed == 1 || file_name.startsWith(QLatin1String("data:"))) {
      QString data_str = file_name;
      int comma_pos = data_str.indexOf(QLatin1Char(','));
      if (comma_pos >= 0)
        data_str = data_str.mid(comma_pos + 1);
      QByteArray decoded = QByteArray::fromBase64(data_str.toUtf8());
      if (!decoded.isEmpty())
        loaded = image.read_from_data(decoded.constData(), size_t(decoded.size())) == BL_SUCCESS;
    }
    else {
      QString base_path = asset_obj.value(QLatin1String("u")).toString();
      QString absolute_path = file_dir.absoluteFilePath(base_path + file_name);
      QByteArray encoded = QFile::encodeName(absolute_path);
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
    QJsonObject assetObj = assetValue.toObject();
    if (assetObj.contains(QLatin1String("p")))
      load_image_asset(assetObj);
  }

  auto finalize_parent_relationships = [](std::vector<LottieLayer>& layers) {
    QHash<int, int> index_map;
    index_map.reserve(int(layers.size()));
    for (int i = 0; i < int(layers.size()); i++) {
      int layer_index = layers[size_t(i)].index;
      index_map.insert(layer_index, i);
    }

    for (LottieLayer& layer : layers) {
      if (layer.parent_index < 0)
        continue;
      auto it = index_map.constFind(layer.parent_index);
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
      QJsonObject layerObj = layerValue.toObject();

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

      if (layer.type == 4) {
        QJsonArray shapes = layerObj.value(QLatin1String("shapes")).toArray();
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
        auto it = local_image_map.constFind(layer.ref_id);
        if (it != local_image_map.constEnd())
          layer.image_index = *it;
      }
      else if (layer.type == 0 && !layer.ref_id.isEmpty()) {
        auto it = local_precomp_map.constFind(layer.ref_id);
        if (it != local_precomp_map.constEnd())
          layer.precomp_index = *it;
      }
    }
  };

  QHash<QString, int> precomp_index_map;

  for (const QJsonValue& assetValue : assets) {
    if (!assetValue.isObject())
      continue;
    QJsonObject assetObj = assetValue.toObject();
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

  QJsonArray layers = root.value(QLatin1String("layers")).toArray();
  parse_layer_array(layers, _layers);
  resolve_layer_resources(_layers, image_index_map, precomp_index_map);
  assign_track_mattes(_layers);

  bool has_renderable_layer = false;
  for (const LottieLayer& layer : _layers) {
    if ((layer.root && !layer.root->children.empty()) ||
        (layer.type == 2 && layer.image_index >= 0 && layer.image_index < int(_images.size())) ||
        (layer.type == 0 && layer.precomp_index >= 0 && layer.precomp_index < int(_precomps.size()))) {
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

  BLSize target_size = ctx.target_size();
  int canvas_width = std::max(1, int(std::ceil(target_size.w)));
  int canvas_height = std::max(1, int(std::ceil(target_size.h)));

  std::vector<BLMatrix2D> matrix_cache(layers.size());
  std::vector<uint8_t> matrix_valid(layers.size(), 0);
  std::function<BLMatrix2D(size_t)> resolve_matrix = [&](size_t index) -> BLMatrix2D {
    if (matrix_valid[index])
      return matrix_cache[index];

    BLMatrix2D mat = layers[index].transform.matrix(frame);
    int parent = layers[index].parent;
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
    int parent = layers[index].parent;
    if (parent >= 0)
      value *= resolve_opacity(size_t(parent));

    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;

    opacity_cache[index] = value;
    opacity_valid[index] = 1;
    return value;
  };

  for (size_t i = layers.size(); i-- > 0;) {
    const LottieLayer& layer = layers[i];
    bool has_vector = layer.root != nullptr;
    bool has_image = layer.image_index >= 0 && size_t(layer.image_index) < _images.size();
    bool has_precomp = layer.precomp_index >= 0 && size_t(layer.precomp_index) < _precomps.size();
    bool has_matte_target = layer.matte_source >= 0 && layer.matte_mode > 0;
    if (!has_vector && !has_image && !has_precomp && !has_matte_target)
      continue;

    if (layer.hidden && !has_matte_target)
      continue;

    if (frame < layer.in_point || frame >= layer.out_point)
      continue;

    double layer_local_opacity = resolve_opacity(i);
    double layer_opacity = opacity * layer_local_opacity;
    if (layer_opacity <= 0.0 && !has_matte_target)
      continue;

    BLMatrix2D layer_matrix_local = resolve_matrix(i);
    BLMatrix2D layer_matrix = lottie_matrix_multiply(root_matrix, layer_matrix_local);

    if (has_matte_target) {
      int matte_index = layer.matte_source;
      if (matte_index < 0 || size_t(matte_index) >= layers.size())
        continue;

      const LottieLayer& matte_layer = layers[size_t(matte_index)];
      BLMatrix2D matte_matrix_local = resolve_matrix(size_t(matte_index));
      BLMatrix2D matte_matrix = lottie_matrix_multiply(root_matrix, matte_matrix_local);
      double matte_opacity = opacity * resolve_opacity(size_t(matte_index));

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

      BLImage matte_image = render_to_image(matte_layer, matte_matrix, matte_opacity);
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
