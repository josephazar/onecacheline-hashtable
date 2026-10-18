#!/bin/bash
set -e
TAG="$1"; cd ~/htbench_work
source ~/miniconda3/etc/profile.d/conda.sh 2>/dev/null && conda activate htbench 2>/dev/null || true
ARCH=$(uname -m)
case "$ARCH" in x86_64|amd64) ISA="-mavx2 -march=native"; F14_ISA="-msse2";; *) ISA="-march=native"; F14_ISA="-march=armv8-a";; esac
ABSL_MODS="$(pkg-config --list-all 2>/dev/null | awk '/^absl_/{print $1}')"
ABSL_FLAGS="$(pkg-config --cflags --libs $ABSL_MODS 2>/dev/null)"
CXXF="-std=c++20 -O3 -DNDEBUG $ISA -pthread -I third_party -I . -I $CONDA_PREFIX/include -L $CONDA_PREFIX/lib -Wl,-rpath,$CONDA_PREFIX/lib"
FOLLY="-lfolly -lglog -lgflags -ldouble-conversion -lfmt"
g++ -std=c++20 -O3 -DNDEBUG $F14_ISA -I "$CONDA_PREFIX/include" -c f14_bench.cc -o f14_bench.o
g++ $CXXF mem.cc f14_bench.o -o mem $ABSL_FLAGS $FOLLY
mkdir -p "results_mem_$TAG"
echo "map,N,maxrss_kb" > "results_mem_$TAG/mem.csv"
for M in none soa chunk inline paired abseil boost ankerl f14; do ./mem $M >> "results_mem_$TAG/mem.csv"; done
cat "results_mem_$TAG/mem.csv"; echo "MEM_DONE"
