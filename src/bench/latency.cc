// Mesure de la latence mémoire par lookup-line via pointer-chase à dépendance
// stricte (aucun MLP) : chaque accès dépend du précédent -> on lit la latence
// load-to-use d'une ligne de cache, du L1 jusqu'à la DRAM selon la taille.
//   args : [Mo_max]   (défaut 1024)
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
using Clock=std::chrono::steady_clock;

int main(int argc,char**argv){
    size_t MB_MAX = argc>1? std::stoull(argv[1]) : 1024;
    printf("region_MB,ns_per_access\n");
    std::mt19937_64 rng(12345);
    std::vector<size_t> sizes={1,2,4,8,16,32,64,128,256,512,MB_MAX};
    for(size_t mb : sizes){
        if(mb*1024*1024 < 64) continue;
        size_t lines = mb*1024*1024/64;           // une entrée par ligne de cache
        std::vector<uint64_t> a(lines*8, 0);       // 8 uint64 = 64 o par entrée
        // cycle aléatoire (permutation à un seul cycle) sur les indices de ligne
        std::vector<size_t> perm(lines); for(size_t i=0;i<lines;i++) perm[i]=i;
        std::shuffle(perm.begin()+1, perm.end(), rng); // garde 0 en tête
        for(size_t i=0;i<lines;i++) a[perm[i]*8] = perm[(i+1)%lines]*8; // next slot
        // chase
        size_t steps = 100'000'000;
        size_t idx=0; auto t0=Clock::now();
        for(size_t s=0;s<steps;s++) idx = a[idx];
        auto t1=Clock::now();
        volatile size_t sink=idx;(void)sink;
        double ns = std::chrono::duration<double,std::nano>(t1-t0).count()/steps;
        printf("%zu,%.2f\n", mb, ns); fflush(stdout);
    }
    return 0;
}
