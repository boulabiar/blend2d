// Integration check: queues crossing batch boundaries, band edges, curves, fills,
// strokes, affine transforms, gradients, clips and offscreen geometry.
#include <blend2d/blend2d.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static void ok(BLResult r) { if(r) {std::fprintf(stderr,"BLResult %u\n",r); std::abort();} }
static unsigned rng(unsigned& seed) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static std::vector<unsigned char> render(unsigned threads, unsigned seed, int size) {
 BLImage image;ok(image.create(size,size,BL_FORMAT_PRGB32));
 BLContext c;BLContextCreateInfo ci{};ci.thread_count=threads;ok(c.begin(image,&ci));
 ok(c.set_comp_op(BL_COMP_OP_SRC_COPY));ok(c.fill_all(BLRgba32(0xffffffff)));ok(c.set_comp_op(BL_COMP_OP_SRC_OVER));
 for(int i=0;i<18000;i++) {
  double x=int(rng(seed)%(size+64))-32, y=int(rng(seed)%(size+64))-32;
  // Exercise exact/fractional band boundaries and wholly offscreen bounds too.
  if(i%9==0)y=(rng(seed)%9)*32.0+(i%3)*0.5;
  BLPath p;ok(p.move_to(x,y));ok(p.line_to(x+1+(rng(seed)%21),y));
  ok(p.cubic_to(x+32,y+16,x-16,y+32,x+4,y+1+(rng(seed)%21)));ok(p.close());
  ok(c.reset_transform());ok(c.restore_clipping());
  if(i%7==0)ok(c.clip_to_rect(BLRect(4.5,8.5,size-9,size-17)));
  if(i%5==0)ok(c.set_transform(BLMatrix2D(1,0.125,-0.125,1,0.25,0.5)));
  ok(c.set_fill_rule(i%2?BL_FILL_RULE_EVEN_ODD:BL_FILL_RULE_NON_ZERO));
  BLRgba32 color(rng(seed)|0x80000000u);
  if(i%3==0){ok(c.set_stroke_width(0.25+(rng(seed)%8)));ok(c.set_stroke_caps(i%2?BL_STROKE_CAP_ROUND:BL_STROKE_CAP_SQUARE));ok(c.set_stroke_join(i%2?BL_STROKE_JOIN_MITER_CLIP:BL_STROKE_JOIN_ROUND));ok(c.stroke_path(p,color));}
  else if(i%3==1){BLGradient g(BLLinearGradientValues(x,y,x+20,y+20));ok(g.add_stop(0,color));ok(g.add_stop(1,BLRgba32(0x88ffffff)));ok(c.fill_path(p,g));}
  else {ok(c.fill_path(p,color));}
 }
 ok(c.flush(BL_CONTEXT_FLUSH_SYNC));if(c.accumulated_error_flags())std::abort();ok(c.end());
 BLImageData d;ok(image.get_data(&d));std::vector<unsigned char> data(size*size*4);
 for(int y=0;y<size;y++)std::memcpy(data.data()+y*size*4,(char*)d.pixel_data+y*d.stride,size*4);
 return data;
}
int main(int argc,char** argv){
 const char* output=argc>1?argv[1]:"equivalence.rgba";FILE* f=std::fopen(output,"wb");if(!f)return 2;
 for(int size:{31,128,257})for(unsigned seed:{1u,987654321u}){
  auto ref=render(0,seed,size);
  for(unsigned threads:{4u,16u})if(render(threads,seed,size)!=ref){std::fprintf(stderr,"Pixel mismatch size=%d seed=%u threads=%u\n",size,seed,threads);return 1;}
  std::fwrite(ref.data(),1,ref.size(),f);
 }
 std::fclose(f);std::puts("All 6 scenes match exactly at 1/4/16 threads (324,000 draws)." );
}
