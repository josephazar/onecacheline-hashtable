// Option C: does the per-chunk negative filter recover Inline's miss cost while
// keeping its hit? Correctness check (no false negatives) + hit/miss/zipf throughput
// of Inline+Filter (VF) vs Inline (V) vs Paired.
#include "swiss_table_chunkvf.hpp"   // Inline + filter
#include "swiss_table_chunkv.hpp"    // Inline
#include "swiss_table_chunk2.hpp"    // Paired
#include "zipf.hpp"
#include <boost/unordered/unordered_flat_map.hpp>
#include <absl/container/flat_hash_map.h>
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
static inline uint64_t value_of(uint64_t k){ return k*2654435761ULL + 1; }  // never 0
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
    SwissTableChunkVF<uint64_t,uint64_t> vf(size_t(N/0.7)+1);
    SwissTableChunkV <uint64_t,uint64_t> vi(size_t(N/0.7)+1);
    SwissTableChunk2 <uint64_t,uint64_t> pa(size_t(N/0.7)+1);
    for(size_t i=0;i<N;i++){ uint64_t v=value_of(keys[i]); vf.insert(keys[i],v); vi.insert(keys[i],v); pa.insert(keys[i],v); }

    // ---- correctness: no false negatives on present keys, no false positives-as-hit on absent ----
    {
        const size_t C = 5'000'000;
        std::mt19937_64 rng(7); std::uniform_int_distribution<size_t> pk(0,N-1);
        std::vector<uint64_t> qp(C), qa(C); std::vector<uint64_t> op(C), oa(C);
        for(size_t i=0;i<C;i++){ qp[i]=keys[pk(rng)]; qa[i]=splitmix64(N+i+7); }
        vf.find_batch<32>(qp.data(),C,op.data());
        vf.find_batch<32>(qa.data(),C,oa.data());
        size_t fn=0, fp=0;
        for(size_t i=0;i<C;i++){ if(op[i]!=value_of(qp[i])) fn++; if(oa[i]!=0) fp++; }
        std::printf("# correctness: present=%zu false_neg=%zu | absent=%zu wrong_hit=%zu\n",C,fn,C,fp);
        if(fn){ std::printf("# FAIL: false negatives!\n"); return 1; }
    }

    // ---- throughput: hit / miss / zipf ----
    std::mt19937_64 rng(123);
    std::uniform_int_distribution<size_t>  pk(0,N-1);
    std::uniform_int_distribution<uint64_t> pm(N,4*N);
    ZipfGen z(N,0.99);
    std::vector<uint64_t> qhit(Q),qmiss(Q),qzipf(Q);
    for(size_t i=0;i<Q;i++){ qhit[i]=keys[pk(rng)]; qmiss[i]=splitmix64(pm(rng)); qzipf[i]=keys[z.sample(rng)-1]; }
    std::vector<uint64_t> ov(Q,0);

    boost::unordered_flat_map<uint64_t,uint64_t> bo; bo.reserve(N);
    for(size_t i=0;i<N;i++) bo[keys[i]]=value_of(keys[i]);
    absl::flat_hash_map<uint64_t,uint64_t> ab; ab.reserve(N);
    for(size_t i=0;i<N;i++) ab[keys[i]]=value_of(keys[i]);

    auto run=[&](auto& T, std::vector<uint64_t>& q){ T.template find_batch<32>(q.data(),Q,ov.data());
        uint64_t s=0; for(auto x:ov)s+=x; return s; };
    auto scal=[&](auto& M, std::vector<uint64_t>& q){ uint64_t s=0;
        for(auto x:q){ auto it=M.find(x); if(it!=M.end())s+=it->second; } return s; };
    std::printf("table,workload,mops\n");
    struct { const char* wl; std::vector<uint64_t>* q; } WL[]={{"hit",&qhit},{"miss",&qmiss},{"zipf",&qzipf}};
    for(auto& w : WL){
        std::printf("Inline+F,%s,%.1f\n", w.wl, med_mops(Q,[&]{return run(vf,*w.q);}));
        std::printf("Inline,%s,%.1f\n",   w.wl, med_mops(Q,[&]{return run(vi,*w.q);}));
        std::printf("Paired,%s,%.1f\n",   w.wl, med_mops(Q,[&]{return run(pa,*w.q);}));
        std::printf("boost,%s,%.1f\n",    w.wl, med_mops(Q,[&]{return scal(bo,*w.q);}));
        std::printf("abseil,%s,%.1f\n",   w.wl, med_mops(Q,[&]{return scal(ab,*w.q);}));
        std::fflush(stdout);
    }
    return 0;
}
