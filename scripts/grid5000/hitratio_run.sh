#!/bin/bash
set -e
TAG="$1"; cd ~/htbench_work
source ~/miniconda3/etc/profile.d/conda.sh 2>/dev/null && conda activate htbench 2>/dev/null || true
g++ -O3 -march=native -std=c++20 hitratio.cc -o hitratio
mkdir -p "results_hitratio_$TAG"
taskset -c 4 ./hitratio | tee "results_hitratio_$TAG/hitratio.csv"
echo "HITRATIO_DONE"
