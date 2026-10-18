#pragma once
// =============================================================================
//  SwissTableH1H2 - variante du sondage avec split H1/H2 façon Abseil
// =============================================================================
//  Différence avec AsyncSimdSwissTable : la dérivation index/signature.
//
//    Ancienne version :  idx = (h & (cap-1)) & ~31   ;  sig = h & 0x7F
//        -> l'index et la signature partagent les bits bas (recouvrement) :
//           dans un même groupe, les clés ont des bits bas voisins, donc des
//           signatures peu discriminantes -> plus de faux positifs à filtrer.
//
//    Nouvelle version (H1/H2, disjoints) :
//        H1 = h >> 7   -> choisit le groupe   (bits HAUTS)
//        H2 = h & 0x7F -> octet de contrôle   (bits BAS)
//        -> signature indépendante de l'index : filtre SIMD bien plus sélectif.
//
//  Tout le reste (SoA, sondage SIMD AVX2, tombstones, moteur AMAC find_batch,
//  coroutine async_find) est identique à la version précédente.
// =============================================================================

#include <iostream>
#include <vector>
#include <cstdint>
#include <immintrin.h>
#include <bit>
#include <string>

#include "coro_task.hpp" // CoroutinePool + Task<T> partagés
#include <new>
// 64-byte-aligned storage.  A multi-GB std::vector comes from mmap at page+16, so half of the
// 32-byte control groups straddled two cache lines and the staged engine prefetched only the first one.
template<typename T> struct CacheAlignedAlloc {
    using value_type = T;
    CacheAlignedAlloc() = default; template<typename U> CacheAlignedAlloc(const CacheAlignedAlloc<U>&) {}
    T* allocate(size_t n) { return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t(64))); }
    void deallocate(T* p, size_t) { ::operator delete(p, std::align_val_t(64)); }
    template<typename U> bool operator==(const CacheAlignedAlloc<U>&) const { return true; }
    template<typename U> bool operator!=(const CacheAlignedAlloc<U>&) const { return false; }
};
template<typename T> using AVec = std::vector<T, CacheAlignedAlloc<T>>;

template<typename Key, typename Value>
class SwissTableH1H2 {
private:
    static constexpr size_t GROUP_SIZE = 32;
    static constexpr int8_t EMPTY = -128;
    static constexpr int8_t DELETED = -127;

    AVec<int8_t> control_bytes;
    AVec<Key> keys;
    AVec<Value> values;

    size_t capacity;
    size_t size;
    public: size_t used = 0, n_rebuilds = 0, n_grows = 0;   // tombstone count: non-EMPTY slots (live + DELETED), rebuild counters
    void rehash_same() { rehash(capacity); }   // rebuild at the same capacity (tests, timing)
    private:

    size_t hash(const Key& key) const {
        uint64_t h = std::hash<Key>{}(key);
        // Avalanche MurmurHash3 (std::hash<uint64_t> = identité)
        h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    // *** LE changement clé : split H1/H2 (index et signature disjoints) ***
    //   H2 = 7 bits bas  -> octet de contrôle (toujours dans [0,127], bit de signe 0)
    //   H1 = bits hauts  -> groupe ciblé, indépendant de H2
    inline void route(size_t h, size_t& idx, int8_t& sig) const {
        sig = static_cast<int8_t>(h & 0x7F);                       // H2
        idx = ((h >> 7) & (capacity - 1)) & ~(GROUP_SIZE - 1);     // H1
    }

    void rehash(size_t new_capacity) {
        new_capacity = std::bit_ceil(new_capacity);
        if (new_capacity < 32) new_capacity = 32;
#ifdef ASST_VERBOSE
        std::cout << "[Rehash] Agrandissement vers " << new_capacity << " slots..." << std::endl;
#endif
        AVec<int8_t> old_ctrl = std::move(control_bytes);
        AVec<Key> old_keys = std::move(keys);
        AVec<Value> old_values = std::move(values);

        capacity = new_capacity;
        control_bytes.assign(capacity, EMPTY);
        keys.resize(capacity);
        values.resize(capacity);
        size = 0; used = 0;

        for (size_t i = 0; i < old_ctrl.size(); ++i)
            if (old_ctrl[i] != EMPTY && old_ctrl[i] != DELETED)
                insert(old_keys[i], old_values[i]);
    }

public:
    SwissTableH1H2(size_t initial_cap = 32) : size(0) { rehash(initial_cap); }

    size_t count() const { return size; }
    size_t slots() const { return capacity; }

    // ---------------------------------------------------------
    // Insertion (recyclage des tombstones via movemask du bit de signe)
    // ---------------------------------------------------------
    void insert(const Key& key, const Value& value) {
        if (used >= capacity * 0.8) {   // tombstone count: live + tombstones reached the max load
            if (size >= (capacity * 0.8) * 0.875) { ++n_grows; rehash(capacity * 2); }
            else { ++n_rebuilds; rehash(capacity); }   // same capacity: tombstones purged, filters recomputed
        }

        size_t h = hash(key);
        int8_t sig; size_t idx; route(h, idx, sig);

        while (true) {
            __m256i ctrl = _mm256_loadu_si256((__m256i*)&control_bytes[idx]);
            int mask = _mm256_movemask_epi8(ctrl); // EMPTY|DELETED = bit de signe à 1
            if (mask != 0) {
                int bit = std::countr_zero((unsigned)mask);
                size_t s = idx + bit;
                used += (control_bytes[s] == EMPTY); control_bytes[s] = sig;
                keys[s] = key;
                values[s] = value;
                size++;
                return;
            }
            idx = (idx + GROUP_SIZE) & (capacity - 1);
        }
    }

    // ---------------------------------------------------------
    // Sondage SIMD d'un groupe de 32 (identique à la version précédente)
    // ---------------------------------------------------------
    enum class ProbeResult { Found, NotPresent, Continue };

    ProbeResult probe_block(size_t idx, int8_t sig, const Key& key, Value*& out) {
        __m256i ctrl = _mm256_loadu_si256((__m256i*)&control_bytes[idx]);

        __m256i match_sig = _mm256_set1_epi8(sig);
        int sig_mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(ctrl, match_sig));
        while (sig_mask != 0) {
            int bit = std::countr_zero((unsigned)sig_mask);
            if (keys[idx + bit] == key) { out = &values[idx + bit]; return ProbeResult::Found; }
            sig_mask &= (sig_mask - 1);
        }

        __m256i match_empty = _mm256_set1_epi8(EMPTY);
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(ctrl, match_empty)) != 0)
            return ProbeResult::NotPresent;

        return ProbeResult::Continue;
    }

    // ---------------------------------------------------------
    // Recherche synchrone
    // ---------------------------------------------------------
    Value* find_sync(const Key& key) {
        size_t h = hash(key);
        int8_t sig; size_t idx; route(h, idx, sig);
        while (true) {
            Value* out = nullptr;
            switch (probe_block(idx, sig, key, out)) {
                case ProbeResult::Found:      return out;
                case ProbeResult::NotPresent: return nullptr;
                case ProbeResult::Continue:   break;
            }
            idx = (idx + GROUP_SIZE) & (capacity - 1);
        }
    }

    // ---------------------------------------------------------
    // Recherche par LOT - moteur AMAC (interleaving manuel, sans coroutine)
    // ---------------------------------------------------------
    template <int DEPTH>
    void find_batch(const Key* qs, size_t n, Value** out) {
        struct Lane { size_t qi; size_t idx; int8_t sig; bool active; };
        Lane lane[DEPTH];
        size_t next = 0;
        int active = 0;

        auto load = [&](int l) {
            if (next >= n) { lane[l].active = false; return; }
            size_t qi = next++;
            size_t h = hash(qs[qi]);
            int8_t sig; size_t idx; route(h, idx, sig);
            lane[l] = Lane{qi, idx, sig, true};
            __builtin_prefetch(&control_bytes[idx], 0, 3);
            __builtin_prefetch(&keys[idx], 0, 3);
            ++active;
        };

        for (int l = 0; l < DEPTH; ++l) load(l);

        while (active > 0) {
            for (int l = 0; l < DEPTH; ++l) {
                if (!lane[l].active) continue;
                Value* res = nullptr;
                switch (probe_block(lane[l].idx, lane[l].sig, qs[lane[l].qi], res)) {
                    case ProbeResult::Continue:
                        lane[l].idx = (lane[l].idx + GROUP_SIZE) & (capacity - 1);
                        __builtin_prefetch(&control_bytes[lane[l].idx], 0, 3);
                        __builtin_prefetch(&keys[lane[l].idx], 0, 3);
                        break;
                    case ProbeResult::Found:      out[lane[l].qi] = res;     --active; load(l); break;
                    case ProbeResult::NotPresent: out[lane[l].qi] = nullptr; --active; load(l); break;
                }
            }
        }
    }


    // ---------------------------------------------------------
    // AMAC en DEUX étages sur le layout SoA (Kocberber et al.) :
    //   étage 0 : la ligne de contrôle a été préfetchée -> scan SIMD ; s'il y a des
    //             candidats, préfetch des lignes du SEUL slot candidat (ce layout a
    //             trois tableaux : une ligne de clé ET une ligne de valeur, Abseil n'en
    //             aurait qu'une) puis on rend la main ; sinon empty -> absent, ou suivant.
    //   étage 1 : les lignes du candidat sont arrivées -> vérification de la clé.
    // Une lane n'a jamais plus d'un accès dépendant en vol et ne fait jamais de
    // demand-miss dans l'anneau (find_batch ne préfetchait que les 64 premiers octets
    // des 256 octets de clés du groupe : 3 candidats sur 4 étaient des demand-miss).
    // ---------------------------------------------------------
    template <int DEPTH>
    void find_batch_staged(const Key* qs, size_t n, Value** out) {
        struct Lane { size_t qi; size_t idx; uint32_t cand; int8_t sig; uint8_t st; bool active; };
        Lane lane[DEPTH];
        size_t next = 0; int active = 0;
        auto load = [&](int l) {
            if (next >= n) { lane[l].active = false; return; }
            size_t qi = next++;
            size_t h = hash(qs[qi]); int8_t sig; size_t idx; route(h, idx, sig);
            lane[l] = Lane{qi, idx, 0u, sig, 0, true};
            __builtin_prefetch(&control_bytes[idx], 0, 3);
            ++active;
        };
        for (int l = 0; l < DEPTH; ++l) load(l);
        while (active > 0) {
            for (int l = 0; l < DEPTH; ++l) {
                Lane& L = lane[l];
                if (!L.active) continue;
                if (L.st == 0) {
                    __m256i ctrl = _mm256_loadu_si256((const __m256i*)&control_bytes[L.idx]);
                    uint32_t sm = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(ctrl, _mm256_set1_epi8(L.sig)));
                    if (sm) {
                        L.cand = sm; L.st = 1;
                        size_t s = L.idx + std::countr_zero(sm);
                        __builtin_prefetch(&keys[s], 0, 3);
                        __builtin_prefetch(&values[s], 0, 3);
                        continue;
                    }
                    uint32_t em = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(ctrl, _mm256_set1_epi8(EMPTY)));
                    if (em) { out[L.qi] = nullptr; --active; load(l); continue; }
                    L.idx = (L.idx + GROUP_SIZE) & (capacity - 1);
                    __builtin_prefetch(&control_bytes[L.idx], 0, 3);
                } else {
                    size_t s = L.idx + std::countr_zero(L.cand);
                    if (keys[s] == qs[L.qi]) { out[L.qi] = &values[s]; --active; load(l); continue; }
                    L.cand &= L.cand - 1;
                    if (L.cand) {
                        size_t s2 = L.idx + std::countr_zero(L.cand);
                        __builtin_prefetch(&keys[s2], 0, 3);
                        __builtin_prefetch(&values[s2], 0, 3);
                        continue;
                    }
                    __m256i ctrl = _mm256_loadu_si256((const __m256i*)&control_bytes[L.idx]);   // encore en cache
                    uint32_t em = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(ctrl, _mm256_set1_epi8(EMPTY)));
                    if (em) { out[L.qi] = nullptr; --active; load(l); continue; }
                    L.idx = (L.idx + GROUP_SIZE) & (capacity - 1); L.st = 0;
                    __builtin_prefetch(&control_bytes[L.idx], 0, 3);
                }
            }
        }
    }


    // même moteur en deux étages, mais la VALEUR est copiée dans out[] à
    // l'étage 1 (ligne préfetchée à l'étage 0), comme Inline copie la sienne dans l'anneau :
    // contrat de lookup identique pour toutes les cellules de l'ablation.
    template <int DEPTH>
    void find_batch_staged_v(const Key* qs, size_t n, Value* out) {
        struct Lane { size_t qi; size_t idx; uint32_t cand; int8_t sig; uint8_t st; bool active; };
        Lane lane[DEPTH];
        size_t next = 0; int active = 0;
        auto load = [&](int l) {
            if (next >= n) { lane[l].active = false; return; }
            size_t qi = next++;
            size_t h = hash(qs[qi]); int8_t sig; size_t idx; route(h, idx, sig);
            lane[l] = Lane{qi, idx, 0u, sig, 0, true};
            __builtin_prefetch(&control_bytes[idx], 0, 3);
            ++active;
        };
        for (int l = 0; l < DEPTH; ++l) load(l);
        while (active > 0) {
            for (int l = 0; l < DEPTH; ++l) {
                Lane& L = lane[l];
                if (!L.active) continue;
                if (L.st == 0) {
                    __m256i ctrl = _mm256_loadu_si256((const __m256i*)&control_bytes[L.idx]);
                    uint32_t sm = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(ctrl, _mm256_set1_epi8(L.sig)));
                    if (sm) {
                        L.cand = sm; L.st = 1;
                        size_t s = L.idx + std::countr_zero(sm);
                        __builtin_prefetch(&keys[s], 0, 3);
                        __builtin_prefetch(&values[s], 0, 3);
                        continue;
                    }
                    uint32_t em = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(ctrl, _mm256_set1_epi8(EMPTY)));
                    if (em) { out[L.qi] = Value{}; --active; load(l); continue; }
                    L.idx = (L.idx + GROUP_SIZE) & (capacity - 1);
                    __builtin_prefetch(&control_bytes[L.idx], 0, 3);
                } else {
                    size_t s = L.idx + std::countr_zero(L.cand);
                    if (keys[s] == qs[L.qi]) { out[L.qi] = values[s]; --active; load(l); continue; }
                    L.cand &= L.cand - 1;
                    if (L.cand) {
                        size_t s2 = L.idx + std::countr_zero(L.cand);
                        __builtin_prefetch(&keys[s2], 0, 3);
                        __builtin_prefetch(&values[s2], 0, 3);
                        continue;
                    }
                    __m256i ctrl = _mm256_loadu_si256((const __m256i*)&control_bytes[L.idx]);
                    uint32_t em = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(ctrl, _mm256_set1_epi8(EMPTY)));
                    if (em) { out[L.qi] = Value{}; --active; load(l); continue; }
                    L.idx = (L.idx + GROUP_SIZE) & (capacity - 1); L.st = 0;
                    __builtin_prefetch(&control_bytes[L.idx], 0, 3);
                }
            }
        }
    }

    // ---------------------------------------------------------
    // Recherche asynchrone (coroutine) - conservée pour parité
    // ---------------------------------------------------------
    Task<Value*> async_find(Key key) {
        size_t h = hash(key);
        int8_t sig; size_t idx; route(h, idx, sig);
        while (true) {
            __builtin_prefetch(&control_bytes[idx], 0, 3);
            __builtin_prefetch(&keys[idx], 0, 3);
            co_await std::suspend_always{};
            Value* out = nullptr;
            switch (probe_block(idx, sig, key, out)) {
                case ProbeResult::Found:      co_return out;
                case ProbeResult::NotPresent: co_return nullptr;
                case ProbeResult::Continue:   break;
            }
            idx = (idx + GROUP_SIZE) & (capacity - 1);
        }
    }

    // ---------------------------------------------------------
    // Suppression avec tombstones
    // ---------------------------------------------------------
    bool erase(const Key& key) {
        size_t h = hash(key);
        int8_t sig; size_t idx; route(h, idx, sig);
        while (true) {
            __m256i ctrl = _mm256_loadu_si256((__m256i*)&control_bytes[idx]);
            __m256i match_empty = _mm256_set1_epi8(EMPTY);
            int empty_mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(ctrl, match_empty));

            __m256i match_sig = _mm256_set1_epi8(sig);
            int sig_mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(ctrl, match_sig));
            while (sig_mask != 0) {
                int bit = std::countr_zero((unsigned)sig_mask);
                size_t slot = idx + bit;
                if (keys[slot] == key) {
                    control_bytes[slot] = (empty_mask != 0) ? EMPTY : DELETED; if (control_bytes[slot] == EMPTY) used--;
                    keys[slot] = Key{};
                    values[slot] = Value{};
                    size--;
                    return true;
                }
                sig_mask &= (sig_mask - 1);
            }
            if (empty_mask != 0) return false;
            idx = (idx + GROUP_SIZE) & (capacity - 1);
        }
    }
};
