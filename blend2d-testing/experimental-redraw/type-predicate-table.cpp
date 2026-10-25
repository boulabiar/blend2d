#include <blend2d/blend2d.h>
#include <cstdio>
#include <vector>
int main(){
  BLImage image(2,2,BL_FORMAT_PRGB32);BLPattern pattern(image);BLGradient gradient(BLLinearGradientValues(0,0,1,1));BLPath path;BLArray<uint32_t> array;BLString string("test");
  std::vector<BLVar> values={BLVar(),BLVar(BLRgba32(0xff112233)),BLVar(BLRgba64(0xffff111122223333ull)),BLVar(BLRgba(0.1,0.2,0.3,1)),BLVar(gradient),BLVar(pattern),BLVar(image),BLVar(path),BLVar(array),BLVar(string),BLVar(true),BLVar(int64_t(-123)),BLVar(uint64_t(456)),BLVar(0.5)};
  unsigned failures=0,checks=0;
  for(auto& value:values){
    auto test=[&](const char* name,bool actual,BLObjectType type){checks++;bool expected=value.type()==type;if(actual!=expected){failures++;std::printf("type=%u predicate=%s actual=%u expected=%u\n",unsigned(value.type()),name,unsigned(actual),unsigned(expected));}};
    test("rgba32",value.is_rgba32(),BL_OBJECT_TYPE_RGBA32);test("rgba64",value.is_rgba64(),BL_OBJECT_TYPE_RGBA64);test("gradient",value.is_gradient(),BL_OBJECT_TYPE_GRADIENT);test("pattern",value.is_pattern(),BL_OBJECT_TYPE_PATTERN);test("null",value.is_null(),BL_OBJECT_TYPE_NULL);test("image",value.is_image(),BL_OBJECT_TYPE_IMAGE);test("path",value.is_path(),BL_OBJECT_TYPE_PATH);test("string",value.is_string(),BL_OBJECT_TYPE_STRING);test("bool",value.is_bool(),BL_OBJECT_TYPE_BOOL);test("int64",value.is_int64(),BL_OBJECT_TYPE_INT64);test("uint64",value.is_uint64(),BL_OBJECT_TYPE_UINT64);test("double",value.is_double(),BL_OBJECT_TYPE_DOUBLE);
  }
  std::printf("checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
