// ablation2.cc - the §6.2 ablation, version 2:
//   * matched lookup contract: every AMAC cell materialises the value during the lookup (staged_v engines);
//     the original pointer-returning cells (SoA-AMAC, Chunk-AMAC) are kept, labelled "-ptr", with their
//     deferred value pass, so the effect of the contract itself is visible;
//   * paired design: within each resident table, B blocks; each block draws a fresh hit and miss stream and
//     shuffles all cells; equal (block) indices are genuine pairs;
//   * per-key validation of every cell on its warm-up pass against expected values (no checksums).
// Build twice: default (paper hashes) and -DSTRONG_HASH=1.  SoA cells need AVX2 (x86).
//   CSV: cell,workload,log2n,block,order,mops
#include "swiss_table_chunk.hpp"       // patched copy: adds find_batch_staged_v
#include "swiss_table_chunkv.hpp"      // patched copy: adds find_coro
#ifdef __AVX2__
#include "swiss_table_h1h2.hpp"        // patched copy: adds find_batch_staged / find_batch_staged_v
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
struct Cell { std::string name; std::function<void(const std::vector<uint64_t>&, std::vector<uint64_t>&)> run; };

static int LOG2N;
static void measure_table(const std::vector<Cell>& cells, size_t N, size_t Q, int blocks, uint64_t seed) {
    std::mt19937_64 rng(seed); std::vector<uint64_t> out(Q);
    for (int b = 0; b < blocks; b++) {
        std::vector<uint64_t> qhit(Q), qmiss(Q);
        for (size_t i = 0; i < Q; i++) { qhit[i] = splitmix64(rng() & (N - 1)); qmiss[i] = splitmix64(N + rng() % (3 * N)); }
        std::vector<int> order(2 * cells.size()); for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
        std::shuffle(order.begin(), order.end(), rng);
        for (size_t pos = 0; pos < order.size(); pos++) {
            const Cell& c = cells[order[pos] / 2]; bool miss = order[pos] % 2; auto& q = miss ? qmiss : qhit;
            c.run(q, out);                                                   // warm-up pass, validated per key
            for (size_t i = 0; i < Q; i++) { uint64_t exp = miss ? 0 : value_of(q[i]); if (out[i] != exp) { std::fprintf(stderr, "VALIDATION FAILED: %s %s at %zu\n", c.name.c_str(), miss ? "miss" : "hit", i); std::exit(1); } }
            auto t0 = Clock::now(); c.run(q, out); auto t1 = Clock::now();
            std::printf("%s,%s,%d,%d,%zu,%.2f\n", c.name.c_str(), miss ? "miss" : "hit", LOG2N, b, pos, Q / std::chrono::duration<double>(t1 - t0).count() / 1e6);
        }
        std::fflush(stdout);
    }
}

int main(int argc, char** argv){
    LOG2N = argc > 1 ? atoi(argv[1]) : 27;
    const size_t N = 1ull << LOG2N, Q = argc > 2 ? strtoull(argv[2], nullptr, 10) : 2'000'000; const int blocks = argc > 3 ? atoi(argv[3]) : 15;
    std::printf("cell,workload,log2n,block,order,mops\n");
    std::vector<uint64_t> keys(N); for (size_t i = 0; i < N; i++) keys[i] = splitmix64(i);
    std::vector<uint64_t*> op(Q, nullptr);
    auto ptr_to_val = [&](std::vector<uint64_t>& out) { for (size_t i = 0; i < out.size(); i++) out[i] = op[i] ? *op[i] : 0; };

#ifdef __AVX2__
    { auto soa = std::make_unique<SwissTableH1H2<uint64_t,uint64_t>>(size_t(N/0.7)+1);
      for (size_t i = 0; i < N; i++) soa->insert(keys[i], value_of(keys[i]));
      measure_table({
        {"SoA-sync",          [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto* p = soa->find_sync(q[i]); o[i] = p ? *p : 0; } }},
        {"SoA-AMAC-ptr",      [&](auto& q, auto& o){ soa->find_batch<32>(q.data(), q.size(), op.data()); ptr_to_val(o); }},
        {"SoA-AMAC-staged-v", [&](auto& q, auto& o){ soa->find_batch_staged_v<32>(q.data(), q.size(), o.data()); }},
      }, N, Q, blocks, 1001); }
#endif
    { auto chk = std::make_unique<SwissTableChunk<uint64_t,uint64_t>>(size_t(N/0.7)+1);
      for (size_t i = 0; i < N; i++) chk->insert(keys[i], value_of(keys[i]));
      measure_table({
        {"Chunk-sync",          [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto* p = chk->find_sync(q[i]); o[i] = p ? *p : 0; } }},
        {"Chunk-AMAC-ptr",      [&](auto& q, auto& o){ chk->find_batch<32>(q.data(), q.size(), op.data()); ptr_to_val(o); }},
        {"Chunk-AMAC-staged-v", [&](auto& q, auto& o){ chk->find_batch_staged_v<32>(q.data(), q.size(), o.data()); }},
      }, N, Q, blocks, 1002); }
    { auto inl = std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1);
      for (size_t i = 0; i < N; i++) inl->insert(keys[i], value_of(keys[i]));
      measure_table({
        {"Inline-sync", [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto* p = inl->find_sync(q[i]); o[i] = p ? *p : 0; } }},
        {"Inline-AMAC", [&](auto& q, auto& o){ inl->find_batch<32>(q.data(), q.size(), o.data()); }},
        {"Inline-coro", [&](auto& q, auto& o){ inl->find_coro<32>(q.data(), q.size(), o.data()); }},
      }, N, Q, blocks, 1003); }
    return 0;
}
