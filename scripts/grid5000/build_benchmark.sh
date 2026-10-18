#!/usr/bin/env bash
# Compile le banc d'essai benchmark_hashmaps.cc.
# Les adversaires sont activés automatiquement selon les libs présentes
# (via __has_include) ; ce script ne fait que fournir les bons flags de link.
set -euo pipefail
cd "$(dirname "$0")"

CXX=${CXX:-g++}
CXXFLAGS="-std=c++20 -O3 -DNDEBUG -mavx2 -march=native -I third_party -I ."
OUT=benchmark_hashmaps

# --- Google Benchmark (obligatoire) ------------------------------------------
if ! pkg-config --exists benchmark; then
  echo "ERREUR: Google Benchmark introuvable. Installez google-benchmark-devel." >&2
  exit 1
fi
BENCH_FLAGS="$(pkg-config --cflags --libs benchmark) -pthread"

# --- Abseil (optionnel) : on link TOUS les modules absl disponibles ----------
ABSL_FLAGS=""
if pkg-config --exists absl_raw_hash_set 2>/dev/null; then
  ABSL_MODS="$(pkg-config --list-all 2>/dev/null | awk '/^absl_/{print $1}')"
  ABSL_FLAGS="$(pkg-config --cflags --libs $ABSL_MODS 2>/dev/null || true)"
  echo "[build] Abseil détecté."
else
  echo "[build] Abseil absent (le contender abseil sera désactivé)."
fi

# Boost.Unordered et ankerl sont header-only : aucun flag de link requis.
echo "[build] Boost : $(pkg-config --exists boost 2>/dev/null && echo présent || ([ -f /usr/include/boost/version.hpp ] && echo présent || echo absent))"
echo "[build] ankerl: $([ -f third_party/ankerl/unordered_dense.h ] && echo présent || echo absent)"

set -x
$CXX $CXXFLAGS benchmark_hashmaps.cc -o "$OUT" $BENCH_FLAGS $ABSL_FLAGS
set +x
echo "OK -> ./$OUT"
echo "Exemples :"
echo "  ./$OUT                                  # tout"
echo "  ./$OUT --benchmark_filter='A_Hit'       # un scénario"
echo "  ./$OUT --benchmark_filter='_async'      # comparer juste l'async"
echo "  ./$OUT --benchmark_repetitions=5 --benchmark_report_aggregates_only=true"
