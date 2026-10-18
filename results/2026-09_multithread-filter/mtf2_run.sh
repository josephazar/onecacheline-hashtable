#!/bin/bash
# Multi-thread Inline+F rerun with the FIXED filter (FILTER_MODE 2, bits from a second multiply), same probe_mt
# driver and locality-1 hint as the July mt table.  Private build dir; output ~/htbench_work/results_mtf2_TAG/mt.csv
TAG="$1"; W=~/htbench_work; OUT=$W/results_mtf2_$TAG; mkdir -p "$OUT"
source ~/miniconda3/etc/profile.d/conda.sh && conda activate htbench
ABSL_FLAGS="$(pkg-config --cflags --libs $(pkg-config --list-all 2>/dev/null | awk '/^absl_/{print $1}') 2>/dev/null)"
B=$(mktemp -d $W/bin_mtf2_${TAG}_XXXXXX); trap 'rm -rf "$B"' EXIT
cp $W/*.hpp $W/*.cc "$B"/ && cp -r $W/third_party "$B"/ && cp $W/mtf2/swiss_table_chunkvf.hpp "$B"/ && cd "$B"
{ hostname; date; grep -m2 -A1 "ifndef FILTER_MODE" swiss_table_chunkvf.hpp; } > "$OUT/machine.txt"
CXXF="-std=c++20 -O3 -DNDEBUG -mavx2 -march=native -pthread -I third_party -I . -I $CONDA_PREFIX/include -L $CONDA_PREFIX/lib -Wl,-rpath,$CONDA_PREFIX/lib"
g++ -std=c++20 -O3 -DNDEBUG -msse2 -I "$CONDA_PREFIX/include" -c f14_bench.cc -o f14_bench.o && \
g++ $CXXF probe_mt.cc f14_bench.o -o probe_mt $ABSL_FLAGS -lfolly -lglog -lgflags -ldouble-conversion -lfmt || { echo BUILD_FAIL > "$OUT/progress.log"; exit 1; }
NP=$(nproc)
{ MACHINE=$TAG ./probe_mt 26 $NP miss; MACHINE=$TAG ./probe_mt 26 $NP hit | tail -n +2; } > "$OUT/mt.csv" 2> "$OUT/mt.err"
echo "rc=$? RUN_DONE $(date)" > "$OUT/progress.log"
