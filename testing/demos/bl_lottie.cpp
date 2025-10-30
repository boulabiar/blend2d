#include "bl_lottie.h"

#include <QtCore/QFile>
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

  QJsonObject data;
  int animated = ks.value(QLatin1String("a")).toInt();
  if (!animated) {
    data = extract_path_object(ks.value(QLatin1String("k")));
  }
  else {
    QJsonArray frames = ks.value(QLatin1String("k")).toArray();
    for (const QJsonValue& entry : frames) {
      if (!entry.isObject())
        continue;
      QJsonObject key = entry.toObject();
      QJsonArray shapes = key.value(QLatin1String("s")).toArray();
      if (shapes.isEmpty())
        continue;
      data = extract_path_object(shapes.first());
      if (!data.isEmpty())
        break;
    }
  }

  if (data.isEmpty())
    return nullptr;

  QJsonArray vertices = data.value(QLatin1String("v")).toArray();
  QJsonArray in_tangents = data.value(QLatin1String("i")).toArray();
  QJsonArray out_tangents = data.value(QLatin1String("o")).toArray();
  bool closed = data.value(QLatin1String("c")).toBool(false);

  if (vertices.isEmpty())
    return nullptr;

  if (in_tangents.size() != vertices.size() || out_tangents.size() != vertices.size())
    return nullptr;

  auto path = std::make_unique<LottieShapePath>();
  BLPath& bl_path = path->path;

  auto point_from = [](const QJsonArray& arr) -> LottieVec2 {
    LottieVec2 v {};
    if (arr.size() >= 2) {
      v.x = arr.at(0).toDouble();
      v.y = arr.at(1).toDouble();
    }
    return v;
  };

  LottieVec2 first = point_from(vertices.first().toArray());
  bl_path.move_to(first.x, first.y);

  for (int i = 0; i < vertices.size() - 1; i++) {
    LottieVec2 p0 = point_from(vertices.at(i).toArray());
    LottieVec2 p1 = point_from(vertices.at(i + 1).toArray());
    LottieVec2 o0 = point_from(out_tangents.at(i).toArray());
    LottieVec2 i1 = point_from(in_tangents.at(i + 1).toArray());

    bl_path.cubic_to(p0.x + o0.x,
                     p0.y + o0.y,
                     p1.x + i1.x,
                     p1.y + i1.y,
                     p1.x,
                     p1.y);
  }

  if (closed) {
    LottieVec2 last = point_from(vertices.last().toArray());
    LottieVec2 o_last = point_from(out_tangents.last().toArray());
    LottieVec2 i_first = point_from(in_tangents.first().toArray());

    bl_path.cubic_to(last.x + o_last.x,
                     last.y + o_last.y,
                     first.x + i_first.x,
                     first.y + i_first.y,
                     first.x,
                     first.y);

    bl_path.close();
  }

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

  if (radius <= 0.0) {
    path->path.add_rect(x, y, sx, sy, direction);
  }
  else {
    double clamped = std::min(radius, std::min(std::abs(sx), std::abs(sy)) * 0.5);
    BLRoundRect rr(x, y, sx, sy, clamped, clamped);
    path->path.add_round_rect(rr, direction);
  }

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
  path->path.add_ellipse(BLEllipse(cx, cy, rx, ry), direction);

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
            BLPath transformed(path->path);
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
            BLPath transformed(path->path);
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
            BLPath transformed(path->path);
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

  QJsonArray layers = root.value(QLatin1String("layers")).toArray();
  _layers.reserve(layers.size());

  bool has_renderable_layer = false;

  for (const QJsonValue& layerValue : layers) {
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
    layer.in_point = layerObj.value(QLatin1String("ip")).toDouble(_in_point);
    layer.out_point = layerObj.value(QLatin1String("op")).toDouble(_out_point);
    parse_transform_object(layerObj.value(QLatin1String("ks")).toObject(), layer.transform);

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
      if (!root_group->children.empty()) {
        layer.root = std::move(root_group);
        has_renderable_layer = true;
      }
    }

    _layers.push_back(std::move(layer));
  }

  std::unordered_map<int, size_t> index_map;
  index_map.reserve(_layers.size());
  for (size_t i = 0; i < _layers.size(); i++) {
    int layer_index = _layers[i].index;
    index_map[layer_index] = i;
  }

  for (LottieLayer& layer : _layers) {
    if (layer.parent_index < 0)
      continue;
    auto it = index_map.find(layer.parent_index);
    if (it != index_map.end())
      layer.parent = int(it->second);
  }

  if (!has_renderable_layer) {
    if (error_message)
      *error_message = QString::fromLatin1("No supported shape layers found in %1").arg(path);
    return false;
  }

  return true;
}

void LottieComposition::render(BLContext& ctx, double frame, const BLMatrix2D& root_matrix, double opacity) const {
  if (_layers.empty())
    return;

  std::vector<BLMatrix2D> matrix_cache(_layers.size());
  std::vector<uint8_t> matrix_valid(_layers.size(), 0);
  std::function<BLMatrix2D(size_t)> resolve_matrix = [&](size_t index) -> BLMatrix2D {
    if (matrix_valid[index])
      return matrix_cache[index];

    BLMatrix2D mat = _layers[index].transform.matrix(frame);
    int parent = _layers[index].parent;
    if (parent >= 0)
      mat = lottie_matrix_multiply(resolve_matrix(size_t(parent)), mat);

    matrix_cache[index] = mat;
    matrix_valid[index] = 1;
    return mat;
  };

  std::vector<double> opacity_cache(_layers.size());
  std::vector<uint8_t> opacity_valid(_layers.size(), 0);
  std::function<double(size_t)> resolve_opacity = [&](size_t index) -> double {
    if (opacity_valid[index])
      return opacity_cache[index];

    double value = _layers[index].transform.opacity_at(frame);
    int parent = _layers[index].parent;
    if (parent >= 0)
      value *= resolve_opacity(size_t(parent));

    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;

    opacity_cache[index] = value;
    opacity_valid[index] = 1;
    return value;
  };

  for (size_t i = _layers.size(); i-- > 0;) {
    const LottieLayer& layer = _layers[i];
    if (!layer.root)
      continue;

    if (frame < layer.in_point || frame >= layer.out_point)
      continue;

    double layer_opacity = opacity * resolve_opacity(i);
    if (layer_opacity <= 0.0)
      continue;

    BLMatrix2D layer_matrix = lottie_matrix_multiply(root_matrix, resolve_matrix(i));
    render_group(*layer.root, ctx, frame, layer_matrix, layer_opacity);
  }
}
