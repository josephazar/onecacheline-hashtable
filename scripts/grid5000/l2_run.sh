#!/usr/bin/env bash
# ARM re-run of the paper's engine experiments with prefetch locality 2 (pldl2keep), which the
# September hint sweep showed to be the best hint on all three ARM cores (the paper used locality 1).
# usage: l2_run.sh <TAG> [steps]   steps = comma list among lat,depth,mt,filt,ycsb,zs (default all)
set -uo pipefail
TAG=${1:-arm}; STEPS=${2:-lat,depth,mt,filt,ycsb,zs}; cd ~/htbench_work
OUT=results_l2_$TAG; mkdir -p $OUT
CH=~/miniconda3; [ "$(uname -m)" = aarch64 ] && CH=~/miniconda3-aarch64
source "$CH"/etc/profile.d/conda.sh; conda activate htbench
ABSL_FLAGS="$(pkg-config --cflags --libs $(pkg-config --list-all 2>/dev/null | awk '/^absl_/{print $1}') 2>/dev/null)"
FOLLY="-lfolly -lglog -lgflags -ldouble-conversion -lfmt"
BIN=$(mktemp -d ~/htbench_work/bin_l2_${TAG}_XXXXXX); trap 'rm -rf "$BIN"' EXIT
cp *.cc *.hpp $BIN/ 2>/dev/null; cp -r third_party $BIN/ 2>/dev/null
for f in $BIN/swiss_table_*.hpp; do sed -i -E 's/__builtin_prefetch\((.*), *0, *[0-3]\)/__builtin_prefetch(\1, 0, 2)/' $f; done
CXXF="-std=c++20 -O3 -DNDEBUG -march=native -pthread -I $BIN/third_party -I $BIN -I $CONDA_PREFIX/include -L $CONDA_PREFIX/lib -Wl,-rpath,$CONDA_PREFIX/lib"
cd $BIN
{ echo "# $TAG $(date -u +%FT%TZ) prefetch locality 2"; lscpu | grep -iE "Model name|^Architecture|Socket|Core\(s\) per|NUMA|L3|CPU max"; echo "g++=$(g++ --version|head -1)"; } > ~/htbench_work/$OUT/machine.txt
log(){ echo "[$(date '+%F %T')] $*" | tee -a ~/htbench_work/$OUT/progress.log; }
has(){ [[ ",$STEPS," == *",$1,"* ]]; }
g++ $CXXF simd_test.cc -o simd_test && ./simd_test > ~/htbench_work/$OUT/simd_selftest.txt 2>&1
grep -q SIMD_SELFTEST_OK ~/htbench_work/$OUT/simd_selftest.txt || { log "SIMD_SELFTEST_FAIL"; exit 1; }
g++ -std=c++20 -O3 -DNDEBUG -march=armv8-a -I $CONDA_PREFIX/include -c f14_bench.cc -o f14_bench.o
NUMA="numactl --localalloc"; command -v numactl >/dev/null || NUMA=""
g++ $CXXF -S probe_depth.cc -o hintcheck.s 2>/dev/null
log "hint check (probe_depth.s): pldl2keep=$(grep -c pldl2keep hintcheck.s) pldl1keep=$(grep -c pldl1keep hintcheck.s) pldl3keep=$(grep -c pldl3keep hintcheck.s)"
if has lat;   then g++ $CXXF latency.cc -o latency     && { log latency;    taskset -c 4 ./latency 2048 > ~/htbench_work/$OUT/latency.csv 2>~/htbench_work/$OUT/latency.err; log "  rc=$?"; }; fi
if has depth; then g++ $CXXF probe_depth.cc -o probe_depth $ABSL_FLAGS && { log depth; taskset -c 4 ./probe_depth 27 > ~/htbench_work/$OUT/depth.txt 2>~/htbench_work/$OUT/depth.err; log "  rc=$?"; }; fi
if has mt;    then g++ $CXXF probe_mt.cc f14_bench.o -o probe_mt $ABSL_FLAGS $FOLLY && { log mt; NP=$(nproc)
      MACHINE=$TAG ./probe_mt 26 $NP hit  > ~/htbench_work/$OUT/mt_hit.csv  2>~/htbench_work/$OUT/mt.err
      MACHINE=$TAG ./probe_mt 26 $NP miss > ~/htbench_work/$OUT/mt_miss.csv 2>>~/htbench_work/$OUT/mt.err; log "  rc=$?"; }; fi
if has filt;  then g++ $CXXF filt.cc -o filt $ABSL_FLAGS && { log filt; $NUMA taskset -c 4 ./filt > ~/htbench_work/$OUT/filt.csv 2>~/htbench_work/$OUT/filt.err; log "  rc=$?"; }; fi
if has ycsb;  then g++ $CXXF ycsb.cc -o ycsb $ABSL_FLAGS && { log ycsb; MACHINE=$TAG $NUMA taskset -c 4 ./ycsb 27 > ~/htbench_work/$OUT/ycsb.csv 2>~/htbench_work/$OUT/ycsb.err; log "  rc=$?"; }; fi
if has zs;    then g++ $CXXF zs.cc -o zs $ABSL_FLAGS && { log zs; MACHINE=$TAG $NUMA taskset -c 4 ./zs 27 > ~/htbench_work/$OUT/zs.csv 2>~/htbench_work/$OUT/zs.err; log "  rc=$?"; }; fi
log RUN_DONE
