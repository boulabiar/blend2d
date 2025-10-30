#ifndef BL_DEMO_LOTTIE_H_INCLUDED
#define BL_DEMO_LOTTIE_H_INCLUDED

#include <blend2d.h>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <limits>
#include <memory>
#include <vector>

struct LottieVec2 {
  double x {};
  double y {};
};

struct LottieColor {
  double r {};
  double g {};
  double b {};
  double a {1.0};
};

double lottie_lerp(double a, double b, double t) noexcept;
LottieVec2 lottie_lerp(const LottieVec2& a, const LottieVec2& b, double t) noexcept;
LottieColor lottie_lerp(const LottieColor& a, const LottieColor& b, double t) noexcept;
BLMatrix2D lottie_matrix_multiply(const BLMatrix2D& a, const BLMatrix2D& b) noexcept;

template<typename T>
struct LottieKeyframe {
  double time {};
  T value {};
};

template<typename T>
struct LottieAnimatedValue {
  bool animated {};
  T value {};
  std::vector<LottieKeyframe<T>> keyframes;

  T evaluate(double frame) const;
};

template<typename T>
T LottieAnimatedValue<T>::evaluate(double frame) const {
  if (!animated || keyframes.empty())
    return value;

  if (frame <= keyframes.front().time)
    return keyframes.front().value;

  for (size_t i = 0; i + 1 < keyframes.size(); i++) {
    const auto& k0 = keyframes[i];
    const auto& k1 = keyframes[i + 1];
    if (frame < k1.time) {
      double denom = k1.time - k0.time;
      double t = denom != 0.0 ? (frame - k0.time) / denom : 0.0;
      if (t < 0.0) t = 0.0;
      if (t > 1.0) t = 1.0;
      return lottie_lerp(k0.value, k1.value, t);
    }
  }

  return keyframes.back().value;
}

struct LottieTransform {
  LottieTransform();
  LottieAnimatedValue<LottieVec2> anchor;
  LottieAnimatedValue<LottieVec2> position;
  LottieAnimatedValue<LottieVec2> scale;
  LottieAnimatedValue<double> rotation;
  LottieAnimatedValue<double> skew;
  LottieAnimatedValue<double> skew_axis;
  LottieAnimatedValue<double> opacity;

  BLMatrix2D matrix(double frame) const;
  double opacity_at(double frame) const;
};

struct LottieNode {
  enum Type {
    kGroup,
    kPath,
    kFill,
    kStroke,
    kGradientFill
  };

  explicit LottieNode(Type type) : type(type) {}
  virtual ~LottieNode() = default;

  Type type;
};

struct LottieShapePath : public LottieNode {
  struct ShapeData {
    std::vector<LottieVec2> vertices;
    std::vector<LottieVec2> in_tangents;
    std::vector<LottieVec2> out_tangents;
    bool closed {};
  };

  struct Keyframe {
    double time {};
    bool hold {};
    ShapeData shape;
  };

  LottieShapePath();

  bool animated {};
  ShapeData shape;
  std::vector<Keyframe> keyframes;

  mutable BLPath cached_path;
  mutable double cached_frame {std::numeric_limits<double>::quiet_NaN()};
  mutable bool cache_valid {};
  mutable ShapeData interpolated_shape;

  const BLPath& path_at(double frame) const;
};

struct LottieFill : public LottieNode {
  LottieFill();
  LottieAnimatedValue<LottieColor> color;
  LottieAnimatedValue<double> opacity;
  int fill_rule {};
};

struct LottieGradientStop {
  double offset {};
  LottieColor color;
};

struct LottieGradientFill : public LottieNode {
  LottieGradientFill();
  int gradient_type {1};
  int fill_rule {1};
  LottieAnimatedValue<LottieVec2> start;
  LottieAnimatedValue<LottieVec2> end;
  LottieAnimatedValue<double> opacity;
  std::vector<LottieGradientStop> stops;
};

struct LottieStroke : public LottieNode {
  LottieStroke();
  LottieAnimatedValue<LottieColor> color;
  LottieAnimatedValue<double> opacity;
  LottieAnimatedValue<double> width;
  int cap {};
  int join {};
  double miter_limit {4.0};
};

struct LottieGroup : public LottieNode {
  LottieGroup();
  LottieTransform transform;
  std::vector<std::unique_ptr<LottieNode>> children;
};

struct LottieLayer {
  QString name;
  int index {};
  int type {};
  int parent_index {-1};
  int parent {-1};
  double in_point {};
  double out_point {};
  LottieTransform transform;
  std::unique_ptr<LottieGroup> root;
};

class LottieComposition {
public:
  bool load_from_file(const QString& path, QString* error_message);

  bool is_valid() const noexcept { return !_layers.empty(); }
  double width() const noexcept { return _width; }
  double height() const noexcept { return _height; }
  double frame_rate() const noexcept { return _frame_rate; }
  double in_point() const noexcept { return _in_point; }
  double out_point() const noexcept { return _out_point; }
  const QString& name() const noexcept { return _name; }

  void render(BLContext& ctx, double frame, const BLMatrix2D& root_matrix, double opacity = 1.0) const;

private:
  double _width {};
  double _height {};
  double _frame_rate {60.0};
  double _in_point {};
  double _out_point {};
  QString _name;

  std::vector<LottieLayer> _layers;
};

#endif // BL_DEMO_LOTTIE_H_INCLUDED
