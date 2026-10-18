// overflow_count.cc - does a load below 0.5 mean no chunk has overflowed?
// A global load bound cannot guarantee it: four keys can hash to one three-slot chunk while the table is mostly
// empty.  This driver measures, for an Inline+F table of fixed chunk count filled to each load: the fraction of
// full chunks, the fraction of chunks whose overflow filter is non-zero, and on a fresh absent-key stream the
// average chunks per negative lookup and the fraction of negative lookups that had to consult the filter.
// Deterministic given the seed; machine independent.  Build with -DFILTER_MODE=2 from inside a header directory.
//   CSV: load,keys,chunks,full_frac,overflowed_frac,chunks_per_negative,consulted_frac,filtered_frac
#include "swiss_table_chunkvf.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <memory>

static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }

int main(int argc, char** argv){
    const int LOG2C = argc > 1 ? atoi(argv[1]) : 23;          // chunks = 2^LOG2C, three slots each
    const size_t Q = argc > 2 ? strtoull(argv[2], nullptr, 10) : 1'000'000;
    std::printf("load,keys,chunks,full_frac,overflowed_frac,chunks_per_negative,consulted_frac,filtered_frac\n");
    for (double load : {0.2, 0.3, 0.4, 0.5, 0.6, 2.0 / 3.0}) {
        const size_t chunks = 1ull << LOG2C, n = (size_t)(load * chunks * 3);
        auto t = std::make_unique<SwissTableChunkVF<uint64_t,uint64_t>>(chunks * 3 - 2 * 3);   // rehash(cap/3+2) -> bit_ceil = chunks
        if (t->chunk_count() != chunks) { std::fprintf(stderr, "chunk count %zu != %zu\n", t->chunk_count(), chunks); return 1; }
        for (size_t i = 0; i < n; i++) { uint64_t k = splitmix64(i); t->insert(k, k); }
        if (t->chunk_count() != chunks) { std::fprintf(stderr, "table rehashed at load %.2f\n", load); return 1; }
        std::mt19937_64 rng(77); double steps = 0; size_t consulted = 0, filtered = 0;
        for (size_t i = 0; i < Q; i++) { uint64_t k = splitmix64(n + rng() % (3 * n + 1)); int f = 0; size_t s = t->neg_chunks(k, f); steps += s; filtered += f; consulted += (f || s > 1); }
        std::printf("%.3f,%zu,%zu,%.5f,%.5f,%.4f,%.5f,%.5f\n", load, n, chunks, (double)t->full_chunks() / chunks, (double)t->overflowed_chunks() / chunks, steps / Q, (double)consulted / Q, (double)filtered / Q);
        std::fflush(stdout);
    }
    return 0;
}
