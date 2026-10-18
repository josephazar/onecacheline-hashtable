#!/bin/bash
# 2x2 ablation run: layout x engine, header-only (no abseil link needed).
set -e
TAG="$1"
cd ~/htbench_work
source ~/miniconda3/etc/profile.d/conda.sh 2>/dev/null && conda activate htbench 2>/dev/null || true
command -v g++ >/dev/null || { echo "NO_GXX"; exit 1; }
g++ --version | head -1
g++ -O3 -march=native -std=c++20 ablation.cc -o ablation
mkdir -p "results_ablation_$TAG"
./ablation | tee "results_ablation_$TAG/ablation.csv"
{ uname -a; lscpu 2>/dev/null | grep -iE 'model name|^Architecture'; } > "results_ablation_$TAG/machine.txt"
echo "ABLATION_DONE"
