// A+B: throughput of the three value-placement layouts vs HIT RATIO h, to validate
// the model's predicted crossover h* = 1 - 1/m (m = Inline's effective miss cost).
// The best layout is the one minimising average lines/lookup C(h)=h*C_hit+(1-h)*C_miss;
// Inline (C_hit=1) wins at high h, Paired/Detached (C_miss=1, 7 slots) win at low h.
#include "swiss_table_chunkv.hpp"   // Inline
#include "swiss_table_chunk.hpp"    // Detached
#include "swiss_table_chunk2.hpp"   // Paired
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
    const int R=11; double r[R];
    { volatile uint64_t s=f(); (void)s; }
    for(int i=0;i<R;i++){ auto a=Clock::now(); volatile uint64_t s=f(); (void)s;
        auto b=Clock::now(); r[i]=w/std::chrono::duration<double>(b-a).count()/1e6; }
    std::sort(r,r+R); return r[R/2];
}

int main(){
    const size_t N = 1ull<<27, Q = 2'000'000;
    std::vector<uint64_t> keys(N); for(size_t i=0;i<N;i++) keys[i]=splitmix64(i);
    SwissTableChunkV<uint64_t,uint64_t> inl(size_t(N/0.7)+1);
    SwissTableChunk <uint64_t,uint64_t> det(size_t(N/0.7)+1);
    SwissTableChunk2<uint64_t,uint64_t> pai(size_t(N/0.7)+1);
    for(size_t i=0;i<N;i++){ uint64_t v=value_of(keys[i]);
        inl.insert(keys[i],v); det.insert(keys[i],v); pai.insert(keys[i],v); }
    std::vector<uint64_t>  ov(Q,0);
    std::vector<uint64_t*> op(Q,nullptr);

    std::mt19937_64 rng(123);
    std::uniform_int_distribution<size_t>  pk(0,N-1);
    std::uniform_int_distribution<uint64_t> pm(N,4*N);
    std::uniform_real_distribution<double>  u(0,1);

    std::printf("layout,h,mops\n");
    for(int hi=0; hi<=10; ++hi){
        double h = hi/10.0;
        std::vector<uint64_t> q(Q);
        for(size_t i=0;i<Q;i++) q[i] = (u(rng) < h) ? keys[pk(rng)] : splitmix64(pm(rng));
        auto I=[&](){ inl.find_batch<32>(q.data(),Q,ov.data()); uint64_t s=0; for(auto x:ov)s+=x; return s; };
        auto P=[&](){ pai.find_batch<32>(q.data(),Q,ov.data()); uint64_t s=0; for(auto x:ov)s+=x; return s; };
        auto D=[&](){ det.find_batch<32>(q.data(),Q,op.data()); uint64_t s=0; for(auto*p:op)if(p)s+=*p; return s; };
        std::printf("Inline,%.1f,%.1f\n",   h, med_mops(Q,I));
        std::printf("Paired,%.1f,%.1f\n",   h, med_mops(Q,P));
        std::printf("Detached,%.1f,%.1f\n", h, med_mops(Q,D));
        std::fflush(stdout);
    }
    return 0;
}
