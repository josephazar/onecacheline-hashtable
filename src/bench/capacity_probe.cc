// capacity_probe.cc - actual capacity, load and footprint of every table at N keys, built exactly
// as the drivers build them (production maps: reserve(N) then N inserts; ours: constructor(N/0.7+1) then N inserts).
//   ./capacity_probe [log2n=27]        CSV: table,keys,slots,load,bytes_table
// Footprints count the slot/bucket arrays only (Abseil: slots + control bytes; Boost: groups + elements; ankerl:
// buckets + dense vector; F14: chunks), from the documented layouts, not from allocator statistics.
#include "swiss_table_chunk.hpp"
#include "swiss_table_chunkv.hpp"
#ifndef FILTER_MODE
#define FILTER_MODE 2
#endif
#include "swiss_table_chunkvf.hpp"
#include "swiss_table_chunk2.hpp"
#include <absl/container/flat_hash_map.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <ankerl/unordered_dense.h>
#ifndef NO_F14
#include <folly/container/F14Map.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
static void row(const char* t, size_t n, size_t slots, double bytes) { std::printf("%s,%zu,%zu,%.4f,%.0f\n", t, n, slots, double(n) / slots, bytes); std::fflush(stdout); }
template <class T> size_t nslots(T& t) { if constexpr (requires { t.slot_cap(); }) return t.slot_cap(); else return t.slots(); }
int main(int argc, char** argv) {
    const int L = argc > 1 ? atoi(argv[1]) : 27; const size_t N = 1ull << L;
    std::printf("table,keys,slots,load,bytes_table\n");
    { auto t = std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) t->insert(splitmix64(i), i); row("Inline", N, nslots(*t), nslots(*t) / 3.0 * 64); }
    { auto t = std::make_unique<SwissTableChunkVF<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) t->insert(splitmix64(i), i); row("Inline+F", N, nslots(*t), nslots(*t) / 3.0 * 64); }
    { auto t = std::make_unique<SwissTableChunk2<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) t->insert(splitmix64(i), i); row("Paired", N, nslots(*t), nslots(*t) / 7.0 * 128); }
    { auto t = std::make_unique<SwissTableChunk<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) t->insert(splitmix64(i), i); row("Detached", N, nslots(*t), nslots(*t) / 7.0 * 64 + nslots(*t) * 8.0); }
    { absl::flat_hash_map<uint64_t,uint64_t> m; m.reserve(N); for (size_t i = 0; i < N; i++) m[splitmix64(i)] = i; row("Abseil", N, m.capacity(), m.capacity() * 17.0); }
    { boost::unordered_flat_map<uint64_t,uint64_t> m; m.reserve(N); for (size_t i = 0; i < N; i++) m[splitmix64(i)] = i; row("Boost", N, m.bucket_count(), m.bucket_count() * 16.0 + m.bucket_count() / 15.0 * 16); }
    { ankerl::unordered_dense::map<uint64_t,uint64_t> m; m.reserve(N); for (size_t i = 0; i < N; i++) m[splitmix64(i)] = i; row("ankerl", N, m.bucket_count(), m.bucket_count() * 8.0 + N * 16.0); }
#ifndef NO_F14
    { folly::F14ValueMap<uint64_t,uint64_t> m; m.reserve(N); for (size_t i = 0; i < N; i++) m[splitmix64(i)] = i; row("F14", N, m.bucket_count(), m.bucket_count() / 14.0 * 240); }
#endif
    return 0;
}
