// =============================================================================
//  simd_test.cc - validation SUR MATÉRIEL de la couche chunk_simd.hpp
// =============================================================================
//  Compare les quatre primitives (set1/load/cmpeq/movemask) à une référence
//  scalaire indépendante, sur des vecteurs aléatoires + cas plantés (aiguille,
//  EMPTY/DELETED). Lancé en tête de campagne sur CHAQUE nœud : sur x86 il
//  contrôle le chemin SSE2 natif, sur ARM le chemin NEON. Sortie :
//  "SIMD_SELFTEST_OK" (rc 0) ou un message d'échec (rc != 0).
// =============================================================================
#include "chunk_simd.hpp"
#include <cstdio>
#include <cstdint>

static int ref_match(const int8_t* c, int8_t b) {
    int m = 0; for (int i = 0; i < 16; ++i) if (c[i] == b) m |= (1 << i); return m;
}
static int ref_sign(const int8_t* c) {
    int m = 0; for (int i = 0; i < 16; ++i) if (c[i] & 0x80) m |= (1 << i); return m;
}

int main() {
    uint64_t st = 88172645463325252ULL;
    auto rnd = [&]{ st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; };
    const int8_t EMPTY = -128, DELETED = -127;

    for (int t = 0; t < 500000; ++t) {
        int8_t c[16];
        for (int i = 0; i < 16; ++i) c[i] = (int8_t)(rnd() & 0xFF);
        int8_t needle = (int8_t)(rnd() & 0xFF);
        // plante des cas réalistes : aiguille présente, et octets EMPTY/DELETED
        switch (t % 4) {
            case 0: c[rnd() % 16] = needle; break;
            case 1: c[rnd() % 16] = EMPTY;  break;
            case 2: c[rnd() % 16] = DELETED; needle = EMPTY; break;
            default: break;
        }
        __m128i v = _mm_loadu_si128((const __m128i*)c);
        int got_match = _mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8(needle))) & 0xFFFF;
        int got_sign  = _mm_movemask_epi8(v) & 0xFFFF;
        if (got_match != ref_match(c, needle) || got_sign != ref_sign(c)) {
            printf("SIMD_SELFTEST_FAIL t=%d  match got=%04x exp=%04x  sign got=%04x exp=%04x\n",
                   t, got_match, ref_match(c, needle), got_sign, ref_sign(c));
            printf("  needle=%02x  ctrl=", (uint8_t)needle);
            for (int i = 0; i < 16; ++i) printf("%02x ", (uint8_t)c[i]);
            printf("\n");
            return 1;
        }
    }
    printf("SIMD_SELFTEST_OK\n");
    return 0;
}
