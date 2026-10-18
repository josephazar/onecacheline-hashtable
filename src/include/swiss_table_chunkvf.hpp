#pragma once
#ifndef HT_MAXLOAD
#define HT_MAXLOAD 0.8
#endif
// =============================================================================
//  SwissTableChunkVF - Inline (valeur co-logée, 3 slots/chunk) + FILTRE de négatifs
// =============================================================================
//  Nouveauté (v3) : le chunk Inline n'utilise que 56 des 64 octets de la ligne
//  (ctrl[8] + 3 clés + 3 valeurs). Les 8 octets restants portent un FILTRE de Bloom
//  64 bits par chunk, résumant les empreintes des clés qui ont DÉBORDÉ de ce chunk
//  (home plein au moment de l'insertion). Un lookup NÉGATIF dont le chunk home est
//  plein consulte le filtre - déjà dans la ligne chargée - et termine en UNE ligne
//  si l'empreinte est absente, au lieu de scanner la chaîne de débordement.
//  Coût : 0 octet de mémoire en plus, 0 ligne de cache en plus.  But : garder le hit
//  d'Inline (1 ligne, valeur chaude) ET ramener le miss au coût de Paired (1 ligne).
#include "chunk_simd.hpp"   // shim SSE2->NEON portable (x86 + aarch64), comme les autres tables
#include <cstdint>
#include <vector>
#include <bit>
#include <functional>
#ifndef FILTER_MODE
#define FILTER_MODE 0
#endif

template<typename Key, typename Value>
class SwissTableChunkVF {
    static constexpr int    SLOTS    = 3;
    static constexpr int    LANEMASK = 0x07;
    static constexpr int8_t EMPTY    = -128;
    static constexpr int8_t DELETED  = -127;
    static constexpr uint64_t PHI    = 0x9E3779B97F4A7C15ULL;

    struct alignas(64) Chunk {
        int8_t   ctrl[8];       // 3 utiles (+5 pad, masqués)
        Key      key[SLOTS];    // 8  -> 32
        Value    val[SLOTS];    // 32 -> 56
        uint64_t of;            // 56 -> 64 : filtre de Bloom des débordements
    };

    std::vector<Chunk> chunks;
    size_t num_chunks;
    int    index_shift;
    size_t size;
    public: size_t used = 0, n_rebuilds = 0, n_grows = 0;   // tombstone count: non-EMPTY slots (live + DELETED), rebuild counters
    void rehash_same() { rehash(num_chunks); }   // rebuild at the same capacity (tests, timing)
    private:

    uint64_t hash(const Key& key) const {
        uint64_t x = (uint64_t)std::hash<Key>{}(key);
#if defined(STRONG_HASH) && STRONG_HASH
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33; return x;
#else
        return x * PHI;
#endif
    }
    inline void route(uint64_t h, size_t& c, int8_t& sig) const {
        c   = h >> index_shift;
        sig = (int8_t)((h >> (index_shift - 7)) & 0x7F);
    }
    // masque de Bloom (2 bits) depuis les bits BAS du hash (disjoints du routage)
    // FILTER_MODE 0 : original (bits 0..11 de h = key*PHI : une multiplication
    // conserve les zéros de queue, donc des clés alignées 4 Ko envoient les deux bits sur le bit 0).
    // 1 : remix fmix64 de l'entrée du filtre seulement . 2 : remix à UNE
    // multiplication, bits pris en HAUT du produit (h*C est un autre hash multiplicatif de la clé).
    // Le routage, H2, le layout et le placement des clés sont identiques dans les trois modes.
    static inline uint64_t bloom(uint64_t h) {
#if FILTER_MODE == 1
        h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33;
        return (1ull << (h & 63)) | (1ull << ((h >> 6) & 63));
#elif FILTER_MODE == 2
        uint64_t g = h * 0xC2B2AE3D27D4EB4FULL;
        return (1ull << (g >> 58)) | (1ull << ((g >> 52) & 63));
#else
        return (1ull << (h & 63)) | (1ull << ((h >> 6) & 63));
#endif
    }

    static Chunk empty_chunk() { Chunk c; for (int i=0;i<8;++i) c.ctrl[i]=EMPTY; c.of=0; return c; }

    void rehash(size_t new_nc) {
        new_nc = std::bit_ceil(new_nc); if (new_nc < 2) new_nc = 2;
        std::vector<Chunk> old = std::move(chunks);
        num_chunks = new_nc; index_shift = 64 - std::countr_zero(num_chunks);
        chunks.assign(num_chunks, empty_chunk()); size = 0; used = 0;
        for (auto& ch : old)
            for (int b=0;b<SLOTS;++b)
                if (ch.ctrl[b]!=EMPTY && ch.ctrl[b]!=DELETED) insert(ch.key[b], ch.val[b]);
    }

    enum class ProbeResult { Found, NotPresent, Continue };
    ProbeResult probe_chunk(size_t c, int8_t sig, const Key& key, Value& outv) {
        Chunk& ch = chunks[c];
        __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
        int sm = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(sig))) & LANEMASK;
        while (sm) { int b = std::countr_zero((unsigned)sm);
            if (ch.key[b]==key){ outv=ch.val[b]; return ProbeResult::Found; } sm &= sm-1; }
        int em = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(EMPTY))) & LANEMASK;
        return em ? ProbeResult::NotPresent : ProbeResult::Continue;
    }

public:
    SwissTableChunkVF(size_t initial_cap = 16) : num_chunks(0), size(0) {
        rehash(initial_cap / SLOTS + 2);
    }
    size_t count() const { return size; }
    size_t slot_cap() const { return num_chunks * SLOTS; }
    size_t bytes() const { return chunks.capacity() * sizeof(Chunk); }
    static constexpr size_t chunk_bytes() { return sizeof(Chunk); }

    // Robustesse (#3) : nb de chunks visités par un lookup NÉGATIF, et si le filtre
    // au home a court-circuité (filtered=1) ou non (faux positif / home non plein).
    size_t neg_chunks(const Key& key, int& filtered) {
        filtered = 0;
        uint64_t h = hash(key); int8_t sig; size_t c; route(h, c, sig);
        bool home = true; size_t steps = 1;
        while (true) {
            Value v{};
            ProbeResult r = probe_chunk(c, sig, key, v);
            if (r != ProbeResult::Continue) return steps;   // slot vide -> absent
            if (home) {
                uint64_t bm = bloom(h);
                if ((chunks[c].of & bm) != bm) { filtered = 1; return steps; }  // filtre
                home = false;   // faux positif ou débordement réel -> on continue
            }
            c = (c + 1) & (num_chunks - 1); ++steps;
        }
    }

    // synchronous lookup with the home-chunk filter: the scalar counterpart of
    // find_batch, used by the production-prefetch comparison (prod_prefetch.cc).  Same decision as the engine:
    // fingerprint match -> full-key compare; EMPTY in the chunk -> absent; at the home chunk, a clear filter bit
    // -> absent; otherwise walk on.
    Value* find_sync(const Key& key) {
        uint64_t h = hash(key); int8_t sig; size_t c; route(h, c, sig); bool home = true;
        while (true) {
            Chunk& ch = chunks[c];
            __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
            int sm = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(sig))) & LANEMASK;
            while (sm) { int b = std::countr_zero((unsigned)sm); if (ch.key[b] == key) return &ch.val[b]; sm &= sm - 1; }
            int em = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(EMPTY))) & LANEMASK;
            if (em) return nullptr;
            if (home) { uint64_t bm = bloom(h); if ((ch.of & bm) != bm) return nullptr; home = false; }
            c = (c + 1) & (num_chunks - 1);
        }
    }

    void insert(const Key& key, const Value& value) {
        if (used >= num_chunks * SLOTS * HT_MAXLOAD) {   // tombstone count: live + tombstones reached the max load
            if (size >= (num_chunks * SLOTS * HT_MAXLOAD) * 0.875) { ++n_grows; rehash(num_chunks * 2); }
            else { ++n_rebuilds; rehash(num_chunks); }   // same capacity: tombstones purged, filters recomputed
        }
        uint64_t h = hash(key);
        int8_t sig; size_t home; route(h, home, sig);
        size_t c = home;
        while (true) {
            Chunk& ch = chunks[c];
            __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
            int mask = _mm_movemask_epi8(ctrl) & LANEMASK;
            if (mask) {
                int b = std::countr_zero((unsigned)mask);
                used += (ch.ctrl[b] == EMPTY); ch.ctrl[b]=sig; ch.key[b]=key; ch.val[b]=value; size++;
                if (c != home) chunks[home].of |= bloom(h);   // débordé -> marquer le home
                return;
            }
            c = (c + 1) & (num_chunks - 1);
        }
    }


    // suppression : tombstone, ou EMPTY si le chunk contient déjà un EMPTY (règle
    // Abseil : un tel chunk n'a jamais été plein, aucune chaîne ne le traverse). Les bits du filtre
    // ne sont JAMAIS effacés (un filtre non comptant ne peut pas) : ils sont reconstruits par rebuild().
    bool erase(const Key& key) {
        uint64_t h = hash(key); int8_t sig; size_t c; route(h, c, sig);
        for (;;) {
            Chunk& ch = chunks[c];
            __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
            int em = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(EMPTY))) & LANEMASK;
            int sm = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(sig))) & LANEMASK;
            while (sm) { int b = std::countr_zero((unsigned)sm);
                if (ch.key[b] == key) { ch.ctrl[b] = em ? EMPTY : DELETED; if (ch.ctrl[b] == EMPTY) used--; ch.key[b] = Key{}; ch.val[b] = Value{}; size--; return true; }
                sm &= sm - 1; }
            if (em) return false;
            if ((chunks[c].of & bloom(h)) != bloom(h) && c == (h >> index_shift)) return false;   // filtre au home : absent
            c = (c + 1) & (num_chunks - 1);
        }
    }
    // Reconstruction en place (même capacité) : purge les tombstones et recalcule les filtres.
    void rebuild() { rehash(num_chunks); }
    size_t tombstones() const { size_t t = 0; for (auto& ch : chunks) for (int b = 0; b < SLOTS; ++b) t += ch.ctrl[b] == DELETED; return t; }
    // occupancy diagnostics for the overflow study: chunks whose filter is
    // non-zero (at least one key overflowed out of them) and chunks with no EMPTY slot (a negative lookup
    // whose home is such a chunk must consult the filter or walk on).
    size_t overflowed_chunks() const { size_t t = 0; for (auto& ch : chunks) t += ch.of != 0; return t; }
    size_t full_chunks() const { size_t t = 0; for (auto& ch : chunks) { bool full = true; for (int b = 0; b < SLOTS; ++b) full &= ch.ctrl[b] != EMPTY; t += full; } return t; }
    size_t chunk_count() const { return num_chunks; }

    // --- Variantes d'OPTIMISATION du chemin hit (étude vfopt) -------------------
    // Le surcoût hit résiduel (1-7 % vs Inline) est un effet de codegen : corps de
    // boucle plus gros que chunkv, chaîne if/else au lieu du switch, gestion du
    // filtre inlinée dans la boucle chaude. Trois variantes A/B :
    //   find_batch_b : structure switch identique à chunkv + bloc filtre [[unlikely]]
    //   find_batch_h : le hash est stocké dans la lane (pas de recalcul au filtre)
    //   find_batch_c : gestion du filtre externalisée en fonction froide noinline

    __attribute__((noinline,cold)) bool overflow_absent(size_t c, const Key& k) const {
        uint64_t bm = bloom(hash(k));
        return (chunks[c].of & bm) != bm;
    }

    template <int DEPTH>
    void find_batch_b(const Key* qs, size_t n, Value* out) {
        struct Lane { size_t qi; size_t c; int8_t sig; bool home; bool active; };
        Lane lane[DEPTH]; size_t next=0; int active=0;
        auto load = [&](int l){
            if (next>=n){ lane[l].active=false; return; }
            size_t qi=next++; uint64_t h=hash(qs[qi]); int8_t sig; size_t c; route(h,c,sig);
            lane[l]=Lane{qi,c,sig,true,true};
            __builtin_prefetch(&chunks[c], 0, 3); ++active;
        };
        for (int l=0;l<DEPTH;++l) load(l);
        while (active>0) {
            for (int l=0;l<DEPTH;++l) {
                if (!lane[l].active) continue;
                Value v{};
                switch (probe_chunk(lane[l].c, lane[l].sig, qs[lane[l].qi], v)) {
                    case ProbeResult::Found:      out[lane[l].qi]=v;       --active; load(l); break;
                    case ProbeResult::NotPresent: out[lane[l].qi]=Value{}; --active; load(l); break;
                    case ProbeResult::Continue:
                        if (lane[l].home) [[unlikely]] {
                            uint64_t bm = bloom(hash(qs[lane[l].qi]));
                            if ((chunks[lane[l].c].of & bm) != bm) {
                                out[lane[l].qi]=Value{}; --active; load(l); break;
                            }
                            lane[l].home=false;
                        }
                        lane[l].c=(lane[l].c+1)&(num_chunks-1);
                        __builtin_prefetch(&chunks[lane[l].c], 0, 3);
                        break;
                }
            }
        }
    }

    template <int DEPTH>
    void find_batch_h(const Key* qs, size_t n, Value* out) {
        struct Lane { uint64_t h; size_t qi; size_t c; int8_t sig; bool home; bool active; };
        Lane lane[DEPTH]; size_t next=0; int active=0;
        auto load = [&](int l){
            if (next>=n){ lane[l].active=false; return; }
            size_t qi=next++; uint64_t h=hash(qs[qi]); int8_t sig; size_t c; route(h,c,sig);
            lane[l]=Lane{h,qi,c,sig,true,true};
            __builtin_prefetch(&chunks[c], 0, 3); ++active;
        };
        for (int l=0;l<DEPTH;++l) load(l);
        while (active>0) {
            for (int l=0;l<DEPTH;++l) {
                if (!lane[l].active) continue;
                Value v{};
                switch (probe_chunk(lane[l].c, lane[l].sig, qs[lane[l].qi], v)) {
                    case ProbeResult::Found:      out[lane[l].qi]=v;       --active; load(l); break;
                    case ProbeResult::NotPresent: out[lane[l].qi]=Value{}; --active; load(l); break;
                    case ProbeResult::Continue:
                        if (lane[l].home) [[unlikely]] {
                            uint64_t bm = bloom(lane[l].h);
                            if ((chunks[lane[l].c].of & bm) != bm) {
                                out[lane[l].qi]=Value{}; --active; load(l); break;
                            }
                            lane[l].home=false;
                        }
                        lane[l].c=(lane[l].c+1)&(num_chunks-1);
                        __builtin_prefetch(&chunks[lane[l].c], 0, 3);
                        break;
                }
            }
        }
    }

    template <int DEPTH>
    void find_batch_c(const Key* qs, size_t n, Value* out) {
        struct Lane { size_t qi; size_t c; int8_t sig; bool home; bool active; };
        Lane lane[DEPTH]; size_t next=0; int active=0;
        auto load = [&](int l){
            if (next>=n){ lane[l].active=false; return; }
            size_t qi=next++; uint64_t h=hash(qs[qi]); int8_t sig; size_t c; route(h,c,sig);
            lane[l]=Lane{qi,c,sig,true,true};
            __builtin_prefetch(&chunks[c], 0, 3); ++active;
        };
        for (int l=0;l<DEPTH;++l) load(l);
        while (active>0) {
            for (int l=0;l<DEPTH;++l) {
                if (!lane[l].active) continue;
                Value v{};
                switch (probe_chunk(lane[l].c, lane[l].sig, qs[lane[l].qi], v)) {
                    case ProbeResult::Found:      out[lane[l].qi]=v;       --active; load(l); break;
                    case ProbeResult::NotPresent: out[lane[l].qi]=Value{}; --active; load(l); break;
                    case ProbeResult::Continue:
                        if (lane[l].home) [[unlikely]] {
                            if (overflow_absent(lane[l].c, qs[lane[l].qi])) {
                                out[lane[l].qi]=Value{}; --active; load(l); break;
                            }
                            lane[l].home=false;
                        }
                        lane[l].c=(lane[l].c+1)&(num_chunks-1);
                        __builtin_prefetch(&chunks[lane[l].c], 0, 3);
                        break;
                }
            }
        }
    }

    // Recherche par lot (AMAC) avec terminaison de négatif par filtre au chunk home.
    template <int DEPTH>
    void find_batch(const Key* qs, size_t n, Value* out) {
        // Lane de MÊME taille qu'Inline (pas de champ h) : le filtre ne s'active qu'au
        // home plein (rare), où l'on RECALCULE le hash paresseusement -> les hits ne
        // paient ni le calcul du bloom ni un état de lane plus large.
        struct Lane { size_t qi; size_t c; int8_t sig; bool home; bool active; };
        Lane lane[DEPTH]; size_t next=0; int active=0;
        auto load = [&](int l){
            if (next>=n){ lane[l].active=false; return; }
            size_t qi=next++; uint64_t h=hash(qs[qi]); int8_t sig; size_t c; route(h,c,sig);
            lane[l]=Lane{qi,c,sig,true,true};
            __builtin_prefetch(&chunks[c], 0, 3); ++active;
        };
        for (int l=0;l<DEPTH;++l) load(l);
        while (active>0) {
            for (int l=0;l<DEPTH;++l) {
                if (!lane[l].active) continue;
                Value v{};
                ProbeResult r = probe_chunk(lane[l].c, lane[l].sig, qs[lane[l].qi], v);
                if (r==ProbeResult::Found)      { out[lane[l].qi]=v;       --active; load(l); }
                else if (r==ProbeResult::NotPresent) { out[lane[l].qi]=Value{}; --active; load(l); }
                else {  // Continue : chunk plein, pas de match
                    if (lane[l].home) {   // au home : consulter le filtre (hash relu, rare)
                        uint64_t bm = bloom(hash(qs[lane[l].qi]));
                        if ((chunks[lane[l].c].of & bm) != bm) {
                            out[lane[l].qi]=Value{}; --active; load(l); continue;  // absent -> 1 ligne
                        }
                        lane[l].home=false;
                    }
                    lane[l].c=(lane[l].c+1)&(num_chunks-1);
                    __builtin_prefetch(&chunks[lane[l].c], 0, 3);
                }
            }
        }
    }
};
