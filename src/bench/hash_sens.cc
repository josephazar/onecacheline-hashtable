// Hash-sensitivity micro-benchmark (§ hash sensitivity, reader request).
// Measures Inline (v4) hit throughput vs table size for two hash functions:
//   default  = cheap Fibonacci (1 multiply)          [build w/o -DSTRONG_HASH]
//   strong   = murmur3 fmix64 (2 mult + 3 xorshift)  [build w/ -DSTRONG_HASH=1]
// In-cache sizes isolate the compute ceiling T_cpu of Eq.(roofline); DRAM sizes
// show the hash choice is invisible once memory latency binds.
#include "swiss_table_chunkv.hpp"
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
    { volatile uint64_t s=f(); (void)s; }                 // warmup
    for(int i=0;i<R;i++){ auto a=Clock::now(); volatile uint64_t s=f(); (void)s;
        auto b=Clock::now(); r[i]=w/std::chrono::duration<double>(b-a).count()/1e6; }
    std::sort(r,r+R); return r[R/2];
}

int main(){
    const char* HASH =
#if defined(STRONG_HASH) && STRONG_HASH
        "strong";
#else
        "fibo";
#endif
    const int sizes[] = {14,16,18,20,22,25,27};           // in-cache -> DRAM
    const size_t Q = 2'000'000;
    std::printf("hash,log2n,mops\n");
    for(int lg : sizes){
        size_t N = 1ull<<lg;
        std::vector<uint64_t> keys(N); for(size_t i=0;i<N;i++) keys[i]=splitmix64(i);
        std::mt19937_64 rng(123); std::uniform_int_distribution<size_t> pk(0,N-1);
        std::vector<uint64_t> qhit(Q); for(size_t i=0;i<Q;i++) qhit[i]=keys[pk(rng)];
        SwissTableChunkV<uint64_t,uint64_t> cv(size_t(N/0.7)+1);
        for(size_t i=0;i<N;i++) cv.insert(keys[i], value_of(keys[i]));
        std::vector<uint64_t> ov(Q,0);
        auto run=[&](){ cv.find_batch<32>(qhit.data(),Q,ov.data());
                        uint64_t s=0; for(auto x:ov) s+=x; return s; };
        std::printf("%s,%d,%.1f\n", HASH, lg, med_mops(Q,run));
        std::fflush(stdout);
    }
    return 0;
}
