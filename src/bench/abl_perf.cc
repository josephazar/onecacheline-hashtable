// abl_perf.cc - the ablation cells under hardware counters .
// Answers two evaluation questions with counters instead of argument:
//   (1) why the staged engine returns 1.2x on the separate-array SwissTable (SoA) but 1.9-2.4x on Detached, which
//       also has a dependent second line: instructions and cycles per lookup of every cell show how much of the
//       gap is compute (T_cpu) and how much is memory;
//   (2) how many DRAM lines a positive Inline lookup really moves (the model uses C_hit = 1; the probe visits
//       1.24 chunks): LLC read misses per lookup on the synchronous cells are demand-miss line counts.
// Same tables, cells and per-key validation as ablation2.cc; R repetitions per cell (default 5), no pairing
// needed because counters are per lookup.  Works at any table size: run at log2n=16 (in cache) for T_cpu and at
// 27 (DRAM) for the line counts.  Counters are read with perf_event_open (perf_counters.hpp); if the testbed
// blocks them the columns are -1 and the mops column is still valid.
//   CSV: cell,workload,log2n,rep,mops,instr_per_lookup,cycles_per_lookup,l1d_miss_per_lookup,llc_miss_per_lookup,dtlb_miss_per_lookup
#include "swiss_table_chunk.hpp"
#include "swiss_table_chunkv.hpp"
#ifdef __AVX2__
#include "swiss_table_h1h2.hpp"
#endif
#include "perf_counters.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <chrono>
#include <functional>
#include <string>
#include <memory>

static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
static inline uint64_t value_of(uint64_t k){ return k*2654435761ULL + 1; }
using Clock = std::chrono::steady_clock;
struct Cell { std::string name; std::function<void(const std::vector<uint64_t>&, std::vector<uint64_t>&)> run; };
static int LOG2N;

static void measure(const std::vector<Cell>& cells, size_t N, size_t Q, int reps, uint64_t seed) {
    std::mt19937_64 rng(seed); std::vector<uint64_t> out(Q); PerfCounters pc;
    for (const Cell& c : cells) for (int miss = 0; miss < 2; miss++) {
        std::vector<uint64_t> q(Q); for (size_t i = 0; i < Q; i++) q[i] = miss ? splitmix64(N + rng() % (3 * N)) : splitmix64(rng() & (N - 1));
        c.run(q, out); for (size_t i = 0; i < Q; i++) if (out[i] != (miss ? 0 : value_of(q[i]))) { std::fprintf(stderr, "VALIDATION FAILED %s\n", c.name.c_str()); std::exit(1); }
        for (int r = 0; r < reps; r++) {
            pc.start(); auto t0 = Clock::now(); c.run(q, out); auto t1 = Clock::now(); pc.stop();
            double mops = Q / std::chrono::duration<double>(t1 - t0).count() / 1e6;
            std::printf("%s,%s,%d,%d,%.2f", c.name.c_str(), miss ? "miss" : "hit", LOG2N, r, mops);
            for (int i = 0; i < PerfCounters::NC; i++) { if (pc.val[i] < 0) std::printf(",-1"); else std::printf(",%.3f", (double)pc.val[i] / Q); }
            std::printf("\n");
        }
        std::fflush(stdout);
    }
}

int main(int argc, char** argv){
    LOG2N = argc > 1 ? atoi(argv[1]) : 27; const size_t N = 1ull << LOG2N, Q = argc > 2 ? strtoull(argv[2], nullptr, 10) : 2'000'000; const int reps = argc > 3 ? atoi(argv[3]) : 5;
    std::printf("cell,workload,log2n,rep,mops,instr_per_lookup,cycles_per_lookup,l1d_miss_per_lookup,llc_miss_per_lookup,dtlb_miss_per_lookup\n");
    { PerfCounters probe; std::fprintf(stderr, "hardware counters: %s\n", probe.any() ? "available" : "NOT available (columns will be -1)"); }
    std::vector<uint64_t> keys(N); for (size_t i = 0; i < N; i++) keys[i] = splitmix64(i);
#ifdef __AVX2__
    { auto soa = std::make_unique<SwissTableH1H2<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) soa->insert(keys[i], value_of(keys[i]));
      measure({{"SoA-sync",          [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto* p = soa->find_sync(q[i]); o[i] = p ? *p : 0; } }},
               {"SoA-AMAC-staged-v", [&](auto& q, auto& o){ soa->find_batch_staged_v<32>(q.data(), q.size(), o.data()); }}}, N, Q, reps, 2001); }
#endif
    { auto chk = std::make_unique<SwissTableChunk<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) chk->insert(keys[i], value_of(keys[i]));
      measure({{"Chunk-sync",          [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto* p = chk->find_sync(q[i]); o[i] = p ? *p : 0; } }},
               {"Chunk-AMAC-staged-v", [&](auto& q, auto& o){ chk->find_batch_staged_v<32>(q.data(), q.size(), o.data()); }}}, N, Q, reps, 2002); }
    { auto inl = std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) inl->insert(keys[i], value_of(keys[i]));
      measure({{"Inline-sync", [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto* p = inl->find_sync(q[i]); o[i] = p ? *p : 0; } }},
               {"Inline-AMAC", [&](auto& q, auto& o){ inl->find_batch<32>(q.data(), q.size(), o.data()); }}}, N, Q, reps, 2003); }
    return 0;
}
