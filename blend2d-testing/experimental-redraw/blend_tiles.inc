// Experimental retained replay partition. Each owner submits a complete ordered
// draw stream into its disjoint integer row range. All coordinates stay global.
#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#ifdef __linux__
#include <sched.h>
#endif
#if defined(__GNUC__)
extern "C" BLResult bl_probe_set_band_range(BLContextCore*,uint32_t,uint32_t) noexcept __attribute__((weak));
#else
static constexpr BLResult (*bl_probe_set_band_range)(BLContextCore*,uint32_t,uint32_t) noexcept = nullptr;
#endif
static bool exact_tile_bands() {
  const char* mode=std::getenv("BLEND_PROBE_TILE_MODE");return !mode || std::strcmp(mode,"clip")!=0;
}
static std::vector<int> expand_cpu_list(const std::string& text) {
  std::vector<int> result;std::istringstream stream(text);std::string item;
  while(std::getline(stream,item,',')){
    auto dash=item.find('-');int first=std::stoi(item),last=dash==std::string::npos?first:std::stoi(item.substr(dash+1));
    for(int i=first;i<=last;i++)result.push_back(i);
  }
  return result;
}
static std::vector<std::vector<int>> tile_domains() {
  std::vector<std::vector<int>> result;
#ifdef __linux__
  cpu_set_t allowed;CPU_ZERO(&allowed);check(sched_getaffinity(0,sizeof(allowed),&allowed)?1:0);
  std::set<std::vector<int>> seen;
  for(int cpu=0;cpu<CPU_SETSIZE;cpu++)if(CPU_ISSET(cpu,&allowed)) {
    for(int index=0;index<8;index++) {
      std::string base="/sys/devices/system/cpu/cpu"+std::to_string(cpu)+"/cache/index"+std::to_string(index)+"/";
      int level=0;std::ifstream(base+"level")>>level;if(level!=3)continue;
      std::string list;std::ifstream(base+"shared_cpu_list")>>list;if(list.empty())continue;
      std::vector<int> group;for(int n:expand_cpu_list(list))if(CPU_ISSET(n,&allowed))group.push_back(n);
      if(!group.empty()&&seen.insert(group).second)result.push_back(std::move(group));
    }
  }
#endif
  if(result.empty())result.push_back({});
  return result;
}
struct TileOwner {
  Bench bench;
  std::mutex mutex;std::condition_variable changed;
  bool ready=false,run=false,done=true,stop=false,cached=false;
  std::thread owner;
  TileOwner(BLImageData data, int y0,int y1,unsigned threads,unsigned queue,std::vector<int> cpus,const BlProbePackedView* packed) {
    bench.render_packed=packed;bench.asynchronous=true;bench.restricted=!exact_tile_bands();bench.base_clip=BLRect(0,y0,data.size.w,y1-y0);
    owner=std::thread([this,data,threads,queue,cpus=std::move(cpus)]{
#ifdef __linux__
      if(!cpus.empty()){cpu_set_t mask;CPU_ZERO(&mask);for(int c:cpus)CPU_SET(c,&mask);check(sched_setaffinity(0,sizeof(mask),&mask)?1:0);}
#endif
      check(bench.image.create_from_data(data.size.w,data.size.h,BL_FORMAT_PRGB32,data.pixel_data,data.stride));
      BLContextCreateInfo info{};info.thread_count=threads;info.command_queue_limit=queue;
      info.flags=BL_CONTEXT_CREATE_FLAG_ISOLATED_THREAD_POOL;
      check(bench.context.begin(bench.image,&info));configure_async_limit(bench.context);
      if(bench.context.thread_count()!=threads)std::abort();
      if(exact_tile_bands()) {
        if(!bl_probe_set_band_range){std::fprintf(stderr,"Band ownership needs the domain-bands experimental library.\n");std::abort();}
        check(bl_probe_set_band_range(&bench.context,uint32_t(bench.base_clip.y),uint32_t(bench.base_clip.y+bench.base_clip.h)));
      }
      std::unique_lock<std::mutex> lock(mutex);ready=true;changed.notify_all();
      for(;;){changed.wait(lock,[this]{return run||stop;});if(stop)break;
        bool cached_now=cached;run=false;lock.unlock();
        if(cached_now){render_cached_dispatch(&bench);}
        else render<false>(&bench);
        lock.lock();done=true;changed.notify_all();
      }
      lock.unlock();check(bench.context.end());
    });
    std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[this]{return ready;});
  }
  void submit(bool use_cache){std::lock_guard<std::mutex> lock(mutex);cached=use_cache;done=false;run=true;changed.notify_all();}
  void wait(){std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[this]{return done;});}
  ~TileOwner(){{std::lock_guard<std::mutex> lock(mutex);stop=true;changed.notify_all();}owner.join();}
};
struct TileGroup {std::vector<std::unique_ptr<TileOwner>> owners;size_t input=0,assigned=0,points=0,assigned_points=0;bool reported=false;};
static void tile_begin(Bench* bench,unsigned threads,unsigned queue) {
  unsigned count=unsigned(std::strtoul(std::getenv("BLEND_PROBE_TILES"),nullptr,10));
  count=std::max(1u,std::min({count,threads,unsigned(bench->image.height()/64)}));
  auto* group=new TileGroup;bench->tiles=group;
  BLImageData data;check(bench->image.get_data(&data));auto domains=tile_domains();
  for(unsigned i=0;i<count;i++){
    int y0=int((uint64_t(i)*data.size.h/count)/64*64),y1=i+1==count?data.size.h:int((uint64_t(i+1)*data.size.h/count)/64*64);
    if(count==2 && std::getenv("BLEND_PROBE_TILE_CUT")){
      int cut=int(std::strtol(std::getenv("BLEND_PROBE_TILE_CUT"),nullptr,10));
      cut=std::max(64,std::min((data.size.h-1)/64*64,cut/64*64));
      if(i==0)y1=cut;else y0=cut;
    }
    unsigned workers=threads/count+(i<threads%count?1:0);
    group->owners.push_back(std::make_unique<TileOwner>(data,y0,y1,workers,queue,domains[i%domains.size()],&bench->packed_view));
    std::fprintf(stderr,"TILE owner=%u rows=%d:%d threads=%u domain=%zu\n",i,y0,y1,workers,i%domains.size());
  }
}
static void tile_add(Bench* parent,const Draw& source) {
  // Conservative control-point bounds are transformed before clipping. Strokes
  // expand by the miter limit and AA margin; no contour or draw is reordered.
  parent->tiles->input++;parent->tiles->points+=source.path.size();
  BLBox box;check(source.path.get_control_box(&box));
  BLPoint corners[]={source.transform.map_point(BLPoint(box.x0,box.y0)),source.transform.map_point(BLPoint(box.x0,box.y1)),source.transform.map_point(BLPoint(box.x1,box.y0)),source.transform.map_point(BLPoint(box.x1,box.y1))};
  double low=corners[0].y,high=low;for(auto p:corners){low=std::min(low,p.y);high=std::max(high,p.y);}
  double pad=2.+source.width*0.5*3.863703305*std::hypot(source.transform.m01,source.transform.m11);
  low=std::max(low-pad,source.clip.y);high=std::min(high+pad,source.clip.y+source.clip.h);
  for(auto& owner:parent->tiles->owners){
    BLRect stripe=owner->bench.base_clip;
    bool filter=!(std::getenv("BLEND_PROBE_TILE_FILTER") && std::strcmp(std::getenv("BLEND_PROBE_TILE_FILTER"),"0")==0);
    if(filter && (high<=stripe.y||low>=stripe.y+stripe.h))continue;
    Draw draw=source;draw.updates=0;
    double y0=std::max(source.clip.y,stripe.y),y1=std::min(source.clip.y+source.clip.h,stripe.y+stripe.h);
    if(!exact_tile_bands()){draw.clip.y=y0;draw.clip.h=std::max(0.,y1-y0);}
    parent->tiles->assigned++;parent->tiles->assigned_points+=draw.path.size();
    append_draw(owner->bench,std::move(draw));
  }
}
static void tile_render(Bench* parent,bool cached){if(!parent->tiles->reported){auto* g=parent->tiles;std::fprintf(stderr,"TILE_PLAN inputs=%zu assigned=%zu input_points=%zu assigned_points=%zu\n",g->input,g->assigned,g->points,g->assigned_points);g->reported=true;}for(auto& owner:parent->tiles->owners)owner->submit(cached);for(auto& owner:parent->tiles->owners)owner->wait();}
static void tile_free(Bench* parent){delete parent->tiles;parent->tiles=nullptr;}
