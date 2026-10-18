// nh_compare.cc - focused comparison against NeighborHash (Zhang et al., CIKM 2024, arXiv 2409.00400,
// github.com/slow-steppers/NeighborHash at the commit pinned in nh_build.sh, header-only, MIT licence).
// NeighborHash is linear-probing-like open addressing with 16-byte slots {key, value}: the top 12 bits of the value word
// hold a signed offset to the next key of the same home chain (relative, +-2047 slots), the low 52 bits the payload.
// A key lives in its home slot or on a chain that starts there, placed in the home cache line when possible.
// Cells, each a plain call over a query array, paired protocol of prod_prefetch.cc:
//   Inline-sync, Inline-AMAC{16,,64}     SwissTableChunkV, find_sync loop / find_batch<D> (our ring; no suffix = D=32)
//   InlineF-sync, InlineF-AMAC{16,,64}   SwissTableChunkVF with FILTER_MODE 2 (fixed filter), same calls
//   NH-sync                              find() loop (scalar chain walk)
//   NH-AMAC{8,16,32,64}                  amac_find_with_index<P>: scalar AMAC ring, P lanes, 1 prefetch per slot hop
//   NH-vfind              [AVX-512]      vfind() loop: home line compared with one 512-bit load, then chain walk
//   NH-SIMD               [AVX-512]      simd_find: 8 keys per gather, no prefetch
//   NH-SIMDAMAC{8,16,32,64} [AVX-512]    simd_amac_find<P>: P groups of 8 gather lanes, prefetch per lane
//   NH79-*                               the same NH cells on a table at NeighborHash's own benchmark load (0.79)
// Tables and load.  NeighborHash's default policy rounds capacity to a power of two and grows at 0.81, so 2^27 keys land
// in 2^28 slots (load 0.50, 4 GiB) - the same bytes as Inline (2^26 chunks of 64 B, 3 slots each, load 0.67).  NH79 uses
// a policy (FastRangePolicy below, through the PolicyTraits hook the library exposes) with capacity ceil(N/0.79) and
// slot = (hash>>32)*capacity>>32, identical in scalar and AVX-512 form; everything else is NeighborHash's code.
// Matched work.  Keys splitmix64(i); values value_of(k) = (k*2654435761+1) & (2^52-1): NeighborHash stores 52-bit
// payloads, so the SAME masked values go into every table (row offsets fit easily).  NeighborHash reserves no key value
// (key 0 is handled by its "invisible slot"); the driver still reports whether 0 is among the keys.  NH batch calls
// report hits only, through a callback: NH-AMAC* writes out[idx] on a hit and leaves misses untouched (out is cleared
// before every validation pass, untimed), our engines store a 0 per miss.  NH-SIMD* give values WITHOUT the query
// index (callback key argument is 0), so they append values in completion order; they are validated as a multiset and
// do not deliver the per-query association a row-offset lookup needs - report them apart.  Prefetch hint matched:
// absl::PrefetchToLocalCache = __builtin_prefetch(p,0,3) (shim in nh_shim/), our headers use locality 3 too.
// Protocol as prod_prefetch.cc: B blocks per resident set of tables, fresh hit and miss streams per block, every cell
// order-shuffled within a block, per-key validation on the warm-up pass, then one timed pass; one untimed block first.
// Footprint (bytes), capacity (slots) and load of each table, and NeighborHash's share of keys whose hit stays in one
// cache line (is_one_cacheline_access on a 1M-key sample), go to stderr.
//   Usage: nh_compare [log2n=27] [Q=2000000] [blocks=9]      CSV: cell,workload,log2n,block,order,mops
// Build: see the repository README (headers from src/include, NeighborHash cloned at the pinned commit).
#include "swiss_table_chunkv.hpp"
#ifndef FILTER_MODE
#define FILTER_MODE 2                      // Inline+F with the fixed bit positions (Section: aligned keys)
#endif
#include "swiss_table_chunkvf.hpp"
#include "neighbor_hash/neighbor_hash.h"
#include "neighbor_hash/common_policy.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <functional>
#include <string>
#include <memory>

// NeighborHash at a chosen load: exact capacity, multiply-shift range reduction on the top 32 hash bits (capacity < 2^32).
template <class K, class V> struct FastRangePolicy : neighbor::policy::DefaultPolicy<K, V> {
    static size_t NormalizeCapacity(size_t n) { return (n + 3) & ~size_t(3); }       // whole 64-byte lines of 4 slots
    struct Hash2Slot {
        uint64_t operator()(size_t h, size_t cap) const { return ((h >> 32) * cap) >> 32; }
#ifdef NEIGHBOR_HASH_SIMD_FIND
        __m512i v_hash2slot(__m512i h, size_t cap) const {
            return _mm512_srli_epi64(_mm512_mul_epu32(_mm512_srli_epi64(h, 32), _mm512_set1_epi64(cap)), 32); }
#endif
    };
};
using NHPow2 = neighbor::NeighborHashMap<uint64_t, uint64_t, neighbor::policy::DefaultPolicy<uint64_t, uint64_t>>;
using NHFr   = neighbor::NeighborHashMap<uint64_t, uint64_t, FastRangePolicy<uint64_t, uint64_t>>;
static_assert(NHPow2::kPayloadMask == NHFr::kPayloadMask, "same payload width");
#ifndef NH_LF
#define NH_LF 0.79                          // load of NeighborHash's own RandomAccess benchmark (LF argument 79)
#endif

static constexpr uint64_t VMASK = NHPow2::kPayloadMask;                        // 2^52 - 1
static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
static inline uint64_t value_of(uint64_t k){ return (k*2654435761ULL + 1) & VMASK; }
using Clock = std::chrono::steady_clock;
using Q_t = std::vector<uint64_t>;
// append = false: out[i] is the answer for q[i] (0 on a miss).  append = true: run returns how many values it appended.
struct Cell { std::string name; bool append; std::function<size_t(const Q_t&, Q_t&)> run; };

template <class M> static void nh_cells(std::vector<Cell>& cells, const std::string& p, M* m) {
    cells.push_back({p + "-sync", false, [m](const Q_t& q, Q_t& o){ for (size_t i = 0; i < q.size(); i++) { auto it = m->find(q[i]); o[i] = it == m->end() ? 0 : uint64_t(it->second); } return size_t(0); }});
    auto amac = [&]<int P>() { cells.push_back({p + "-AMAC" + std::to_string(P), false, [m](const Q_t& q, Q_t& o){
        m->template amac_find_with_index<P>(q.data(), (int64_t)q.size(), [&o](int64_t i, uint64_t, uint64_t v){ o[i] = v; }); return size_t(0); }}); };
    amac.template operator()<8>(); amac.template operator()<16>(); amac.template operator()<32>(); amac.template operator()<64>();
#ifdef NEIGHBOR_HASH_SIMD_FIND
    cells.push_back({p + "-vfind", false, [m](const Q_t& q, Q_t& o){ for (size_t i = 0; i < q.size(); i++) { auto it = m->vfind(q[i]); o[i] = it == m->end() ? 0 : uint64_t(it->second); } return size_t(0); }});
    cells.push_back({p + "-SIMD", true, [m](const Q_t& q, Q_t& o){ size_t c = 0;
        m->simd_find(q.data(), (int64_t)q.size(), [&](uint64_t, uint64_t v){ o[c++] = v; }); return c; }});
    auto samac = [&]<int P>() { cells.push_back({p + "-SIMDAMAC" + std::to_string(P), true, [m](const Q_t& q, Q_t& o){ size_t c = 0;
        m->template simd_amac_find<P>(q.data(), (int64_t)q.size(), [&](uint64_t, uint64_t v){ o[c++] = v; }); return c; }}); };
    samac.template operator()<8>(); samac.template operator()<16>(); samac.template operator()<32>(); samac.template operator()<64>();
#endif
}
template <class M> static void nh_report(const char* name, M& m, const Q_t& keys) {
    size_t one = 0, S = std::min<size_t>(keys.size(), 1u << 20);
    for (size_t i = 0; i < S; i++) one += m.is_one_cacheline_access(keys[(i * 2654435761ULL) % keys.size()]);
    std::fprintf(stderr, "table %-8s bytes=%zu slots=%zu keys=%zu load=%.4f one_line_hit_share=%.4f (sample %zu)\n", name,
        (m.bucket_count() + 1) * sizeof(typename M::slot_type), m.bucket_count(), m.size(), m.load_factor(), double(one) / S, S);
}

int main(int argc, char** argv){
    const int LOG2N = argc > 1 ? atoi(argv[1]) : 27; const size_t N = 1ull << LOG2N, Q = argc > 2 ? strtoull(argv[2], nullptr, 10) : 2'000'000; const int blocks = argc > 3 ? atoi(argv[3]) : 9;
    std::printf("cell,workload,log2n,block,order,mops\n");
#ifdef NEIGHBOR_HASH_SIMD_FIND
    std::fprintf(stderr, "build: NEIGHBOR_HASH_SIMD_FIND on (AVX-512 cells present)\n");
#else
    std::fprintf(stderr, "build: NEIGHBOR_HASH_SIMD_FIND off (scalar NH cells only)\n");
#endif
#ifdef NEIGHBOR_HASH_HUGEPAGE
    std::fprintf(stderr, "build: NEIGHBOR_HASH_HUGEPAGE on (NH tables on MAP_HUGETLB 2 MiB pages, ours on the default allocator)\n");
#endif
    Q_t keys(N), vals(N); size_t zero_keys = 0;
    for (size_t i = 0; i < N; i++) { keys[i] = splitmix64(i); vals[i] = value_of(keys[i]); zero_keys += keys[i] == 0; }
    std::fprintf(stderr, "keys: N=2^%d, key 0 present: %s, values masked to %d bits\n", LOG2N, zero_keys ? "yes" : "no", (int)NHPow2::kPayloadBitCount);
    auto inl = std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) inl->insert(keys[i], vals[i]);
    auto inf = std::make_unique<SwissTableChunkVF<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) inf->insert(keys[i], vals[i]);
    auto nh  = std::make_unique<NHPow2>(size_t(std::ceil(N / 0.81)) + 1); for (size_t i = 0; i < N; i++) nh->insert(keys[i], vals[i]);
    auto nhf = std::make_unique<NHFr>(size_t(std::ceil(N / NH_LF)));     for (size_t i = 0; i < N; i++) nhf->insert(keys[i], vals[i]);
    std::fprintf(stderr, "table %-8s bytes=%zu slots=%zu keys=%zu load=%.4f\n", "Inline",  inl->bytes(), inl->slot_cap(), inl->count(), double(inl->count()) / inl->slot_cap());
    std::fprintf(stderr, "table %-8s bytes=%zu slots=%zu keys=%zu load=%.4f\n", "InlineF", inf->bytes(), inf->slot_cap(), inf->count(), double(inf->count()) / inf->slot_cap());
    nh_report("NH", *nh, keys); nh_report("NH79", *nhf, keys);
    std::fprintf(stderr, "tables built: N=2^%d\n", LOG2N);

    std::vector<Cell> cells = {
        {"Inline-sync",  false, [&](const Q_t& q, Q_t& o){ for (size_t i = 0; i < q.size(); i++) { auto* p = inl->find_sync(q[i]); o[i] = p ? *p : 0; } return size_t(0); }},
        {"Inline-AMAC",  false, [&](const Q_t& q, Q_t& o){ inl->find_batch<32>(q.data(), q.size(), o.data()); return size_t(0); }},
        {"Inline-AMAC16", false, [&](const Q_t& q, Q_t& o){ inl->find_batch<16>(q.data(), q.size(), o.data()); return size_t(0); }},
        {"Inline-AMAC64", false, [&](const Q_t& q, Q_t& o){ inl->find_batch<64>(q.data(), q.size(), o.data()); return size_t(0); }},
        {"InlineF-sync", false, [&](const Q_t& q, Q_t& o){ for (size_t i = 0; i < q.size(); i++) { auto* p = inf->find_sync(q[i]); o[i] = p ? *p : 0; } return size_t(0); }},
        {"InlineF-AMAC", false, [&](const Q_t& q, Q_t& o){ inf->find_batch<32>(q.data(), q.size(), o.data()); return size_t(0); }},
        {"InlineF-AMAC16", false, [&](const Q_t& q, Q_t& o){ inf->find_batch<16>(q.data(), q.size(), o.data()); return size_t(0); }},
        {"InlineF-AMAC64", false, [&](const Q_t& q, Q_t& o){ inf->find_batch<64>(q.data(), q.size(), o.data()); return size_t(0); }},
    };
    nh_cells(cells, "NH", nh.get()); nh_cells(cells, "NH79", nhf.get());

    std::mt19937_64 rng(3003); Q_t out(Q), exp_sorted(Q), got(Q);
    for (int b = -1; b < blocks; b++) {   // block -1: one untimed warm-up pass over every cell after the builds
        Q_t qhit(Q), qmiss(Q);
        for (size_t i = 0; i < Q; i++) { qhit[i] = splitmix64(rng() & (N - 1)); qmiss[i] = splitmix64(N + rng() % (3 * N)); }
        for (size_t i = 0; i < Q; i++) exp_sorted[i] = value_of(qhit[i]); std::sort(exp_sorted.begin(), exp_sorted.end());
        std::vector<int> order(2 * cells.size()); for (size_t i = 0; i < order.size(); i++) order[i] = (int)i; std::shuffle(order.begin(), order.end(), rng);
        for (size_t pos = 0; pos < order.size(); pos++) {
            const Cell& c = cells[order[pos] / 2]; bool miss = order[pos] % 2; auto& q = miss ? qmiss : qhit;
            std::fill(out.begin(), out.end(), 0); size_t cnt = c.run(q, out); bool ok = true; size_t bad = 0;
            if (!c.append) { for (size_t i = 0; i < Q && ok; i++) if (out[i] != (miss ? 0 : value_of(q[i]))) { ok = false; bad = i; } }
            else if (miss) ok = cnt == 0;
            else if ((ok = cnt == Q)) { std::copy(out.begin(), out.end(), got.begin()); std::sort(got.begin(), got.end()); ok = got == exp_sorted; }
            if (!ok) { std::fprintf(stderr, "VALIDATION FAILED %s %s (index %zu, count %zu)\n", c.name.c_str(), miss ? "miss" : "hit", bad, cnt); return 1; }
            if (b < 0) continue;
            auto t0 = Clock::now(); c.run(q, out); auto t1 = Clock::now();
            std::printf("%s,%s,%d,%d,%zu,%.2f\n", c.name.c_str(), miss ? "miss" : "hit", LOG2N, b, pos, Q / std::chrono::duration<double>(t1 - t0).count() / 1e6);
        }
        std::fflush(stdout);
    }
    return 0;
}
