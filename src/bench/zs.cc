// Zipf skew sweep: does the skewed-lookup win depend on the
// exact YCSB exponent s=0.99? Sweep s over {0.50,0.75,0.99,1.25} at 2^27, Inline
// (v4, AMAC D=32) vs Abseil/Boost/ankerl queried synchronously, each map measured
// in ISOLATION (build -> measure -> free) like iso.cc.
//   CSV: machine,log2n,s,variant,mops,ci_lo,ci_hi
#include "swiss_table_chunkv.hpp"    // Inline (v4)
#include "zipf.hpp"
#include <absl/container/flat_hash_map.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <ankerl/unordered_dense.h>
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

static const char* MACHINE; static int LOG2N;
static void emit(double s,const char* var,Mops m){
    printf("%s,%d,%.2f,%s,%.1f,%.1f,%.1f\n",MACHINE,LOG2N,s,var,m.med,m.lo,m.hi); fflush(stdout);
}

int main(int argc,char**argv){
    MACHINE=getenv("MACHINE")?getenv("MACHINE"):"unknown";
    LOG2N=argc>1?atoi(argv[1]):27;
    size_t N=1ull<<LOG2N, Q=2'000'000;
    const double SKEW[]={0.50,0.75,0.99,1.25};
    printf("machine,log2n,s,variant,mops,ci_lo,ci_hi\n");
    std::vector<uint64_t> keys(N); for(size_t i=0;i<N;i++) keys[i]=splitmix64(i);
    // one query batch per skew, fixed seed
    std::vector<std::vector<uint64_t>> qs;
    for(double s : SKEW){
        std::mt19937_64 rng(123); ZipfGen z(N,s);
        std::vector<uint64_t> q(Q); for(size_t i=0;i<Q;i++) q[i]=keys[z.sample(rng)-1];
        qs.push_back(std::move(q));
    }
    std::vector<uint64_t> ov(Q,0);

    { auto t=std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1);
      for(size_t i=0;i<N;i++) t->insert(keys[i],value_of(keys[i]));
      for(int k=0;k<4;k++){
          auto f=[&]{t->find_batch<32>(qs[k].data(),Q,ov.data());uint64_t s=0;for(auto x:ov)s+=x;return s;};
          emit(SKEW[k],"v4",mops(Q,f)); } }
    { auto t=std::make_unique<absl::flat_hash_map<uint64_t,uint64_t>>(); t->reserve(N);
      for(size_t i=0;i<N;i++) (*t)[keys[i]]=value_of(keys[i]);
      for(int k=0;k<4;k++){
          auto f=[&]{uint64_t s=0;for(auto q:qs[k]){auto it=t->find(q);if(it!=t->end())s+=it->second;}return s;};
          emit(SKEW[k],"abseil",mops(Q,f)); } }
    { auto t=std::make_unique<boost::unordered_flat_map<uint64_t,uint64_t>>(); t->reserve(N);
      for(size_t i=0;i<N;i++) (*t)[keys[i]]=value_of(keys[i]);
      for(int k=0;k<4;k++){
          auto f=[&]{uint64_t s=0;for(auto q:qs[k]){auto it=t->find(q);if(it!=t->end())s+=it->second;}return s;};
          emit(SKEW[k],"boost",mops(Q,f)); } }
    { auto t=std::make_unique<ankerl::unordered_dense::map<uint64_t,uint64_t>>(); t->reserve(N);
      for(size_t i=0;i<N;i++) (*t)[keys[i]]=value_of(keys[i]);
      for(int k=0;k<4;k++){
          auto f=[&]{uint64_t s=0;for(auto q:qs[k]){auto it=t->find(q);if(it!=t->end())s+=it->second;}return s;};
          emit(SKEW[k],"ankerl",mops(Q,f)); } }
    return 0;
}
