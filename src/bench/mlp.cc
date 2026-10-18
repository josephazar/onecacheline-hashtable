// #5 independent measurement of memory-level parallelism N_M (MSHR/line-fill count)
// and DRAM latency L_m, to close the roofline's circularity (N_M was inferred from
// the depth-sweep knee). Classic K-chain pointer-chase: one random Sattolo cycle over
// a DRAM-resident array; K independent runners chased in lockstep. Aggregate hop rate
// rises with K until the core saturates its outstanding-miss slots -> that plateau,
// divided by the single-chain rate, is the sustained N_M. L_m = single-chain hop time.
#include <cstdint>
#include <cstdio>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>

using Clock = std::chrono::steady_clock;

int main(){
    const size_t M = 1ull<<27;                 // 128M elts * 8B = 1 GiB, deep in DRAM
    std::vector<uint64_t> A(M);
    for(size_t i=0;i<M;i++) A[i]=i;
    // Sattolo's algorithm -> a single cycle of length M (defeats prefetchers)
    std::mt19937_64 rng(12345);
    for(size_t i=M-1;i>0;i--){ std::uniform_int_distribution<size_t> d(0,i-1);
        std::swap(A[i],A[d(rng)]); }

    const int Ks[] = {1,2,3,4,6,8,12,16,24,32,48,64};
    const size_t LOADS = 12'000'000;           // ~constant work per K
    std::printf("K,Mhops_per_s\n");
    double base = 0;
    double peak = 0;
    for(int K : Ks){
        std::vector<size_t> r(K);
        for(int k=0;k<K;k++) r[k] = (size_t)((double)k/K * M);   // spread starts
        size_t T = LOADS / K; if(T<1) T=1;
        double best = 0;
        for(int rep=0; rep<5; ++rep){
            auto a=Clock::now();
            for(size_t t=0;t<T;t++)
                for(int k=0;k<K;k++) r[k]=A[r[k]];
            auto b=Clock::now();
            volatile size_t sink=0; for(int k=0;k<K;k++) sink+=r[k]; (void)sink;
            double s=std::chrono::duration<double>(b-a).count();
            double mh = (double)K*T/s/1e6;
            if(mh>best) best=mh;
        }
        std::printf("%d,%.2f\n",K,best);
        std::fflush(stdout);
        if(K==1) base=best;
        if(best>peak) peak=best;
    }
    // L_m from single chain; N_M = saturated / single-chain hop rate
    std::printf("# L_m_ns=%.1f  N_M=%.1f  peak_Mhops=%.1f\n",
                1000.0/base, peak/base, peak);
    return 0;
}
