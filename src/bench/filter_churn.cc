// filter_churn.cc - the Inline+F table under delete/insert churn at constant load, with the new erase:
// bits are never cleared, so the filter's false-positive rate drifts; rebuild() (rehash in place) resets it.
// Every checkpoint validates sampled present and absent keys through find_batch.   Build per FILTER_MODE.
//   CSV: filter_mode,ops,live,tombstone_frac,avg_chunks_neg,consulted_frac,fp_rate,avg_chunks_pos,check
#include "swiss_table_chunkvf.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
static inline uint64_t value_of(uint64_t k){ return k*2654435761ULL + 1; }
int main(int argc, char** argv){
    const int LOG2N = argc > 1 ? atoi(argv[1]) : 24; const size_t N = 1ull << LOG2N; const size_t total = (argc > 2 ? atoi(argv[2]) : 20) * 1000000ull, period = 1000000, rebuild_at = (argc > 3 ? atoi(argv[3]) : 15) * 1000000ull;
    std::printf("filter_mode,ops,live,tombstone_frac,avg_chunks_neg,consulted_frac,fp_rate,avg_chunks_pos,check\n");
    SwissTableChunkVF<uint64_t,uint64_t> t(size_t(N/0.7)+1);
    std::vector<uint64_t> live(N); for (size_t i = 0; i < N; i++) { live[i] = i; t.insert(splitmix64(i), value_of(splitmix64(i))); }
    uint64_t next_idx = N; std::mt19937_64 rng(5); std::vector<uint64_t> qs(200000), out(200000);
    auto report = [&](size_t ops, const char* tag){
        size_t M = 200000, consulted = 0, fp = 0, bad = 0; double cn = 0, cp = 0;
        for (size_t i = 0; i < M; i++) { qs[i] = splitmix64((1ull << 40) + (rng() % (1ull << 30))); int f; size_t s = t.neg_chunks(qs[i], f); cn += s; if (f) consulted++; else if (s > 1) { consulted++; fp++; } }
        t.find_batch<32>(qs.data(), M, out.data()); for (size_t i = 0; i < M; i++) bad += out[i] != 0;
        for (size_t i = 0; i < M; i++) { qs[i] = splitmix64(live[rng() % live.size()]); int f; cp += t.neg_chunks(qs[i], f); }   // neg_chunks counts chunks until found too
        t.find_batch<32>(qs.data(), M, out.data()); for (size_t i = 0; i < M; i++) bad += out[i] != value_of(qs[i]);
        std::printf("%d,%zu%s,%zu,%.5f,%.4f,%.4f,%.5f,%.4f,%s\n", FILTER_MODE, ops, tag, live.size(), (double)t.tombstones() / (t.chunk_count() * 3), cn / M, (double)consulted / M, consulted ? (double)fp / consulted : 0.0, cp / M, bad ? "FAIL" : "ok");
        std::fflush(stdout); if (bad) { std::fprintf(stderr, "CORRECTNESS FAILURE at %zu ops (%zu bad)\n", ops, bad); std::exit(1); }
    };
    report(0, "");
    for (size_t op = 1; op <= total; op++) {
        size_t i = rng() % live.size(); uint64_t idx = live[i]; live[i] = next_idx;
        if (!t.erase(splitmix64(idx))) { std::fprintf(stderr, "erase failed at op %zu\n", op); return 1; }
        uint64_t k = splitmix64(next_idx++); t.insert(k, value_of(k));
        if (op == rebuild_at) { report(op, "-before-rebuild"); t.rebuild(); report(op, "-after-rebuild"); }
        else if (op % period == 0) report(op, "");
    }
    return 0;
}
