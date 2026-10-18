#!/usr/bin/env bash
# =============================================================================
#  run_portable.sh - campagne reproductible sur une machine cible (ex. cluster5).
#  Capture les métadonnées, (option) fige les fréquences, build, mesure, CSV.
#
#  Usage :
#     MACHINE=cluster5 ./run_portable.sh            # détection auto des tailles
#     MACHINE=cluster5 ./run_portable.sh 20 24 26 28
#     MACHINE=cluster5 PIN=1 ./run_portable.sh      # fige fréquences (sudo)
#
#  Sorties (dans results_<MACHINE>/):
#     machine.txt        métadonnées (CPU, LLC, NUMA, hugepages, paranoid, ...)
#     throughput.csv     hit/miss/zipf x {v3,v4,v5,abseil,boost,ankerl} x tailles
#     perf.txt           compteurs HW (si perf_event_paranoid<=2)
# =============================================================================
set -uo pipefail
cd "$(dirname "$0")"

MACHINE="${MACHINE:-$(hostname -s 2>/dev/null || echo host)}"
PIN="${PIN:-0}"
CPU="${PERF_CPU:-3}"
OUT="results_${MACHINE}"
mkdir -p "$OUT"
export MACHINE

echo "=================================================================="
echo " Campagne hashtable - machine=$MACHINE  cœur=$CPU  PIN=$PIN"
echo "=================================================================="

# ---------- 0) Dépendances ----------
need=(g++ pkg-config); for t in "${need[@]}"; do command -v "$t" >/dev/null || { echo "MANQUE: $t"; exit 1; }; done
pkg-config --exists benchmark || echo "[avert] google-benchmark absent (le banc Google Benchmark ne buildera pas, les sondes oui)."
for h in absl/container/flat_hash_map.h boost/unordered/unordered_flat_map.hpp; do
  echo "#include <$h>" | g++ -std=c++20 -E -x c++ - >/dev/null 2>&1 || echo "[avert] header manquant: $h"
done
if [ ! -f third_party/ankerl/unordered_dense.h ]; then
  echo "[info] téléchargement ankerl (header unique)..."
  mkdir -p third_party/ankerl
  curl -fsSL -o third_party/ankerl/unordered_dense.h \
    https://raw.githubusercontent.com/martinus/unordered_dense/v4.4.0/include/ankerl/unordered_dense.h \
    || echo "[avert] échec téléchargement ankerl - fournir le header manuellement."
fi

# ---------- 1) Métadonnées machine ----------
{
  echo "# machine = $MACHINE   date = $(date -u +%FT%TZ)"
  echo "## CPU"; lscpu | grep -iE 'model name|^Architecture|Socket|Core|Thread|NUMA node\(s\)|CPU max|L1d|L2|L3|Flags' | sed 's/  */ /g'
  echo "## AVX"; grep -oE 'avx2|avx512f|avx512bw' /proc/cpuinfo | sort -u | tr '\n' ' '; echo
  echo "## RAM"; grep MemTotal /proc/meminfo
  echo "## NUMA"; (numactl -H 2>/dev/null | grep -E 'available|node [0-9]+ size') || echo "numactl absent"
  echo "## Hugepages"; grep -E 'HugePages_Total|Hugepagesize' /proc/meminfo
  echo "## perf_event_paranoid = $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null)"
  echo "## governor = $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
  echo "## turbo(no_turbo) = $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo n/a)  boost = $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo n/a)"
  echo "## g++"; g++ --version | head -1
} > "$OUT/machine.txt"
echo "[ok] métadonnées -> $OUT/machine.txt"
LLC_KB=$(lscpu | awk -F: '/L3/{gsub(/[^0-9]/,"",$2);print $2}' | head -1)
RAM_KB=$(awk '/MemTotal/{print $2}' /proc/meminfo)

# ---------- 2) (option) Figer les fréquences ----------
if [ "$PIN" = "1" ]; then
  echo "[pin] gouverneur performance + turbo off (sudo requis)..."
  sudo cpupower frequency-set -g performance >/dev/null 2>&1 || echo "[avert] cpupower a échoué"
  echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo >/dev/null 2>&1 || \
  echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost      >/dev/null 2>&1 || echo "[avert] turbo non modifiable"
fi

# ---------- 3) Build ----------
ABSL_MODS="$(pkg-config --list-all 2>/dev/null | awk '/^absl_/{print $1}')"
ABSL_FLAGS="$(pkg-config --cflags --libs $ABSL_MODS 2>/dev/null || true)"
CXXF="-std=c++20 -O3 -DNDEBUG -mavx2 -march=native -I third_party -I ."
echo "[build] probe_portable + perf_driver ..."
g++ $CXXF probe_portable.cc -o probe_portable $ABSL_FLAGS || { echo "BUILD probe_portable FAIL"; exit 1; }
g++ $CXXF perf_driver.cc    -o perf_driver    $ABSL_FLAGS || echo "[avert] perf_driver non buildé"
echo "[ok] build"

# ---------- 4) Tailles : auto selon LLC et RAM ----------
if [ "$#" -ge 1 ]; then
  SIZES="$*"
else
  # latency-bound = table >> LLC. On part ~8x LLC et on monte tant que la RAM suit.
  # ~4 contendeurs construits simultanément ; budget mémoire ~ keys*64 octets/table.
  SIZES="20 24"
  base=24
  # monte jusqu'à utiliser au plus ~60% de la RAM (4 tables * keys*~64o)
  for e in 25 26 27 28 29; do
    keys=$((1<<e)); mem_gb=$(( keys*64*4/1024/1024/1024 ))
    [ $(( mem_gb*1024*1024 )) -lt $(( RAM_KB*60/100 )) ] && SIZES="$SIZES $e"
  done
fi
echo "[info] tailles testées (log2 N) : $SIZES   (LLC=${LLC_KB}KB, RAM=$((RAM_KB/1024/1024))GB)"

# ---------- 5) Débit (CSV) ----------
NUMA=""
command -v numactl >/dev/null && numactl -H 2>/dev/null | grep -q 'available: [2-9]' && NUMA="numactl --cpunodebind=0 --membind=0"
[ -n "$NUMA" ] && echo "[info] multi-NUMA détecté -> $NUMA"
echo "[run] débit (médiane/5)..."
$NUMA taskset -c "$CPU" ./probe_portable $SIZES | tee "$OUT/throughput.csv"
echo "[ok] -> $OUT/throughput.csv"

# ---------- 6) Compteurs perf ----------
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 9)
if [ -x ./perf_driver ] && [ "${PARANOID:-9}" -le 2 ]; then
  echo "[run] compteurs perf (mécanisme)..."
  bigN=$(echo $SIZES | tr ' ' '\n' | sort -n | tail -1)
  {
    for d in uni miss zipf; do for c in chunk2_amac chunk_amac abseil boost; do
      $NUMA taskset -c "$CPU" ./perf_driver "$c" "$bigN" "$d" 2>/dev/null
    done; done
  } | tee "$OUT/perf.txt"
else
  echo "[skip] perf : paranoid=$PARANOID (>2). Pour activer : sudo sysctl kernel.perf_event_paranoid=2"
fi

echo "=================================================================="
echo " Terminé. Récupère le dossier : $OUT/  (machine.txt, throughput.csv, perf.txt)"
[ "$PIN" = "1" ] && echo " Restaurer turbo : sudo cpupower frequency-set -g powersave ; echo 0 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo"
echo "=================================================================="
