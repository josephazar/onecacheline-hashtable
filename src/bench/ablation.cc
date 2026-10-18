// 2x2 ablation: layout {SoA, Chunked} x engine {Sync, Batched-AMAC}.
// Answers: does the speedup come from BATCHING or from the co-located CHUNK?
// A fair group-prefetching baseline (SoA + AMAC) is the key cell: if it closed the
// gap, the win would be batching; it does not -> halving lines-per-probe is what pays.
//   SoA-sync    = SwissTableH1H2::find_sync   (a plain SIMD SwissTable, ~abseil)
//   SoA-AMAC    = SwissTableH1H2::find_batch  (SoA + software-pipelined prefetch = Chen'04/AMAC)
//   Chunk-sync  = SwissTableChunk::find_sync  (co-located chunk, scalar)
//   Chunk-AMAC  = SwissTableChunk::find_batch (Detached: our layout + engine)
//   Inline-AMAC = SwissTableChunkV::find_batch(Inline: the tuned default)
#include "swiss_table_h1h2.hpp"
#include "swiss_table_chunk.hpp"
#include "swiss_table_chunkv.hpp"
#ifdef WITH_ABSEIL
#include <absl/container/flat_hash_map.h>
#endif
#include <cstdint>
#include <cstdio>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>

static inline uint64_t splitmix64(uint64_t x){
    x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;
    x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31);
}
static inline uint64_t value_of(uint64_t k){ return k*2654435761ULL + 1; }
using Clock = std::chrono::steady_clock;

template<class F> static double med_mops(size_t w, F f){
    const int R=15; double r[R];
    { volatile uint64_t s=f(); (void)s; }
    for(int i=0;i<R;i++){ auto a=Clock::now(); volatile uint64_t s=f(); (void)s;
        auto b=Clock::now(); r[i]=w/std::chrono::duration<double>(b-a).count()/1e6; }
    std::sort(r,r+R); return r[R/2];
}

int main(){
    const int sizes[] = {22, 27};                 // in-cache-ish, and DRAM
    const size_t Q = 2'000'000;
    std::printf("cell,workload,log2n,mops\n");
    for(int lg : sizes){
        size_t N = 1ull<<lg;
        std::vector<uint64_t> keys(N); for(size_t i=0;i<N;i++) keys[i]=splitmix64(i);
        std::mt19937_64 rng(123);
        std::uniform_int_distribution<size_t> pk(0,N-1);
        std::uniform_int_distribution<uint64_t> pm(N,4*N);
        std::vector<uint64_t> qhit(Q), qmiss(Q);
        for(size_t i=0;i<Q;i++){ qhit[i]=keys[pk(rng)]; qmiss[i]=splitmix64(pm(rng)); }

        SwissTableH1H2<uint64_t,uint64_t>  soa(size_t(N/0.7)+1);
        SwissTableChunk<uint64_t,uint64_t> chk(size_t(N/0.7)+1);
        SwissTableChunkV<uint64_t,uint64_t> inl(size_t(N/0.7)+1);
        for(size_t i=0;i<N;i++){ uint64_t v=value_of(keys[i]);
            soa.insert(keys[i],v); chk.insert(keys[i],v); inl.insert(keys[i],v); }
#ifdef WITH_ABSEIL
        absl::flat_hash_map<uint64_t,uint64_t> abs; abs.reserve(N);
        for(size_t i=0;i<N;i++) abs[keys[i]]=value_of(keys[i]);
#endif
        std::vector<uint64_t*> op(Q,nullptr); std::vector<uint64_t> ov(Q,0);

        auto soa_sync=[&](std::vector<uint64_t>&q){ uint64_t s=0;
            for(auto x:q){ auto*p=soa.find_sync(x); if(p)s+=*p; } return s; };
        auto soa_amac=[&](std::vector<uint64_t>&q){ soa.find_batch<32>(q.data(),Q,op.data());
            uint64_t s=0; for(auto*p:op) if(p)s+=*p; return s; };
        auto chk_sync=[&](std::vector<uint64_t>&q){ uint64_t s=0;
            for(auto x:q){ auto*p=chk.find_sync(x); if(p)s+=*p; } return s; };
        auto chk_amac=[&](std::vector<uint64_t>&q){ chk.find_batch<32>(q.data(),Q,op.data());
            uint64_t s=0; for(auto*p:op) if(p)s+=*p; return s; };
        auto inl_amac=[&](std::vector<uint64_t>&q){ inl.find_batch<32>(q.data(),Q,ov.data());
            uint64_t s=0; for(auto v:ov) s+=v; return s; };
#ifdef WITH_ABSEIL
        auto abs_sync=[&](std::vector<uint64_t>&q){ uint64_t s=0;
            for(auto x:q){ auto it=abs.find(x); if(it!=abs.end())s+=it->second; } return s; };
#endif
        struct { const char* wl; std::vector<uint64_t>* q; } WL[]={{"hit",&qhit},{"miss",&qmiss}};
        for(auto& w : WL){
            std::printf("SoA-sync,%s,%d,%.1f\n",   w.wl,lg,med_mops(Q,[&]{return soa_sync(*w.q);}));
            std::printf("SoA-AMAC,%s,%d,%.1f\n",   w.wl,lg,med_mops(Q,[&]{return soa_amac(*w.q);}));
            std::printf("Chunk-sync,%s,%d,%.1f\n", w.wl,lg,med_mops(Q,[&]{return chk_sync(*w.q);}));
            std::printf("Chunk-AMAC,%s,%d,%.1f\n", w.wl,lg,med_mops(Q,[&]{return chk_amac(*w.q);}));
            std::printf("Inline-AMAC,%s,%d,%.1f\n",w.wl,lg,med_mops(Q,[&]{return inl_amac(*w.q);}));
#ifdef WITH_ABSEIL
            std::printf("abseil-sync,%s,%d,%.1f\n",w.wl,lg,med_mops(Q,[&]{return abs_sync(*w.q);}));
#endif
            std::fflush(stdout);
        }
    }
    return 0;
}
