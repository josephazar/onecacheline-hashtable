// vfdiag : DIAGNOSTIC du coût hit résiduel d'Inline+F (vfopt a montré qu'il est
// insensible au codegen du moteur - 3 réécritures = même coût).
// Deux discriminants :
//  1) SWEEP DU FACTEUR DE CHARGE : à load ~0.35 quasi aucun chunk n'est plein →
//     le chemin filtre n'est jamais pris, VF et Inline exécutent des chemins
//     identiques. Coût disparaît => algorithmique (hits débordés). Persiste =>
//     artefact (codegen/mémoire).
//  2) COMPTEURS PERF (perf_event_open, si autorisé) : instructions et cycles par
//     lookup → compute en plus vs IPC en moins.
// CSV: machine,log2n,load,workload,variant,mops,ci_lo,ci_hi,inst_per_q,cyc_per_q
#include "swiss_table_chunkvf.hpp"
#include "swiss_table_chunkv.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <memory>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

static inline uint64_t splitmix64(uint64_t x){
    x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;
    x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31);
}
static inline uint64_t value_of(uint64_t k){ return k*2654435761ULL + 1; }
using Clock = std::chrono::steady_clock;

// --- perf_event_open minimal (instructions + cycles), best-effort ---
struct PerfPair {
    int fd_i = -1, fd_c = -1;
    PerfPair(){
        perf_event_attr a{}; a.size=sizeof(a); a.disabled=1; a.exclude_kernel=1; a.exclude_hv=1;
        a.type=PERF_TYPE_HARDWARE; a.config=PERF_COUNT_HW_INSTRUCTIONS;
        fd_i=(int)syscall(SYS_perf_event_open,&a,0,-1,-1,0);
        a.config=PERF_COUNT_HW_CPU_CYCLES;
        fd_c=(int)syscall(SYS_perf_event_open,&a,0,-1,-1,0);
    }
    void start(){ if(fd_i>=0){ioctl(fd_i,PERF_EVENT_IOC_RESET,0);ioctl(fd_i,PERF_EVENT_IOC_ENABLE,0);}
                  if(fd_c>=0){ioctl(fd_c,PERF_EVENT_IOC_RESET,0);ioctl(fd_c,PERF_EVENT_IOC_ENABLE,0);} }
    void stop(uint64_t& inst, uint64_t& cyc){
        inst=cyc=0;
        if(fd_i>=0){ioctl(fd_i,PERF_EVENT_IOC_DISABLE,0); if(read(fd_i,&inst,8)!=8)inst=0;}
        if(fd_c>=0){ioctl(fd_c,PERF_EVENT_IOC_DISABLE,0); if(read(fd_c,&cyc,8)!=8)cyc=0;}
    }
    ~PerfPair(){ if(fd_i>=0)close(fd_i); if(fd_c>=0)close(fd_c); }
};

template<class F> static void med_ci(size_t w, F f, double& med, double& lo, double& hi,
                                     double& ipq, double& cpq){
    const int R=15; double r[R];
    { volatile uint64_t s=f(); (void)s; }
    for(int i=0;i<R;i++){ auto a=Clock::now(); volatile uint64_t s=f(); (void)s;
        auto b=Clock::now(); r[i]=w/std::chrono::duration<double>(b-a).count()/1e6; }
    PerfPair pp; uint64_t inst=0,cyc=0; const int RP=5;
    pp.start(); for(int i=0;i<RP;i++){ volatile uint64_t s=f(); (void)s; } pp.stop(inst,cyc);
    ipq = inst ? (double)inst/(RP*w) : 0.0; cpq = cyc ? (double)cyc/(RP*w) : 0.0;
    std::sort(r,r+R); med=r[R/2]; lo=r[3]; hi=r[11];
}

int main(int argc, char** argv){
    const int LOG2N = argc>1 ? atoi(argv[1]) : 27;
    const size_t N = 1ull<<LOG2N, Q = 2'000'000;
    const char* MACHINE = getenv("MACHINE") ? getenv("MACHINE") : "local";
    const double LOADS[] = {0.35, 0.50, 0.70};   // 0.70 = régime du papier (load réel 2/3)

    std::vector<uint64_t> keys(N); for(size_t i=0;i<N;i++) keys[i]=splitmix64(i);
    std::mt19937_64 rng(123);
    std::uniform_int_distribution<size_t>  pk(0,N-1);
    std::uniform_int_distribution<uint64_t> pm(N,4*N);
    std::vector<uint64_t> qhit(Q),qmiss(Q);
    for(size_t i=0;i<Q;i++){ qhit[i]=keys[pk(rng)]; qmiss[i]=splitmix64(pm(rng)); }
    std::vector<uint64_t> ov(Q,0);
    auto sum=[&]{ uint64_t s=0; for(auto x:ov)s+=x; return s; };

    std::printf("machine,log2n,load,workload,variant,mops,ci_lo,ci_hi,inst_per_q,cyc_per_q\n");
    for (double LOAD : LOADS){
        // une table à la fois (leçon iso) : VF puis V, mêmes clés, capacité N/LOAD
        for (int which=0; which<2; ++which){
            const char* name = which==0 ? "vf_v0" : "Inline";
            std::unique_ptr<SwissTableChunkVF<uint64_t,uint64_t>> vf;
            std::unique_ptr<SwissTableChunkV <uint64_t,uint64_t>> vi;
            if (which==0){ vf=std::make_unique<SwissTableChunkVF<uint64_t,uint64_t>>(size_t(N/LOAD)+1);
                           for(size_t i=0;i<N;i++) vf->insert(keys[i],value_of(keys[i])); }
            else         { vi=std::make_unique<SwissTableChunkV <uint64_t,uint64_t>>(size_t(N/LOAD)+1);
                           for(size_t i=0;i<N;i++) vi->insert(keys[i],value_of(keys[i])); }
            struct { const char* wl; std::vector<uint64_t>* q; } WL[]={{"hit",&qhit},{"miss",&qmiss}};
            for(auto& w : WL){
                double m,lo,hi,ipq,cpq;
                auto fb=[&]{ if(which==0) vf->find_batch<32>(w.q->data(),Q,ov.data());
                             else         vi->find_batch<32>(w.q->data(),Q,ov.data());
                             return sum(); };
                med_ci(Q,fb,m,lo,hi,ipq,cpq);
                std::printf("%s,%d,%.2f,%s,%s,%.1f,%.1f,%.1f,%.2f,%.2f\n",
                            MACHINE,LOG2N,LOAD,w.wl,name,m,lo,hi,ipq,cpq);
                std::fflush(stdout);
            }
        }
    }
    return 0;
}
