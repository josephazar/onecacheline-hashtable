#pragma once
// =============================================================================
//  chunk_simd.hpp - couche portable pour le scan des octets de contrôle
// =============================================================================
//  Les tables (v3/v4/v5) scannent leurs 16 octets de contrôle avec quatre
//  primitives SSE2 : _mm_loadu_si128, _mm_set1_epi8, _mm_cmpeq_epi8 et
//  _mm_movemask_epi8 (cmpeq -> masque 1 bit/octet, puis countr_zero/&=x-1).
//
//  Ce header fournit ces quatre primitives :
//    * x86_64        : <immintrin.h> natif (SSE2, aucun changement).
//    * aarch64 (ARM) : équivalents NEON. _mm_movemask_epi8 n'existe pas sur ARM ;
//                      on le reconstitue (bit de signe de chaque octet -> bit i),
//                      ce qui préserve EXACTEMENT la sémantique x86 attendue par
//                      le code appelant (mêmes positions de bits, même LANEMASK).
//    * autre         : repli scalaire portable (correct, non vectorisé).
//
//  Le code des tables est ainsi identique sur toutes les architectures : seul
//  l'#include <immintrin.h> est remplacé par #include "chunk_simd.hpp".
// =============================================================================

#include <cstdint>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
// ---- x86 : SSE2 natif -------------------------------------------------------
#include <immintrin.h>

#elif defined(__aarch64__)
// ---- ARM 64 bits : NEON -----------------------------------------------------
#include <arm_neon.h>

using __m128i = uint8x16_t;

static inline __m128i _mm_loadu_si128(const __m128i* p) {
    return vld1q_u8(reinterpret_cast<const uint8_t*>(p));
}
static inline __m128i _mm_set1_epi8(int8_t v) {
    return vdupq_n_u8(static_cast<uint8_t>(v));
}
static inline __m128i _mm_cmpeq_epi8(__m128i a, __m128i b) {
    return vceqq_u8(a, b);   // 0xFF par octet égal, 0x00 sinon
}
// Reconstitue _mm_movemask_epi8 : bit i = bit de signe de l'octet i.
// (cmpeq donne 0xFF -> bit de signe à 1 ; EMPTY/DELETED ont aussi le bit 7 à 1.)
// Implementation canonique "shift-reduce" (cf. sse2neon) : on isole le bit de
// signe de chaque octet puis on l'accumule par decalages-additions successifs
// (vsra) jusqu'a regrouper 8 bits par moitie. Eprouvee sur tous les coeurs ARM.
static inline int _mm_movemask_epi8(__m128i v) {
    const uint16x8_t hi  = vreinterpretq_u16_u8(vshrq_n_u8(v, 7));        // bit de signe -> bit 0 de chaque octet
    const uint32x4_t p16 = vreinterpretq_u32_u16(vsraq_n_u16(hi, hi, 7)); // paires d'octets fusionnees
    const uint64x2_t p32 = vreinterpretq_u64_u32(vsraq_n_u32(p16, p16, 14));
    const uint8x16_t p64 = vreinterpretq_u8_u64(vsraq_n_u64(p32, p32, 28));
    return vgetq_lane_u8(p64, 0) | (static_cast<int>(vgetq_lane_u8(p64, 8)) << 8);
}

#else
// ---- Repli scalaire portable ------------------------------------------------
struct __m128i { int8_t b[16]; };

static inline __m128i _mm_loadu_si128(const __m128i* p) {
    __m128i r; const int8_t* s = reinterpret_cast<const int8_t*>(p);
    for (int i = 0; i < 16; ++i) r.b[i] = s[i];
    return r;
}
static inline __m128i _mm_set1_epi8(int8_t v) {
    __m128i r; for (int i = 0; i < 16; ++i) r.b[i] = v; return r;
}
static inline __m128i _mm_cmpeq_epi8(__m128i a, __m128i b) {
    __m128i r; for (int i = 0; i < 16; ++i) r.b[i] = (a.b[i] == b.b[i]) ? int8_t(-1) : int8_t(0);
    return r;
}
static inline int _mm_movemask_epi8(__m128i a) {
    int m = 0; for (int i = 0; i < 16; ++i) if (a.b[i] & 0x80) m |= (1 << i);
    return m;
}
#endif
