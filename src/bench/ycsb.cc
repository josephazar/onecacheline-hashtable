// Mixed read/update workloads: YCSB-B (95% read / 5% update),
// YCSB-A (50/50) and pure-update, zipf(0.99) request distribution over present
// keys, single thread @2^27, each map in ISOLATION (build -> measure -> free).
// An update overwrites the value of an existing key in place (no insert/resize).
// Inline (v4): the whole mixed stream rides the AMAC engine per 1024-op block via
// find_batch_ptr (the engine overlaps the probe; the store is applied to the
// returned in-line slot). Baselines: the same stream fully synchronous
// (find; find->second = v).
//   CSV: machine,log2n,mix,variant,mops,ci_lo,ci_hi
#include "swiss_table_chunkv.hpp"    // Inline (v4)
#include "zipf.hpp"
#include <absl/container/flat_hash_map.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <ankerl/unordered_dense.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
static void emit(const char* mix,const char* var,Mops m){
    printf("%s,%d,%s,%s,%.1f,%.1f,%.1f\n",MACHINE,LOG2N,mix,var,m.med,m.lo,m.hi); fflush(stdout);
}

int main(int argc,char**argv){
    MACHINE=getenv("MACHINE")?getenv("MACHINE"):"unknown";
    LOG2N=argc>1?atoi(argv[1]):27;
    size_t N=1ull<<LOG2N, Q=2'000'000;
    struct Mix{const char* name; double pu;};
    const Mix MIX[]={{"b05",0.05},{"a50",0.50},{"u100",1.00}};
    printf("machine,log2n,mix,variant,mops,ci_lo,ci_hi\n");
    std::vector<uint64_t> keys(N); for(size_t i=0;i<N;i++) keys[i]=splitmix64(i);
    // per-mix op streams: zipf(0.99) key + update flag, fixed seed
    std::vector<std::vector<uint64_t>> qk(3);
    std::vector<std::vector<uint8_t>>  up(3);
    for(int m=0;m<3;m++){
        std::mt19937_64 rng(123); ZipfGen z(N,0.99);
        std::uniform_real_distribution<double> U(0.0,1.0);
        qk[m].resize(Q); up[m].resize(Q);
        for(size_t i=0;i<Q;i++){ qk[m][i]=keys[z.sample(rng)-1]; up[m][i]=U(rng)<MIX[m].pu; }
    }
    const size_t B=1024;
    std::vector<uint64_t*> rp(B);

    { auto t=std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1);
      for(size_t i=0;i<N;i++) t->insert(keys[i],value_of(keys[i]));
      for(int m=0;m<3;m++){
          const uint64_t* K=qk[m].data(); const uint8_t* Uq=up[m].data();
          auto f=[&]{ uint64_t s=0;
              for(size_t i=0;i<Q;i+=B){ size_t e=std::min(Q,i+B), nb=e-i;
                  t->find_batch_ptr<32>(K+i,nb,rp.data());
                  for(size_t j=0;j<nb;j++){ uint64_t* p=rp[j]; if(!p) continue;
                      if(Uq[i+j]) *p=K[i+j]^0x5eedULL; else s+=*p; } }
              return s; };
          emit(MIX[m].name,"v4",mops(Q,f)); } }
    { auto t=std::make_unique<absl::flat_hash_map<uint64_t,uint64_t>>(); t->reserve(N);
      for(size_t i=0;i<N;i++) (*t)[keys[i]]=value_of(keys[i]);
      for(int m=0;m<3;m++){
          const uint64_t* K=qk[m].data(); const uint8_t* Uq=up[m].data();
          auto f=[&]{ uint64_t s=0;
              for(size_t i=0;i<Q;i++){ auto it=t->find(K[i]);
                  if(Uq[i]) it->second=K[i]^0x5eedULL; else s+=it->second; }
              return s; };
          emit(MIX[m].name,"abseil",mops(Q,f)); } }
    { auto t=std::make_unique<boost::unordered_flat_map<uint64_t,uint64_t>>(); t->reserve(N);
      for(size_t i=0;i<N;i++) (*t)[keys[i]]=value_of(keys[i]);
      for(int m=0;m<3;m++){
          const uint64_t* K=qk[m].data(); const uint8_t* Uq=up[m].data();
          auto f=[&]{ uint64_t s=0;
              for(size_t i=0;i<Q;i++){ auto it=t->find(K[i]);
                  if(Uq[i]) it->second=K[i]^0x5eedULL; else s+=it->second; }
              return s; };
          emit(MIX[m].name,"boost",mops(Q,f)); } }
    { auto t=std::make_unique<ankerl::unordered_dense::map<uint64_t,uint64_t>>(); t->reserve(N);
      for(size_t i=0;i<N;i++) (*t)[keys[i]]=value_of(keys[i]);
      for(int m=0;m<3;m++){
          const uint64_t* K=qk[m].data(); const uint8_t* Uq=up[m].data();
          auto f=[&]{ uint64_t s=0;
              for(size_t i=0;i<Q;i++){ auto it=t->find(K[i]);
                  if(Uq[i]) it->second=K[i]^0x5eedULL; else s+=it->second; }
              return s; };
          emit(MIX[m].name,"ankerl",mops(Q,f)); } }
    return 0;
}
