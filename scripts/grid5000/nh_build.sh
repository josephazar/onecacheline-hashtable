#!/bin/bash
# Build nh_compare .  Usage: nh_build.sh NH_DIR [OUT_BINARY]
#   NH_DIR      where NeighborHash is cloned (created if missing), checked out at the pinned commit below
#   OUT_BINARY  default: nh_compare next to this script
# Compiles from INSIDE this directory so the quote-includes resolve as intended.  If headers/ holds the October bundle
# (swiss_table_chunkv.hpp, swiss_table_chunkvf.hpp, chunk_simd.hpp, ...), it is used with -I headers.  If swiss_table_chunkv.hpp and
# swiss_table_chunkvf.hpp have been copied here (node layout), they are used (-DR4_LOCAL_HEADERS); otherwise the driver
# includes ../pf3/swiss_table_chunkv.hpp and ../r3/swiss_table_chunkvf.hpp, and -I ../pf3 supplies chunk_simd.hpp.
# NeighborHash is header-only; its only external include, absl/base/prefetch.h, comes from nh_shim/ (no Abseil needed).
# Environment knobs:
#   NH_SIMD=auto|0|1   -DNEIGHBOR_HASH_SIMD_FIND (AVX-512 F/DQ/VL/BW cells); auto = on when /proc/cpuinfo has them
#   NH_HUGEPAGE=0|1    -DNEIGHBOR_HASH_HUGEPAGE: NH tables on MAP_HUGETLB 2 MiB pages (needs vm.nr_hugepages reserved,
#                      >= 3400 pages at 2^27 for NH + NH79; aborts otherwise).  Default 0: NH uses aligned_alloc, so
#                      both designs get the same page policy (THP setting of the node).
#   CXX (default g++), EXTRA (extra flags, e.g. -DNH_LF=0.7)
set -euo pipefail
NH_REPO=https://github.com/slow-steppers/NeighborHash
NH_COMMIT=274a11558b211d6e5ca5ff9d2e5e9373f3ac90b8   # main, 2025-01-06 "Merge pull request #15" (tags v0.1, v0.2 are older)
NH_DIR="${1:?usage: nh_build.sh NH_DIR [OUT_BINARY]}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${2:-$HERE/nh_compare}"
NH_DIR="$(mkdir -p "$(dirname "$NH_DIR")" && cd "$(dirname "$NH_DIR")" && pwd)/$(basename "$NH_DIR")"
[ -d "$NH_DIR/.git" ] || git clone --quiet "$NH_REPO" "$NH_DIR"
git -C "$NH_DIR" cat-file -e "$NH_COMMIT^{commit}" 2>/dev/null || git -C "$NH_DIR" fetch --quiet origin
git -C "$NH_DIR" -c advice.detachedHead=false checkout --quiet "$NH_COMMIT"
echo "NeighborHash at $(git -C "$NH_DIR" rev-parse HEAD)" >&2

FLAGS=""
case "${NH_SIMD:-auto}" in
  1) FLAGS+=" -DNEIGHBOR_HASH_SIMD_FIND" ;;
  auto) if [ -r /proc/cpuinfo ] && grep -q avx512f /proc/cpuinfo && grep -q avx512dq /proc/cpuinfo \
           && grep -q avx512vl /proc/cpuinfo && grep -q avx512bw /proc/cpuinfo; then FLAGS+=" -DNEIGHBOR_HASH_SIMD_FIND"; fi ;;
esac
[ "${NH_HUGEPAGE:-0}" = 1 ] && FLAGS+=" -DNEIGHBOR_HASH_HUGEPAGE"
cd "$HERE"
if [ -f swiss_table_chunkv.hpp ] && [ -f swiss_table_chunkvf.hpp ]; then FLAGS+=" -DR4_LOCAL_HEADERS"
elif [ -f headers/swiss_table_chunkv.hpp ] && [ -f headers/swiss_table_chunkvf.hpp ]; then FLAGS+=" -DR4_LOCAL_HEADERS -I headers"   # October bundle
else FLAGS+=" -I ../pf3"; fi
set -x
${CXX:-g++} -std=c++20 -O3 -DNDEBUG -march=native $FLAGS ${EXTRA:-} -I nh_shim -I "$NH_DIR" nh_compare.cc -o "$OUT"
