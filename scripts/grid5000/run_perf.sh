#!/usr/bin/env bash
# Mesure des compteurs matériels (perf_event_open auto-instrumenté) pour PROUVER
# le mécanisme : misses par lookup, IPC, parallélisme mémoire (MLP).
#
#   ./run_perf.sh [log2N] [uni|zipf]
#
# Pour des chiffres publiables, figer d'abord les fréquences (nécessite root) :
#   sudo cpupower frequency-set -g performance
#   echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost          # turbo off (AMD/intel_pstate)
#   echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo  # turbo off (intel_pstate)
# et idéalement isoler un cœur (isolcpus=) + taskset -c <cpu>.
set -euo pipefail
cd "$(dirname "$0")"

N=${1:-26}        # 2^26 = 64 M par défaut (régime RAM profonde)
DIST=${2:-uni}    # uni | zipf
CPU=${PERF_CPU:-3}

echo "=== Vérification des permissions perf ==="
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "?")
echo "perf_event_paranoid = $PARANOID  (<=2 requis ; sinon: sudo sysctl kernel.perf_event_paranoid=2)"

echo "=== Compilation de perf_driver ==="
ABSL_MODS="$(pkg-config --list-all 2>/dev/null | awk '/^absl_/{print $1}')"
g++ -std=c++20 -O3 -DNDEBUG -mavx2 -march=native -I third_party -I . \
    perf_driver.cc -o perf_driver $(pkg-config --cflags --libs $ABSL_MODS)
echo "OK"

echo
echo "=== Compteurs sur la phase de lookup - N=2^$N, dist=$DIST, cœur $CPU ==="
echo "(taskset -c $CPU pour épingler le thread)"
echo
for c in chunk_amac chunk_sync soa_amac h1h2_amac abseil boost; do
    taskset -c "$CPU" ./perf_driver "$c" "$N" "$DIST" 2>/dev/null || echo "$c: échec"
done

echo
echo "Lecture : LLCmiss/lk = nb de défauts de cache de dernier niveau PAR lookup"
echo "          (le layout chunk vise ~la moitié du SoA : 1 ligne au lieu de 2)."
echo "          MLP = nb moyen de défauts L1 en vol (l1d_pend_miss.pending /"
echo "          .pending_cycles) - plus c'est haut, plus on masque la latence."
