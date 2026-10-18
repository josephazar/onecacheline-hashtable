// #4 string keys: does the co-design (chunk + AMAC) still win when the key is a
// std::string stored out-of-line, so EVERY map pays a key-dereference miss?
#include "swiss_table_str.hpp"
#include <absl/container/flat_hash_map.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
#include <random>
#include <chrono>
#include <algorithm>

static inline uint64_t sm(uint64_t x){x+=0x9e3779b97f4a7c15ULL;x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;x=(x^(x>>27))*0x94d049bb133111ebULL;return x^(x>>31);}
static std::string mkkey(uint64_t i){ char b[16]; snprintf(b,16,"%015llx",(unsigned long long)(sm(i)&0xFFFFFFFFFFFFFFFULL)); return std::string(b); } // 15 chars, SSO
using C=std::chrono::steady_clock;
template<class F> static double mm(size_t w,F f){ const int R=15; double r[R]; {volatile uint64_t s=f();(void)s;}
    for(int i=0;i<R;i++){auto a=C::now();volatile uint64_t s=f();(void)s;auto b=C::now();r[i]=w/std::chrono::duration<double>(b-a).count()/1e6;} std::sort(r,r+R); return r[R/2]; }

int main(){
    const size_t N=1ull<<26, Q=2'000'000;                       // 2^26 strings, DRAM
    std::vector<std::string> keys(N); for(size_t i=0;i<N;i++) keys[i]=mkkey(i);
    SwissTableStr<uint64_t> ck(size_t(N/0.7)+1);      for(size_t i=0;i<N;i++) ck.insert(keys[i], i+1);
    absl::flat_hash_map<std::string,uint64_t> ab; ab.reserve(N); for(size_t i=0;i<N;i++) ab[keys[i]]=i+1;
    boost::unordered_flat_map<std::string,uint64_t> bo; bo.reserve(N); for(size_t i=0;i<N;i++) bo[keys[i]]=i+1;

    std::mt19937_64 r(123); std::uniform_int_distribution<size_t> pk(0,N-1);
    std::vector<std::string> qhit(Q),qmiss(Q); for(size_t i=0;i<Q;i++){ qhit[i]=keys[pk(r)]; qmiss[i]=mkkey(N+i+7); }
    std::vector<uint64_t> ov(Q,0);

    auto ours=[&](std::vector<std::string>&q){ ck.find_batch<32>(q.data(),Q,ov.data()); uint64_t s=0; for(auto x:ov)s+=x; return s; };
    auto sca =[&](auto&M,std::vector<std::string>&q){ uint64_t s=0; for(auto&x:q){auto it=M.find(x); if(it!=M.end())s+=it->second;} return s; };
    std::printf("table,workload,mops\n");
    for(auto* wl : {"hit","miss"}){
        auto& q = (wl[0]=='h') ? qhit : qmiss;
        std::printf("Chunk+AMAC,%s,%.1f\n", wl, mm(Q,[&]{return ours(q);}));
        std::printf("abseil,%s,%.1f\n",     wl, mm(Q,[&]{return sca(ab,q);}));
        std::printf("boost,%s,%.1f\n",      wl, mm(Q,[&]{return sca(bo,q);}));
        std::fflush(stdout);
    }
    return 0;
}
