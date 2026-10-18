#!/bin/bash
# Runs the three feature-store profiles with the version-2 simulator (5 reps each) into results/.
# Usage: ./fs_run.sh [BIN]      BIN defaults to ./featurestore.  On Linux wrap with: taskset -c 4 numactl --localalloc
B="${1:-./featurestore}"; mkdir -p results
$B --logn 26 --zipf 0.99 --hit 0.95 --kmed 32  --lambdas 250000,500000,1000000,2000000,3000000 --taus 20,100 --config recommender     > results/recommender.csv     2> results/recommender.log
$B --logn 26 --zipf 0.60 --hit 0.80 --kmed 8   --lambdas 1000000,2000000,4000000,8000000,12000000 --taus 20,100 --config iot-registry  > results/iot-registry.csv    2> results/iot-registry.log
$B --logn 26 --zipf 0.99 --hit 1.00 --kmed 128 --lambdas 100000,200000,400000,600000,800000 --taus 20,100 --config batch-retrieval   > results/batch-retrieval.csv 2> results/batch-retrieval.log
echo RUN_DONE
