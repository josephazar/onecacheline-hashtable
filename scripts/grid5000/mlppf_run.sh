#!/usr/bin/env bash
set -uo pipefail
TAG=${1:-node}; cd ~/htbench_work; OUT=results_mlppf_$TAG; mkdir -p $OUT
CH=~/miniconda3; [ "$(uname -m)" = aarch64 ] && CH=~/miniconda3-aarch64
source "$CH"/etc/profile.d/conda.sh; conda activate htbench
BIN=$(mktemp -d ~/htbench_work/bin_mpf_${TAG}_XXXXXX); trap 'rm -rf "$BIN"' EXIT
cp mlp_pf.cc mlp.cc $BIN/; cd $BIN
log(){ echo "[$(date '+%F %T')] $*" | tee -a ~/htbench_work/$OUT/progress.log; }
{ echo "# $TAG $(date -u +%FT%TZ)"; lscpu | grep -iE "Model name|^Architecture|CPU max|L3"; } > ~/htbench_work/$OUT/machine.txt
g++ -std=c++20 -O3 -DNDEBUG -march=native mlp_pf.cc -o mlp_pf || { log BUILD_FAIL; exit 1; }
g++ -std=c++20 -O3 -DNDEBUG -march=native mlp.cc -o mlp
log "mlp_pf"; numactl --localalloc taskset -c 4 ./mlp_pf 29 32000000 5 > ~/htbench_work/$OUT/mlp_pf.csv 2> ~/htbench_work/$OUT/mlp_pf.err; log "  rc=$?"
log "mlp";    numactl --localalloc taskset -c 4 ./mlp > ~/htbench_work/$OUT/mlp.csv 2> ~/htbench_work/$OUT/mlp.err; log "  rc=$?"
log RUN_DONE
