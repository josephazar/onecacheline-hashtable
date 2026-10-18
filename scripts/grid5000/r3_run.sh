#!/bin/bash
# late-September campaign.  Same conventions as r2_run.sh: one tag per node, taskset -c 4,
# numactl --localalloc, headers copied from ~/htbench_work/r2/{orig,pf2,pf3}, binaries built INSIDE the header dir.
# Usage: r3_run.sh TAG [STEPS]   STEPS = comma list among env,prod,perf,ovf,ycsb,fs   (default: all)
#   env   environment report (kernel, THP, governor, library versions)           -> env.txt              (1 min)
#   prod  prefetch-capable production maps vs Inline, 9 paired blocks, 2^27       -> prod_prefetch.csv    (~25 min)
#   perf  ablation cells under hardware counters, in cache (2^16) and in DRAM (2^27) -> abl_perf_{16,27}.csv (~15 min)
#   ovf   overflowed chunks per load (deterministic)                              -> overflow_count.csv   (2 min)
#   ycsb  ordering contract of the mixed-update engine against a serial reference -> ycsb_check.csv       (3 min)
#   fs    feature-store multi-get workload, 3 profiles x 5 reps                   -> fs_*.csv             (~30 min)
# Hint: x86 nodes use pf3 (L1 keep, as the ablation tables), ARM nodes use pf2 (L2 keep, as every ARM table).
TAG="$1"; STEPS="${2:-env,prod,perf,ovf,ycsb,fs}"
SRC=~/htbench_work/r3; HDR=~/htbench_work/r2; OUT=~/htbench_work/results_r3_$TAG; mkdir -p "$OUT"
CH=~/miniconda3; [ "$(uname -m)" = aarch64 ] && CH=~/miniconda3-aarch64
source "$CH"/etc/profile.d/conda.sh && conda activate htbench
ARCH=$(uname -m)
case "$ARCH" in x86_64) ISA="-march=native"; F14_ISA="-msse2"; H=pf3;; *) ISA="-march=native"; F14_ISA="-march=armv8-a"; H=pf2;; esac
ABSL_FLAGS="$(pkg-config --cflags --libs $(pkg-config --list-all 2>/dev/null | awk '/^absl_/{print $1}') 2>/dev/null)"
INC="-I $CONDA_PREFIX/include -L $CONDA_PREFIX/lib -Wl,-rpath,$CONDA_PREFIX/lib"
ANKERL="-I $HDR/iso/third_party"
CXX="g++ -std=c++20 -O3 -DNDEBUG $ISA -pthread"
BIN=$(mktemp -d ~/htbench_work/bin_r3_${TAG}_XXXXXX); trap 'rm -rf "$BIN"' EXIT
cp -r "$HDR/$H" "$BIN/h" && cp "$SRC"/*.cc "$SRC"/*.hpp "$BIN/h"/ 2>/dev/null
# r3/swiss_table_chunkvf.hpp = the r2 header + find_sync + occupancy counters , shipped at locality 3;
# bring it to the node's hint so every cell of a binary carries the same one
[ "$H" = pf2 ] && sed -i -E 's/__builtin_prefetch\(([^;]*), *0, *3\)/__builtin_prefetch(\1,0,2)/' "$BIN/h/swiss_table_chunkvf.hpp"
NUMA="numactl --localalloc"; command -v numactl >/dev/null || NUMA=""
RUN="$NUMA taskset -c 4"
log(){ echo "[$(date '+%F %T')] $*" | tee -a "$OUT/progress.log"; }
has(){ [[ ",$STEPS," == *",$1,"* ]]; }
{ hostname; date; lscpu; g++ --version | head -1; echo "STEPS=$STEPS HINTDIR=$H"; } > "$OUT/machine.txt"
log "start $TAG on $(hostname) steps=$STEPS hint=$H"
hintcount(){ objdump -d "$1" | grep -cE "$2"; }
if [ "$ARCH" = x86_64 ]; then L1RE='prefetcht0'; L3RE='prefetcht2'; else L1RE='pldl1keep'; L3RE='pldl3keep'; fi
cd "$BIN/h"

if has env; then bash "$SRC/env_report.sh" > "$OUT/env.txt" 2>&1; log "env report written"; fi

if has prod; then
  g++ -std=c++20 -O3 -DNDEBUG $F14_ISA -I "$CONDA_PREFIX/include" -c f14_pf.cc -o f14_pf.o \
    && $CXX $INC $ANKERL prod_prefetch.cc f14_pf.o -o prod_prefetch $ABSL_FLAGS -lfolly -lglog -lgflags -ldouble-conversion -lfmt \
    || { log "F14 build failed, retrying without F14"; $CXX -DNO_F14 $INC $ANKERL prod_prefetch.cc -o prod_prefetch $ABSL_FLAGS; }
  echo "prod L1=$(hintcount prod_prefetch $L1RE) L3=$(hintcount prod_prefetch $L3RE)" >> "$OUT/hintcheck.txt"
  log "prod_prefetch"; $RUN ./prod_prefetch 27 2000000 9 > "$OUT/prod_prefetch.csv" 2> "$OUT/prod_prefetch.err"; log "  rc=$?"
fi
if has perf; then
  # counters need perf_event_paranoid <= 2; on Grid'5000 sudo-g5k gives root on a whole-node job (no kadeploy)
  P=$(cat /proc/sys/kernel/perf_event_paranoid); log "perf_event_paranoid=$P"
  if [ "$P" -gt 1 ] && command -v sudo-g5k >/dev/null; then sudo-g5k sysctl -w kernel.perf_event_paranoid=1 >/dev/null 2>&1; log "  after sudo-g5k: $(cat /proc/sys/kernel/perf_event_paranoid)"; fi
  $CXX abl_perf.cc -o abl_perf && { echo "perf L1=$(hintcount abl_perf $L1RE) L3=$(hintcount abl_perf $L3RE)" >> "$OUT/hintcheck.txt"
    for L in 16 27; do log "abl_perf log2n=$L"; $RUN ./abl_perf $L 2000000 5 > "$OUT/abl_perf_$L.csv" 2> "$OUT/abl_perf_$L.err"; log "  rc=$? $(head -1 $OUT/abl_perf_$L.err)"; done; }
fi
if has ovf; then
  $CXX -DFILTER_MODE=2 overflow_count.cc -o overflow_count && { log "overflow_count"; $RUN ./overflow_count 23 1000000 > "$OUT/overflow_count.csv" 2> "$OUT/overflow_count.err"; log "  rc=$?"; }
fi
if has ycsb; then
  $CXX ycsb_check.cc -o ycsb_check && { log "ycsb_check"; $RUN ./ycsb_check 24 2000000 > "$OUT/ycsb_check.csv" 2> "$OUT/ycsb_check.err"; log "  rc=$?"; }
fi
if has fs; then
  $CXX $INC featurestore.cc -o featurestore $ABSL_FLAGS && {
    echo "fs L1=$(hintcount featurestore $L1RE) L3=$(hintcount featurestore $L3RE)" >> "$OUT/hintcheck.txt"
    log "featurestore recommender"; $RUN ./featurestore --logn 26 --zipf 0.99 --hit 0.95 --kmed 32  --lambdas 250000,500000,1000000,2000000,3000000 --taus 20,100 --config recommender > "$OUT/fs_recommender.csv" 2> "$OUT/fs_recommender.err"; log "  rc=$?"
    log "featurestore iot-registry"; $RUN ./featurestore --logn 26 --zipf 0.60 --hit 0.80 --kmed 8 --lambdas 1000000,2000000,4000000,8000000,12000000 --taus 20,100 --config iot-registry > "$OUT/fs_iot-registry.csv" 2> "$OUT/fs_iot-registry.err"; log "  rc=$?"
    log "featurestore batch-retrieval"; $RUN ./featurestore --logn 26 --zipf 0.99 --hit 1.00 --kmed 128 --lambdas 100000,200000,400000,600000,800000 --taus 20,100 --config batch-retrieval > "$OUT/fs_batch-retrieval.csv" 2> "$OUT/fs_batch-retrieval.err"; log "  rc=$?"; }
fi
log "RUN_DONE"
