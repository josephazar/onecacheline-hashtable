#pragma once
// Minimal stand-in for absl/base/prefetch.h, the only Abseil header NeighborHash's neighbor_hash.h includes.
// It keeps the build free of Abseil.  Same semantics as Abseil's PrefetchToLocalCache on GCC/Clang:
// __builtin_prefetch(addr, 0, 3) = prefetcht0 on x86, prfm pldl1keep on AArch64 (the hint our pf3 headers use).
// Defining ABSL_HAVE_PREFETCH enables NeighborHash's amac_find / simd_amac_find, as Abseil's header does.
#define ABSL_HAVE_PREFETCH 1
namespace absl {
__attribute__((always_inline)) inline void PrefetchToLocalCache(const void* addr) { __builtin_prefetch(addr, 0, 3); }
}  // namespace absl
