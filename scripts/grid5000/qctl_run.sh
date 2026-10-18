#!/usr/bin/env bash
# Control for the roofline ceiling on Grace: does the plateau fall when the query stream no longer fits L3?
set -uo pipefail
TAG=${1:-grace}; cd ~/htbench_work; OUT=results_qctl_$TAG; mkdir -p $OUT
CH=~/miniconda3; [ "$(uname -m)" = aarch64 ] && CH=~/miniconda3-aarch64
source "$CH"/etc/profile.d/conda.sh; conda activate htbench
BIN=$(mktemp -d ~/htbench_work/bin_q_${TAG}_XXXXXX); trap 'rm -rf "$BIN"' EXIT
cp *.cc *.hpp $BIN/; cp -r third_party $BIN/ 2>/dev/null
for f in $BIN/swiss_table_*.hpp; do sed -i -E 's/__builtin_prefetch\((.*), *0, *[0-3]\)/__builtin_prefetch(\1, 0, 2)/' $f; done
cd $BIN; CXXF="-std=c++20 -O3 -DNDEBUG -march=native -pthread -I $BIN -I $BIN/third_party"
log(){ echo "[$(date '+%F %T')] $*" | tee -a ~/htbench_work/$OUT/progress.log; }
g++ $CXXF -S depth_ext.cc -o hint.s; log "hint check: $(grep -oE 'prfm[[:space:]]+pld[a-z0-9]+' hint.s | sort | uniq -c | tr '\n' ' ')"
g++ $CXXF depth_ext.cc -o depth_ext || { log BUILD_FAIL; exit 1; }
for Q in 2000000 8000000 32000000 64000000; do
  log "Q=$Q"; numactl --localalloc taskset -c 4 ./depth_ext 27 5 $Q > ~/htbench_work/$OUT/depth_ext_Q$Q.csv 2> ~/htbench_work/$OUT/depth_ext_Q$Q.err; log "  rc=$?"
done
log RUN_DONE
