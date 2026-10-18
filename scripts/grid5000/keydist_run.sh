#!/bin/bash
set -e
TAG="$1"; cd ~/htbench_work
source ~/miniconda3/etc/profile.d/conda.sh 2>/dev/null && conda activate htbench 2>/dev/null || true
g++ --version | head -1
for H in 0 1; do g++ -O3 -march=native -std=c++20 -DSTRONG_HASH=$H keydist.cc -o kd_$H; done
mkdir -p "results_keydist_$TAG"
{ ./kd_0; ./kd_1 | tail -n +2; } | tee "results_keydist_$TAG/keydist.csv"
echo "KEYDIST_DONE"
