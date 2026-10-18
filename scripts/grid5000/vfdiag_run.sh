#!/bin/bash
set -e
TAG="$1"; cd ~/htbench_work
CH=~/miniconda3; [ "$(uname -m)" = aarch64 ] && CH=~/miniconda3-aarch64
source "$CH"/etc/profile.d/conda.sh && conda activate htbench
case "$(uname -m)" in x86_64|amd64) ISA="-mavx2 -march=native";; *) ISA="-march=native";; esac
CXXF="-std=c++20 -O3 -DNDEBUG $ISA -pthread -I third_party -I ."
mkdir -p "results_vfdiag_$TAG"
g++ $CXXF simd_test.cc -o simd_test && ./simd_test | tee "results_vfdiag_$TAG/simd_selftest.txt"
g++ $CXXF vfdiag.cc -o vfdiag
NUMA="numactl --localalloc"; command -v numactl >/dev/null || NUMA=""
MACHINE=$TAG $NUMA taskset -c 4 ./vfdiag 27 | tee "results_vfdiag_$TAG/vfdiag.csv"
echo "VFDIAG_DONE"
