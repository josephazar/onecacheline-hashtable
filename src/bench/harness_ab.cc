// harness_ab.cc - why do the scalar production maps measure 8-19% lower in prod_prefetch
// (Table "production prefetch", Sept. 24) than in the main and isolated harnesses (July / Sept. 23)?
// The two harnesses differ in exactly three ways, which this driver crosses on ONE host in ONE process run:
//   residency  iso: build one map, measure it, free it (main/iso harness)   co: every map resident at once (prod_prefetch)
//   loop       sum: s += it->second (main/iso harness)                      store: o[i] = value into a Q-entry array (prod_prefetch)
//   streams    both use fresh uniform hit/miss streams per block; the generators are those of prod_prefetch here.
// Maps: Boost unordered_flat_map, Abseil flat_hash_map, ankerl::unordered_dense (all reserve(N)), and our Inline table
// with the ring at D=32 (it always writes an output array; its "sum" cell sums that array afterwards, outside timing).
// Paired blocks: every block draws fresh streams and runs every (map, loop, workload) cell in shuffled order.
//   ./harness_ab LOG2N Q BLOCKS            CSV: residency,map,loop,workload,block,order,mops
#include "swiss_table_chunkv.hpp"
#include <absl/container/flat_hash_map.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>

static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
static inline uint64_t value_of(uint64_t k){ return k*2654435761ULL + 1; }
using Clock = std::chrono::steady_clock;
using Q = std::vector<uint64_t>;
struct Map { std::string name; std::function<void()> build, drop; std::function<uint64_t(const Q&)> sum; std::function<void(const Q&, Q&)> store; };
static volatile uint64_t sink;

int main(int argc, char** argv){
    const int LOG2N = argc > 1 ? atoi(argv[1]) : 27; const size_t N = 1ull << LOG2N, NQ = argc > 2 ? strtoull(argv[2], nullptr, 10) : 2'000'000;
    const int blocks = argc > 3 ? atoi(argv[3]) : 9;
    std::printf("residency,map,loop,workload,block,order,mops\n");
    std::vector<uint64_t> keys(N); for (size_t i = 0; i < N; i++) keys[i] = splitmix64(i);
    std::unique_ptr<absl::flat_hash_map<uint64_t,uint64_t>> ab; std::unique_ptr<boost::unordered_flat_map<uint64_t,uint64_t>> bo;
    std::unique_ptr<ankerl::unordered_dense::map<uint64_t,uint64_t>> an; std::unique_ptr<SwissTableChunkV<uint64_t,uint64_t>> inl;
    auto scalar = [](auto& m) {   // the two loop shapes over any map with find()/end()
        return std::make_pair(std::function<uint64_t(const Q&)>([&m](const Q& q){ uint64_t s = 0; for (uint64_t x : q) { auto it = m->find(x); if (it != m->end()) s += it->second; } return s; }),
                              std::function<void(const Q&, Q&)>([&m](const Q& q, Q& o){ for (size_t i = 0; i < q.size(); i++) { auto it = m->find(q[i]); o[i] = it == m->end() ? 0 : it->second; } })); };
    auto [ab_sum, ab_store] = scalar(ab); auto [bo_sum, bo_store] = scalar(bo); auto [an_sum, an_store] = scalar(an);
    Q tmp(NQ);
    std::vector<Map> maps = {
        {"boost",  [&]{ bo = std::make_unique<boost::unordered_flat_map<uint64_t,uint64_t>>(); bo->reserve(N); for (auto k : keys) (*bo)[k] = value_of(k); }, [&]{ bo.reset(); }, bo_sum, bo_store},
        {"abseil", [&]{ ab = std::make_unique<absl::flat_hash_map<uint64_t,uint64_t>>(); ab->reserve(N); for (auto k : keys) (*ab)[k] = value_of(k); }, [&]{ ab.reset(); }, ab_sum, ab_store},
        {"ankerl", [&]{ an = std::make_unique<ankerl::unordered_dense::map<uint64_t,uint64_t>>(); an->reserve(N); for (auto k : keys) (*an)[k] = value_of(k); }, [&]{ an.reset(); }, an_sum, an_store},
        {"Inline-AMAC", [&]{ inl = std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (auto k : keys) inl->insert(k, value_of(k)); }, [&]{ inl.reset(); },
                        [&](const Q& q){ inl->find_batch<32>(q.data(), q.size(), tmp.data()); uint64_t s = 0; for (auto x : tmp) s += x; return s; },
                        [&](const Q& q, Q& o){ inl->find_batch<32>(q.data(), q.size(), o.data()); }}};
    std::mt19937_64 rng(3003); Q out(NQ);
    auto measure = [&](const char* res, std::vector<Map*> ms, int b) {
        Q qhit(NQ), qmiss(NQ);
        for (size_t i = 0; i < NQ; i++) { qhit[i] = splitmix64(rng() & (N - 1)); qmiss[i] = splitmix64(N + rng() % (3 * N)); }
        uint64_t ref_hit = 0; for (auto x : qhit) ref_hit += value_of(x);
        std::vector<int> order(ms.size() * 4); for (size_t i = 0; i < order.size(); i++) order[i] = (int)i; std::shuffle(order.begin(), order.end(), rng);
        for (size_t pos = 0; pos < order.size(); pos++) {
            Map& m = *ms[order[pos] / 4]; bool store = (order[pos] / 2) % 2, miss = order[pos] % 2; const Q& q = miss ? qmiss : qhit;
            uint64_t want = miss ? 0 : ref_hit, got;   // validation pass, then the timed pass
            if (store) { m.store(q, out); got = 0; for (auto x : out) got += x; } else got = m.sum(q);
            if (got != want) { std::fprintf(stderr, "VALIDATION FAILED %s\n", m.name.c_str()); std::exit(1); }
            auto t0 = Clock::now(); if (store) m.store(q, out); else sink = m.sum(q); auto t1 = Clock::now();
            std::printf("%s,%s,%s,%s,%d,%zu,%.2f\n", res, m.name.c_str(), store ? "store" : "sum", miss ? "miss" : "hit", b, pos, NQ / std::chrono::duration<double>(t1 - t0).count() / 1e6);
        }
        std::fflush(stdout);
    };
    // 1. isolated: build -> measure every block -> free, one map at a time (main / iso harness)
    for (auto& m : maps) { m.build(); for (int b = 0; b < blocks; b++) measure("iso", {&m}, b); m.drop(); }
    // 2. co-resident: every map built, then every block interleaves all of them (prod_prefetch)
    for (auto& m : maps) m.build();
    std::fprintf(stderr, "co-resident: all maps built, N=2^%d\n", LOG2N);
    for (int b = 0; b < blocks; b++) { std::vector<Map*> all; for (auto& m : maps) all.push_back(&m); measure("co", all, b); }
    return 0;
}
