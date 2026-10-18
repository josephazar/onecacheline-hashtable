// #8 hash robustness on NON-IDEAL keys.
// Inline table, cheap Fibonacci hash [default] vs strong murmur3 [-DSTRONG_HASH=1],
// over realistic structured key distributions. Reports hit/miss throughput AND the
// average probe length (chunks visited) -- the direct evidence of clustering.
#include "swiss_table_chunkv.hpp"
#include <cstdint>
#include <cstdio>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <cstring>

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

// key generators over index i (raw, structured -- NOT pre-hashed)
static uint64_t g_random (uint64_t i){ return splitmix64(i); }        // ideal (control)
static uint64_t g_seq    (uint64_t i){ return i + 1; }                // sequential IDs
static uint64_t g_aligned(uint64_t i){ return (i + 1) << 12; }        // page-aligned pointers
static uint64_t g_struct (uint64_t i){ return ((i % (1u<<20)) )      // 20-bit id ...
                                            | ((i >> 20) << 40); }    // ... + category in high bits

int main(){
    const char* HASH =
#if defined(STRONG_HASH) && STRONG_HASH
        "strong";
#else
        "fibo";
#endif
    struct { const char* name; uint64_t(*gen)(uint64_t); } DIST[] = {
        {"random",g_random},{"seq",g_seq},{"aligned",g_aligned},{"struct",g_struct}};
    const int lg = 27; const size_t N = 1ull<<lg, Q = 2'000'000;
    std::printf("hash,dist,hit_mops,miss_mops,avg_probelen\n");
    for(auto& D : DIST){
        std::vector<uint64_t> keys(N); for(size_t i=0;i<N;i++) keys[i]=D.gen(i);
        // miss keys: same generator, indices beyond N (not inserted)
        std::vector<uint64_t> qhit(Q), qmiss(Q);
        std::mt19937_64 rng(123); std::uniform_int_distribution<size_t> pk(0,N-1);
        for(size_t i=0;i<Q;i++){ qhit[i]=keys[pk(rng)]; qmiss[i]=D.gen(N + i + 7); }
        SwissTableChunkV<uint64_t,uint64_t> cv(size_t(N/0.7)+1);
        for(size_t i=0;i<N;i++) cv.insert(keys[i], value_of(keys[i]));
        std::vector<uint64_t> ov(Q,0);
        auto hit =[&](){ cv.find_batch<32>(qhit.data(),Q,ov.data());  uint64_t s=0; for(auto v:ov)s+=v; return s; };
        auto miss=[&](){ cv.find_batch<32>(qmiss.data(),Q,ov.data()); uint64_t s=0; for(auto v:ov)s+=v; return s; };
        // average probe length over a sample of present keys
        size_t S=200000, tot=0; for(size_t i=0;i<S;i++) tot+=cv.probe_len(qhit[i]);
        double avg_pl = (double)tot/S;
        std::printf("%s,%s,%.1f,%.1f,%.3f\n", HASH, D.name,
                    med_mops(Q,hit), med_mops(Q,miss), avg_pl);
        std::fflush(stdout);
    }
    return 0;
}
