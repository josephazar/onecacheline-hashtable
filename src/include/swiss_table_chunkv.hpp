#pragma once
#ifndef HT_MAXLOAD
#define HT_MAXLOAD 0.8      // seuil de rehash (fraction de slots occupés). Override
#endif                      // -DHT_MAXLOAD=0.97 pour l'étude facteur de charge (#7).
// =============================================================================
//  SwissTableChunkV (v4) - valeur CO-LOGÉE dans le chunk + hash bon marché
// =============================================================================
//  Deux changements par rapport à v3 (SwissTableChunk) :
//
//  1) VALEUR DANS LE CHUNK. Un chunk de 64 o contient ctrl + 3 clés + 3 valeurs :
//        struct Chunk { int8_t ctrl[8]; Key key[3]; Value val[3]; }; // 8+24+24=56
//     Un hit lit donc la valeur dans la MÊME ligne que le sondage (déjà chaude)
//     -> zéro défaut de cache résiduel pour la valeur (v3 en payait un, la valeur
//     étant dans un tableau séparé). Contrepartie : 3 slots/chunk au lieu de 7
//     -> davantage de débordements (chaque débordement = 1 ligne de plus).
//
//  2) HASH BON MARCHÉ. Une seule multiplication (constante de Fibonacci) au lieu
//     du finaliseur Murmur (5 opérations). Index et signature sont pris dans les
//     bits HAUTS du produit (les mieux mélangés), par décalage -> disjoints.
//
//  But : mesurer si « 1 ligne par hit, valeur incluse » + hash léger gagnent dans
//  le régime RAM profonde (et grappillent en cache), face à v3 et aux SOTA.
// =============================================================================

#include <iostream>
#include <vector>
#include <cstdint>
#include "chunk_simd.hpp"
#include <bit>
#include <string>
#include <optional>
#include "coro_task.hpp"   // moteur coroutine pour comparaison mesurée

template<typename Key, typename Value>
class SwissTableChunkV {
private:
    static constexpr int    SLOTS    = 3;
    static constexpr int    LANEMASK = 0x07;  // 3 lanes utiles
    static constexpr int8_t EMPTY    = -128;
    static constexpr int8_t DELETED  = -127;
    static constexpr uint64_t PHI    = 0x9E3779B97F4A7C15ULL; // 2^64 / golden ratio

    struct alignas(64) Chunk {
        int8_t ctrl[8];        // 3 utiles (+5 padding, masqués)
        Key    key[SLOTS];     // offset 8  -> 32
        Value  val[SLOTS];     // offset 32 -> 56  (clé ET valeur dans la ligne)
    };

    std::vector<Chunk> chunks;
    size_t num_chunks;
    int    index_shift;        // 64 - log2(num_chunks)
    size_t size;
    public: size_t used = 0, n_rebuilds = 0, n_grows = 0;   // tombstone count: non-EMPTY slots (live + DELETED), rebuild counters
    void rehash_same() { rehash(num_chunks); }   // rebuild at the same capacity (tests, timing)
    private:

    // Hash : par défaut bon marché (1 multiplication de Fibonacci).
    // -DSTRONG_HASH=1 : finalizer fort type murmur3 (fmix64, 2 mult + 3 xorshift)
    // pour l'étude de sensibilité de T_cpu (§ hash sensitivity).
    uint64_t hash(const Key& key) const {
        uint64_t x = (uint64_t)std::hash<Key>{}(key);
#if defined(STRONG_HASH) && STRONG_HASH
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;                 // déjà uniformément mélangé -> route depuis les bits hauts
#else
        return x * PHI;           // 1 multiplication
#endif
    }
    // Index et signature depuis les bits HAUTS du produit (bien mélangés), disjoints.
    inline void route(uint64_t h, size_t& c, int8_t& sig) const {
        c   = h >> index_shift;                              // top log2(nc) bits
        sig = (int8_t)((h >> (index_shift - 7)) & 0x7F);     // 7 bits suivants
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
    SwissTableChunkV(size_t initial_cap = 16) : num_chunks(0), size(0) {
        rehash(initial_cap / SLOTS + 2);
    }
    size_t count() const { return size; }
    size_t slots() const { return num_chunks * SLOTS; }

    void insert(const Key& key, const Value& value) {
        if (used >= num_chunks * SLOTS * HT_MAXLOAD) {   // tombstone count: live + tombstones reached the max load
            if (size >= (num_chunks * SLOTS * HT_MAXLOAD) * 0.875) { ++n_grows; rehash(num_chunks * 2); }
            else { ++n_rebuilds; rehash(num_chunks); }   // same capacity: tombstones purged, filters recomputed
        }
        uint64_t h = hash(key);
        int8_t sig; size_t c; route(h, c, sig);
        while (true) {
            Chunk& ch = chunks[c];
            __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
            int mask = _mm_movemask_epi8(ctrl) & LANEMASK;   // EMPTY|DELETED
            if (mask != 0) {
                int b = std::countr_zero((unsigned)mask);
                used += (ch.ctrl[b] == EMPTY); ch.ctrl[b] = sig; ch.key[b] = key; ch.val[b] = value;
                size++; return;
            }
            c = (c + 1) & (num_chunks - 1);
        }
    }

    enum class ProbeResult { Found, NotPresent, Continue };

    // La valeur est COPIÉE dans outv pendant que la ligne du chunk est chaude.
    ProbeResult probe_chunk(size_t c, int8_t sig, const Key& key, Value& outv) {
        Chunk& ch = chunks[c];
        __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
        int sm = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(sig))) & LANEMASK;
        while (sm != 0) {
            int b = std::countr_zero((unsigned)sm);
            if (ch.key[b] == key) { outv = ch.val[b]; return ProbeResult::Found; }
            sm &= (sm - 1);
        }
        int em = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(EMPTY))) & LANEMASK;
        return em ? ProbeResult::NotPresent : ProbeResult::Continue;
    }

    // Recherche synchrone (renvoie un pointeur vers la valeur dans le chunk).
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

    // Longueur de sondage (nb de chunks visités) - étude de robustesse du hash (#8).
    size_t probe_len(const Key& key) {
        uint64_t h = hash(key);
        int8_t sig; size_t c; route(h, c, sig);
        size_t steps = 1;
        while (true) {
            Chunk& ch = chunks[c];
            __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
            int sm = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(sig))) & LANEMASK;
            while (sm != 0) {
                int b = std::countr_zero((unsigned)sm);
                if (ch.key[b] == key) return steps;
                sm &= (sm - 1);
            }
            int em = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(EMPTY))) & LANEMASK;
            if (em) return steps;
            c = (c + 1) & (num_chunks - 1);
            ++steps;
        }
    }

    // Octets alloués par la table (pour l'étude d'empreinte mémoire #7).
    size_t bytes() const { return chunks.capacity() * sizeof(Chunk); }
    size_t slot_cap() const { return num_chunks * SLOTS; }   // capacité en slots (#7)
    static constexpr size_t chunk_bytes() { return sizeof(Chunk); }

    // Recherche par LOT (AMAC). out[i] reçoit la valeur (copiée à chaud) ou Value{}.
    // 1 seul prefetch par sonde ; la valeur ne coûte PLUS de défaut résiduel.
    template <int DEPTH>
    void find_batch(const Key* qs, size_t n, Value* out) {
        struct Lane { size_t qi; size_t c; int8_t sig; bool active; };
        Lane lane[DEPTH];
        size_t next = 0; int active = 0;
        auto load = [&](int l) {
            if (next >= n) { lane[l].active = false; return; }
            size_t qi = next++;
            uint64_t h = hash(qs[qi]);
            int8_t sig; size_t c; route(h, c, sig);
            lane[l] = Lane{qi, c, sig, true};
            __builtin_prefetch(&chunks[c], 0, 3);
            ++active;
        };
        for (int l = 0; l < DEPTH; ++l) load(l);
        while (active > 0) {
            for (int l = 0; l < DEPTH; ++l) {
                if (!lane[l].active) continue;
                Value v{};
                switch (probe_chunk(lane[l].c, lane[l].sig, qs[lane[l].qi], v)) {
                    case ProbeResult::Continue:
                        lane[l].c = (lane[l].c + 1) & (num_chunks - 1);
                        __builtin_prefetch(&chunks[lane[l].c], 0, 3);
                        break;
                    case ProbeResult::Found:      out[lane[l].qi] = v;       --active; load(l); break;
                    case ProbeResult::NotPresent: out[lane[l].qi] = Value{};  --active; load(l); break;
                }
            }
        }
    }

    // Recherche par LOT (AMAC), variante POINTEUR : out[i] reçoit l'adresse de la
    // valeur dans le chunk (nullptr si absente). Même moteur que find_batch ;
    // permet les mises à jour en place par lot (workloads mixtes lecture/écriture).
    template <int DEPTH>
    void find_batch_ptr(const Key* qs, size_t n, Value** out) {
        struct Lane { size_t qi; size_t c; int8_t sig; bool active; };
        Lane lane[DEPTH];
        size_t next = 0; int active = 0;
        auto load = [&](int l) {
            if (next >= n) { lane[l].active = false; return; }
            size_t qi = next++;
            uint64_t h = hash(qs[qi]);
            int8_t sig; size_t c; route(h, c, sig);
            lane[l] = Lane{qi, c, sig, true};
            __builtin_prefetch(&chunks[c], 0, 3);
            ++active;
        };
        for (int l = 0; l < DEPTH; ++l) load(l);
        while (active > 0) {
            for (int l = 0; l < DEPTH; ++l) {
                if (!lane[l].active) continue;
                Chunk& ch = chunks[lane[l].c];
                __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
                int sm = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(lane[l].sig))) & LANEMASK;
                Value* hit = nullptr;
                while (sm != 0) {
                    int b = std::countr_zero((unsigned)sm);
                    if (ch.key[b] == qs[lane[l].qi]) { hit = &ch.val[b]; break; }
                    sm &= (sm - 1);
                }
                if (hit) { out[lane[l].qi] = hit; --active; load(l); continue; }
                int em = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(EMPTY))) & LANEMASK;
                if (em) { out[lane[l].qi] = nullptr; --active; load(l); }
                else {
                    lane[l].c = (lane[l].c + 1) & (num_chunks - 1);
                    __builtin_prefetch(&chunks[lane[l].c], 0, 3);
                }
            }
        }
    }


    // ---------------------------------------------------------
    // moteur COROUTINE sur le chunk Inline : même sonde, même prefetch,
    // même profondeur que find_batch ; seule la mécanique de suspension change
    // (frame de coroutine recyclé par CoroutinePool, resume() indirect). Donne la
    // mesure directe "coroutine vs anneau manuel" que le titre du papier suppose.
    // ---------------------------------------------------------
    Task<Value> coro_find(Key key, size_t c, int8_t sig) {   // (c, sig) routés et ligne déjà préfetchée par l'anneau
        for (;;) {
            co_await std::suspend_always{};                    // la ligne arrive pendant que les autres lanes tournent
            Value v{};
            switch (probe_chunk(c, sig, key, v)) {             // SIMD hors du frame (cf. v1)
                case ProbeResult::Found:      co_return v;
                case ProbeResult::NotPresent: co_return Value{};
                case ProbeResult::Continue:   break;
            }
            c = (c + 1) & (num_chunks - 1);
            __builtin_prefetch(&chunks[c], 0, 3);
        }
    }
    template <int DEPTH>
    void find_coro(const Key* qs, size_t n, Value* out) {
        // comme load() du moteur manuel : hash, routage et PREMIER prefetch
        // sont faits ici, immédiatement, puis la coroutine est créée suspendue ; sa première
        // reprise sonde une ligne déjà demandée (pas de visite d'anneau perdue).
        std::optional<Task<Value>> ring[DEPTH]; size_t qi_of[DEPTH];
        size_t next = 0; int alive = 0;
        auto launch = [&](int i) {
            size_t qi = next++; uint64_t h = hash(qs[qi]); int8_t sig; size_t c; route(h, c, sig);
            __builtin_prefetch(&chunks[c], 0, 3);
            qi_of[i] = qi; ring[i].emplace(coro_find(qs[qi], c, sig));
        };
        for (int i = 0; i < DEPTH && next < n; ++i) { launch(i); ++alive; }
        while (alive > 0) {
            for (int i = 0; i < DEPTH; ++i) {
                if (!ring[i]) continue;
                if (ring[i]->resume()) continue;
                out[qi_of[i]] = ring[i]->get_result();
                if (next < n) launch(i); else { ring[i].reset(); --alive; }
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
