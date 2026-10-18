#!/bin/bash
set -e
TAG="$1"; cd ~/htbench_work
source ~/miniconda3/etc/profile.d/conda.sh 2>/dev/null && conda activate htbench 2>/dev/null || true
g++ --version | head -1
g++ -O3 -march=native -std=c++20 mlp.cc -o mlp
mkdir -p "results_mlp_$TAG"
taskset -c 4 ./mlp | tee "results_mlp_$TAG/mlp.csv"
{ lscpu 2>/dev/null | grep -iE 'model name|^Architecture'; } >> "results_mlp_$TAG/mlp.csv"
echo "MLP_DONE"
