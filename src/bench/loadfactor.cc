// #7 load-factor sweep: does Inline's 3-slot group degrade at high load (0.9)?
// Compares Inline (3 slots/chunk) vs Detached (7 slots/chunk): probe length and
// hit/miss throughput as a function of achieved load factor. Build with
// -DHT_MAXLOAD=0.99 so the target load is reached without an auto-rehash.
#include "swiss_table_chunkv.hpp"
#include "swiss_table_chunk.hpp"
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
    const int R=9; double r[R];
    { volatile uint64_t s=f(); (void)s; }
    for(int i=0;i<R;i++){ auto a=Clock::now(); volatile uint64_t s=f(); (void)s;
        auto b=Clock::now(); r[i]=w/std::chrono::duration<double>(b-a).count()/1e6; }
    std::sort(r,r+R); return r[R/2];
}

int main(){
    const size_t N = 1ull<<24, Q = 1'000'000;
    const double targets[] = {0.5,0.6,0.7,0.8,0.9,0.95};
    std::vector<uint64_t> keys(N); for(size_t i=0;i<N;i++) keys[i]=splitmix64(i);
    std::mt19937_64 rng(123); std::uniform_int_distribution<size_t> pk(0,N-1);
    std::vector<uint64_t> qhit(Q), qmiss(Q);
    for(size_t i=0;i<Q;i++){ qhit[i]=keys[pk(rng)]; qmiss[i]=splitmix64(N+i+7); }
    std::vector<uint64_t*> op(Q,nullptr); std::vector<uint64_t> ov(Q,0);
    std::printf("layout,target_load,achieved_load,probelen,hit_mops,miss_mops\n");
    for(double L : targets){
        size_t cap = size_t(N / L);   // capacité en slots
        {
            SwissTableChunkV<uint64_t,uint64_t> cv(cap);
            for(size_t i=0;i<N;i++) cv.insert(keys[i], value_of(keys[i]));
            double lf = double(cv.count())/cv.slot_cap();
            size_t S=200000, tot=0; for(size_t i=0;i<S;i++) tot+=cv.probe_len(qhit[i]);
            auto hit =[&](){ cv.find_batch<32>(qhit.data(),Q,ov.data());  uint64_t s=0; for(auto v:ov)s+=v; return s; };
            auto miss=[&](){ cv.find_batch<32>(qmiss.data(),Q,ov.data()); uint64_t s=0; for(auto v:ov)s+=v; return s; };
            std::printf("Inline,%.2f,%.3f,%.3f,%.1f,%.1f\n",L,lf,(double)tot/S,med_mops(Q,hit),med_mops(Q,miss));
        }
        {
            SwissTableChunk<uint64_t,uint64_t> ck(cap);
            for(size_t i=0;i<N;i++) ck.insert(keys[i], value_of(keys[i]));
            double lf = double(ck.count())/ck.slot_cap();
            size_t S=200000, tot=0; for(size_t i=0;i<S;i++) tot+=ck.probe_len(qhit[i]);
            auto hit =[&](){ ck.find_batch<32>(qhit.data(),Q,op.data());  uint64_t s=0; for(auto*p:op)if(p)s+=*p; return s; };
            auto miss=[&](){ ck.find_batch<32>(qmiss.data(),Q,op.data()); uint64_t s=0; for(auto*p:op)if(p)s+=*p; return s; };
            std::printf("Detached,%.2f,%.3f,%.3f,%.1f,%.1f\n",L,lf,(double)tot/S,med_mops(Q,hit),med_mops(Q,miss));
        }
        std::fflush(stdout);
    }
    return 0;
}
