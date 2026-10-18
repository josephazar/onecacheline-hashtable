// filter_cost.cc - what the filter fix costs and buys, in throughput, on the Inline+F table.
// Cells: Inline (no filter) and Inline+F, on random keys and on random 4 KB-aligned keys; hit and miss;
// find_batch<32>.  Build once per FILTER_MODE (0 original, 1 fmix remix, 2 one-multiply remix).
// One table resident at a time; 9 blocks with fresh streams and shuffled cells; per-key validation.
//   CSV: filter_mode,keys,table,workload,block,order,mops,avg_chunks_neg
#include "swiss_table_chunkvf.hpp"
#include "swiss_table_chunkv.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <memory>
#include <functional>
#include <string>
static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
static inline uint64_t value_of(uint64_t k){ return k*2654435761ULL + 1; }
using Clock = std::chrono::steady_clock;
int main(int argc, char** argv){
    const int LOG2N = argc > 1 ? atoi(argv[1]) : 27; const size_t N = 1ull << LOG2N, Q = 2'000'000; const int blocks = argc > 2 ? atoi(argv[2]) : 9;
    std::printf("filter_mode,keys,table,workload,block,order,mops,avg_chunks_neg\n");
    for (int aligned = 0; aligned < 2; aligned++) {
        auto key = [&](uint64_t i){ return aligned ? (splitmix64(i) << 12) : splitmix64(i); };   // aligned: multiples of 4096, still unique
        std::vector<uint64_t> keys(N); for (size_t i = 0; i < N; i++) keys[i] = key(i);
        for (int which = 0; which < 2; which++) {
            std::unique_ptr<SwissTableChunkVF<uint64_t,uint64_t>> vf; std::unique_ptr<SwissTableChunkV<uint64_t,uint64_t>> vi;
            if (which) { vf = std::make_unique<SwissTableChunkVF<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) vf->insert(keys[i], value_of(keys[i])); }
            else       { vi = std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1);  for (size_t i = 0; i < N; i++) vi->insert(keys[i], value_of(keys[i])); }
            auto run = [&](const std::vector<uint64_t>& q, std::vector<uint64_t>& o){ if (which) vf->find_batch<32>(q.data(), q.size(), o.data()); else vi->find_batch<32>(q.data(), q.size(), o.data()); };
            std::mt19937_64 rng(77 + aligned); std::vector<uint64_t> out(Q);
            for (int b = 0; b < blocks; b++) {
                std::vector<uint64_t> qhit(Q), qmiss(Q);
                for (size_t i = 0; i < Q; i++) { qhit[i] = keys[rng() & (N - 1)]; qmiss[i] = key(N + rng() % (3 * N)); }
                double neg_chunks = 0;                     // structural probe count for this block's absent keys (sampled)
                if (which) { size_t S = 100000; double s = 0; for (size_t i = 0; i < S; i++) { int f; s += vf->neg_chunks(qmiss[i], f); } neg_chunks = s / S; }
                else       { size_t S = 100000; double s = 0; for (size_t i = 0; i < S; i++) s += vi->probe_len(qmiss[i]); neg_chunks = s / S; }
                int order[2] = {0, 1}; if (rng() & 1) std::swap(order[0], order[1]);
                for (int pos = 0; pos < 2; pos++) {
                    bool miss = order[pos]; auto& q = miss ? qmiss : qhit;
                    run(q, out); for (size_t i = 0; i < Q; i++) if (out[i] != (miss ? 0 : value_of(q[i]))) { std::fprintf(stderr, "VALIDATION FAILED mode %d aligned %d table %d\n", FILTER_MODE, aligned, which); return 1; }
                    auto t0 = Clock::now(); run(q, out); auto t1 = Clock::now();
                    std::printf("%d,%s,%s,%s,%d,%d,%.2f,%.4f\n", FILTER_MODE, aligned ? "aligned4096" : "random", which ? "Inline+F" : "Inline", miss ? "miss" : "hit", b, pos, Q / std::chrono::duration<double>(t1 - t0).count() / 1e6, miss ? neg_chunks : 0.0);
                }
                std::fflush(stdout);
            }
        }
    }
    return 0;
}
