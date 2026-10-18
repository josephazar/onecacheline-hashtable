// churn_cost.cc - what the tombstone-triggered rebuild costs under delete/insert churn.
// Patched headers only (tombstone_fix.py).  Each table is built with N keys at the paper's operating load (2/3 for the
// 3-slot chunk), then ROUNDS x N pairs "erase a random live key, insert a fresh key" are timed in slices of N/4 pairs.
// Reported per table: ns per pair over the whole run (rebuilds included), number of same-capacity rebuilds and
// doublings, the time of one same-capacity rebuild, pairs per rebuild and the used (live + tombstone) fraction at the end
// (filter rate: see filter_churn in the paper; not repeated here).
//   CSV: table,log2n,pairs,ns_per_pair,rebuilds,grows,pairs_per_rebuild,rebuild_ms,used_frac_end,live_frac
#include "swiss_table_chunk.hpp"
#include "swiss_table_chunkv.hpp"
#ifndef FILTER_MODE
#define FILTER_MODE 2
#endif
#include "swiss_table_chunkvf.hpp"
#include "swiss_table_chunk2.hpp"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <random>
#include <chrono>
#include <memory>
using Clock = std::chrono::steady_clock;
static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
template <class T> size_t nslots(T& t) { if constexpr (requires { t.slot_cap(); }) return t.slot_cap(); else return t.slots(); }

template <class T> void run(const char* name, int log2n, double rounds) {
    const size_t N = 1ull << log2n;
    auto t = std::make_unique<T>(size_t(N / 0.7) + 1);
    std::vector<uint64_t> keys(N); uint64_t next = 1;
    for (size_t i = 0; i < N; i++) { keys[i] = splitmix64(next++); t->insert(keys[i], keys[i] + 1); }
    std::mt19937_64 rng(77);
    const size_t pairs = size_t(rounds * N), slice = N / 4;
    double total = 0, rebuild_s = 0; size_t r0 = t->n_rebuilds;
    for (size_t done = 0; done < pairs; done += slice) {
        size_t rb = t->n_rebuilds + t->n_grows;
        auto t0 = Clock::now();
        for (size_t p = 0; p < slice; p++) { size_t i = rng() % N; t->erase(keys[i]); keys[i] = splitmix64(next++); t->insert(keys[i], keys[i] + 1); }
        double s = std::chrono::duration<double>(Clock::now() - t0).count(); total += s;
        (void)rb;
    }
    // time one rebuild separately (same work as a triggered one) to report its cost
    size_t nrb = t->n_rebuilds - r0; double used_end = double(t->used) / nslots(*t);
    { auto t0 = Clock::now(); t->rehash_same(); rebuild_s = std::chrono::duration<double>(Clock::now() - t0).count(); }
    std::printf("%s,%d,%zu,%.1f,%zu,%zu,%.0f,%.0f,%.3f,%.3f\n", name, log2n, pairs, total / pairs * 1e9, nrb, t->n_grows,
                nrb ? double(pairs) / nrb : 0.0, rebuild_s * 1e3, used_end, double(t->count()) / nslots(*t));
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    int log2n = argc > 1 ? atoi(argv[1]) : 24; double rounds = argc > 2 ? atof(argv[2]) : 4.0;
    std::printf("table,log2n,pairs,ns_per_pair,rebuilds,grows,pairs_per_rebuild,rebuild_ms,used_frac_end,live_frac\n");
    run<SwissTableChunkV<uint64_t, uint64_t>>("Inline", log2n, rounds);
    run<SwissTableChunkVF<uint64_t, uint64_t>>("Inline+F", log2n, rounds);
    run<SwissTableChunk2<uint64_t, uint64_t>>("Paired", log2n, rounds);
    run<SwissTableChunk<uint64_t, uint64_t>>("Detached", log2n, rounds);
    return 0;
}
