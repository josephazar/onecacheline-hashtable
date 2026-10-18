#pragma once
// =============================================================================
//  SwissTableChunk2 (v5) - chunk 2 lignes + AMAC à 2 étages (chaining)
// =============================================================================
//  Objectif : combiner les forces de v3 (miss rapides, 7 slots) et v4 (hit avec
//  valeur chaude) en un seul design.
//
//  LAYOUT - chunk de 128 o = 2 lignes de cache :
//      ligne 1 (0..63)   : int8_t ctrl[8] + Key   key[7]   (8 + 56)
//      ligne 2 (64..127) : Value val[7] (+ padding)
//  Clés et contrôle dans la ligne 1 ; valeurs dans la ligne 2.
//
//  AMAC À 2 ÉTAGES (vrai « Asynchronous Memory Access Chaining ») :
//    étage 0 : préfetch + sonde la ligne ctrl/clés.
//        - empty trouvé (miss)        -> fini, UNE seule ligne touchée.
//        - clé trouvée (hit)          -> préfetch la ligne des valeurs, étage 1.
//        - chunk plein, pas de clé    -> chunk suivant, reste étage 0.
//    étage 1 : la ligne des valeurs est arrivée -> lit la valeur, fini.
//  Conséquence : miss = 1 ligne (comme v3) ; hit = 2 lignes CHAÎNÉES et
//  recouvertes sur le lot (valeur jamais lue à froid, comme v4) ; 7 slots =
//  peu de débordements. Hash bon marché (1 multiplication) hérité de v4.
// =============================================================================

#include <iostream>
#include <vector>
#include <cstdint>
#include "chunk_simd.hpp"
#include <bit>
#include <string>

template<typename Key, typename Value>
class SwissTableChunk2 {
private:
    static constexpr int    SLOTS    = 7;
    static constexpr int    LANEMASK = 0x7F;  // 7 lanes utiles
    static constexpr int8_t EMPTY    = -128;
    static constexpr int8_t DELETED  = -127;
    static constexpr uint64_t PHI    = 0x9E3779B97F4A7C15ULL;

    struct alignas(64) Chunk {
        int8_t ctrl[8];        // ligne 1 : 0..7
        Key    key[SLOTS];     // ligne 1 : 8..63
        Value  val[SLOTS];     // ligne 2 : 64..119  (paddé à 128 par alignas)
    };
    static_assert(sizeof(Chunk) == 128 || sizeof(Key) != 8 || sizeof(Value) != 8,
                  "Pour des cles/valeurs de 8 o, un chunk doit faire 128 o (2 lignes).");

    std::vector<Chunk> chunks;
    size_t num_chunks;
    int    index_shift;
    size_t size;
    public: size_t used = 0, n_rebuilds = 0, n_grows = 0;   // tombstone count: non-EMPTY slots (live + DELETED), rebuild counters
    void rehash_same() { rehash(num_chunks); }   // rebuild at the same capacity (tests, timing)
    private:

    uint64_t hash(const Key& key) const { return (uint64_t)std::hash<Key>{}(key) * PHI; }
    inline void route(uint64_t h, size_t& c, int8_t& sig) const {
        c   = h >> index_shift;
        sig = (int8_t)((h >> (index_shift - 7)) & 0x7F);
    }
    static Chunk empty_chunk() { Chunk c; for (int i = 0; i < 8; ++i) c.ctrl[i] = EMPTY; return c; }

    void rehash(size_t new_nc) {
        new_nc = std::bit_ceil(new_nc);
        if (new_nc < 2) new_nc = 2;
#ifdef ASST_VERBOSE
        std::cout << "[Rehash] " << new_nc << " chunks (" << new_nc * SLOTS << " slots)..." << std::endl;
#endif
        std::vector<Chunk> old = std::move(chunks);
        num_chunks = new_nc;
        index_shift = 64 - std::countr_zero(num_chunks);
        chunks.assign(num_chunks, empty_chunk());
        size = 0; used = 0;
        for (auto& ch : old)
            for (int b = 0; b < SLOTS; ++b)
                if (ch.ctrl[b] != EMPTY && ch.ctrl[b] != DELETED)
                    insert(ch.key[b], ch.val[b]);
    }

public:
    SwissTableChunk2(size_t initial_cap = 16) : num_chunks(0), size(0) {
        rehash(initial_cap / SLOTS + 2);
    }
    size_t count() const { return size; }
    size_t slots() const { return num_chunks * SLOTS; }

    void insert(const Key& key, const Value& value) {
        if (used >= num_chunks * SLOTS * 0.8) {   // tombstone count: live + tombstones reached the max load
            if (size >= (num_chunks * SLOTS * 0.8) * 0.875) { ++n_grows; rehash(num_chunks * 2); }
            else { ++n_rebuilds; rehash(num_chunks); }   // same capacity: tombstones purged, filters recomputed
        }
        uint64_t h = hash(key);
        int8_t sig; size_t c; route(h, c, sig);
        while (true) {
            Chunk& ch = chunks[c];
            __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
            int mask = _mm_movemask_epi8(ctrl) & LANEMASK;
            if (mask != 0) {
                int b = std::countr_zero((unsigned)mask);
                used += (ch.ctrl[b] == EMPTY); ch.ctrl[b] = sig; ch.key[b] = key; ch.val[b] = value;
                size++; return;
            }
            c = (c + 1) & (num_chunks - 1);
        }
    }

    // Recherche synchrone (valeur lue à froid : 1 défaut résiduel - référence latence).
    Value* find_sync(const Key& key) {
        uint64_t h = hash(key);
        int8_t sig; size_t c; route(h, c, sig);
        while (true) {
            Chunk& ch = chunks[c];
            __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
            int sm = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(sig))) & LANEMASK;
            while (sm != 0) {
                int b = std::countr_zero((unsigned)sm);
                if (ch.key[b] == key) return &ch.val[b];
                sm &= (sm - 1);
            }
            int em = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(EMPTY))) & LANEMASK;
            if (em) return nullptr;
            c = (c + 1) & (num_chunks - 1);
        }
    }

    // Recherche par LOT - AMAC à 2 étages. out[i] = valeur (chaude) ou Value{}.
    template <int DEPTH>
    void find_batch(const Key* qs, size_t n, Value* out) {
        struct Lane { size_t qi; size_t c; int8_t sig; int slot; uint8_t st; bool active; };
        Lane L[DEPTH];
        size_t next = 0; int active = 0;
        auto load = [&](int l) {
            if (next >= n) { L[l].active = false; return; }
            size_t qi = next++;
            uint64_t h = hash(qs[qi]);
            int8_t sig; size_t c; route(h, c, sig);
            L[l] = Lane{qi, c, sig, 0, 0, true};
            __builtin_prefetch(&chunks[c], 0, 3);   // ligne 1 (ctrl + clés)
            ++active;
        };
        for (int l = 0; l < DEPTH; ++l) load(l);

        while (active > 0) {
            for (int l = 0; l < DEPTH; ++l) {
                if (!L[l].active) continue;
                Chunk& ch = chunks[L[l].c];

                if (L[l].st == 0) {                     // --- étage 0 : sonde ctrl/clés
                    __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
                    int sm = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(L[l].sig))) & LANEMASK;
                    bool found = false;
                    while (sm != 0) {
                        int b = std::countr_zero((unsigned)sm);
                        if (ch.key[b] == qs[L[l].qi]) {  // clé confirmée (ligne 1, chaude)
                            L[l].slot = b;
                            __builtin_prefetch(&ch.val[b], 0, 3); // ligne 2 : on la chaîne
                            L[l].st = 1;
                            found = true;
                            break;
                        }
                        sm &= (sm - 1);
                    }
                    if (found) continue;
                    int em = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(EMPTY))) & LANEMASK;
                    if (em) { out[L[l].qi] = Value{}; --active; load(l); }   // miss : 1 ligne
                    else { L[l].c = (L[l].c + 1) & (num_chunks - 1);
                           __builtin_prefetch(&chunks[L[l].c], 0, 3); }
                } else {                                // --- étage 1 : valeur arrivée
                    out[L[l].qi] = ch.val[L[l].slot];
                    --active; load(l);
                }
            }
        }
    }

    bool erase(const Key& key) {
        uint64_t h = hash(key);
        int8_t sig; size_t c; route(h, c, sig);
        while (true) {
            Chunk& ch = chunks[c];
            __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
            int em = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(EMPTY))) & LANEMASK;
            int sm = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(sig))) & LANEMASK;
            while (sm != 0) {
                int b = std::countr_zero((unsigned)sm);
                if (ch.key[b] == key) {
                    ch.ctrl[b] = em ? EMPTY : DELETED; if (ch.ctrl[b] == EMPTY) used--;
                    ch.key[b] = Key{}; ch.val[b] = Value{};
                    size--; return true;
                }
                sm &= (sm - 1);
            }
            if (em) return false;
            c = (c + 1) & (num_chunks - 1);
        }
    }
};
