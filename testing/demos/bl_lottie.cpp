#include "bl_lottie.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>

#include <algorithm>
#include <cmath>

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
  if (ks.value(QLatin1String("a")).toInt() != 0)
    return nullptr;

  QJsonObject data = ks.value(QLatin1String("k")).toObject();
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

void render_fill(const LottieFill& fill,
                 BLContext& ctx,
                 double frame,
                 const BLMatrix2D& matrix,
                 double opacity,
                 const std::vector<const LottieShapePath*>& paths) {
  if (paths.empty())
    return;

  double style_opacity = fill.opacity.evaluate(frame) * 0.01;
  double final_opacity = opacity * style_opacity;
  if (final_opacity <= 0.0)
    return;

  BLFillRule rule = fill.fill_rule == 2 ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;
  ctx.set_fill_rule(rule);

  LottieColor color = fill.color.evaluate(frame);
  BLRgba32 rgba = make_rgba32(color, final_opacity);

  BLPath combined;
  for (const LottieShapePath* path : paths) {
    if (!path)
      continue;
    BLPath transformed(path->path);
    transformed.transform(matrix);
    combined.add_path(transformed);
  }

  if (!combined.is_empty())
    ctx.fill_path(combined, rgba);
}

void render_stroke(const LottieStroke& stroke,
                   BLContext& ctx,
                   double frame,
                   const BLMatrix2D& matrix,
                   double opacity,
                   const std::vector<const LottieShapePath*>& paths) {
  if (paths.empty())
    return;

  double style_opacity = stroke.opacity.evaluate(frame) * 0.01;
  double final_opacity = opacity * style_opacity;
  if (final_opacity <= 0.0)
    return;

  double width = stroke.width.evaluate(frame);
  ctx.set_stroke_width(width);
  ctx.set_stroke_caps(map_stroke_cap(stroke.cap));
  ctx.set_stroke_join(map_stroke_join(stroke.join));
  ctx.set_stroke_miter_limit(stroke.miter_limit);

  LottieColor color = stroke.color.evaluate(frame);
  BLRgba32 rgba = make_rgba32(color, final_opacity);

  BLPath combined;
  for (const LottieShapePath* path : paths) {
    if (!path)
      continue;
    BLPath transformed(path->path);
    transformed.transform(matrix);
    combined.add_path(transformed);
  }

  if (!combined.is_empty())
    ctx.stroke_path(combined, rgba);
}

void render_group(const LottieGroup& group, BLContext& ctx, double frame, const BLMatrix2D& parent_matrix, double opacity) {
  double local_opacity = opacity * group.transform.opacity_at(frame);
  if (local_opacity <= 0.0)
    return;

  BLMatrix2D matrix = lottie_matrix_multiply(parent_matrix, group.transform.matrix(frame));
  std::vector<const LottieShapePath*> path_stack;
  path_stack.reserve(8);
  bool path_consumed = false;

  for (const auto& child : group.children) {
    switch (child->type) {
      case LottieNode::kGroup:
        render_group(static_cast<const LottieGroup&>(*child), ctx, frame, matrix, local_opacity);
        break;

      case LottieNode::kPath:
        if (path_consumed && !path_stack.empty()) {
          path_stack.clear();
          path_consumed = false;
        }
        path_stack.push_back(static_cast<const LottieShapePath*>(child.get()));
        break;

      case LottieNode::kFill:
        render_fill(static_cast<const LottieFill&>(*child), ctx, frame, matrix, local_opacity, path_stack);
        path_consumed = true;
        break;

      case LottieNode::kStroke:
        render_stroke(static_cast<const LottieStroke&>(*child), ctx, frame, matrix, local_opacity, path_stack);
        path_consumed = true;
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
  for (const QJsonValue& layerValue : layers) {
    if (!layerValue.isObject())
      continue;
    QJsonObject layerObj = layerValue.toObject();
    int type = layerObj.value(QLatin1String("ty")).toInt();
    if (type != 4)
      continue;

    LottieLayer layer {};
    layer.type = type;
    layer.name = layerObj.value(QLatin1String("nm")).toString();
    layer.in_point = layerObj.value(QLatin1String("ip")).toDouble(_in_point);
    layer.out_point = layerObj.value(QLatin1String("op")).toDouble(_out_point);
    parse_transform_object(layerObj.value(QLatin1String("ks")).toObject(), layer.transform);

    auto root_group = std::make_unique<LottieGroup>();
    QJsonArray shapes = layerObj.value(QLatin1String("shapes")).toArray();
    for (const QJsonValue& shapeValue : shapes) {
      if (!shapeValue.isObject())
        continue;
      std::unique_ptr<LottieNode> node = parse_shape_item(shapeValue.toObject());
      if (node)
        root_group->children.push_back(std::move(node));
    }

    layer.root = std::move(root_group);
    if (layer.root && !layer.root->children.empty())
      _layers.push_back(std::move(layer));
  }

  if (_layers.empty()) {
    if (error_message)
      *error_message = QString::fromLatin1("No supported shape layers found in %1").arg(path);
    return false;
  }

  return true;
}

void LottieComposition::render(BLContext& ctx, double frame, const BLMatrix2D& root_matrix, double opacity) const {
  if (_layers.empty())
    return;

  for (const LottieLayer& layer : _layers) {
    if (!layer.root)
      continue;

    if (frame < layer.in_point || frame >= layer.out_point)
      continue;

    double layer_opacity = opacity * layer.transform.opacity_at(frame);
    if (layer_opacity <= 0.0)
      continue;

    BLMatrix2D layer_matrix = lottie_matrix_multiply(root_matrix, layer.transform.matrix(frame));
    render_group(*layer.root, ctx, frame, layer_matrix, layer_opacity);
  }
}
