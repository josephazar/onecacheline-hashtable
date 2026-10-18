#!/usr/bin/env bash
# Campagne complète et uniforme sur un nœud Grid'5000 : latence, débit (avec IC),
# sensibilité à la profondeur + insertion, scaling multi-thread, compteurs perf.
# Tout est comparé aux mêmes méthodes de production (abseil/boost/ankerl).
#   usage : bash ~/g5k_run.sh <TAG>     (TAG = étiquette machine, ex. nancy_gres_zen4)
set -uo pipefail
cd ~/htbench_work
# Prefixe conda specifique a l'archi (voir g5k_setup.sh) : x86 -> ~/miniconda3,
# ARM -> ~/miniconda3-aarch64.
CH=~/miniconda3; [ "$(uname -m)" = aarch64 ] && CH=~/miniconda3-aarch64
source "$CH"/etc/profile.d/conda.sh; conda activate htbench
TAG=${1:-g5k_node}; OUT=results_$TAG; mkdir -p $OUT
ABSL_MODS="$(pkg-config --list-all 2>/dev/null | awk '/^absl_/{print $1}')"
ABSL_FLAGS="$(pkg-config --cflags --libs $ABSL_MODS 2>/dev/null)"
# ISA : AVX2 sur x86 ; sur ARM la couche chunk_simd.hpp passe en NEON (baseline armv8).
case "$(uname -m)" in
  x86_64|amd64) ISA="-mavx2 -march=native" ;;
  *)            ISA="-march=native" ;;
esac
# F14 (f14_bench.cc) : ISA baseline SANS CRC32 -> folly choisit le mode "Simd"
# (= celui du libfolly conda), sinon échec de link sur la garde ODR F14LinkCheck.
# F14 reste pleinement SIMD (SSE2/NEON) ; seul le hash CRC est absent.
case "$(uname -m)" in
  x86_64|amd64) F14_ISA="-msse2" ;;
  *)            F14_ISA="-march=armv8-a" ;;
esac
CXXF="-std=c++20 -O3 -DNDEBUG $ISA -pthread -I third_party -I . -I $CONDA_PREFIX/include -L $CONDA_PREFIX/lib -Wl,-rpath,$CONDA_PREFIX/lib"
# folly::F14ValueMap (baseline chunké co-localisé) + ses deps runtime. Requis par
# probe_portable et probe_mt uniquement (latency/depth/perf ne l'utilisent pas).
FOLLY="-lfolly -lglog -lgflags -ldouble-conversion -lfmt"
{ echo "# $TAG $(date -u +%FT%TZ)"; lscpu | grep -iE "Model name|^Architecture|Socket|Core\(s\) per|Thread|NUMA node|L3|CPU max"; grep MemTotal /proc/meminfo; echo "g++=$(g++ --version|head -1)"; } > $OUT/machine.txt

# Répertoire binaire PAR JOB : plusieurs jobs d'un même site partagent ~/htbench_work
# via NFS -> sans ça, des nœuds différents (ISA différente !) s'écrasent leurs
# binaires et un Zen1 finit par exécuter un binaire AVX-512 -> Illegal instruction.
BIN=$(mktemp -d ~/htbench_work/bin_${TAG}_XXXXXX)
trap 'rm -rf "$BIN"' EXIT

echo "[simd-selftest]"   # valide la couche SSE2/NEON SUR CE matériel avant tout
g++ $CXXF simd_test.cc -o $BIN/simd_test && $BIN/simd_test | tee $OUT/simd_selftest.txt
grep -q SIMD_SELFTEST_OK $OUT/simd_selftest.txt || { echo SIMD_SELFTEST_FAIL; exit 1; }

echo "[build]"          # cœur de la campagne : doit compiler partout (x86 + ARM)
# F14 isolé dans sa propre TU avec l'ISA baseline (voir F14_ISA) -> objet lié aux 2 probes.
g++ -std=c++20 -O3 -DNDEBUG $F14_ISA -I $CONDA_PREFIX/include -c f14_bench.cc -o $BIN/f14_bench.o && \
g++ $CXXF probe_portable.cc $BIN/f14_bench.o -o $BIN/probe_portable $ABSL_FLAGS $FOLLY && \
g++ $CXXF latency.cc        -o $BIN/latency && \
g++ $CXXF probe_depth.cc    -o $BIN/probe_depth    $ABSL_FLAGS && \
g++ $CXXF probe_mt.cc       $BIN/f14_bench.o -o $BIN/probe_mt $ABSL_FLAGS $FOLLY && echo BUILD_OK || { echo BUILD_FAIL; exit 1; }
# perf_driver inclut les variantes v1/v2 (intrinsics AVX2 non portés) : build best-effort.
# Inutile sur ARM (perf bloqué) ; sur x86 il sert aux compteurs si paranoid<=2.
g++ $CXXF perf_driver.cc -o $BIN/perf_driver $ABSL_FLAGS 2>/dev/null && echo PERF_BUILD_OK || echo "PERF_BUILD_SKIP (non-x86 ou v1/v2 non portes)"

CORE=4              # cœur isolé arbitraire sur le socket 0
NPROC=$(nproc)
# numactl --membind=N jette EINVAL sur certains EPYC multi-NUMA ; --localalloc est
# robuste et, avec le pinning mono-cœur + first-touch, garde la mémoire locale.
NUMA="numactl --localalloc"
$NUMA taskset -c $CORE true 2>/dev/null || NUMA=""

echo "[latency]";       taskset -c $CORE $BIN/latency 2048 | tee $OUT/latency.csv
echo "[throughput]";    MACHINE=$TAG $NUMA taskset -c $CORE $BIN/probe_portable 20 24 26 27 28 | tee $OUT/throughput.csv
echo "[depth+insert]";  taskset -c $CORE $BIN/probe_depth 27 | tee $OUT/depth.txt

echo "[multithread]"
MACHINE=$TAG $BIN/probe_mt 26 $NPROC hit  | tee $OUT/mt_hit.csv
MACHINE=$TAG $BIN/probe_mt 26 $NPROC miss | tee $OUT/mt_miss.csv

echo "[perf-counters]"
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 3)
echo "# perf_event_paranoid=$PARANOID" > $OUT/perf.txt
if [ "$PARANOID" -le 2 ] && [ -x $BIN/perf_driver ]; then
  for who in chunk2_amac chunk_amac chunk_sync soa_amac abseil boost; do
    for d in uni miss zipf; do
      MACHINE=$TAG taskset -c $CORE $BIN/perf_driver $who 26 $d 40 2>/dev/null | tee -a $OUT/perf.txt
    done
  done
else
  echo "# perf blocked (paranoid=$PARANOID > 2); counters skipped on this node" | tee -a $OUT/perf.txt
fi
echo RUN_DONE
