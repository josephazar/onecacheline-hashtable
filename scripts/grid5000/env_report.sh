#!/bin/bash
# env_report.sh - the environment facts needed to interpret the measurements, one file per node.
# Usage: ./env_report.sh > env_<tag>.txt      (run inside the conda env used for the benchmarks)
echo "host: $(hostname)"; echo "date: $(date)"; echo "uname: $(uname -a)"
echo "--- cpu"; lscpu | grep -E "Model name|Architecture|CPU\(s\)|Thread|Core|Socket|NUMA|L1d|L2|L3|MHz"
echo "--- frequency"; for f in /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor /sys/devices/system/cpu/cpu4/cpufreq/scaling_driver /sys/devices/system/cpu/intel_pstate/no_turbo /sys/devices/system/cpu/cpufreq/boost; do [ -r "$f" ] && echo "$f = $(cat $f)"; done
echo "--- memory / pages"; echo "PAGESIZE = $(getconf PAGESIZE)"; for f in /sys/kernel/mm/transparent_hugepage/enabled /sys/kernel/mm/transparent_hugepage/defrag /sys/kernel/mm/transparent_hugepage/khugepaged/defrag; do [ -r "$f" ] && echo "$f = $(cat $f)"; done
grep -E "MemTotal|HugePages_Total|Hugepagesize" /proc/meminfo
command -v numactl >/dev/null && numactl -H | head -20
echo "--- kernel knobs"; for f in /proc/sys/kernel/perf_event_paranoid /proc/sys/kernel/randomize_va_space /proc/sys/vm/overcommit_memory /proc/sys/kernel/numa_balancing; do [ -r "$f" ] && echo "$f = $(cat $f)"; done
echo "--- smt"; [ -r /sys/devices/system/cpu/smt/active ] && echo "smt active = $(cat /sys/devices/system/cpu/smt/active)"
echo "--- toolchain"; g++ --version | head -1; ldd --version | head -1; command -v clang++ >/dev/null && clang++ --version | head -1
echo "--- libraries (conda)"; conda list 2>/dev/null | grep -iE "^(abseil|libabseil|boost|libboost|folly|ankerl|gcc|gxx|libstdcxx|glog|gflags|fmt|double-conversion)"
echo "--- library headers actually on the include path"; for h in absl/base/config.h boost/version.hpp folly/folly-config.h ankerl/unordered_dense.h; do p=$(echo | g++ -x c++ -E -Wp,-v - 2>&1 | grep "^ /" | while read d; do [ -f "$d/$h" ] && echo "$d/$h" && break; done); [ -n "$p" ] && echo "$h -> $p"; done
[ -n "$CONDA_PREFIX" ] && { grep -E "define BOOST_LIB_VERSION" "$CONDA_PREFIX/include/boost/version.hpp" 2>/dev/null; grep -E "ABSL_LTS_RELEASE_VERSION|ABSL_LTS_RELEASE_PATCH_LEVEL" "$CONDA_PREFIX/include/absl/base/options.h" 2>/dev/null; grep -E "ANKERL_UNORDERED_DENSE_VERSION_(MAJOR|MINOR|PATCH)" "$CONDA_PREFIX/include/ankerl/unordered_dense.h" 2>/dev/null | head -3; grep -E "FOLLY_VERSION" "$CONDA_PREFIX/include/folly/folly-config.h" 2>/dev/null; }
echo "--- build flags used by r2_run.sh / r3_run.sh"; echo "g++ -std=c++20 -O3 -DNDEBUG -march=native -pthread ; F14 TU at -msse2 (x86) / -march=armv8-a (ARM)"
echo "--- hint check of the last binaries (from results dirs, if present)"; ls ~/htbench_work/results_r2_*/hintcheck.txt 2>/dev/null | head -3
