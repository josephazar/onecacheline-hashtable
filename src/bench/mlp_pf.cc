// mlp_pf.cc - memory-level parallelism reachable with PREFETCHES, as the AMAC engine uses them, next to the
// dependent pointer chase of mlp.cc.  The chase measures how many *demand* misses a core keeps in flight; a
// prefetch retires without holding a load buffer entry, so the two numbers need not agree, and on Grace the
// engine sustains more than the chase predicts.  Same region, same random access pattern, distance D ahead.
//   CSV: kind,D,rep,mops   (kind = chase for the K-chain baseline, pf for the prefetch-ahead gather)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
using Clock = std::chrono::steady_clock;
static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
int main(int argc, char** argv){
    const int LOG2M = argc > 1 ? atoi(argv[1]) : 29;          // 2^29 * 8 B = 4 GiB, like the 2^27-key table
    const size_t Q = argc > 2 ? strtoull(argv[2], nullptr, 10) : 32'000'000, reps = argc > 3 ? atoi(argv[3]) : 5;
    const size_t M = 1ull << LOG2M, MASK = M - 1;
    std::vector<uint64_t> A(M); for (size_t i = 0; i < M; i++) A[i] = splitmix64(i);
    std::vector<uint64_t> idx(Q); for (size_t i = 0; i < Q; i++) idx[i] = splitmix64(i * 0x9E3779B97F4A7C15ULL + 7) & MASK;
    std::printf("kind,D,rep,mops\n");
    auto emit = [&](const char* k, int D, size_t r, double s){ std::printf("%s,%d,%zu,%.2f\n", k, D, r, Q / s / 1e6); std::fflush(stdout); };
    for (int D : {0, 1, 2, 4, 8, 16, 24, 32, 48, 64, 96, 128, 192, 256}) {
        for (size_t r = 0; r < reps + 1; r++) {                 // first pass is warm-up
            auto t0 = Clock::now(); uint64_t s = 0;
            if (D == 0) { for (size_t i = 0; i < Q; i++) s += A[idx[i]]; }
            else { size_t i = 0;
                for (; i + D < Q; i++) { __builtin_prefetch(&A[idx[i + D]], 0, 2); s += A[idx[i]]; }
                for (; i < Q; i++) s += A[idx[i]]; }
            auto t1 = Clock::now(); volatile uint64_t sink = s; (void)sink;
            if (r) emit("pf", D, r - 1, std::chrono::duration<double>(t1 - t0).count());
        }
    }
    // dependent-chain baseline in the same region: K independent chains of random hops (mlp.cc's method)
    std::vector<uint64_t> P(M); { std::vector<uint64_t> perm(M); for (size_t i = 0; i < M; i++) perm[i] = i;
        std::mt19937_64 rng(12345); std::shuffle(perm.begin(), perm.end(), rng);
        for (size_t i = 0; i + 1 < M; i++) P[perm[i]] = perm[i + 1]; P[perm[M - 1]] = perm[0]; }
    for (int K : {1, 2, 4, 8, 16, 24, 32, 48, 64}) {
        std::vector<size_t> c(K); for (int k = 0; k < K; k++) c[k] = (size_t)((double)k / K * M);
        const size_t T = 8'000'000 / K;
        for (size_t r = 0; r < reps + 1; r++) {
            auto t0 = Clock::now();
            for (size_t t = 0; t < T; t++) for (int k = 0; k < K; k++) c[k] = P[c[k]];
            auto t1 = Clock::now(); volatile size_t sink = 0; for (int k = 0; k < K; k++) sink += c[k]; (void)sink;
            if (r) std::printf("chase,%d,%zu,%.2f\n", K, r - 1, (double)K * T / std::chrono::duration<double>(t1 - t0).count() / 1e6), std::fflush(stdout);
        }
    }
    return 0;
}
