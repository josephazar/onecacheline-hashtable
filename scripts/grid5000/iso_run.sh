#!/bin/bash
set -e
TAG="$1"; cd ~/htbench_work
CH=~/miniconda3; [ "$(uname -m)" = aarch64 ] && CH=~/miniconda3-aarch64
source "$CH"/etc/profile.d/conda.sh && conda activate htbench
case "$(uname -m)" in x86_64|amd64) ISA="-mavx2 -march=native"; F14_ISA="-msse2";; *) ISA="-march=native"; F14_ISA="-march=armv8-a";; esac
ABSL_MODS="$(pkg-config --list-all 2>/dev/null | awk '/^absl_/{print $1}')"
ABSL_FLAGS="$(pkg-config --cflags --libs $ABSL_MODS 2>/dev/null)"
CXXF="-std=c++20 -O3 -DNDEBUG $ISA -pthread -I third_party -I . -I $CONDA_PREFIX/include -L $CONDA_PREFIX/lib -Wl,-rpath,$CONDA_PREFIX/lib"
FOLLY="-lfolly -lglog -lgflags -ldouble-conversion -lfmt"
mkdir -p "results_iso_$TAG"
g++ $CXXF simd_test.cc -o simd_test && ./simd_test | tee "results_iso_$TAG/simd_selftest.txt"
g++ -std=c++20 -O3 -DNDEBUG $F14_ISA -I "$CONDA_PREFIX/include" -c f14_bench.cc -o f14_bench.o
g++ $CXXF iso.cc f14_bench.o -o iso $ABSL_FLAGS $FOLLY
NUMA="numactl --localalloc"; command -v numactl >/dev/null || NUMA=""
MACHINE=$TAG $NUMA taskset -c 4 ./iso 27 | tee "results_iso_$TAG/throughput.csv"
echo "ISO_DONE"
