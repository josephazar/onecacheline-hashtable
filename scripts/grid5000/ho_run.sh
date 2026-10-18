#!/bin/bash
# Held-out validation on a node NOT used to calibrate the tuner : isolated single-thread rerun (r2 iso bundle,
# locality 1 as the July x86 rows), both N_M estimators (mlp_pf gather + mlp chase), extended depth sweep (Inline +
# Paired, D to 384) to score the depth rule, and the hit-ratio sweep to score the layout rule.
# Usage: ho_run.sh TAG   -> ~/htbench_work/results_ho_TAG/
TAG="$1"; W=~/htbench_work; OUT=$W/results_ho_$TAG; mkdir -p "$OUT"
source ~/miniconda3/etc/profile.d/conda.sh && conda activate htbench
ABSL_FLAGS="$(pkg-config --cflags --libs $(pkg-config --list-all 2>/dev/null | awk '/^absl_/{print $1}') 2>/dev/null)"
INC="-I $CONDA_PREFIX/include -L $CONDA_PREFIX/lib -Wl,-rpath,$CONDA_PREFIX/lib"
CXX="g++ -std=c++20 -O3 -DNDEBUG -mavx2 -march=native -pthread"
NUMA="numactl --localalloc"; command -v numactl >/dev/null || NUMA=""; RUN="$NUMA taskset -c 4"
log(){ echo "[$(date '+%F %T')] $*" | tee -a "$OUT/progress.log"; }
B=$(mktemp -d $W/bin_ho_${TAG}_XXXXXX); trap 'rm -rf "$B"' EXIT
{ hostname; date; lscpu; g++ --version | head -1; } > "$OUT/machine.txt"
log "start $TAG on $(hostname)"
# 1. isolated single-thread rerun
mkdir -p $B/iso && cp -r $W/r2/iso/* $B/iso/ && cd $B/iso
g++ -std=c++20 -O3 -DNDEBUG -msse2 -I "$CONDA_PREFIX/include" -c f14_bench.cc -o f14_bench.o \
  && $CXX -I third_party -I . $INC iso2.cc f14_bench.o -o iso2 $ABSL_FLAGS -lfolly -lglog -lgflags -ldouble-conversion -lfmt \
  && { log "iso2"; MACHINE=$TAG $RUN ./iso2 27 > "$OUT/throughput.csv" 2> "$OUT/iso_raw.csv"; log "  rc=$?"; } || log "BUILD FAIL iso2"
# 2-4. microbenchmarks and sweeps from the main bundle
mkdir -p $B/w && cp $W/*.hpp $W/mlp_pf.cc $W/mlp.cc $W/depth_ext.cc $W/hitratio.cc $B/w/ && cp $W/mtf2/swiss_table_chunkvf.hpp $B/w/ && cd $B/w
$CXX mlp_pf.cc -o mlp_pf && { log "mlp_pf"; $RUN ./mlp_pf 29 32000000 5 > "$OUT/mlp_pf.csv" 2> "$OUT/mlp_pf.err"; log "  rc=$?"; }
$CXX mlp.cc -o mlp && { log "mlp"; $RUN ./mlp > "$OUT/mlp.csv" 2> "$OUT/mlp.err"; log "  rc=$?"; }
$CXX depth_ext.cc -o depth_ext && { log "depth_ext"; $RUN ./depth_ext 27 7 > "$OUT/depth_ext.csv" 2> "$OUT/depth_ext.err"; log "  rc=$?"; }
$CXX hitratio.cc -o hitratio && { log "hitratio"; $RUN ./hitratio > "$OUT/hitratio.csv" 2> "$OUT/hitratio.err"; log "  rc=$?"; }
log RUN_DONE
