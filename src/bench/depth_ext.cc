// depth_ext.cc - extended AMAC depth sweep, Inline (v4) and Paired (v5), all repetitions logged.
// The September hint sweep moved Grace's knee past the old sweep's last point (D=96), so the model's
// knee argument needs a wider range.  CSV: layout,depth,rep,mops
#include "swiss_table_chunkv.hpp"
#include "swiss_table_chunk2.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <chrono>
using Clock = std::chrono::steady_clock;
static inline uint64_t splitmix64(uint64_t x){x+=0x9e3779b97f4a7c15ULL;x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;x=(x^(x>>27))*0x94d049bb133111ebULL;return x^(x>>31);}
static inline uint64_t value_of(uint64_t k){return k*2654435761ULL+1;}
static size_t Q; static std::vector<uint64_t> q, out;
template<class T, int D> static void run(const char* name, T& t, int reps){
    t.template find_batch<D>(q.data(), Q, out.data());                       // warm-up
    for (int r = 0; r < reps; r++) {
        auto a = Clock::now(); t.template find_batch<D>(q.data(), Q, out.data());
        uint64_t s = 0; for (auto x : out) s += x; volatile uint64_t sink = s; (void)sink;
        auto b = Clock::now();
        std::printf("%s,%d,%d,%.2f\n", name, D, r, Q / std::chrono::duration<double>(b - a).count() / 1e6);
    }
    std::fflush(stdout);
}
template<class T> static void sweep(const char* name, T& t, int reps){
    run<T,4>(name,t,reps);   run<T,8>(name,t,reps);   run<T,16>(name,t,reps);  run<T,24>(name,t,reps);
    run<T,32>(name,t,reps);  run<T,48>(name,t,reps);  run<T,64>(name,t,reps);  run<T,96>(name,t,reps);
    run<T,128>(name,t,reps); run<T,160>(name,t,reps); run<T,192>(name,t,reps); run<T,256>(name,t,reps);
    run<T,320>(name,t,reps); run<T,384>(name,t,reps);
}
int main(int argc, char** argv){
    const int LOG2N = argc > 1 ? atoi(argv[1]) : 27; const int reps = argc > 2 ? atoi(argv[2]) : 7;
    const size_t N = 1ull << LOG2N; Q = argc > 3 ? strtoull(argv[3], nullptr, 10) : 2'000'000;   // query-stream length: at Q=2M the touched lines (~128 MB) nearly fit Grace's 117 MB L3
    std::vector<uint64_t> keys(N); for (size_t i = 0; i < N; i++) keys[i] = splitmix64(i);
    std::mt19937_64 rng(123); std::uniform_int_distribution<size_t> pk(0, N - 1);
    q.resize(Q); for (auto& x : q) x = keys[pk(rng)]; out.assign(Q, 0);
    std::printf("layout,depth,rep,mops\n"); std::fprintf(stderr, "Q=%zu touched_MB=%.0f\n", Q, Q * 64.0 / 1e6);
    { SwissTableChunkV<uint64_t,uint64_t> t(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) t.insert(keys[i], value_of(keys[i])); sweep("Inline", t, reps); }
    { SwissTableChunk2<uint64_t,uint64_t> t(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) t.insert(keys[i], value_of(keys[i])); sweep("Paired", t, reps); }
    return 0;
}
