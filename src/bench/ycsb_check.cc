// ycsb_check.cc - the ordering contract of the mixed read/update benchmark (ycsb.cc), checked against a serial
// reference.  The engine resolves a block of 1024 operations' slot addresses out of order (find_batch_ptr), then
// applies the block's reads and writes IN ARRIVAL ORDER through the returned pointers.  Updates never move a slot,
// so the pointer of an earlier probe stays valid, and a read that follows an update of the same key inside the
// block observes the update.  This driver makes that observable: every update writes a VERSIONED value
// (a function of the operation index, not of the key), and every read is compared with a std::unordered_map
// driven serially by the same stream.  It also counts the same-key pairs inside blocks that the check exercised.
//   Output: mix, ops, reads, updates, intra-block same-key pairs, mismatches (must be 0)
#include "swiss_table_chunkv.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <cmath>

static inline uint64_t splitmix64(uint64_t x){ x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
static inline uint64_t value_of(uint64_t k){ return k*2654435761ULL + 1; }
static inline uint64_t version_value(size_t op){ return splitmix64(op + 0x1234567ULL) | 1; }   // never equals value_of, never 0

struct Zipf { double s, n, hx1, hn, sfix;   // rejection inversion, ranks 1..n
    static double h1(double x){ return std::fabs(x) > 1e-8 ? std::log1p(x) / x : 1 - 0.5 * x; }
    static double h2(double x){ return std::fabs(x) > 1e-8 ? std::expm1(x) / x : 1 + 0.5 * x; }
    double hI(double x) const { double lx = std::log(x); return h2((1 - s) * lx) * lx; }
    double h(double x) const { return std::exp(-s * std::log(x)); }
    double hIinv(double x) const { double t = x * (1 - s); if (t < -1) t = -1; return std::exp(h1(t) * x); }
    Zipf(uint64_t n_, double s_) : s(s_), n((double)n_) { hx1 = hI(1.5) - 1; hn = hI(n + 0.5); sfix = 2 - hIinv(hI(2.5) - h(2)); }
    template<class R> uint64_t operator()(R& r) const { std::uniform_real_distribution<double> U(0, 1);
        for (;;) { double u = hn + U(r) * (hx1 - hn), x = hIinv(u); uint64_t k = (uint64_t)(x + 0.5); if (k < 1) k = 1; else if ((double)k > n) k = (uint64_t)n;
                   if (k - x <= sfix || u >= hI((double)k + 0.5) - h((double)k)) return k; } } };

int main(int argc, char** argv){
    const int LOG2N = argc > 1 ? atoi(argv[1]) : 24; const size_t N = 1ull << LOG2N, Q = argc > 2 ? strtoull(argv[2], nullptr, 10) : 2'000'000, B = 1024;
    std::vector<uint64_t> keys(N); for (size_t i = 0; i < N; i++) keys[i] = splitmix64(i);
    struct Mix { const char* name; double pu; } MIX[] = {{"b05", 0.05}, {"a50", 0.50}, {"u100", 1.00}};
    std::printf("mix,ops,reads,updates,intra_block_same_key_pairs,mismatches\n");
    for (auto& m : MIX) {
        std::mt19937_64 rng(123); Zipf z(N, 0.99); std::uniform_real_distribution<double> U(0, 1);
        std::vector<uint64_t> qk(Q); std::vector<uint8_t> up(Q);
        for (size_t i = 0; i < Q; i++) { qk[i] = keys[z(rng) - 1]; up[i] = U(rng) < m.pu; }
        // serial reference
        std::unordered_map<uint64_t,uint64_t> ref; ref.reserve(N); for (size_t i = 0; i < N; i++) ref[keys[i]] = value_of(keys[i]);
        std::vector<uint64_t> expect(Q, 0);
        for (size_t i = 0; i < Q; i++) { auto& v = ref[qk[i]]; if (up[i]) v = version_value(i); else expect[i] = v; }
        // engine: probe a block out of order, apply in arrival order
        auto t = std::make_unique<SwissTableChunkV<uint64_t,uint64_t>>(size_t(N/0.7)+1); for (size_t i = 0; i < N; i++) t->insert(keys[i], value_of(keys[i]));
        std::vector<uint64_t*> rp(B); size_t mism = 0, reads = 0, updates = 0, pairs = 0;
        for (size_t i = 0; i < Q; i += B) { size_t e = std::min(Q, i + B), nb = e - i;
            t->find_batch_ptr<32>(qk.data() + i, nb, rp.data());
            std::unordered_map<uint64_t,int> seen; seen.reserve(2 * nb);
            for (size_t j = 0; j < nb; j++) { uint64_t* p = rp[j]; if (!p) { std::fprintf(stderr, "key not found\n"); return 1; }
                pairs += seen[qk[i + j]]++ > 0;
                if (up[i + j]) { *p = version_value(i + j); updates++; } else { reads++; mism += *p != expect[i + j]; } } }
        std::printf("%s,%zu,%zu,%zu,%zu,%zu\n", m.name, Q, reads, updates, pairs, mism);
    }
    return 0;
}
