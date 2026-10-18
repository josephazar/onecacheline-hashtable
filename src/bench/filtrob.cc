// #3 filter robustness: at the table's operating load, how often does the 64-bit
// per-chunk filter actually short-circuit a negative lookup (vs. false-positive and
// continue), and how much does it cut the average negative probe length? Plus a
// memory proof that Inline+F == Inline (the filter lives in existing padding).
#include "swiss_table_chunkvf.hpp"   // Inline+F
#include "swiss_table_chunkv.hpp"    // Inline
#include <cstdint>
#include <cstdio>
#include <vector>
#include <random>

static inline uint64_t sm(uint64_t x){x+=0x9e3779b97f4a7c15ULL;x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;x=(x^(x>>27))*0x94d049bb133111ebULL;return x^(x>>31);}
static inline uint64_t vo(uint64_t k){return k*2654435761ULL+1;}

int main(){
    // memory proof (deterministic, machine-independent)
    std::printf("# sizeof Inline+F chunk = %zu B, Inline chunk = %zu B (filter in padding)\n",
                SwissTableChunkVF<uint64_t,uint64_t>::chunk_bytes(),
                SwissTableChunkV<uint64_t,uint64_t>::chunk_bytes());
    const size_t N = 1ull<<24, Q = 4'000'000;
    std::vector<uint64_t> keys(N); for(size_t i=0;i<N;i++) keys[i]=sm(i);
    SwissTableChunkVF<uint64_t,uint64_t> vf(size_t(N/0.7)+1);
    SwissTableChunkV <uint64_t,uint64_t> vi(size_t(N/0.7)+1);
    for(size_t i=0;i<N;i++){ vf.insert(keys[i],vo(keys[i])); vi.insert(keys[i],vo(keys[i])); }
    double load = double(vf.count())/vf.slot_cap();

    std::mt19937_64 r(9);
    std::vector<uint64_t> qm(Q); for(size_t i=0;i<Q;i++) qm[i]=sm(N+i+7);   // absent keys

    size_t sum_vf=0, sum_vi=0, fullhome=0, filtered=0, fp=0;
    for(size_t i=0;i<Q;i++){
        int f; size_t s = vf.neg_chunks(qm[i], f);
        sum_vf += s;
        if(f){ fullhome++; filtered++; }         // home full, filter rejected in 1 line
        else if(s>1){ fullhome++; fp++; }        // home full, filter false-positive -> continued
        sum_vi += vi.probe_len(qm[i]);           // Inline (no filter) negative probe length
    }
    std::printf("load=%.3f  Q=%zu\n", load, Q);
    std::printf("avg negative chunks:  Inline+F=%.3f  Inline=%.3f  (reduction %.2fx)\n",
                (double)sum_vf/Q, (double)sum_vi/Q, (double)sum_vi/sum_vf);
    std::printf("full-home negatives=%zu  filter-rejected(1 line)=%zu  false-positive=%zu  FP-rate=%.2f%%\n",
                fullhome, filtered, fp, fullhome? 100.0*fp/fullhome : 0.0);
    return 0;
}
