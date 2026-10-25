#include <blend2d/blend2d.h>
#include <cstdio>
int main() {
  BLGradient gradient(BLLinearGradientValues(0,0,1,1));
  BLVar value(gradient);
  bool wrong=value.type()!=BL_OBJECT_TYPE_RGBA32 && value.is_rgba32();
  std::printf("type=%u expected_gradient=%u is_rgba32=%u (expected false)\n",unsigned(value.type()),unsigned(BL_OBJECT_TYPE_GRADIENT),unsigned(value.is_rgba32()));
  return wrong ? 1 : 0;
}
