#!/usr/bin/env bash
# Extended depth sweep (D=4..384, Inline + Paired, all reps) at prefetch locality 2, plus the independent
# N_M / L_m pointer-chase (mlp.cc), which the paper only has for the x86 cores.
# usage: depth2_run.sh <TAG>
set -uo pipefail
TAG=${1:-arm}; cd ~/htbench_work; OUT=results_depth2_$TAG; mkdir -p $OUT
CH=~/miniconda3; [ "$(uname -m)" = aarch64 ] && CH=~/miniconda3-aarch64
source "$CH"/etc/profile.d/conda.sh; conda activate htbench
BIN=$(mktemp -d ~/htbench_work/bin_d2_${TAG}_XXXXXX); trap 'rm -rf "$BIN"' EXIT
cp *.cc *.hpp $BIN/ 2>/dev/null; cp -r third_party $BIN/ 2>/dev/null
for f in $BIN/swiss_table_*.hpp; do sed -i -E 's/__builtin_prefetch\((.*), *0, *[0-3]\)/__builtin_prefetch(\1, 0, 2)/' $f; done
cd $BIN
CXXF="-std=c++20 -O3 -DNDEBUG -march=native -pthread -I $BIN -I $BIN/third_party"
log(){ echo "[$(date '+%F %T')] $*" | tee -a ~/htbench_work/$OUT/progress.log; }
{ echo "# $TAG $(date -u +%FT%TZ) locality 2"; lscpu | grep -iE "Model name|^Architecture|CPU max"; } > ~/htbench_work/$OUT/machine.txt
g++ $CXXF -S depth_ext.cc -o hint.s && log "hint check: pldl2keep=$(grep -c pldl2keep hint.s) pldl1keep=$(grep -c pldl1keep hint.s) pldl3keep=$(grep -c pldl3keep hint.s) prefetcht1=$(grep -c prefetcht1 hint.s)"
NUMA="numactl --localalloc"; command -v numactl >/dev/null || NUMA=""
g++ $CXXF depth_ext.cc -o depth_ext && { log "depth_ext"; $NUMA taskset -c 4 ./depth_ext 27 7 > ~/htbench_work/$OUT/depth_ext.csv 2> ~/htbench_work/$OUT/depth_ext.err; log "  rc=$?"; }
g++ $CXXF mlp.cc -o mlp && { log "mlp"; $NUMA taskset -c 4 ./mlp > ~/htbench_work/$OUT/mlp.csv 2> ~/htbench_work/$OUT/mlp.err; log "  rc=$?"; }
log RUN_DONE
