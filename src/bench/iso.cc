// Isolated throughput: each map is built, measured, and FREED before the next, so
// only ONE map is resident at a time -> no cross-map co-residency artifact (the
// representative single-table condition). Same CSV as probe_portable so the existing
// aggregation/figure scripts consume it unchanged.
//   CSV: machine,log2n,keys,workload,variant,mops,ci_lo,ci_hi
#include "swiss_table_chunk.hpp"     // Detached  (v3)
#include "swiss_table_chunkv.hpp"    // Inline    (v4)
#include "swiss_table_chunkvf.hpp"   // Inline+F  (vf)
#include "swiss_table_chunk2.hpp"    // Paired    (v5)
#include "zipf.hpp"
#include <absl/container/flat_hash_map.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <ankerl/unordered_dense.h>
extern "C" void     f14_build(const uint64_t*, const uint64_t*, size_t);
extern "C" uint64_t f14_probe(const uint64_t*, size_t);
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>
#include <algorithm>

static inline uint64_t splitmix64(uint64_t x){x+=0x9e3779b97f4a7c15ULL;x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;x=(x^(x>>27))*0x94d049bb133111ebULL;return x^(x>>31);}
static inline uint64_t value_of(uint64_t k){return k*2654435761ULL+1;}
using Clock=std::chrono::steady_clock;
struct Mops{double med,lo,hi;};
template<class F> static Mops mops(size_t w,F f){
    const int R=15; double r[R];
    { volatile uint64_t s=f(); (void)s; }
    for(int i=0;i<R;i++){auto a=Clock::now();volatile uint64_t s=f();(void)s;auto b=Clock::now();
        r[i]=w/std::chrono::duration<double>(b-a).count()/1e6;}
    std::sort(r,r+R); return {r[R/2],r[3],r[R-4]};
}

static const char* MACHINE;
static int LOG2N;
static void emit(const char* wl,const char* var,Mops m,size_t N){
    printf("%s,%d,%zu,%s,%s,%.1f,%.1f,%.1f\n",MACHINE,LOG2N,N,wl,var,m.med,m.lo,m.hi); fflush(stdout);
}

int main(int argc,char**argv){
    MACHINE=getenv("MACHINE")?getenv("MACHINE"):"unknown";
    LOG2N=argc>1?atoi(argv[1]):27;
    size_t N=1ull<<LOG2N, Q=2'000'000;
    printf("machine,log2n,keys,workload,variant,mops,ci_lo,ci_hi\n");
    std::vector<uint64_t> keys(N); for(size_t i=0;i<N;i++) keys[i]=splitmix64(i);
    std::mt19937_64 rng(123);
    std::uniform_int_distribution<size_t> pk(0,N-1);
    std::uniform_int_distribution<uint64_t> pm(N,4*N);
    ZipfGen z(N,0.99);
    std::vector<uint64_t> qhit(Q),qmiss(Q),qzipf(Q);
    for(size_t i=0;i<Q;i++){qhit[i]=keys[pk(rng)];qmiss[i]=splitmix64(pm(rng));qzipf[i]=keys[z.sample(rng)-1];}
    struct { const char* wl; std::vector<uint64_t>* q; } WL[]={{"hit",&qhit},{"miss",&qmiss},{"zipf",&qzipf}};
    std::vector<uint64_t*> op(Q,nullptr); std::vector<uint64_t> ov(Q,0);

    // ---- our chunked tables: build -> measure -> free, one at a time ----
    { auto t=std::make_unique<SwissTableChunk<uint64_t,uint64_t>>(size_t(N/0.7)+1);
      for(size_t i=0;i<N;i++) t->insert(keys[i],value_of(keys[i]));
      auto k=[&](std::vector<uint64_t>&q){t->find_batch<32>(q.data(),Q,op.data());uint64_t s=0;for(auto*p:op)if(p)s+=*p;return s;};
      for(auto&w:WL) emit(w.wl,"v3",mops(Q,[&]{return k(*w.q);}),N); }
    { auto t=std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1);
      for(size_t i=0;i<N;i++) t->insert(keys[i],value_of(keys[i]));
      auto k=[&](std::vector<uint64_t>&q){t->find_batch<32>(q.data(),Q,ov.data());uint64_t s=0;for(auto x:ov)s+=x;return s;};
      for(auto&w:WL) emit(w.wl,"v4",mops(Q,[&]{return k(*w.q);}),N); }
    { auto t=std::make_unique<SwissTableChunkVF<uint64_t,uint64_t>>(size_t(N/0.7)+1);
      for(size_t i=0;i<N;i++) t->insert(keys[i],value_of(keys[i]));
      auto k=[&](std::vector<uint64_t>&q){t->find_batch<32>(q.data(),Q,ov.data());uint64_t s=0;for(auto x:ov)s+=x;return s;};
      for(auto&w:WL) emit(w.wl,"vf",mops(Q,[&]{return k(*w.q);}),N); }
    { auto t=std::make_unique<SwissTableChunk2<uint64_t,uint64_t>>(size_t(N/0.7)+1);
      for(size_t i=0;i<N;i++) t->insert(keys[i],value_of(keys[i]));
      auto k=[&](std::vector<uint64_t>&q){t->find_batch<32>(q.data(),Q,ov.data());uint64_t s=0;for(auto x:ov)s+=x;return s;};
      for(auto&w:WL) emit(w.wl,"v5",mops(Q,[&]{return k(*w.q);}),N); }
    // ---- production baselines: build -> measure -> free ----
    { auto t=std::make_unique<absl::flat_hash_map<uint64_t,uint64_t>>(); t->reserve(N);
      for(size_t i=0;i<N;i++) (*t)[keys[i]]=value_of(keys[i]);
      auto k=[&](std::vector<uint64_t>&q){uint64_t s=0;for(auto x:q){auto it=t->find(x);if(it!=t->end())s+=it->second;}return s;};
      for(auto&w:WL) emit(w.wl,"abseil",mops(Q,[&]{return k(*w.q);}),N); }
    { auto t=std::make_unique<boost::unordered_flat_map<uint64_t,uint64_t>>(); t->reserve(N);
      for(size_t i=0;i<N;i++) (*t)[keys[i]]=value_of(keys[i]);
      auto k=[&](std::vector<uint64_t>&q){uint64_t s=0;for(auto x:q){auto it=t->find(x);if(it!=t->end())s+=it->second;}return s;};
      for(auto&w:WL) emit(w.wl,"boost",mops(Q,[&]{return k(*w.q);}),N); }
    { auto t=std::make_unique<ankerl::unordered_dense::map<uint64_t,uint64_t>>(); t->reserve(N);
      for(size_t i=0;i<N;i++) (*t)[keys[i]]=value_of(keys[i]);
      auto k=[&](std::vector<uint64_t>&q){uint64_t s=0;for(auto x:q){auto it=t->find(x);if(it!=t->end())s+=it->second;}return s;};
      for(auto&w:WL) emit(w.wl,"ankerl",mops(Q,[&]{return k(*w.q);}),N); }
    // ---- F14 last (global inside f14_bench; nothing co-resident after it) ----
    { std::vector<uint64_t> vals(N); for(size_t i=0;i<N;i++) vals[i]=value_of(keys[i]);
      f14_build(keys.data(),vals.data(),N);
      for(auto&w:WL) emit(w.wl,"f14",mops(Q,[&]{return f14_probe(w.q->data(),Q);}),N); }
    return 0;
}
