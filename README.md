# One cache line, many misses in flight

Code, raw measurements and analysis scripts for the paper

> R. Couturier and J. Azar, *One Cache Line, Many Misses in Flight: A Chunked SIMD Hash Table with
> Coroutine-Free Memory-Access Overlap, and What It Takes to Measure One*, submitted to Parallel Computing, 2026.

The table keeps its SIMD control bytes and its keys in the same 64-byte line, so a probe touches one line whose
address comes from the hash alone. Lookups are processed in batches by a small hand-written AMAC ring
(asynchronous memory-access chaining): a fixed number of lanes on the stack, each one prefetching the line of its
lookup and probing it once it has arrived. No coroutine frames, no heap allocation in the loop. A 64-bit filter
stored in the padding bytes of the line lets most negative lookups stop after one line.

Every number in the paper can be regenerated from the files in `results/` with the scripts in `analysis/`.

## Repository layout

```
src/include/      the hash tables (header-only)
src/bench/        benchmark drivers, one per experiment
src/third_party/  ankerl::unordered_dense 4.4 (MIT), used as a baseline
scripts/grid5000/ the scripts that ran the experiments on Grid'5000
results/          raw measurements, one folder per campaign and node
analysis/         scripts that turn results/ into the tables and figures of the paper
```

### The tables (`src/include`)

| header | layout in the paper | chunk |
|---|---|---|
| `swiss_table_chunkv.hpp`  | Inline   | 8 control bytes, 3 keys, 3 values in 64 B |
| `swiss_table_chunkvf.hpp` | Inline+F | Inline plus a 64-bit overflow filter in the padding |
| `swiss_table_chunk2.hpp`  | Paired   | 7 keys in one line, their 7 values in the next one |
| `swiss_table_chunk.hpp`   | Detached | 7 keys per line, values in a separate array |
| `swiss_table_h1h2.hpp`    | SoA (ablation baseline) | SwissTable-style separate control array, AVX2, x86 only |

`chunk_simd.hpp` maps the SSE2 intrinsics used by the probes to NEON on AArch64, so the same code runs on both.
All tables take `uint64_t` keys and values. Each one has a synchronous `find_sync` and a batched `find_batch<D>`, where `D` is the
ring depth (32 in the paper); Inline and Inline+F return values, Detached returns pointers.

Deletion writes a tombstone, as in Abseil. The tables count live and deleted slots together and rebuild once
that count reaches 80% of the slots, so an empty slot always exists and every probe terminates.
`src/bench/tombstone_test.cc` checks this, and `scripts/grid5000/tombstone_fix.py` applies the same rule to other
copies of the headers.

The prefetch locality hint is written in the headers as the third argument of `__builtin_prefetch`
(3 = keep in L1). The x86 results use 3. The ARM results use 2 (keep in L2), because on the ARM cores we
measured the hint decides whether the engine helps at all; the run scripts switch it with `sed` on ARM nodes.

## Building

Requirements: a C++20 compiler (GCC 13 or Clang 16 or newer), x86-64 with AVX2 or AArch64, and for the drivers
that compare against production maps: Abseil, Boost (1.81 or newer for `unordered_flat_map`) and folly (F14).
Hardware counters (`abl_perf.cc`) need Linux and `perf_event_paranoid` at 2 or lower.

Header-only drivers build directly:

```bash
cd src/bench
g++ -std=c++20 -O3 -DNDEBUG -march=native -I ../include tombstone_test.cc -o tombstone_test
g++ -std=c++20 -O3 -DNDEBUG -march=native -I ../include churn_cost.cc -o churn_cost
```

Drivers with baselines need the libraries, for example with a conda environment:

```bash
ABSL="$(pkg-config --cflags --libs absl_flat_hash_map)"
g++ -std=c++20 -O3 -DNDEBUG -march=native -I ../include -I ../third_party -I $CONDA_PREFIX/include \
    harness_ab.cc -o harness_ab $ABSL
```

F14 is compiled in its own translation unit (`f14_pf.cc`) at the baseline instruction set of the prebuilt folly
library (`-msse2` on x86, `-march=armv8-a` on ARM) and linked with the driver; `scripts/grid5000/r3_run.sh` shows the
exact command. The NeighborHash comparison (`nh_compare.cc`) needs a clone of
https://github.com/slow-steppers/NeighborHash at commit `274a115`; `scripts/grid5000/nh_build.sh` clones and builds
it. `src/bench/nh_shim/` replaces the single Abseil header NeighborHash includes.

Every driver prints CSV on standard output and a short log on standard error. Measurements in the paper pin one
core with `taskset -c 4` and bind memory with `numactl --localalloc`.

## Results

| folder | content |
|---|---|
| `2026-07_july` | main campaign on ten Grid'5000 nodes: single-thread throughput, depth sweeps, multi-thread scaling, latency, hit-ratio sweeps, memory-level parallelism, YCSB mixes |
| `2026-09_september` | six nodes: prefetch-hint sweep, layout x engine ablation, latency metrics, filter fix, extended depth sweeps, both estimators of memory-level parallelism |
| `2026-09_late-september` | production maps with their prefetch hooks, ablation under hardware counters, overflow counts, feature-store workload on the servers |
| `2026-09_late-september_aligned` | the ablation rerun with the separate-array baseline aligned to 64 bytes |
| `2026-09_multithread-filter` | many-thread runs of Inline+F and repeat runs on Zen 3 |
| `2026-09_held-out-sapphire-rapids` | the node held out from the calibration of the tuning rules |
| `2026-10_october` | NeighborHash comparison, harness factors, termination regression, capacities, rebuild cost; `m4/` holds the same drivers on an Apple M4 Max |
| `apple-m4`, `apple-m4_feature-store` | Apple M4 Max runs of the production-prefetch comparison, in-cache ablation and feature-store simulation |

The July folder also holds a few early runs on a departmental cluster (`results_cluster*`, `results_local`),
which the paper does not use. Inside each campaign, a folder `results_<campaign>_<site>_<node>` holds the CSV files of one node together with
`machine.txt` (CPU, kernel, compiler) and, from September on, `env.txt` (frequency policy, huge pages, library
versions) and `hintcheck.txt` (the prefetch instructions found in each binary). Every repetition the drivers log
is kept; nothing was filtered.

## Reproducing the tables and figures

```bash
cd analysis
pip install -r requirements.txt
python3 gen_tables.py      # writes tables/*.tex
python3 fig_journal.py     # writes figures/*.pdf
python3 model_check.py     # model checks against the depth sweeps
python3 provenance.py      # cross-campaign provenance of the x86 ratios
```

## Platforms

Experiments presented in the paper were carried out using the Grid'5000 testbed, supported by a scientific
interest group hosted by Inria and including CNRS, RENATER and several universities as well as other
organisations (https://www.grid5000.fr). The nodes are AMD EPYC 7301 and 7513, Intel Xeon E5-2650 v4, Gold 6130,
Gold 5220, Platinum 8358 and Gold 6430, Marvell ThunderX2, Ampere Altra and NVIDIA Grace.

## Licence

MIT, see `LICENSE`. `src/third_party/ankerl/unordered_dense.h` keeps its own MIT licence.
