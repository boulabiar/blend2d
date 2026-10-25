// Native-only integration test: links Blend2D, no Rasterizer parser required.
#include "blend_bench_experimental.cpp"
#include <array>
struct Variant {const char* name;unsigned threads,tiles,solid,cache;int limit;bool prepared;unsigned borrow=0;bool record=false;unsigned cut=0;};
static unsigned random_u(unsigned& s){s^=s<<13;s^=s>>17;s^=s<<5;return s;}
static void number_env(const char* key,unsigned value){auto text=std::to_string(value);setenv(key,text.c_str(),1);}
static std::vector<unsigned char> test_render(int size,unsigned seed,Variant variant) {
  unsetenv("BLEND_PROBE_TILE_CUT");if(variant.cut)number_env("BLEND_PROBE_TILE_CUT",variant.cut);
  unsetenv("BLEND_PROBE_RECORD");if(variant.record)setenv("BLEND_PROBE_RECORD","1",1);
  unsetenv("BLEND_PROBE_PREPARED");unsetenv("BLEND_PROBE_ASYNC_LIMIT");
  number_env("BLEND_PROBE_BORROW",variant.borrow);
  number_env("BLEND_PROBE_TILES",variant.tiles);number_env("BLEND_PROBE_SOLID_MODE",variant.solid);number_env("BLEND_PROBE_PRESTROKE",variant.cache);
  if(variant.prepared)setenv("BLEND_PROBE_PREPARED","1",1);
  if(variant.limit>=0)number_env("BLEND_PROBE_ASYNC_LIMIT",unsigned(variant.limit));
  auto* bench=static_cast<Bench*>(bb_new_config(size,size,variant.threads,32768));
  for(int i=0;i<500;i++){
    Draw d;d.flags=0;
    double x=int(random_u(seed)%(size+64))-32,y=int(random_u(seed)%(size+64))-32;
    check(d.path.move_to(x,y));
    if(i%5==0)check(d.path.line_to(x+9.5,y+17.25));
    else if(i%5==1){check(d.path.line_to(x+12.5,y-3.25));check(d.path.line_to(x+17.75,y+18.5));check(d.path.line_to(x-4,y+10.25));check(d.path.close());}
    else {check(d.path.cubic_to(x+24,y-12,x-10,y+28,x+15.25,y+20));check(d.path.quad_to(x-5,y+26,x,y));if(i%2)check(d.path.close());}
    d.transform=i%4?BLMatrix2D::make_identity():BLMatrix2D(0.8,0.125,-0.075,1.05,0.25,-0.5);
    d.clip=i%7?BLRect(0,0,size,size):BLRect(6.25,12.5,size-18.75,size-37.25);
    if(i%29==0)d.clip.h=0;
    d.opacity=(i%7+1)/7.;d.width=i%3?0.:0.25+(i%8);
    if(i%2)d.flags|=1<<1;if(i%3==0)d.flags|=1<<2;else if(i%3==1)d.flags|=1<<3;
    if(i%5)d.flags|=1<<4;
    if(i%5==0){BLGradient g(BLLinearGradientValues(x,y,x+24,y+24));check(g.add_stop(0,BLRgba32(0x3377aaff)));check(g.add_stop(1,BLRgba32(0xddff2211)));d.paint=g;}
    else if(i%11==0){BLImage image;check(image.create(2,2,BL_FORMAT_PRGB32));BLImageData pixels;check(image.get_data(&pixels));for(int yy=0;yy<2;yy++)for(int xx=0;xx<2;xx++)reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels.pixel_data)+yy*pixels.stride)[xx]=0xff003300u+uint32_t((xx+yy)*0x00662244u);BLPattern pattern(image,BL_EXTEND_MODE_REFLECT);d.paint=pattern;}
    else d.paint=BLVar(BLRgba32(random_u(seed)|0x80000000u));
    prepare_stroke(*bench,d);if(bench->tiles)tile_add(bench,d);else append_draw(*bench,std::move(d));
  }
  bb_prepare(bench);
  bb_render_cached(bench);
  std::vector<unsigned char> first(size_t(size)*size*4);bb_pixels(bench,first.data());
  // Retained state/cache data must remain valid across redraws and batch reuse.
  bb_render_cached(bench);std::vector<unsigned char> second(first.size());bb_pixels(bench,second.data());
  if(first!=second){std::fprintf(stderr,"Repeat changed pixels: %s size=%d\n",variant.name,size);std::abort();}
  bb_free(bench);return first;
}
int main(){
  Variant reference={"sync",0,0,0,0,-1,false};
  Variant variants[]={
    {"cached4",4,0,0,0,-1,false},{"cached16",16,0,0,0,-1,false},
    {"prepared",16,0,0,0,-1,true},{"typed",16,0,1,0,-1,true},{"solid-cache",16,0,2,0,-1,true},
    {"async0",16,0,0,0,0,true},{"async32",16,0,0,0,32,true},
    {"heap-outline",16,0,0,4,-1,true},{"packed-ref",16,0,0,5,-1,true},{"packed",16,0,0,6,-1,true},
    {"packed-typed",16,0,1,6,-1,true},{"bands2",16,2,0,0,-1,true},{"bands4",16,4,0,0,-1,true},
    {"packed-bands2",16,2,0,6,-1,true},{"packed32-bands2",32,2,0,6,-1,true},{"packed32-bands4",32,4,0,6,-1,true},{"borrow1",16,0,0,0,-1,true,1},{"borrow2",16,0,0,0,-1,true,2},{"borrow2-bands2",16,2,0,0,-1,true,2},{"record",16,0,0,0,-1,true,0,true},{"record-bands2",16,2,0,0,-1,true,0,true},{"record-packed",16,0,0,6,-1,true,0,true},{"record-packed-bands2",16,2,0,6,-1,true,0,true},{"borrow-cut",16,2,0,0,-1,true,2,false,1280},{"record-packed-cut",32,2,0,6,-1,true,0,true,1280}};
  for(int size:{128,257,321,2048})for(unsigned seed:{1u,987654321u}){
    auto image=test_render(size,seed,reference);
    for(auto variant:variants){if(variant.record && !bl_probe_record_begin)continue;auto actual=test_render(size,seed,variant);if(actual!=image){size_t differences=0;for(size_t i=0;i<image.size();i++)differences+=image[i]!=actual[i];std::fprintf(stderr,"Mismatch %s size=%d seed=%u channels=%zu\n",variant.name,size,seed,differences);return 1;}}
    std::printf("Exact replay: size=%d seed=%u, all variants; two redraws each\n",size,seed);
  }
}
