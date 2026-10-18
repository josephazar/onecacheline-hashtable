#!/bin/bash
# Hash-sensitivity run: build Inline probe with cheap vs strong hash, sweep sizes.
set -e
TAG="$1"
cd ~/htbench_work
source ~/miniconda3/etc/profile.d/conda.sh 2>/dev/null && conda activate htbench 2>/dev/null || true
command -v g++ >/dev/null || { echo "NO_GXX"; exit 1; }
g++ --version | head -1
for H in 0 1; do
  g++ -O3 -march=native -std=c++20 -DSTRONG_HASH=$H hash_sens.cc -o hs_$H
done
mkdir -p "results_hashsens_$TAG"
{ ./hs_0; ./hs_1 | tail -n +2; } | tee "results_hashsens_$TAG/hashsens.csv"
{ uname -a; lscpu 2>/dev/null | grep -iE 'model name|^Architecture|^CPU\(s\)'; } > "results_hashsens_$TAG/machine.txt"
echo "HASHSENS_DONE"
