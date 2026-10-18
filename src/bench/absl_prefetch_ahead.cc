// absl_prefetch_ahead.cc - production baseline: Abseil flat_hash_map with its public
// prefetch(key) API called k lookups ahead in an otherwise ordinary find loop.  Answers "how much of the gain
// does an application-side change on a production map capture?"  hit and miss, 2^27 keys, blocks with fresh
// streams and shuffled cells, per-key validation.   CSV: cell,workload,log2n,block,order,mops
#include <absl/container/flat_hash_map.h>
#include <boost/unordered/unordered_flat_map.hpp>
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
static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
static inline uint64_t value_of(uint64_t k){ return k*2654435761ULL + 1; }
using Clock = std::chrono::steady_clock;
struct Cell { std::string name; std::function<void(const std::vector<uint64_t>&, std::vector<uint64_t>&)> run; };
int main(int argc, char** argv){
    const int LOG2N = argc > 1 ? atoi(argv[1]) : 27; const size_t N = 1ull << LOG2N, Q = 2'000'000; const int blocks = argc > 2 ? atoi(argv[2]) : 9;
    std::printf("cell,workload,log2n,block,order,mops\n");
    std::vector<uint64_t> keys(N); for (size_t i = 0; i < N; i++) keys[i] = splitmix64(i);
    auto m = std::make_unique<absl::flat_hash_map<uint64_t,uint64_t>>(); m->reserve(N); for (size_t i = 0; i < N; i++) (*m)[keys[i]] = value_of(keys[i]);
    auto bo = std::make_unique<boost::unordered_flat_map<uint64_t,uint64_t>>(); bo->reserve(N); for (size_t i = 0; i < N; i++) (*bo)[keys[i]] = value_of(keys[i]);
    auto absl_pa = [&](int k) { return [&, k](const std::vector<uint64_t>& q, std::vector<uint64_t>& o) {
        size_t n = q.size(); for (size_t i = 0; i < n; i++) { if (i + k < n) m->prefetch(q[i + k]); auto it = m->find(q[i]); o[i] = it == m->end() ? 0 : it->second; } }; };
    std::vector<Cell> cells = {
        {"abseil-sync",       [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto it = m->find(q[i]); o[i] = it == m->end() ? 0 : it->second; } }},
        {"abseil-prefetch8",  absl_pa(8)}, {"abseil-prefetch16", absl_pa(16)}, {"abseil-prefetch32", absl_pa(32)}, {"abseil-prefetch64", absl_pa(64)},
        {"boost-sync",        [&](auto& q, auto& o){ for (size_t i = 0; i < q.size(); i++) { auto it = bo->find(q[i]); o[i] = it == bo->end() ? 0 : it->second; } }},
    };
    std::mt19937_64 rng(2024); std::vector<uint64_t> out(Q);
    for (int b = 0; b < blocks; b++) {
        std::vector<uint64_t> qhit(Q), qmiss(Q);
        for (size_t i = 0; i < Q; i++) { qhit[i] = splitmix64(rng() & (N - 1)); qmiss[i] = splitmix64(N + rng() % (3 * N)); }
        std::vector<int> order(2 * cells.size()); for (size_t i = 0; i < order.size(); i++) order[i] = (int)i; std::shuffle(order.begin(), order.end(), rng);
        for (size_t pos = 0; pos < order.size(); pos++) {
            const Cell& c = cells[order[pos] / 2]; bool miss = order[pos] % 2; auto& q = miss ? qmiss : qhit;
            c.run(q, out); for (size_t i = 0; i < Q; i++) if (out[i] != (miss ? 0 : value_of(q[i]))) { std::fprintf(stderr, "VALIDATION FAILED %s\n", c.name.c_str()); return 1; }
            auto t0 = Clock::now(); c.run(q, out); auto t1 = Clock::now();
            std::printf("%s,%s,%d,%d,%zu,%.2f\n", c.name.c_str(), miss ? "miss" : "hit", LOG2N, b, pos, Q / std::chrono::duration<double>(t1 - t0).count() / 1e6);
        }
        std::fflush(stdout);
    }
    return 0;
}
