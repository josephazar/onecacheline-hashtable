#!/bin/bash
set -e
TAG="$1"; cd ~/htbench_work
CH=~/miniconda3; [ "$(uname -m)" = aarch64 ] && CH=~/miniconda3-aarch64
source "$CH"/etc/profile.d/conda.sh && conda activate htbench
case "$(uname -m)" in x86_64|amd64) ISA="-mavx2 -march=native";; *) ISA="-march=native";; esac
ABSL_MODS="$(pkg-config --list-all 2>/dev/null | awk '/^absl_/{print $1}')"
ABSL_FLAGS="$(pkg-config --cflags --libs $ABSL_MODS 2>/dev/null)"
CXXF="-std=c++20 -O3 -DNDEBUG $ISA -pthread -I third_party -I . -I $CONDA_PREFIX/include -L $CONDA_PREFIX/lib -Wl,-rpath,$CONDA_PREFIX/lib"
mkdir -p "results_ycsb_$TAG"
g++ $CXXF simd_test.cc -o simd_test && ./simd_test | tee "results_ycsb_$TAG/simd_selftest.txt"
g++ $CXXF ycsb.cc -o ycsb $ABSL_FLAGS
NUMA="numactl --localalloc"; command -v numactl >/dev/null || NUMA=""
MACHINE=$TAG $NUMA taskset -c 4 ./ycsb 27 | tee "results_ycsb_$TAG/ycsb.csv"
echo "YCSB_DONE"
