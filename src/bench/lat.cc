// #6 latency: (a) throughput/latency vs batch size B for the Inline engine, and
// (b) synchronous point-lookup (B=1) mean latency vs F14/Abseil/Boost -- backs the
// claim that a single GET matches a synchronous chunked map while batches trade a
// little latency for large throughput.
#include "swiss_table_chunkv.hpp"
#include <absl/container/flat_hash_map.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>

extern "C" void     f14_build(const uint64_t*, const uint64_t*, size_t);
extern "C" uint64_t f14_probe(const uint64_t*, size_t);

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
    const size_t N = 1ull<<27, Q = 2'000'000;
    std::vector<uint64_t> keys(N); for(size_t i=0;i<N;i++) keys[i]=splitmix64(i);
    std::mt19937_64 rng(123); std::uniform_int_distribution<size_t> pk(0,N-1);
    std::vector<uint64_t> qhit(Q); for(size_t i=0;i<Q;i++) qhit[i]=keys[pk(rng)];
    std::vector<uint64_t> ov(Q,0);

    SwissTableChunkV<uint64_t,uint64_t> cv(size_t(N/0.7)+1);
    for(size_t i=0;i<N;i++) cv.insert(keys[i], value_of(keys[i]));
    absl::flat_hash_map<uint64_t,uint64_t> abs; abs.reserve(N);
    for(size_t i=0;i<N;i++) abs[keys[i]]=value_of(keys[i]);
    boost::unordered_flat_map<uint64_t,uint64_t> bo; bo.reserve(N);
    for(size_t i=0;i<N;i++) bo[keys[i]]=value_of(keys[i]);
    std::vector<uint64_t> vals(N); for(size_t i=0;i<N;i++) vals[i]=value_of(keys[i]);
    f14_build(keys.data(), vals.data(), N);

    std::printf("kind,label,mops,ns_per_lookup\n");
    // (a) batch-size sweep: process the stream in sub-batches of B
    for(int B : {1,2,4,8,16,32,64,128,256}){
        auto run=[&](){ uint64_t s=0;
            for(size_t i=0;i<Q;i+=B){ size_t n=std::min((size_t)B,Q-i);
                cv.find_batch<32>(qhit.data()+i,n,ov.data()+i); }
            for(auto v:ov)s+=v; return s; };
        double m=med_mops(Q,run);
        std::printf("batch,%d,%.1f,%.1f\n",B,m,1000.0/m);
    }
    // (b) synchronous point lookup (B=1) mean latency vs baselines
    auto inl=[&](){ uint64_t s=0; for(auto x:qhit){auto*p=cv.find_sync(x); if(p)s+=*p;} return s; };
    auto ab =[&](){ uint64_t s=0; for(auto x:qhit){auto it=abs.find(x); if(it!=abs.end())s+=it->second;} return s; };
    auto bs =[&](){ uint64_t s=0; for(auto x:qhit){auto it=bo.find(x);  if(it!=bo.end()) s+=it->second;} return s; };
    auto f4 =[&](){ return f14_probe(qhit.data(),Q); };
    for(auto pr : {std::pair<const char*,double>{"Inline-sync",med_mops(Q,inl)},
                   {"F14",med_mops(Q,f4)}, {"abseil",med_mops(Q,ab)}, {"boost",med_mops(Q,bs)}})
        std::printf("point,%s,%.1f,%.1f\n",pr.first,pr.second,1000.0/pr.second);
    return 0;
}
