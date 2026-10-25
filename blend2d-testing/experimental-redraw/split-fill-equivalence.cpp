// Exercise large multi-contour fills as ONE draw, including holes/overlap,
// both fill rules, open contours, clipping, affine transforms, and gradients.
#include <blend2d/blend2d.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static void ok(BLResult r) { if (r) {std::fprintf(stderr,"BLResult %u\n",r);std::abort();} }
static unsigned rng(unsigned& s){s^=s<<13;s^=s>>17;s^=s<<5;return s;}
static BLPath make_path(unsigned seed, int count) {
  BLPath path;
  constexpr double k=0.5522847498307936;
  for(int i=0;i<count;i++) {
    double x=int(rng(seed)%360)-24, y=int(rng(seed)%360)-24;
    double r=2+(rng(seed)%48), direction=i%3 ? 1. : -1.;
    // Mixed contour orientation and intentional overlap exercise winding sums.
    ok(path.move_to(x+r,y));
    ok(path.cubic_to(x+r,y+direction*k*r,x+k*r,y+direction*r,x,y+direction*r));
    ok(path.cubic_to(x-k*r,y+direction*r,x-r,y+direction*k*r,x-r,y));
    ok(path.cubic_to(x-r,y-direction*k*r,x-k*r,y-direction*r,x,y-direction*r));
    ok(path.cubic_to(x+k*r,y-direction*r,x+r,y-direction*k*r,x+r,y));
    if(i%7)ok(path.close()); // Open subpaths must still close for fills.
  }
  return path;
}
static std::vector<unsigned char> render(unsigned threads, unsigned scene) {
  BLImage image;ok(image.create(321,289,BL_FORMAT_PRGB32));
  BLContext context;BLContextCreateInfo info{};info.thread_count=threads;
  info.command_queue_limit=scene%2 ? 32768 : 0;ok(context.begin(image,&info));
  ok(context.set_comp_op(BL_COMP_OP_SRC_COPY));ok(context.fill_all(BLRgba32(0xffffffff)));
  ok(context.set_comp_op(BL_COMP_OP_SRC_OVER));
  auto small=make_path(123,2); // More than the asynchronous geometry cutoff.
  // Force split-job insertion to cross the 256-job queue boundary.
  for(int i=0;i<250;i++)ok(context.fill_path(small,BLRgba32(0x110000ff)));
  auto large=make_path(23456+scene/2,3000);
  ok(context.set_fill_rule(scene%2 ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO));
  if(scene&2)ok(context.clip_to_rect(BLRect(7.5,12.25,280.25,240.5)));
  if(scene&4)ok(context.set_transform(BLMatrix2D(0.8,0.125,-0.25,1.05,20.25,-10.5)));
  BLGradient gradient(BLLinearGradientValues(0,0,321,289));
  ok(gradient.add_stop(0,BLRgba32(0xaa1a2233)));ok(gradient.add_stop(1,BLRgba32(0x337799ff)));
  ok(context.fill_path(large,gradient));
  // A second split fill in the SAME batch exercises independent merge groups.
  ok(context.fill_path(large,BLRgba32(0x2244aa22)));
  ok(context.reset_transform());ok(context.restore_clipping());
  for(int i=0;i<40;i++)ok(context.fill_path(small,BLRgba32(0x22cc4400)));
  // All-horizontal or wholly clipped partial paths must complete their groups.
  BLPath invisible;
  for(int i=0;i<12000;i++){ok(invisible.move_to(-100,-100));ok(invisible.line_to(-90,-100));ok(invisible.close());}
  ok(context.fill_path(invisible,BLRgba32(0xffff0000)));
  ok(context.flush(BL_CONTEXT_FLUSH_SYNC));if(context.accumulated_error_flags())std::abort();ok(context.end());
  BLImageData data;ok(image.get_data(&data));std::vector<unsigned char> result(321*289*4);
  for(int y=0;y<289;y++)std::memcpy(result.data()+y*321*4,static_cast<char*>(data.pixel_data)+y*data.stride,321*4);
  return result;
}
int main(int argc,char**argv) {
  FILE* out=std::fopen(argc>1?argv[1]:"split-fill.rgba","wb");if(!out)return 2;
  for(unsigned scene=0;scene<8;scene++) {
    auto reference=render(0,scene);
    for(unsigned threads:{1u,4u,16u}) {
      auto pixels=render(threads,scene);
      if(pixels!=reference){std::fprintf(stderr,"Mismatch: scene=%u threads=%u\n",scene,threads);return 1;}
    }
    std::fwrite(reference.data(),1,reference.size(),out);
  }
  std::fclose(out);std::puts("8 giant-fill scenes match synchronous/1/4/16 threads exactly.");
}
