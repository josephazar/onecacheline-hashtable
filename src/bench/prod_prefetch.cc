// prod_prefetch.cc - prefetch-capable PRODUCTION maps as comparators.
// readers ask whether an application-side prefetch loop over Abseil or F14 captures the batched gain,
// since both maps expose a hook for it: Abseil flat_hash_map::prefetch(key) (control + slot lines) and
// F14ValueMap::prehash / prefetch(token) / find(token, key).  Cells, each a plain loop over a query array:
//   abseil-sync, abseil-prefetch{8,16,32,64}   prefetch(q[i+k]) k lookups ahead, then find(q[i])
//   f14-sync, f14-token{8,16,32,64}            prehash+prefetch k ahead in a token ring, then find(token, key)
//   boost-sync, ankerl-sync                    scalar reference points (Boost has no bulk API on unordered_flat_map)
//   Inline-sync, Inline-AMAC                   our layout with the synchronous probe and with the ring at D=32
//   InlineF-sync, InlineF-AMAC                 Inline+F (fixed filter), the layout the paper recommends for mixed traffic
// Paired design as ablation2: B blocks per resident set of tables, fresh hit and miss streams per block, every cell
// order-shuffled within a block, per-key validation on the warm-up pass.  Build from INSIDE the header directory
// (orig/pf2/pf3) so the quote-include picks the intended hint; F14 lives in f14_pf.cc compiled at the baseline ISA.
//   CSV: cell,workload,log2n,block,order,mops
#include "swiss_table_chunkv.hpp"
#ifndef FILTER_MODE
#define FILTER_MODE 2                      // Inline+F with the fixed bit positions (Section: aligned keys)
#endif
#include "swiss_table_chunkvf.hpp"
#include <absl/container/flat_hash_map.h>
#include <boost/unordered/unordered_flat_map.hpp>
#if __has_include(<ankerl/unordered_dense.h>)
#include <ankerl/unordered_dense.h>
#define HAVE_ANKERL 1
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <functional>
#include <string>
#include <memory>

#ifndef NO_F14
extern "C" void     f14_build(const uint64_t* keys, const uint64_t* vals, size_t n);
extern "C" void     f14_probe_sync(const uint64_t* q, size_t n, uint64_t* out);
extern "C" void     f14_probe_token(const uint64_t* q, size_t n, uint64_t* out, int k);
extern "C" void     f14_free();
#endif

static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
static inline uint64_t value_of(uint64_t k){ return k*2654435761ULL + 1; }
using Clock = std::chrono::steady_clock;
struct Cell { std::string name; std::function<void(const std::vector<uint64_t>&, std::vector<uint64_t>&)> run; };

int main(int argc, char** argv){
    const int LOG2N = argc > 1 ? atoi(argv[1]) : 27; const size_t N = 1ull << LOG2N, Q = argc > 2 ? strtoull(argv[2], nullptr, 10) : 2'000'000; const int blocks = argc > 3 ? atoi(argv[3]) : 9;
    std::printf("cell,workload,log2n,block,order,mops\n");
    std::vector<uint64_t> keys(N), vals(N); for (size_t i = 0; i < N; i++) { keys[i] = splitmix64(i); vals[i] = value_of(keys[i]); }
    auto ab = std::make_unique<absl::flat_hash_map<uint64_t,uint64_t>>(); ab->reserve(N); for (size_t i = 0; i < N; i++) (*ab)[keys[i]] = vals[i];
    auto bo = std::make_unique<boost::unordered_flat_map<uint64_t,uint64_t>>(); bo->reserve(N); for (size_t i = 0; i < N; i++) (*bo)[keys[i]] = vals[i];
#ifdef HAVE_ANKERL
    auto an = std::make_unique<ankerl::unordered_dense::map<uint64_t,uint64_t>>(); an->reserve(N); for (size_t i = 0; i < N; i++) (*an)[keys[i]] = vals[i];
#endif
#ifndef NO_F14
    f14_build(keys.data(), vals.data(), N);
#endif
    auto inl = std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) inl->insert(keys[i], vals[i]);
    auto inf = std::make_unique<SwissTableChunkVF<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) inf->insert(keys[i], vals[i]);
    std::fprintf(stderr, "tables built: N=2^%d\n", LOG2N);

    auto absl_pa = [&](int k) { return [&, k](const std::vector<uint64_t>& q, std::vector<uint64_t>& o) {
        size_t n = q.size(); for (size_t i = 0; i < n; i++) { if (i + k < n) ab->prefetch(q[i + k]); auto it = ab->find(q[i]); o[i] = it == ab->end() ? 0 : it->second; } }; };
    std::vector<Cell> cells = {
        {"abseil-sync",       [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto it = ab->find(q[i]); o[i] = it == ab->end() ? 0 : it->second; } }},
        {"abseil-prefetch8",  absl_pa(8)}, {"abseil-prefetch16", absl_pa(16)}, {"abseil-prefetch32", absl_pa(32)}, {"abseil-prefetch64", absl_pa(64)},
        {"boost-sync",        [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto it = bo->find(q[i]); o[i] = it == bo->end() ? 0 : it->second; } }},
#ifdef HAVE_ANKERL
        {"ankerl-sync",       [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto it = an->find(q[i]); o[i] = it == an->end() ? 0 : it->second; } }},
#endif
#ifndef NO_F14
        {"f14-sync",          [&](auto& q, auto& o){ f14_probe_sync(q.data(), q.size(), o.data()); }},
        {"f14-token8",        [&](auto& q, auto& o){ f14_probe_token(q.data(), q.size(), o.data(), 8); }},
        {"f14-token16",       [&](auto& q, auto& o){ f14_probe_token(q.data(), q.size(), o.data(), 16); }},
        {"f14-token32",       [&](auto& q, auto& o){ f14_probe_token(q.data(), q.size(), o.data(), 32); }},
        {"f14-token64",       [&](auto& q, auto& o){ f14_probe_token(q.data(), q.size(), o.data(), 64); }},
#endif
        {"Inline-sync",       [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto* p = inl->find_sync(q[i]); o[i] = p ? *p : 0; } }},
        {"Inline-AMAC",       [&](auto& q, auto& o){ inl->find_batch<32>(q.data(), q.size(), o.data()); }},
        {"InlineF-sync",      [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto* p = inf->find_sync(q[i]); o[i] = p ? *p : 0; } }},
        {"InlineF-AMAC",      [&](auto& q, auto& o){ inf->find_batch<32>(q.data(), q.size(), o.data()); }},
    };
    std::mt19937_64 rng(3003); std::vector<uint64_t> out(Q);
    for (int b = 0; b < blocks; b++) {
        std::vector<uint64_t> qhit(Q), qmiss(Q);
        for (size_t i = 0; i < Q; i++) { qhit[i] = splitmix64(rng() & (N - 1)); qmiss[i] = splitmix64(N + rng() % (3 * N)); }
        std::vector<int> order(2 * cells.size()); for (size_t i = 0; i < order.size(); i++) order[i] = (int)i; std::shuffle(order.begin(), order.end(), rng);
        for (size_t pos = 0; pos < order.size(); pos++) {
            const Cell& c = cells[order[pos] / 2]; bool miss = order[pos] % 2; auto& q = miss ? qmiss : qhit;
            c.run(q, out); for (size_t i = 0; i < Q; i++) if (out[i] != (miss ? 0 : value_of(q[i]))) { std::fprintf(stderr, "VALIDATION FAILED %s %s at %zu\n", c.name.c_str(), miss ? "miss" : "hit", i); return 1; }
            auto t0 = Clock::now(); c.run(q, out); auto t1 = Clock::now();
            std::printf("%s,%s,%d,%d,%zu,%.2f\n", c.name.c_str(), miss ? "miss" : "hit", LOG2N, b, pos, Q / std::chrono::duration<double>(t1 - t0).count() / 1e6);
        }
        std::fflush(stdout);
    }
#ifndef NO_F14
    f14_free();
#endif
    return 0;
}
