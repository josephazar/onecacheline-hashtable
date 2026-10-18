// Measured practical memory bandwidth B_m -- the one roofline parameter the paper
// did not measure. Sequential streaming read (one 8-byte load per 64-byte line,
// hardware prefetcher active), far beyond LLC:
//   - "1core"  : one pinned thread over a 2 GiB private buffer  -> single-thread B_m
//   - "allcore": every hardware thread over a private, first-touched 64 MiB slice
//                (numactl --localalloc => local pages)           -> node B_m
//   CSV: machine,mode,threads,gbs (median of R passes; allcore = aggregate)
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <random>
#include <thread>
#include <vector>
#include <algorithm>

using Clock = std::chrono::steady_clock;
static void pin(int cpu){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu,&s);
    pthread_setaffinity_np(pthread_self(),sizeof(s),&s); }

// touch every 64-byte line of buf[0..n) once (4 lines per iteration)
static uint64_t sweep(const uint64_t* buf, size_t n){
    uint64_t s0=0,s1=0,s2=0,s3=0; size_t i=0;
    for(; i+32<=n; i+=32){ s0+=buf[i]; s1+=buf[i+8]; s2+=buf[i+16]; s3+=buf[i+24]; }
    for(; i<n; i+=8) s0+=buf[i];
    return s0+s1+s2+s3;
}

static double med(std::vector<double> v){ std::sort(v.begin(),v.end()); return v[v.size()/2]; }

int main(){
    const char* MACHINE = getenv("MACHINE") ? getenv("MACHINE") : "unknown";
    printf("machine,mode,threads,gbs\n");

    // ---- single core: 2 GiB buffer, median of 11 passes ----
    {
        pin(4);
        const size_t n = (2ull<<30)/8;                       // 2 GiB of u64
        std::vector<uint64_t> buf(n);
        for(size_t i=0;i<n;i+=512) buf[i]=i;                 // first touch (page granularity)
        volatile uint64_t warm = sweep(buf.data(),n); (void)warm;
        std::vector<double> r;
        for(int k=0;k<11;k++){
            auto a=Clock::now(); volatile uint64_t s=sweep(buf.data(),n); (void)s;
            auto b=Clock::now();
            r.push_back((double)(n*8)/std::chrono::duration<double>(b-a).count()/1e9);
        }
        printf("%s,1core,1,%.1f\n",MACHINE,med(r)); fflush(stdout);
    }

    // ---- all hardware threads: private 64 MiB slice each, median of 5 ----
    {
        const int T = (int)std::thread::hardware_concurrency();
        const size_t n = (64ull<<20)/8;                      // 64 MiB of u64 per thread
        std::vector<std::vector<uint64_t>> bufs(T);
        std::vector<double> agg;
        const int PASSES = 8;
        for(int rep=0; rep<5; rep++){
            std::atomic<int> ready{0}; std::atomic<bool> go{false};
            std::vector<std::thread> th;
            for(int t=0;t<T;t++) th.emplace_back([&,t]{
                pin(t);
                if(bufs[t].empty()){                          // first touch by owner => local pages
                    bufs[t].resize(n);
                    for(size_t i=0;i<n;i+=512) bufs[t][i]=i;
                }
                volatile uint64_t w = sweep(bufs[t].data(),n); (void)w;
                ready.fetch_add(1);
                while(!go.load(std::memory_order_acquire)) {}
                uint64_t s=0; for(int p=0;p<PASSES;p++) s+=sweep(bufs[t].data(),n);
                volatile uint64_t sink=s; (void)sink;
            });
            while(ready.load()<T) {}
            auto a=Clock::now(); go.store(true,std::memory_order_release);
            for(auto& x:th) x.join();
            auto b=Clock::now();
            double bytes=(double)T*PASSES*n*8;
            agg.push_back(bytes/std::chrono::duration<double>(b-a).count()/1e9);
        }
        printf("%s,allcore,%d,%.1f\n",MACHINE,T,med(agg)); fflush(stdout);
    }

    // ---- all threads, RANDOM line order: the byte rate a random-access miss
    //      stream can actually sustain (the relevant node ceiling for the
    //      many-thread negative-lookup wall). Each thread walks its private
    //      64 MiB slice in a shuffled line permutation (uint32 index array,
    //      streamed sequentially: ~6% extra traffic, not counted). ----
    {
        const int T = (int)std::thread::hardware_concurrency();
        const size_t n = (64ull<<20)/8;                      // u64 per slice
        const size_t nl = n/8;                               // 64B lines per slice
        std::vector<std::vector<uint64_t>> bufs(T);
        std::vector<std::vector<uint32_t>> perms(T);
        std::vector<double> agg;
        const int PASSES = 4;
        for(int rep=0; rep<5; rep++){
            std::atomic<int> ready{0}; std::atomic<bool> go{false};
            std::vector<std::thread> th;
            for(int t=0;t<T;t++) th.emplace_back([&,t]{
                pin(t);
                if(bufs[t].empty()){
                    bufs[t].resize(n);
                    for(size_t i=0;i<n;i+=512) bufs[t][i]=i;
                    perms[t].resize(nl);
                    for(size_t i=0;i<nl;i++) perms[t][i]=(uint32_t)i;
                    std::mt19937 rng(0x5eed+t);
                    std::shuffle(perms[t].begin(),perms[t].end(),rng);
                }
                const uint64_t* b=bufs[t].data(); const uint32_t* p=perms[t].data();
                uint64_t w=0; for(size_t i=0;i<nl;i++) w+=b[(size_t)p[i]*8];
                volatile uint64_t sink0=w; (void)sink0;
                ready.fetch_add(1);
                while(!go.load(std::memory_order_acquire)) {}
                uint64_t s=0;
                for(int q=0;q<PASSES;q++) for(size_t i=0;i<nl;i++) s+=b[(size_t)p[i]*8];
                volatile uint64_t sink=s; (void)sink;
            });
            while(ready.load()<T) {}
            auto a=Clock::now(); go.store(true,std::memory_order_release);
            for(auto& x:th) x.join();
            auto b=Clock::now();
            double bytes=(double)T*PASSES*nl*64;
            agg.push_back(bytes/std::chrono::duration<double>(b-a).count()/1e9);
        }
        printf("%s,allrand,%d,%.1f\n",MACHINE,T,med(agg)); fflush(stdout);
    }
    return 0;
}
