// F14ValueMap isolé dans sa propre unité de compilation.
//
// Pourquoi : folly protège F14 par une garde ODR (F14LinkCheck<mode>) où `mode`
// dépend des intrinsics activés à la compilation (Simd=SSE2/NEON, SimdAndCrc si
// SSE4.2/CRC). Le libfolly pré-construit de conda est en mode 1 (Simd, sans CRC).
// Si on compile F14 avec -march=native (SSE4.2 -> CRC) on obtient le mode 2 et le
// link échoue (undefined F14LinkCheck<2>). En isolant F14 ici et en compilant CE
// fichier avec un ISA baseline SANS CRC (-msse2 sur x86, armv8-a sur ARM), F14
// choisit le mode 1 = celui de libfolly. Le reste du harnais (nos tables) garde
// -march=native : il n'instancie pas F14, donc n'émet aucun F14LinkCheck.
//
// F14 reste pleinement SIMD (scan de chunk vectorisé) ; seul le mélange de hash
// CRC32 est absent - c'est exactement le F14 que livre folly en binaire standard.
#include <folly/container/F14Map.h>
#include <cstdint>
#include <cstddef>

static folly::F14ValueMap<uint64_t, uint64_t> g_f14;

// Construit la table (une fois, hors mesure). vals[i] = valeur associée à keys[i].
extern "C" void f14_build(const uint64_t* keys, const uint64_t* vals, size_t n) {
    g_f14.clear();
    g_f14.reserve(n);
    for (size_t i = 0; i < n; ++i) g_f14[keys[i]] = vals[i];
}

// Probe scalaire (find par requête) - même API publique que les autres SOTA,
// pas de batch-prefetch : c'est la comparaison qui isole notre apport AMAC.
// Lecture seule -> sûr en concurrence (probe_mt lit g_f14 depuis T threads).
extern "C" uint64_t f14_probe(const uint64_t* q, size_t n) {
    uint64_t s = 0;
    for (size_t i = 0; i < n; ++i) {
        auto it = g_f14.find(q[i]);
        if (it != g_f14.end()) s += it->second;
    }
    return s;
}

size_t f14_size() { return g_f14.size(); }
