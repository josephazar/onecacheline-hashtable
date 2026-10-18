#!/bin/bash
# October 2026 campaign.  Same conventions as r3_run.sh: one tag per node,
# taskset -c 4, numactl --localalloc, conda env htbench, binaries built INSIDE a copy of the header directory.
# Usage: r4_run.sh TAG [STEPS]      STEPS = comma list among env,nh,hab,tomb,cap,churn   (default: all)
#   env    environment report (kernel, THP, governor, library versions)                      -> env.txt           (1 min)
#   nh     NeighborHash (CIKM 2024) against Inline / Inline+F, scalar + AMAC (+ AVX-512 SIMD-AMAC when the CPU has
#          avx512f/dq/vl/bw), 2^27 keys, 9 paired blocks after one warm-up block               -> nh_compare.csv    (~15 min)
#   hab    harness A/B: each production map alone vs all resident, sum loop vs store loop,
#          2^27 keys, 9 paired blocks (explains the Table 4 vs Table 6 baseline shift)            -> harness_ab.csv    (~25 min)
#   tomb   termination regression: old (live-count) vs new (non-empty-count) rehash trigger,
#          the 4-chunk trace + churn on 8192-key tables, every layout incl. the AVX2 SoA          -> tombstone_*.csv   (~2 min)
#   cap    capacity, load and footprint of every table at 2^27, as the drivers build them         -> capacity.csv      (~3 min)
#   churn  cost of the rebuild trigger under delete+insert churn, 2^27 keys, 2 x N pairs           -> churn_cost.csv    (~5 min)
# Headers: headers/ = the late-September headers at locality 3 (L1 keep) with the October tombstone patch (tombstone_fix.py);
# orig_headers/ = the same without the patch (only for the regression).  The patch touches insert/erase/rehash only,
# no lookup path.  On ARM nodes every header is brought to locality 2 (L2 keep), as every ARM table in the paper;
# NeighborHash keeps its own locality-3 prefetch (nh_shim/absl/base/prefetch.h), as in its repository.
TAG="$1"; STEPS="${2:-env,nh,hab,tomb,cap,churn}"
SRC=~/htbench_work/r4; HDR=~/htbench_work/r2; OUT=~/htbench_work/results_r4_$TAG; mkdir -p "$OUT"
NHDIR=${NHDIR:-~/htbench_work/neighborhash}          # cloned by nh_build.sh if missing (clone it on the frontend if nodes lack GitHub access)
CH=~/miniconda3; [ "$(uname -m)" = aarch64 ] && CH=~/miniconda3-aarch64
source "$CH"/etc/profile.d/conda.sh && conda activate htbench
ARCH=$(uname -m)
case "$ARCH" in x86_64) ISA="-march=native"; L1RE='prefetcht0'; L3RE='prefetcht2';; *) ISA="-march=native"; L1RE='pldl1keep'; L3RE='pldl3keep';; esac
ABSL_FLAGS="$(pkg-config --cflags --libs $(pkg-config --list-all 2>/dev/null | awk '/^absl_/{print $1}') 2>/dev/null)"
INC="-I $CONDA_PREFIX/include -L $CONDA_PREFIX/lib -Wl,-rpath,$CONDA_PREFIX/lib"
ANKERL="-I $HDR/iso/third_party"
CXX="g++ -std=c++20 -O3 -DNDEBUG $ISA -pthread"
BIN=$(mktemp -d ~/htbench_work/bin_r4_${TAG}_XXXXXX); trap 'rm -rf "$BIN"' EXIT
cp -r "$SRC"/. "$BIN"/ && cd "$BIN"
if [ "$ARCH" != x86_64 ]; then for d in headers orig_headers; do sed -i -E 's/__builtin_prefetch\(([^;]*), *0, *3\)/__builtin_prefetch(\1, 0, 2)/' $d/swiss_table_*.hpp; done; fi
NUMA="numactl --localalloc"; command -v numactl >/dev/null || NUMA=""
RUN="$NUMA taskset -c 4"
log(){ echo "[$(date '+%F %T')] $*" | tee -a "$OUT/progress.log"; }
has(){ [[ ",$STEPS," == *",$1,"* ]]; }
hintcount(){ objdump -d "$1" | grep -cE "$2"; }
{ hostname; date; lscpu; g++ --version | head -1; echo "STEPS=$STEPS"; grep -o 'avx512[a-z]*' /proc/cpuinfo | sort -u | tr '\n' ' '; echo; } > "$OUT/machine.txt"
log "start $TAG on $(hostname) steps=$STEPS"

if has env; then bash "$BIN/env_report.sh" > "$OUT/env.txt" 2>&1; log "env report written"; fi

if has nh; then
  CXX=g++ bash nh_build.sh "$NHDIR" "$BIN/nh_compare" > "$OUT/nh_build.log" 2>&1; log "nh build rc=$? ($(grep -c NEIGHBOR_HASH_SIMD_FIND "$OUT/nh_build.log") SIMD flag lines)"
  echo "nh L1=$(hintcount nh_compare $L1RE) L3=$(hintcount nh_compare $L3RE)" >> "$OUT/hintcheck.txt"
  log "nh_compare"; $RUN ./nh_compare 27 2000000 9 > "$OUT/nh_compare.csv" 2> "$OUT/nh_compare.err"; log "  rc=$? $(grep -c 'VALIDATION FAILED' "$OUT/nh_compare.err") validation failures"
fi
if has hab; then
  $CXX $INC $ANKERL -I headers harness_ab.cc -o harness_ab $ABSL_FLAGS && {
    echo "hab L1=$(hintcount harness_ab $L1RE) L3=$(hintcount harness_ab $L3RE)" >> "$OUT/hintcheck.txt"
    log "harness_ab"; $RUN ./harness_ab 27 2000000 9 > "$OUT/harness_ab.csv" 2> "$OUT/harness_ab.err"; log "  rc=$?"; }
fi
if has tomb; then
  $CXX -I orig_headers tombstone_test.cc -o tombstone_test_orig && $CXX -I headers tombstone_test.cc -o tombstone_test_fix && {
    log "tombstone_test (old trigger: HANG expected)"; ./tombstone_test_orig 2000000 > "$OUT/tombstone_test_orig.csv" 2>&1; log "  rc=$?"
    log "tombstone_test (new trigger: ok expected)";   ./tombstone_test_fix 2000000 > "$OUT/tombstone_test_fix.csv" 2>&1; log "  rc=$?"; }
fi
if has cap; then
  $CXX $INC $ANKERL -I headers capacity_probe.cc -o capacity_probe $ABSL_FLAGS -lfolly -lglog -lgflags -ldouble-conversion -lfmt \
    || { log "F14 link failed, capacity without F14"; $CXX $INC $ANKERL -DNO_F14 -I headers capacity_probe.cc -o capacity_probe $ABSL_FLAGS; }
  log "capacity_probe"; $RUN ./capacity_probe 27 > "$OUT/capacity.csv" 2> "$OUT/capacity.err"; log "  rc=$?"
fi
if has churn; then
  $CXX -I headers churn_cost.cc -o churn_cost && { log "churn_cost"; $RUN ./churn_cost 27 2 > "$OUT/churn_cost.csv" 2> "$OUT/churn_cost.err"; log "  rc=$?"; }
fi
log "RUN_DONE"
