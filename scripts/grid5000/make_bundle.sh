#!/usr/bin/env bash
# Fabrique hashtable_bundle.tar.gz : tout le nécessaire pour rejouer la campagne
# sur une machine cible (ex. cluster5). Dépendances système requises là-bas :
# g++ (C++20), pkg-config, abseil-cpp-devel, boost-devel (>=1.82), google-benchmark-devel.
set -euo pipefail
cd "$(dirname "$0")"
FILES=(
  coro_task.hpp zipf.hpp
  async_simd_swiss_table.hpp swiss_table_h1h2.hpp
  swiss_table_chunk.hpp swiss_table_chunkv.hpp swiss_table_chunk2.hpp
  probe_portable.cc perf_driver.cc
  run_portable.sh
  third_party/ankerl/unordered_dense.h
)
miss=0; for f in "${FILES[@]}"; do [ -f "$f" ] || { echo "MANQUE: $f"; miss=1; }; done
[ "$miss" = 0 ] || { echo "Bundle incomplet."; exit 1; }
chmod +x run_portable.sh
tar czf hashtable_bundle.tar.gz "${FILES[@]}"
echo "Créé : hashtable_bundle.tar.gz"
echo "Sur la cible :"
echo "  tar xzf hashtable_bundle.tar.gz && MACHINE=cluster5 PIN=1 ./run_portable.sh"
