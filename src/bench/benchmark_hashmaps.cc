// =============================================================================
//  Banc d'essai : Async-SIMD Swiss Table vs l'état de l'art
// =============================================================================
//
//  Contenders :
//    - AsyncSimdSwissTable (ASYNC, pipeline de coroutines + prefetch)   <- nous
//    - AsyncSimdSwissTable (SYNC, même sondage SIMD sans coroutine)     <- nous
//    - absl::flat_hash_map          (Google Abseil - référence industrie)
//    - boost::unordered_flat_map    (Boost 1.82+ - SIMD synchrone le + rapide)
//    - ankerl::unordered_dense::map (localité de cache, dense)
//    - std::unordered_map           (separate chaining - le "sac de frappe")
//
//  Scénarios (cf. -benchmark_filter pour n'en lancer qu'un) :
//    A) Lookup_Hit    : recherches qui RÉUSSISSENT (cache chaud L1 -> RAM)
//    B) Lookup_Miss   : recherches qui ÉCHOUENT   (teste l'early-exit)
//    C) Insert        : insertions massives depuis une table vide (rehash)
//    D) Lookup_Dense  : recherches sur table à très haute densité de charge
//
//  Méthodologie : Google Benchmark gère warmup, nb d'itérations, et
//  benchmark::DoNotOptimize empêche l'élimination du code mort.
// =============================================================================

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>
#include <random>
#include <unordered_map>
#include <optional>
#include <array>
#include "zipf.hpp"

// Les tables testées (silencieuses : on ne définit pas ASST_VERBOSE)
#include "async_simd_swiss_table.hpp"  // v1 : SoA, sondage bits bas
#include "swiss_table_h1h2.hpp"        // v2 : SoA + split H1/H2
#include "swiss_table_chunk.hpp"       // v3 : chunks 64 o (1 ligne de cache/sonde)

// -----------------------------------------------------------------------------
//  Adversaires détectés à la compilation (compile même si une lib manque)
// -----------------------------------------------------------------------------
#if __has_include(<absl/container/flat_hash_map.h>)
  #include <absl/container/flat_hash_map.h>
  #define HAVE_ABSEIL 1
#endif
#if __has_include(<boost/unordered/unordered_flat_map.hpp>)
  #include <boost/unordered/unordered_flat_map.hpp>
  #define HAVE_BOOST 1
#endif
#if __has_include(<ankerl/unordered_dense.h>)
  #include <ankerl/unordered_dense.h>
  #define HAVE_ANKERL 1
#endif

// -----------------------------------------------------------------------------
//  Paramètres globaux
// -----------------------------------------------------------------------------
// Nb de requêtes par lot (batch). C'est le "travail" mesuré par itération :
// assez grand pour amortir l'event-loop async et avoir beaucoup de prefetch
// en vol simultanément.
static constexpr size_t BATCH = 8192;

// Étendue des tailles de table testées (nb d'éléments).
// 1<<12 (4 K, tient en cache) ... 1<<24 (16 M, tape dans la RAM, ~570 Mo SoA).
// Pousser MAX_N à 1<<26+ pour viser les tables > 1 Go évoquées dans l'énoncé.
#define MIN_N (1 << 12)
#define MAX_N (1 << 24)

// -----------------------------------------------------------------------------
//  Utilitaires de génération de données
// -----------------------------------------------------------------------------
// Bijection 64 bits : garantit des clés UNIQUES et bien distribuées.
// splitmix64(0..N-1) = clés présentes ; splitmix64(N..) = clés garanties absentes.
static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
static inline uint64_t value_of(uint64_t key) { return key * 2654435761ULL + 1; }

// N clés uniques à insérer.
static std::vector<uint64_t> make_keys(size_t n) {
    std::vector<uint64_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = splitmix64(i);
    return v;
}

// BATCH requêtes tirées aléatoirement (graine fixe => reproductible et
// IDENTIQUE pour tous les contenders d'un même scénario).
//   hit == true  -> clés existantes (indices aléatoires dans keys)
//   hit == false -> clés garanties absentes (splitmix64 au-delà de N)
static std::vector<uint64_t> make_queries(const std::vector<uint64_t>& keys,
                                          size_t n, bool hit) {
    std::mt19937_64 rng(0xC0FFEE);
    std::vector<uint64_t> q(BATCH);
    if (hit) {
        std::uniform_int_distribution<size_t> pick(0, n - 1);
        for (auto& x : q) x = keys[pick(rng)];
    } else {
        std::uniform_int_distribution<uint64_t> pick(n, 4ULL * n + BATCH);
        for (auto& x : q) x = splitmix64(pick(rng));
    }
    return q;
}

// BATCH requêtes selon une loi de Zipf d'exposant s (hot-set réaliste).
// Toutes existantes : rang k -> keys[k-1] (clés déjà brouillées par splitmix64,
// donc le hot-set est dispersé dans l'espace de hachage, pas spatialement groupé).
static std::vector<uint64_t> make_queries_zipf(const std::vector<uint64_t>& keys,
                                               size_t n, double s) {
    ZipfGen z(n, s);
    std::mt19937_64 rng(0xC0FFEE);
    std::vector<uint64_t> q(BATCH);
    for (auto& x : q) x = keys[z.sample(rng) - 1];
    return q;
}

// -----------------------------------------------------------------------------
//  Construction des tables (pré-réservée pour ne pas mesurer le setup)
// -----------------------------------------------------------------------------
template <class Map>
static Map build_std(const std::vector<uint64_t>& keys) {
    Map m;
    m.reserve(keys.size());
    for (auto k : keys) m[k] = value_of(k);
    return m;
}

static AsyncSimdSwissTable<uint64_t, uint64_t>
build_asst(const std::vector<uint64_t>& keys, double headroom = 0.7) {
    // Capacité initiale dimensionnée pour éviter tout rehash pendant le remplissage.
    size_t cap_hint = static_cast<size_t>(keys.size() / headroom) + 1;
    AsyncSimdSwissTable<uint64_t, uint64_t> t(cap_hint);
    for (auto k : keys) t.insert(k, value_of(k));
    return t;
}

// =============================================================================
//  SCÉNARIOS A / B / D - LOOKUP
// =============================================================================

// --- Contenders synchrones standards (absl / boost / ankerl / std) -----------
template <class Map>
static void BM_Std_Lookup(benchmark::State& state, bool hit) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    Map m = build_std<Map>(keys);
    auto queries = make_queries(keys, N, hit);

    uint64_t sum = 0;
    for (auto _ : state) {
        for (uint64_t q : queries) {
            auto it = m.find(q);
            benchmark::DoNotOptimize(it);
            if (it != m.end()) sum += it->second;
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * BATCH);
    state.SetLabel(hit ? "hit" : "miss");
}

// --- Notre table, recherche SYNCHRONE (vitesse brute SIMD, sans coroutine) ----
static void BM_ASST_SyncLookup(benchmark::State& state, bool hit) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    auto t = build_asst(keys);
    auto queries = make_queries(keys, N, hit);

    uint64_t sum = 0;
    for (auto _ : state) {
        for (uint64_t q : queries) {
            uint64_t* p = t.find_sync(q);
            benchmark::DoNotOptimize(p);
            if (p) sum += *p;
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * BATCH);
    state.SetLabel(hit ? "hit" : "miss");
}

// --- Notre table, recherche ASYNCHRONE - ANNEAU À PROFONDEUR FIXE -------------
// Run 2 : au lieu d'amorcer 8192 coroutines d'un coup puis de re-balayer tout
// le vecteur (O(tours×N)), on maintient seulement DEPTH requêtes en vol. Dès
// qu'un slot rend son résultat final, on le remplace par la requête suivante.
//  - DEPTH dimensionne le nombre de cache-miss simultanés (sature les MSHR).
//  - Couplé au CoroutinePool, chaque slot recycle son frame (zéro malloc).
template <int DEPTH>
static void BM_ASST_AsyncRing(benchmark::State& state, bool hit) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    auto t = build_asst(keys);
    auto queries = make_queries(keys, N, hit);

    std::array<std::optional<Task<uint64_t*>>, DEPTH> ring;

    for (auto _ : state) {
        uint64_t sum = 0;
        size_t next = 0;   // prochaine requête à lancer
        int alive = 0;     // slots occupés

        // Amorçage : remplir l'anneau
        for (int i = 0; i < DEPTH && next < queries.size(); ++i) {
            ring[i].emplace(t.async_find(queries[next++]));
            ++alive;
        }

        // Boucle d'exécution : on ne balaye que DEPTH slots (taille fixe)
        while (alive > 0) {
            for (int i = 0; i < DEPTH; ++i) {
                if (!ring[i]) continue;
                if (ring[i]->resume()) continue;      // encore en vol -> slot suivant

                // Terminée : on récupère le résultat
                uint64_t* p = ring[i]->get_result();
                if (p) sum += *p;

                if (next < queries.size())            // recycle le slot (frame réutilisé)
                    ring[i].emplace(t.async_find(queries[next++]));
                else { ring[i].reset(); --alive; }    // plus rien à lancer
            }
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * BATCH);
    state.SetLabel(hit ? "hit" : "miss");
}

// --- Notre table, recherche par LOT - AMAC (interleaving manuel, sans coroutine)
// La version "honnête" du MLP logiciel : machine à états microscopique, zéro
// frame, zéro allocation. C'est le seul candidat async qui puisse battre le sync.
template <int DEPTH>
static void BM_ASST_AMAC(benchmark::State& state, bool hit) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    auto t = build_asst(keys);
    auto queries = make_queries(keys, N, hit);
    std::vector<uint64_t*> out(queries.size(), nullptr);

    for (auto _ : state) {
        t.template find_batch<DEPTH>(queries.data(), queries.size(), out.data());
        uint64_t sum = 0;
        for (auto* p : out) if (p) sum += *p;
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * BATCH);
    state.SetLabel(hit ? "hit" : "miss");
}

// --- NOUVELLE VERSION H1/H2 : sync + AMAC -------------------------------------
static SwissTableH1H2<uint64_t, uint64_t>
build_h1h2(const std::vector<uint64_t>& keys, double headroom = 0.7) {
    SwissTableH1H2<uint64_t, uint64_t> t(static_cast<size_t>(keys.size() / headroom) + 1);
    for (auto k : keys) t.insert(k, value_of(k));
    return t;
}

static void BM_H1H2_Sync(benchmark::State& state, bool hit) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    auto t = build_h1h2(keys);
    auto queries = make_queries(keys, N, hit);
    uint64_t sum = 0;
    for (auto _ : state) {
        for (uint64_t q : queries) { uint64_t* p = t.find_sync(q); benchmark::DoNotOptimize(p); if (p) sum += *p; }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * BATCH);
    state.SetLabel(hit ? "hit" : "miss");
}

template <int DEPTH>
static void BM_H1H2_AMAC(benchmark::State& state, bool hit) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    auto t = build_h1h2(keys);
    auto queries = make_queries(keys, N, hit);
    std::vector<uint64_t*> out(queries.size(), nullptr);
    for (auto _ : state) {
        t.template find_batch<DEPTH>(queries.data(), queries.size(), out.data());
        uint64_t sum = 0;
        for (auto* p : out) if (p) sum += *p;
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * BATCH);
    state.SetLabel(hit ? "hit" : "miss");
}

// --- VERSION 3 : chunks compacts (sync + AMAC) --------------------------------
static SwissTableChunk<uint64_t, uint64_t>
build_chunk(const std::vector<uint64_t>& keys, double headroom = 0.7) {
    SwissTableChunk<uint64_t, uint64_t> t(static_cast<size_t>(keys.size() / headroom) + 1);
    for (auto k : keys) t.insert(k, value_of(k));
    return t;
}

static void BM_Chunk_Sync(benchmark::State& state, bool hit) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    auto t = build_chunk(keys);
    auto queries = make_queries(keys, N, hit);
    uint64_t sum = 0;
    for (auto _ : state) {
        for (uint64_t q : queries) { uint64_t* p = t.find_sync(q); benchmark::DoNotOptimize(p); if (p) sum += *p; }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * BATCH);
    state.SetLabel(hit ? "hit" : "miss");
}

template <int DEPTH>
static void BM_Chunk_AMAC(benchmark::State& state, bool hit) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    auto t = build_chunk(keys);
    auto queries = make_queries(keys, N, hit);
    std::vector<uint64_t*> out(queries.size(), nullptr);
    for (auto _ : state) {
        t.template find_batch<DEPTH>(queries.data(), queries.size(), out.data());
        uint64_t sum = 0;
        for (auto* p : out) if (p) sum += *p;
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * BATCH);
    state.SetLabel(hit ? "hit" : "miss");
}

// =============================================================================
//  SCÉNARIO E - LOOKUP ZIPF (hot-set ; exposant s = state.range(1)/100)
// =============================================================================
static constexpr double ZIPF_S = 0.99; // exposant YCSB par défaut

template <class Map>
static void BM_Std_Zipf(benchmark::State& state) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    Map m = build_std<Map>(keys);
    auto queries = make_queries_zipf(keys, N, ZIPF_S);
    uint64_t sum = 0;
    for (auto _ : state) {
        for (uint64_t q : queries) { auto it = m.find(q); benchmark::DoNotOptimize(it); if (it != m.end()) sum += it->second; }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * BATCH);
    state.SetLabel("zipf0.99");
}

static void BM_Chunk_Sync_Zipf(benchmark::State& state) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    auto t = build_chunk(keys);
    auto queries = make_queries_zipf(keys, N, ZIPF_S);
    uint64_t sum = 0;
    for (auto _ : state) {
        for (uint64_t q : queries) { uint64_t* p = t.find_sync(q); benchmark::DoNotOptimize(p); if (p) sum += *p; }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * BATCH);
    state.SetLabel("zipf0.99");
}

template <int DEPTH>
static void BM_Chunk_AMAC_Zipf(benchmark::State& state) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    auto t = build_chunk(keys);
    auto queries = make_queries_zipf(keys, N, ZIPF_S);
    std::vector<uint64_t*> out(queries.size(), nullptr);
    for (auto _ : state) {
        t.template find_batch<DEPTH>(queries.data(), queries.size(), out.data());
        uint64_t sum = 0; for (auto* p : out) if (p) sum += *p;
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * BATCH);
    state.SetLabel("zipf0.99");
}

// =============================================================================
//  SCÉNARIO C - INSERT (depuis une table vide => mesure les rehash)
// =============================================================================
template <class Map>
static void BM_Std_Insert(benchmark::State& state) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    for (auto _ : state) {
        Map m;                          // démarre vide -> croissance/rehash mesurés
        for (auto k : keys) m[k] = value_of(k);
        benchmark::DoNotOptimize(m);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * N);
}

static void BM_ASST_Insert(benchmark::State& state) {
    const size_t N = state.range(0);
    auto keys = make_keys(N);
    for (auto _ : state) {
        AsyncSimdSwissTable<uint64_t, uint64_t> t(32);  // petite -> rehash SoA répétés
        for (auto k : keys) t.insert(k, value_of(k));
        benchmark::DoNotOptimize(&t);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(int64_t(state.iterations()) * N);
}

// =============================================================================
//  Enregistrement des benchmarks
// =============================================================================
using MapStd = std::unordered_map<uint64_t, uint64_t>;
#ifdef HAVE_ABSEIL
using MapAbseil = absl::flat_hash_map<uint64_t, uint64_t>;
#endif
#ifdef HAVE_BOOST
using MapBoost = boost::unordered_flat_map<uint64_t, uint64_t>;
#endif
#ifdef HAVE_ANKERL
using MapAnkerl = ankerl::unordered_dense::map<uint64_t, uint64_t>;
#endif

#define LOOKUP_RANGE ->RangeMultiplier(8)->Range(MIN_N, MAX_N)->Unit(benchmark::kMicrosecond)
#define INSERT_RANGE ->RangeMultiplier(8)->Range(MIN_N, MAX_N)->Unit(benchmark::kMillisecond)
// Scénario D : une seule taille, juste sous le seuil de rehash de NOTRE table
// (load factor ~0.8) => densité maximale, beaucoup de collisions à résoudre.
#define DENSE_N (5 << 20)   /* ~5,24 M éléments */
#define DENSE_RANGE ->Arg(DENSE_N)->Unit(benchmark::kMicrosecond)

// ---- A) Lookup_Hit -----------------------------------------------------------
BENCHMARK_CAPTURE(BM_ASST_AsyncRing<96>, A_Hit/ASST_ring96, true) LOOKUP_RANGE;
BENCHMARK_CAPTURE(BM_ASST_AMAC<32>,      A_Hit/ASST_amac32, true) LOOKUP_RANGE;
BENCHMARK_CAPTURE(BM_H1H2_AMAC<32>,      A_Hit/H1H2_amac32, true) LOOKUP_RANGE;
BENCHMARK_CAPTURE(BM_Chunk_AMAC<32>,     A_Hit/chunk_amac32, true) LOOKUP_RANGE;
BENCHMARK_CAPTURE(BM_Chunk_Sync,         A_Hit/chunk_sync,   true) LOOKUP_RANGE;
BENCHMARK_CAPTURE(BM_ASST_SyncLookup,  A_Hit/ASST_sync,  true) LOOKUP_RANGE;
#ifdef HAVE_ABSEIL
BENCHMARK_CAPTURE(BM_Std_Lookup<MapAbseil>, A_Hit/abseil, true) LOOKUP_RANGE;
#endif
#ifdef HAVE_BOOST
BENCHMARK_CAPTURE(BM_Std_Lookup<MapBoost>,  A_Hit/boost,  true) LOOKUP_RANGE;
#endif
#ifdef HAVE_ANKERL
BENCHMARK_CAPTURE(BM_Std_Lookup<MapAnkerl>, A_Hit/ankerl, true) LOOKUP_RANGE;
#endif
BENCHMARK_CAPTURE(BM_Std_Lookup<MapStd>,    A_Hit/std,    true) LOOKUP_RANGE;

// ---- B) Lookup_Miss (early-exit) --------------------------------------------
BENCHMARK_CAPTURE(BM_ASST_AsyncRing<96>, B_Miss/ASST_ring96, false) LOOKUP_RANGE;
BENCHMARK_CAPTURE(BM_ASST_AMAC<32>,      B_Miss/ASST_amac32, false) LOOKUP_RANGE;
BENCHMARK_CAPTURE(BM_H1H2_AMAC<32>,      B_Miss/H1H2_amac32, false) LOOKUP_RANGE;
BENCHMARK_CAPTURE(BM_Chunk_AMAC<32>,     B_Miss/chunk_amac32, false) LOOKUP_RANGE;
BENCHMARK_CAPTURE(BM_Chunk_Sync,         B_Miss/chunk_sync,   false) LOOKUP_RANGE;
BENCHMARK_CAPTURE(BM_ASST_SyncLookup,  B_Miss/ASST_sync,  false) LOOKUP_RANGE;
#ifdef HAVE_ABSEIL
BENCHMARK_CAPTURE(BM_Std_Lookup<MapAbseil>, B_Miss/abseil, false) LOOKUP_RANGE;
#endif
#ifdef HAVE_BOOST
BENCHMARK_CAPTURE(BM_Std_Lookup<MapBoost>,  B_Miss/boost,  false) LOOKUP_RANGE;
#endif
#ifdef HAVE_ANKERL
BENCHMARK_CAPTURE(BM_Std_Lookup<MapAnkerl>, B_Miss/ankerl, false) LOOKUP_RANGE;
#endif
BENCHMARK_CAPTURE(BM_Std_Lookup<MapStd>,    B_Miss/std,    false) LOOKUP_RANGE;

// ---- C) Insert (rehash) ------------------------------------------------------
BENCHMARK(BM_ASST_Insert) INSERT_RANGE;
#ifdef HAVE_ABSEIL
BENCHMARK_TEMPLATE(BM_Std_Insert, MapAbseil) INSERT_RANGE;
#endif
#ifdef HAVE_BOOST
BENCHMARK_TEMPLATE(BM_Std_Insert, MapBoost) INSERT_RANGE;
#endif
#ifdef HAVE_ANKERL
BENCHMARK_TEMPLATE(BM_Std_Insert, MapAnkerl) INSERT_RANGE;
#endif
BENCHMARK_TEMPLATE(BM_Std_Insert, MapStd) INSERT_RANGE;

// ---- D) Lookup_Dense (haute densité de charge) ------------------------------
BENCHMARK_CAPTURE(BM_ASST_AsyncRing<96>, D_Dense/ASST_ring96, true) DENSE_RANGE;
BENCHMARK_CAPTURE(BM_ASST_AMAC<32>,      D_Dense/ASST_amac32, true) DENSE_RANGE;
BENCHMARK_CAPTURE(BM_H1H2_AMAC<32>,      D_Dense/H1H2_amac32, true) DENSE_RANGE;
BENCHMARK_CAPTURE(BM_Chunk_AMAC<32>,     D_Dense/chunk_amac32, true) DENSE_RANGE;
BENCHMARK_CAPTURE(BM_Chunk_Sync,         D_Dense/chunk_sync,   true) DENSE_RANGE;
BENCHMARK_CAPTURE(BM_ASST_SyncLookup,  D_Dense/ASST_sync,  true) DENSE_RANGE;
#ifdef HAVE_ABSEIL
BENCHMARK_CAPTURE(BM_Std_Lookup<MapAbseil>, D_Dense/abseil, true) DENSE_RANGE;
#endif
#ifdef HAVE_BOOST
BENCHMARK_CAPTURE(BM_Std_Lookup<MapBoost>,  D_Dense/boost,  true) DENSE_RANGE;
#endif
#ifdef HAVE_ANKERL
BENCHMARK_CAPTURE(BM_Std_Lookup<MapAnkerl>, D_Dense/ankerl, true) DENSE_RANGE;
#endif
BENCHMARK_CAPTURE(BM_Std_Lookup<MapStd>,    D_Dense/std,    true) DENSE_RANGE;

// ---- E) Lookup_Zipf (hot-set, s=0.99) ---------------------------------------
BENCHMARK_TEMPLATE(BM_Chunk_AMAC_Zipf, 32)->Name("E_Zipf/chunk_amac32") LOOKUP_RANGE;
BENCHMARK(BM_Chunk_Sync_Zipf)->Name("E_Zipf/chunk_sync")               LOOKUP_RANGE;
#ifdef HAVE_ABSEIL
BENCHMARK_TEMPLATE(BM_Std_Zipf, MapAbseil)->Name("E_Zipf/abseil")      LOOKUP_RANGE;
#endif
#ifdef HAVE_BOOST
BENCHMARK_TEMPLATE(BM_Std_Zipf, MapBoost)->Name("E_Zipf/boost")        LOOKUP_RANGE;
#endif
#ifdef HAVE_ANKERL
BENCHMARK_TEMPLATE(BM_Std_Zipf, MapAnkerl)->Name("E_Zipf/ankerl")      LOOKUP_RANGE;
#endif
BENCHMARK_TEMPLATE(BM_Std_Zipf, MapStd)->Name("E_Zipf/std")            LOOKUP_RANGE;

BENCHMARK_MAIN();
