#!/usr/bin/env python3
"""Median Mops/s per cell for the October nh_compare / harness_ab CSVs (one file per node), plus the ratios the
paper quotes: NeighborHash best AMAC vs Inline/Inline+F at equal depth, and store-vs-sum / all-vs-iso per map."""
import csv, statistics, sys, glob, os
from collections import defaultdict
def med(path, keycols):
    d = defaultdict(list)
    with open(path) as f:
        for r in csv.DictReader(f): d[tuple(r[c] for c in keycols)].append(float(r['mops']))
    return {k: statistics.median(v) for k, v in d.items()}
for path in sorted(glob.glob(sys.argv[1] if len(sys.argv) > 1 else 'results_r4_*/nh_compare.csv')):
    tag = os.path.basename(os.path.dirname(path)); m = med(path, ['cell', 'workload'])
    print(f'\n== {tag}  nh_compare (median of blocks, Mops/s)')
    cells = sorted({c for c, _ in m}, key=lambda c: (c.split('-')[0], c))
    print(f'{"cell":18s} {"hit":>8s} {"miss":>8s}')
    for c in cells: print(f'{c:18s} {m.get((c,"hit"),float("nan")):8.1f} {m.get((c,"miss"),float("nan")):8.1f}')
    def best(prefix, w): return max(((m[k], k[0]) for k in m if k[0].startswith(prefix) and k[1] == w))
    for w, ours in (('hit', 'Inline-AMAC64'), ('miss', 'InlineF-AMAC64')):
        for pre in ('NH-', 'NH79-'):
            b = best(pre, w); print(f'  {w:4s}: best {pre}* = {b[1]} {b[0]:.1f}  vs {ours} {m[(ours,w)]:.1f}  ratio {b[0]/m[(ours,w)]:.2f}')
    hab = os.path.join(os.path.dirname(path), 'harness_ab.csv')
    if os.path.exists(hab):
        h = med(hab, ['residency', 'map', 'loop', 'workload'])
        print(f'  harness_ab: map  workload  iso/sum  iso/store  co/sum   co/store   store/sum(iso)  co/iso(sum)')
        for mp in sorted({k[1] for k in h}):
            for w in ('hit', 'miss'):
                g = lambda r, l: h.get((r, mp, l, w), float('nan'))
                print(f'    {mp:12s} {w:4s} {g("iso","sum"):8.1f} {g("iso","store"):8.1f} {g("co","sum"):8.1f} {g("co","store"):8.1f}   {g("iso","store")/g("iso","sum"):.2f}   {g("co","sum")/g("iso","sum"):.2f}')
