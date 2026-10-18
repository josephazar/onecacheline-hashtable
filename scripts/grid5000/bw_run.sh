#!/bin/bash
set -e
TAG="$1"; cd ~/htbench_work
CH=~/miniconda3; [ "$(uname -m)" = aarch64 ] && CH=~/miniconda3-aarch64
source "$CH"/etc/profile.d/conda.sh && conda activate htbench
g++ -std=c++20 -O3 -DNDEBUG -march=native -pthread bw.cc -o bw
mkdir -p "results_bw_$TAG"
NUMA="numactl --localalloc"; command -v numactl >/dev/null || NUMA=""
MACHINE=$TAG $NUMA ./bw | tee "results_bw_$TAG/bw.csv"
echo "BW_DONE"
