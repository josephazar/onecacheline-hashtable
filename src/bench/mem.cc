// #7 memory footprint: peak resident bytes/key for each map, uniform methodology.
// argv[1] selects one map; all maps are reserve()'d to final size so no rehash
// transient inflates the number. Driver subtracts the "none" (keys+vals only)
// baseline. Run: mem none|soa|chunk|inline|paired|abseil|boost|ankerl|f14
#include "swiss_table_h1h2.hpp"
#include "swiss_table_chunk.hpp"
#include "swiss_table_chunkv.hpp"
#include "swiss_table_chunk2.hpp"
#include <absl/container/flat_hash_map.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/resource.h>

extern "C" void f14_build(const uint64_t*, const uint64_t*, size_t);

static inline uint64_t splitmix64(uint64_t x){
    x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;
    x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31);
}
static inline uint64_t value_of(uint64_t k){ return k*2654435761ULL + 1; }
static long maxrss_kb(){ struct rusage r; getrusage(RUSAGE_SELF,&r); return r.ru_maxrss; }

int main(int argc,char**argv){
    std::string m = argc>1 ? argv[1] : "none";
    const size_t N = 1ull<<24;
    std::vector<uint64_t> keys(N), vals(N);
    for(size_t i=0;i<N;i++){ keys[i]=splitmix64(i); vals[i]=value_of(keys[i]); }

    // leak the map on purpose so it is resident when we read maxrss
    if(m=="soa"){ auto*t=new SwissTableH1H2<uint64_t,uint64_t>(size_t(N/0.7)+1);
        for(size_t i=0;i<N;i++) t->insert(keys[i],vals[i]); }
    else if(m=="chunk"){ auto*t=new SwissTableChunk<uint64_t,uint64_t>(size_t(N/0.7)+1);
        for(size_t i=0;i<N;i++) t->insert(keys[i],vals[i]); }
    else if(m=="inline"){ auto*t=new SwissTableChunkV<uint64_t,uint64_t>(size_t(N/0.7)+1);
        for(size_t i=0;i<N;i++) t->insert(keys[i],vals[i]); }
    else if(m=="paired"){ auto*t=new SwissTableChunk2<uint64_t,uint64_t>(size_t(N/0.7)+1);
        for(size_t i=0;i<N;i++) t->insert(keys[i],vals[i]); }
    else if(m=="abseil"){ auto*t=new absl::flat_hash_map<uint64_t,uint64_t>(); t->reserve(N);
        for(size_t i=0;i<N;i++) (*t)[keys[i]]=vals[i]; }
    else if(m=="boost"){ auto*t=new boost::unordered_flat_map<uint64_t,uint64_t>(); t->reserve(N);
        for(size_t i=0;i<N;i++) (*t)[keys[i]]=vals[i]; }
    else if(m=="ankerl"){ auto*t=new ankerl::unordered_dense::map<uint64_t,uint64_t>(); t->reserve(N);
        for(size_t i=0;i<N;i++) (*t)[keys[i]]=vals[i]; }
    else if(m=="f14"){ f14_build(keys.data(),vals.data(),N); }
    // m=="none": baseline (keys+vals vectors only)

    std::printf("%s,%zu,%ld\n", m.c_str(), N, maxrss_kb());
    return 0;
}
