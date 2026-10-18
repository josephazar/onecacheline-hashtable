// tombstone_test.cc - does every probe terminate after deletions?
// Build twice, once against the late-September headers and once against the patched ones (tombstone_fix.py):
//   g++ -O2 -std=c++20 -march=native -I orig_headers -o tombstone_test_orig tombstone_test.cc
//   g++ -O2 -std=c++20 -march=native -I headers      -o tombstone_test_fix  tombstone_test.cc
// Test 1 replays readers's trace on the 3-slot Inline and Inline+F tables (4 chunks, 12 slots, 8 live keys):
//   insert a..h (homes 0,0,0,1,1,1,2,2); erase d, insert x (home 0, overflows into chunk 1); erase x, insert i (home 3);
//   erase e, insert j (home 3); erase f, insert k (home 3); erase a, insert l (home 2); then look up x.
// Test 2 churns every layout (Detached, Inline, Inline+F, Paired, separate-array SoA) at constant live load 2/3:
//   erase a random live key, insert a fresh one, and every CHECK operations look up every live key (value checked) and
//   the 4096 most recently erased keys (must be absent).  Every lookup batch runs in a forked child with a 5 s alarm,
//   so a probe that never ends is reported as HANG instead of hanging the test.
// CSV on stdout: test,table,ops,status,used,live,slots,rebuilds,grows
#include "swiss_table_chunk.hpp"
#include "swiss_table_chunkv.hpp"
#ifndef FILTER_MODE
#define FILTER_MODE 2
#endif
#include "swiss_table_chunkvf.hpp"
#include "swiss_table_chunk2.hpp"
#if defined(__x86_64__)
#include "swiss_table_h1h2.hpp"   // AVX2 separate-array table: x86 only
#endif
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <random>
#include <unordered_map>
#include <deque>
#include <string>
#include <memory>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

template <class T> size_t nslots(T& t) { if constexpr (requires { t.slot_cap(); }) return t.slot_cap(); else return t.slots(); }
template <class T> concept HasUsed = requires(T t) { t.used; t.n_rebuilds; t.n_grows; };
template <class T> void counters(T& t, size_t& u, size_t& r, size_t& g) {
    if constexpr (HasUsed<T>) { u = t.used; r = t.n_rebuilds; g = t.n_grows; } else { u = r = g = 0; }
}
static inline uint64_t value_of(uint64_t k) { return k * 2654435761ULL + 1; }
static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }

// Runs f() in a child; returns "ok", "WRONG" (f returned false) or "HANG" (killed by the alarm).
template <class F> static const char* guarded(F f, int seconds = 5) {
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { alarm(seconds); _exit(f() ? 0 : 2); }
    int st = 0; waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) return WTERMSIG(st) == SIGALRM ? "HANG" : "CRASH";
    return WEXITSTATUS(st) == 0 ? "ok" : "WRONG";
}

// ---- test 1: readers's trace -------------------------------------------------------------------------------
template <class T> void trace(const char* name) {
    auto t = std::make_unique<T>(6);                       // rehash(6/3 + 2) -> 4 chunks of 3 slots
    const uint64_t PHI = 0x9E3779B97F4A7C15ULL;
    auto home = [&](uint64_t k) { return (k * PHI) >> 62; };   // top 2 bits pick one of 4 chunks
    std::vector<uint64_t> pool[4]; for (uint64_t k = 1; pool[0].size() < 8 || pool[1].size() < 8 || pool[2].size() < 8 || pool[3].size() < 8; k++) pool[home(k)].push_back(k);
    uint64_t a = pool[0][0], b = pool[0][1], c = pool[0][2], x = pool[0][3], d = pool[1][0], e = pool[1][1], f = pool[1][2];
    uint64_t g = pool[2][0], h = pool[2][1], l = pool[2][2], i = pool[3][0], j = pool[3][1], k = pool[3][2];
    for (uint64_t q : {a, b, c, d, e, f, g, h}) t->insert(q, value_of(q));
    t->erase(d); t->insert(x, value_of(x));
    t->erase(x); t->insert(i, value_of(i));
    t->erase(e); t->insert(j, value_of(j));
    t->erase(f); t->insert(k, value_of(k));
    t->erase(a); t->insert(l, value_of(l));
    const char* s1 = guarded([&] { return t->find_sync(x) == nullptr; });
    const char* s2 = guarded([&] { uint64_t q[1] = {x}, o[1] = {1}; t->template find_batch<32>(q, 1, o); return o[0] == 0; });
    const char* s3 = guarded([&] { for (uint64_t q : {b, c, g, h, i, j, k, l}) { auto* p = t->find_sync(q); if (!p || *p != value_of(q)) return false; } return true; });
    size_t u, r, gr; counters(*t, u, r, gr);
    std::printf("trace_find_sync,%s,12,%s,%zu,%zu,%zu,%zu,%zu\n", name, s1, u, t->count(), (size_t)12, r, gr);
    std::printf("trace_find_batch,%s,12,%s,%zu,%zu,%zu,%zu,%zu\n", name, s2, u, t->count(), (size_t)12, r, gr);
    std::printf("trace_live_keys,%s,12,%s,%zu,%zu,%zu,%zu,%zu\n", name, s3, u, t->count(), (size_t)12, r, gr);
}

// ---- test 2: churn at constant live load ------------------------------------------------------------------------
template <class T> void churn(const char* name, size_t init_cap, size_t live, size_t ops, size_t CHECK) {
    auto t = std::make_unique<T>(init_cap);
    std::mt19937_64 rng(4242); uint64_t next = 1;
    std::vector<uint64_t> keys; keys.reserve(live);
    std::deque<uint64_t> erased;
    for (size_t n = 0; n < live; n++) { uint64_t k = splitmix64(next++); t->insert(k, value_of(k)); keys.push_back(k); }
    size_t slots = nslots(*t);
    const char* status = "ok"; size_t done = 0;
    for (size_t op = 1; op <= ops; op++) {
        size_t idx = rng() % keys.size(); uint64_t old = keys[idx];
        if (!t->erase(old)) { status = "WRONG"; done = op; break; }
        erased.push_back(old); if (erased.size() > 4096) erased.pop_front();
        uint64_t k = splitmix64(next++); t->insert(k, value_of(k)); keys[idx] = k;
        if (op % CHECK == 0 || op == ops) {
            status = guarded([&] {
                for (uint64_t q : keys) { auto* p = t->find_sync(q); if (!p || *p != value_of(q)) return false; }
                for (uint64_t q : erased) if (t->find_sync(q)) return false;
                return true; });
            done = op;
            size_t u, r, g; counters(*t, u, r, g);
            if (std::string(status) != "ok" || op == ops) {
                std::printf("churn,%s,%zu,%s,%zu,%zu,%zu,%zu,%zu\n", name, done, status, u, t->count(), nslots(*t), r, g);
                break;
            }
        }
    }
    (void)slots;
}

int main(int argc, char** argv) {
    size_t ops = argc > 1 ? strtoull(argv[1], nullptr, 10) : 2'000'000;
    std::printf("test,table,ops,status,used,live,slots,rebuilds,grows\n");
    trace<SwissTableChunkV<uint64_t, uint64_t>>("Inline");
    trace<SwissTableChunkVF<uint64_t, uint64_t>>("Inline+F");
    // 2^12 slots-ish tables, live load 2/3 of the slots they end up with
    const size_t L = 1 << 13;
    churn<SwissTableChunk<uint64_t, uint64_t>>("Detached", size_t(L / 0.7) + 1, L, ops, 20000);
    churn<SwissTableChunkV<uint64_t, uint64_t>>("Inline", size_t(L / 0.7) + 1, L, ops, 20000);
    churn<SwissTableChunkVF<uint64_t, uint64_t>>("Inline+F", size_t(L / 0.7) + 1, L, ops, 20000);
    churn<SwissTableChunk2<uint64_t, uint64_t>>("Paired", size_t(L / 0.7) + 1, L, ops, 20000);
#if defined(__x86_64__)
    churn<SwissTableH1H2<uint64_t, uint64_t>>("SoA", size_t(L / 0.7) + 1, L, ops, 20000);
#endif
    return 0;
}
