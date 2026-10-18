#pragma once
#ifndef HT_MAXLOAD
#define HT_MAXLOAD 0.8      // seuil de rehash; override -DHT_MAXLOAD pour l'étude
#endif                      // facteur de charge (#7).
// =============================================================================
//  SwissTableChunk - layout "chunk" compact (1 ligne de cache par sonde)
// =============================================================================
//  Versions précédentes : SoA = control_bytes[] et keys[] dans deux tableaux
//  séparés -> 2 lignes de cache par sonde (scan contrôle + vérif clé).
//
//  Ici (inspiré de F14 / abseil) : la table est un tableau de CHUNKS de 64 o
//  (= une ligne de cache), chacun contenant SES octets de contrôle ET SES clés :
//
//      struct Chunk { int8_t ctrl[8]; Key keys[7]; };   // 8 + 7*8 = 64 o
//
//  Conséquence : après un match de signature, la clé est DÉJÀ dans la ligne
//  chargée -> 1 seule ligne de cache par sonde, et 1 seul prefetch (vs 2).
//  Les valeurs restent dans un tableau séparé (touché une fois, sur hit).
//
//  Routage H1/H2 conservé : H1 = h>>7 choisit le chunk, H2 = h&0x7F = signature.
//  NB : le gain "1 ligne" suppose des clés de 8 octets (uint64). Pour des clés
//  plus grandes le chunk déborde sur plusieurs lignes (toujours correct).
// =============================================================================

#include <iostream>
#include <vector>
#include <cstdint>
#include "chunk_simd.hpp"
#include <bit>
#include <string>

template<typename Key, typename Value>
class SwissTableChunk {
private:
    static constexpr int    SLOTS   = 7;     // clés par chunk
    static constexpr int    LANEMASK = 0x7F; // 7 lanes utiles (bits 0..6)
    static constexpr int8_t EMPTY   = -128;  // 0x80, bit de signe à 1
    static constexpr int8_t DELETED = -127;  // 0x81, bit de signe à 1

    struct alignas(64) Chunk {
        int8_t ctrl[8];      // 7 octets de contrôle (+1 padding, masqué)
        Key    keys[SLOTS];  // 7 clés co-localisées dans la MÊME ligne
    };
    static_assert(sizeof(Chunk) == 64 || sizeof(Key) != 8,
                  "Avec des cles de 8 octets, un chunk doit faire 64 o (1 ligne).");

    std::vector<Chunk> chunks;
    std::vector<Value> values;     // valeurs séparées : num_chunks*SLOTS
    size_t num_chunks;
    size_t size;
    public: size_t used = 0, n_rebuilds = 0, n_grows = 0;   // tombstone count: non-EMPTY slots (live + DELETED), rebuild counters
    void rehash_same() { rehash(num_chunks); }   // rebuild at the same capacity (tests, timing)
    private:

    size_t hash(const Key& key) const {
        uint64_t h = std::hash<Key>{}(key);
        h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    static Chunk empty_chunk() {
        Chunk c;
        for (int i = 0; i < 8; ++i) c.ctrl[i] = EMPTY;
        return c;
    }

    void rehash(size_t new_num_chunks) {
        new_num_chunks = std::bit_ceil(new_num_chunks);
        if (new_num_chunks < 2) new_num_chunks = 2;
#ifdef ASST_VERBOSE
        std::cout << "[Rehash] " << new_num_chunks << " chunks ("
                  << new_num_chunks * SLOTS << " slots)..." << std::endl;
#endif
        std::vector<Chunk> old_chunks = std::move(chunks);
        std::vector<Value> old_values = std::move(values);

        num_chunks = new_num_chunks;
        chunks.assign(num_chunks, empty_chunk());
        values.resize(num_chunks * SLOTS);
        size = 0; used = 0;

        for (size_t c = 0; c < old_chunks.size(); ++c)
            for (int b = 0; b < SLOTS; ++b) {
                int8_t cb = old_chunks[c].ctrl[b];
                if (cb != EMPTY && cb != DELETED)
                    insert(old_chunks[c].keys[b], old_values[c * SLOTS + b]);
            }
    }

    inline void route(size_t h, size_t& c, int8_t& sig) const {
        sig = static_cast<int8_t>(h & 0x7F);     // H2
        c   = (h >> 7) & (num_chunks - 1);        // H1 -> indice de chunk
    }

public:
    SwissTableChunk(size_t initial_cap = 16) : num_chunks(0), size(0) {
        rehash(initial_cap / SLOTS + 2);
    }

    size_t count() const { return size; }
    size_t slots() const { return num_chunks * SLOTS; }

    // ---------------------------------------------------------
    // Insertion (recyclage des tombstones)
    // ---------------------------------------------------------
    void insert(const Key& key, const Value& value) {
        if (used >= num_chunks * SLOTS * HT_MAXLOAD) {   // tombstone count: live + tombstones reached the max load
            if (size >= (num_chunks * SLOTS * HT_MAXLOAD) * 0.875) { ++n_grows; rehash(num_chunks * 2); }
            else { ++n_rebuilds; rehash(num_chunks); }   // same capacity: tombstones purged, filters recomputed
        }

        size_t h = hash(key);
        int8_t sig; size_t c; route(h, c, sig);

        while (true) {
            Chunk& ch = chunks[c];
            __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
            // EMPTY|DELETED = bit de signe à 1, dans les 7 lanes utiles
            int mask = _mm_movemask_epi8(ctrl) & LANEMASK;
            if (mask != 0) {
                int b = std::countr_zero((unsigned)mask);
                used += (ch.ctrl[b] == EMPTY); ch.ctrl[b] = sig;
                ch.keys[b] = key;
                values[c * SLOTS + b] = value;
                size++;
                return;
            }
            c = (c + 1) & (num_chunks - 1); // chunk plein -> sonde le suivant
        }
    }

    // ---------------------------------------------------------
    // Sondage d'UN chunk : 1 seule ligne de cache (ctrl + clés ensemble)
    // ---------------------------------------------------------
    enum class ProbeResult { Found, NotPresent, Continue };

    ProbeResult probe_chunk(size_t c, int8_t sig, const Key& key, Value*& out) {
        Chunk& ch = chunks[c];
        __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);

        int sm = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(sig))) & LANEMASK;
        while (sm != 0) {
            int b = std::countr_zero((unsigned)sm);
            if (ch.keys[b] == key) {          // clé DÉJÀ dans la ligne chargée
                out = &values[c * SLOTS + b]; // (seul accès hors-ligne, sur hit)
                return ProbeResult::Found;
            }
            sm &= (sm - 1);
        }
        int em = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(EMPTY))) & LANEMASK;
        if (em != 0) return ProbeResult::NotPresent; // early-exit
        return ProbeResult::Continue;
    }

    // ---------------------------------------------------------
    // Recherche synchrone
    // ---------------------------------------------------------
    Value* find_sync(const Key& key) {
        size_t h = hash(key);
        int8_t sig; size_t c; route(h, c, sig);
        while (true) {
            Value* out = nullptr;
            switch (probe_chunk(c, sig, key, out)) {
                case ProbeResult::Found:      return out;
                case ProbeResult::NotPresent: return nullptr;
                case ProbeResult::Continue:   break;
            }
            c = (c + 1) & (num_chunks - 1);
        }
    }

    // Longueur de sondage + accesseurs de charge (études #7/#8).
    size_t probe_len(const Key& key) {
        size_t h = hash(key); int8_t sig; size_t c; route(h, c, sig);
        size_t steps = 1;
        while (true) {
            Value* out = nullptr;
            ProbeResult r = probe_chunk(c, sig, key, out);
            if (r != ProbeResult::Continue) return steps;
            c = (c + 1) & (num_chunks - 1); ++steps;
        }
    }
    size_t slot_cap() const { return num_chunks * SLOTS; }

    // ---------------------------------------------------------
    // Recherche par LOT - AMAC. Un SEUL prefetch par sonde (la ligne du chunk
    // contient déjà la clé), contre deux dans les versions SoA.
    // ---------------------------------------------------------
    template <int DEPTH>
    void find_batch(const Key* qs, size_t n, Value** out) {
        struct Lane { size_t qi; size_t c; int8_t sig; bool active; };
        Lane lane[DEPTH];
        size_t next = 0;
        int active = 0;

        auto load = [&](int l) {
            if (next >= n) { lane[l].active = false; return; }
            size_t qi = next++;
            size_t h = hash(qs[qi]);
            int8_t sig; size_t c; route(h, c, sig);
            lane[l] = Lane{qi, c, sig, true};
            __builtin_prefetch(&chunks[c], 0, 3);   // 1 seul prefetch !
            ++active;
        };

        for (int l = 0; l < DEPTH; ++l) load(l);

        while (active > 0) {
            for (int l = 0; l < DEPTH; ++l) {
                if (!lane[l].active) continue;
                Value* res = nullptr;
                switch (probe_chunk(lane[l].c, lane[l].sig, qs[lane[l].qi], res)) {
                    case ProbeResult::Continue:
                        lane[l].c = (lane[l].c + 1) & (num_chunks - 1);
                        __builtin_prefetch(&chunks[lane[l].c], 0, 3);
                        break;
                    case ProbeResult::Found:      out[lane[l].qi] = res;     --active; load(l); break;
                    case ProbeResult::NotPresent: out[lane[l].qi] = nullptr; --active; load(l); break;
                }
            }
        }
    }


    // Detached en DEUX étages avec valeur matérialisée : étage 0 sonde la
    // ligne du chunk ; sur clé confirmée, préfetch de la valeur (tableau séparé) et étage 1
    // la copie dans out[]. Miss = 1 ligne ; hit = 2 lignes chaînées et recouvertes (c'est le
    // moteur de Paired appliqué au layout Detached). Contrat de lookup identique à Inline.
    template <int DEPTH>
    void find_batch_staged_v(const Key* qs, size_t n, Value* out) {
        struct Lane { size_t qi; size_t c; const Value* vp; int8_t sig; uint8_t st; bool active; };
        Lane lane[DEPTH];
        size_t next = 0; int active = 0;
        auto load = [&](int l) {
            if (next >= n) { lane[l].active = false; return; }
            size_t qi = next++;
            size_t h = hash(qs[qi]); int8_t sig; size_t c; route(h, c, sig);
            lane[l] = Lane{qi, c, nullptr, sig, 0, true};
            __builtin_prefetch(&chunks[c], 0, 3);
            ++active;
        };
        for (int l = 0; l < DEPTH; ++l) load(l);
        while (active > 0) {
            for (int l = 0; l < DEPTH; ++l) {
                Lane& L = lane[l];
                if (!L.active) continue;
                if (L.st == 1) { out[L.qi] = *L.vp; --active; load(l); continue; }
                Value* res = nullptr;
                switch (probe_chunk(L.c, L.sig, qs[L.qi], res)) {
                    case ProbeResult::Found:      L.vp = res; L.st = 1; __builtin_prefetch(res, 0, 3); break;
                    case ProbeResult::NotPresent: out[L.qi] = Value{}; --active; load(l); break;
                    case ProbeResult::Continue:   L.c = (L.c + 1) & (num_chunks - 1); __builtin_prefetch(&chunks[L.c], 0, 3); break;
                }
            }
        }
    }

    // ---------------------------------------------------------
    // Suppression avec tombstones
    // ---------------------------------------------------------
    bool erase(const Key& key) {
        size_t h = hash(key);
        int8_t sig; size_t c; route(h, c, sig);
        while (true) {
            Chunk& ch = chunks[c];
            __m128i ctrl = _mm_loadu_si128((const __m128i*)ch.ctrl);
            int em = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(EMPTY))) & LANEMASK;
            int sm = _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(sig))) & LANEMASK;
            while (sm != 0) {
                int b = std::countr_zero((unsigned)sm);
                if (ch.keys[b] == key) {
                    ch.ctrl[b] = (em != 0) ? EMPTY : DELETED; if (ch.ctrl[b] == EMPTY) used--;
                    ch.keys[b] = Key{};
                    values[c * SLOTS + b] = Value{};
                    size--;
                    return true;
                }
                sm &= (sm - 1);
            }
            if (em != 0) return false;
            c = (c + 1) & (num_chunks - 1);
        }
    }
};
