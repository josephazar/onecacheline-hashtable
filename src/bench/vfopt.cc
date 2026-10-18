// vfopt : réduire le coût HIT résiduel d'Inline+F (1-7 % vs Inline sur G5K).
// A/B des moteurs find_batch de SwissTableChunkVF (même table, mêmes données) :
//   v0 = find_batch   (actuel, if/else, filtre inliné)
//   vb = find_batch_b (switch identique à chunkv + bloc filtre [[unlikely]])
//   vh = find_batch_h (hash stocké dans la lane, pas de recalcul au filtre)
//   vc = find_batch_c (filtre externalisé en fonction froide noinline)
// Référence : Inline (SwissTableChunkV::find_batch). Header-only (pas d'abseil/boost).
// CSV: machine,log2n,workload,variant,mops,ci_lo,ci_hi
#include "swiss_table_chunkvf.hpp"
#include "swiss_table_chunkv.hpp"
#include "zipf.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
// médiane de R répétitions + IC 95% non paramétrique (rangs 3/12 pour R=15)
template<class F> static void med_ci(size_t w, F f, double& med, double& lo, double& hi){
    const int R=15; double r[R];
    { volatile uint64_t s=f(); (void)s; }
    for(int i=0;i<R;i++){ auto a=Clock::now(); volatile uint64_t s=f(); (void)s;
        auto b=Clock::now(); r[i]=w/std::chrono::duration<double>(b-a).count()/1e6; }
    std::sort(r,r+R); med=r[R/2]; lo=r[3]; hi=r[11];
}

int main(int argc, char** argv){
    const int LOG2N = argc>1 ? atoi(argv[1]) : 27;
    const size_t N = 1ull<<LOG2N, Q = 2'000'000;
    const char* MACHINE = getenv("MACHINE") ? getenv("MACHINE") : "local";

    std::vector<uint64_t> keys(N); for(size_t i=0;i<N;i++) keys[i]=splitmix64(i);
    SwissTableChunkVF<uint64_t,uint64_t> vf(size_t(N/0.7)+1);
    SwissTableChunkV <uint64_t,uint64_t> vi(size_t(N/0.7)+1);
    for(size_t i=0;i<N;i++){ uint64_t v=value_of(keys[i]); vf.insert(keys[i],v); vi.insert(keys[i],v); }

    // correction : 0 faux négatif / 0 faux hit, pour CHAQUE variante
    {
        const size_t C = 1'000'000;
        std::mt19937_64 rng(7); std::uniform_int_distribution<size_t> pk(0,N-1);
        std::vector<uint64_t> qp(C), qa(C), o(C);
        for(size_t i=0;i<C;i++){ qp[i]=keys[pk(rng)]; qa[i]=splitmix64(N+i+7); }
        auto check=[&](const char* name, auto&& fb){
            size_t fn=0, fp=0;
            fb(qp.data(),C,o.data()); for(size_t i=0;i<C;i++) if(o[i]!=value_of(qp[i])) fn++;
            fb(qa.data(),C,o.data()); for(size_t i=0;i<C;i++) if(o[i]!=0) fp++;
            std::printf("# correctness %s: false_neg=%zu wrong_hit=%zu\n",name,fn,fp);
            if(fn||fp){ std::printf("# FAIL\n"); exit(1); }
        };
        check("v0",[&](const uint64_t*q,size_t n,uint64_t*out){ vf.find_batch<32>(q,n,out); });
        check("vb",[&](const uint64_t*q,size_t n,uint64_t*out){ vf.find_batch_b<32>(q,n,out); });
        check("vh",[&](const uint64_t*q,size_t n,uint64_t*out){ vf.find_batch_h<32>(q,n,out); });
        check("vc",[&](const uint64_t*q,size_t n,uint64_t*out){ vf.find_batch_c<32>(q,n,out); });
    }

    std::mt19937_64 rng(123);
    std::uniform_int_distribution<size_t>  pk(0,N-1);
    std::uniform_int_distribution<uint64_t> pm(N,4*N);
    ZipfGen z(N,0.99);
    std::vector<uint64_t> qhit(Q),qmiss(Q),qzipf(Q);
    for(size_t i=0;i<Q;i++){ qhit[i]=keys[pk(rng)]; qmiss[i]=splitmix64(pm(rng)); qzipf[i]=keys[z.sample(rng)-1]; }
    std::vector<uint64_t> ov(Q,0);
    auto sum=[&]{ uint64_t s=0; for(auto x:ov)s+=x; return s; };

    std::printf("machine,log2n,workload,variant,mops,ci_lo,ci_hi\n");
    struct { const char* wl; std::vector<uint64_t>* q; } WL[]={{"hit",&qhit},{"miss",&qmiss},{"zipf",&qzipf}};
    for(auto& w : WL){
        auto emit=[&](const char* name, auto&& fb){
            double m,lo,hi; med_ci(Q,[&]{ fb(w.q->data(),Q,ov.data()); return sum(); },m,lo,hi);
            std::printf("%s,%d,%s,%s,%.1f,%.1f,%.1f\n",MACHINE,LOG2N,w.wl,name,m,lo,hi);
            std::fflush(stdout);
        };
        emit("Inline",[&](const uint64_t*q,size_t n,uint64_t*out){ vi.find_batch<32>(q,n,out); });
        emit("vf_v0",[&](const uint64_t*q,size_t n,uint64_t*out){ vf.find_batch<32>(q,n,out); });
        emit("vf_b", [&](const uint64_t*q,size_t n,uint64_t*out){ vf.find_batch_b<32>(q,n,out); });
        emit("vf_h", [&](const uint64_t*q,size_t n,uint64_t*out){ vf.find_batch_h<32>(q,n,out); });
        emit("vf_c", [&](const uint64_t*q,size_t n,uint64_t*out){ vf.find_batch_c<32>(q,n,out); });
    }
    return 0;
}
