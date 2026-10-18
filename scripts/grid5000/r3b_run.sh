#!/bin/bash
# late-September (aligned): the ablation with the separate-array SwissTable's arrays 64-byte aligned (swiss_table_h1h2.hpp here),
# plus, in the same reservation, the original unaligned header as a control.  pf3 (L1 keep) headers, x86 only.
# Usage: r3b_run.sh TAG     -> ~/htbench_work/results_r3b_TAG/
TAG="$1"; SRC=~/htbench_work/r3b; R3=~/htbench_work/r3; HDR=~/htbench_work/r2/pf3; OUT=~/htbench_work/results_r3b_$TAG; mkdir -p "$OUT"
source ~/miniconda3/etc/profile.d/conda.sh && conda activate htbench
CXX="g++ -std=c++20 -O3 -DNDEBUG -march=native -pthread"
BIN=$(mktemp -d ~/htbench_work/bin_r3b_${TAG}_XXXXXX); trap 'rm -rf "$BIN"' EXIT
NUMA="numactl --localalloc"; command -v numactl >/dev/null || NUMA=""; RUN="$NUMA taskset -c 4"
log(){ echo "[$(date '+%F %T')] $*" | tee -a "$OUT/progress.log"; }
{ hostname; date; lscpu; g++ --version | head -1; } > "$OUT/machine.txt"
P=$(cat /proc/sys/kernel/perf_event_paranoid); [ "$P" -gt 1 ] && command -v sudo-g5k >/dev/null && sudo-g5k sysctl -w kernel.perf_event_paranoid=1 >/dev/null 2>&1
log "start $TAG on $(hostname) paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)"
for V in aligned orig; do
  mkdir -p "$BIN/$V" && cp "$HDR"/* "$BIN/$V"/ && cp "$R3"/abl_perf.cc "$R3"/perf_counters.hpp "$BIN/$V"/
  [ $V = aligned ] && cp "$SRC/swiss_table_h1h2.hpp" "$BIN/$V"/
  cd "$BIN/$V"
  echo "$V: $(grep -c CacheAlignedAlloc swiss_table_h1h2.hpp) CacheAlignedAlloc refs" >> "$OUT/variant_check.txt"
  for S in 0 1; do
    [ $V = orig ] && [ $S = 1 ] && continue
    $CXX -DSTRONG_HASH=$S ablation2.cc -o abl_$S || { log "BUILD FAIL ablation2 $V $S"; continue; }
    echo "$V strong=$S L1=$(objdump -d abl_$S | grep -c prefetcht0) L3=$(objdump -d abl_$S | grep -c prefetcht2)" >> "$OUT/hintcheck.txt"
    log "ablation2 $V strong=$S"; $RUN ./abl_$S 27 2000000 15 > "$OUT/ablation2_${V}_strong$S.csv" 2> "$OUT/ablation2_${V}_strong$S.err"; log "  rc=$?"
  done
  if [ $V = aligned ]; then
    $CXX abl_perf.cc -o abl_perf && for L in 16 27; do log "abl_perf $V log2n=$L"; $RUN ./abl_perf $L 2000000 5 > "$OUT/abl_perf_${V}_$L.csv" 2> "$OUT/abl_perf_${V}_$L.err"; log "  rc=$? $(head -1 $OUT/abl_perf_${V}_$L.err)"; done
  fi
done
log "RUN_DONE"
