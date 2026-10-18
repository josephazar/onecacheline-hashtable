// lat2.cc - point latency measured three ways, so the paper can name each correctly:
//   (a) scalar stream ns/op: a loop of synchronous finds over independent keys (lat.cc's "point" rows;
//       the out-of-order core overlaps consecutive lookups, so this is reciprocal throughput);
//   (b) serialised dependent chain: the table stores value(key_i) = key_{next(i)} along one random
//       cycle and the loop does k = find(k), so lookup i+1 cannot start before lookup i returns - //       this is the latency one isolated request sees;
//   (c) batched amortised ns/op of the AMAC engine vs batch size (lat.cc's "batch" rows).
// Inline (v4) vs Abseil vs Boost.  CSV: kind,label,rep,ns_per_lookup
#include "swiss_table_chunkv.hpp"
#include <absl/container/flat_hash_map.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <chrono>
#include <memory>
#include <algorithm>
#include <functional>

static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
using Clock = std::chrono::steady_clock;
static constexpr uint64_t A = 6364136223846793005ULL, C = 1442695040888963407ULL;   // full-period LCG: one random cycle over [0,N)
static double ns_of(double t0, size_t n){ return (std::chrono::duration<double,std::nano>(Clock::now().time_since_epoch()).count() - t0) / n; }
static double now(){ return std::chrono::duration<double,std::nano>(Clock::now().time_since_epoch()).count(); }

template<class Find, class Batch>
static void measure(const char* label, const std::vector<uint64_t>& qhit, size_t N, Find find, Batch batch){
    const int R = 5; const size_t M = 1'000'000;
    for (int rep = 0; rep < R; rep++) {
        double t0 = now(); uint64_t s = 0; for (auto x : qhit) s += find(x); volatile uint64_t sink = s; (void)sink;
        std::printf("scalar-stream,%s,%d,%.2f\n", label, rep, ns_of(t0, qhit.size()));
        uint64_t k = splitmix64((size_t)(7 + rep) & (N - 1)); t0 = now();
        for (size_t i = 0; i < M; i++) k = find(k);                       // dependent chain
        sink = k; std::printf("serialized-chain,%s,%d,%.2f\n", label, rep, ns_of(t0, M));
        if (batch) for (int B : {1, 8, 32, 256}) { t0 = now(); batch(B); std::printf("batched-B%d,%s,%d,%.2f\n", B, label, rep, ns_of(t0, qhit.size())); }
    }
    std::fflush(stdout);
}

int main(int argc, char** argv){
    const int LOG2N = argc > 1 ? atoi(argv[1]) : 27; const size_t N = 1ull << LOG2N, Q = 2'000'000;
    std::printf("kind,label,rep,ns_per_lookup\n");
    std::vector<uint64_t> keys(N); for (size_t i = 0; i < N; i++) keys[i] = splitmix64(i);
    std::mt19937_64 rng(123); std::uniform_int_distribution<size_t> pk(0, N - 1);
    std::vector<uint64_t> qhit(Q); for (size_t i = 0; i < Q; i++) qhit[i] = keys[pk(rng)];
    std::vector<uint64_t> ov(Q);
    auto chain_val = [&](size_t i){ return splitmix64((A * i + C) & (N - 1)); };     // value = next key on the cycle
    { auto t = std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1);
      for (size_t i = 0; i < N; i++) t->insert(keys[i], chain_val(i));
      measure("Inline", qhit, N, [&](uint64_t k){ auto* p = t->find_sync(k); return p ? *p : 0ULL; },
              std::function<void(int)>([&](int B){ for (size_t i = 0; i < Q; i += B) t->find_batch<32>(qhit.data() + i, std::min((size_t)B, Q - i), ov.data() + i); })); }
    { auto t = std::make_unique<absl::flat_hash_map<uint64_t,uint64_t>>(); t->reserve(N);
      for (size_t i = 0; i < N; i++) (*t)[keys[i]] = chain_val(i);
      measure("abseil", qhit, N, [&](uint64_t k){ auto it = t->find(k); return it == t->end() ? 0ULL : it->second; }, std::function<void(int)>()); }
    { auto t = std::make_unique<boost::unordered_flat_map<uint64_t,uint64_t>>(); t->reserve(N);
      for (size_t i = 0; i < N; i++) (*t)[keys[i]] = chain_val(i);
      measure("boost", qhit, N, [&](uint64_t k){ auto it = t->find(k); return it == t->end() ? 0ULL : it->second; }, std::function<void(int)>()); }
    return 0;
}
