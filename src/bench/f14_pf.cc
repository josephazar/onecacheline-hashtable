// f14_pf.cc - F14ValueMap in its own translation unit (same reason as the artifact's f14_bench.cc: folly's
// F14LinkCheck ODR guard ties the intrinsics mode to the prebuilt library, so this file is compiled at the
// baseline ISA: -msse2 on x86, -march=armv8-a on ARM).  Adds the token pipeline the journal readers asked for:
// prehash(key) + prefetch(token) k lookups ahead, then find(token, key).  This is F14's documented way of
// overlapping independent cold lookups (F14Map.h, "prehash"/"prefetch").
#include <folly/container/F14Map.h>
#include <cstdint>
#include <cstddef>

static folly::F14ValueMap<uint64_t, uint64_t>* g_f14 = nullptr;

extern "C" void f14_build(const uint64_t* keys, const uint64_t* vals, size_t n) {
    delete g_f14; g_f14 = new folly::F14ValueMap<uint64_t, uint64_t>(); g_f14->reserve(n);
    for (size_t i = 0; i < n; ++i) (*g_f14)[keys[i]] = vals[i];
}
extern "C" void f14_free() { delete g_f14; g_f14 = nullptr; }

extern "C" void f14_probe_sync(const uint64_t* q, size_t n, uint64_t* out) {
    for (size_t i = 0; i < n; ++i) { auto it = g_f14->find(q[i]); out[i] = it == g_f14->end() ? 0 : it->second; }
}

// token ring of depth k (k <= 64): token for q[i] was computed and prefetched k iterations earlier
extern "C" void f14_probe_token(const uint64_t* q, size_t n, uint64_t* out, int k) {
    folly::F14HashToken ring[64];
    if (k > 64) k = 64; if (k < 1) k = 1;
    size_t pre = n < (size_t)k ? n : (size_t)k;
    for (size_t i = 0; i < pre; ++i) { ring[i] = g_f14->prehash(q[i]); g_f14->prefetch(ring[i]); }
    for (size_t i = 0; i < n; ++i) {
        const folly::F14HashToken tok = ring[i % k];
        if (i + k < n) { ring[i % k] = g_f14->prehash(q[i + k]); g_f14->prefetch(ring[i % k]); }
        auto it = g_f14->find(tok, q[i]); out[i] = it == g_f14->end() ? 0 : it->second;
    }
}
