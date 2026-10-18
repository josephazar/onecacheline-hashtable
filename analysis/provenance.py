#!/usr/bin/env python3
"""where do the scalar-baseline differences between campaigns come from?
Collects, for the three x86 nodes of the September campaign, every campaign's Inline-engine and best-scalar-map
positive-lookup throughput at 2^27 with the host and the frequency policy recorded in each campaign's machine file."""
import csv, os, re, statistics as st
R = os.path.dirname(os.path.abspath(__file__)) + '/..'
NODES = [('Zen 3', 'results_chuc_zen3', 'lille_zen3'), ('Ice Lake', 'results_lille_chirop_icelake', 'lille_icelake'),
         ('Cascade Lake', 'results_nancy_gros_cascadelake', 'nancy_cascadelake')]
def mach(p):
    host = mhz = boost = '?'
    if not os.path.exists(p): return host, mhz, boost
    t = open(p).read()
    m = re.search(r'^(\S+\.grid5000\.fr)', t, re.M) or re.search(r'host: (\S+)', t) or re.search(r'^# (\S+)', t, re.M); host = m.group(1) if m else '?'
    m = re.search(r'CPU max MHz:\s+([\d.]+)', t); mhz = m.group(1) if m else '?'
    m = re.search(r'Frequency boost:\s+(\S+)', t); boost = m.group(1) if m else 'n/a'
    return host, mhz, boost
def tput(p, wl='hit'):
    d = {}
    for r in csv.DictReader(open(p)):
        if r.get('log2n', '27') == '27' and r['workload'] == wl: d[r['variant']] = float(r['mops'])
    return d
def prod(p, wl='hit'):
    d = {}
    for r in csv.DictReader(open(p)):
        if r['workload'] == wl: d.setdefault(r['cell'], []).append(float(r['mops']))
    return {k: st.median(v) for k, v in d.items()}
rows = []
for name, july, tag in NODES:
    for wl in ('hit', 'miss'):
        h, mhz, b = mach(f'{R}/results/2026-07_july/{july}/machine.txt'); d = tput(f'{R}/results/2026-07_july/{july}/throughput.csv', wl)
        best = max(d[k] for k in ('abseil', 'boost', 'ankerl')); bk = max(('abseil', 'boost', 'ankerl'), key=lambda k: d[k])
        rows.append((name, wl, 'July, main harness, L3 keep', h, mhz, b, d['v4'], bk, best))
        h, mhz, b = mach(f'{R}/results/2026-09_september/results_r2_{tag}/machine.txt')
        for hint, lab in (('', 'L3 keep'), ('_pf2', 'L2 keep'), ('_pf3', 'L1 keep')):
            p = f'{R}/results/2026-09_september/results_r2_{tag}/iso{hint}_throughput.csv'
            if not os.path.exists(p): continue
            d = tput(p, wl); best = max(d[k] for k in ('abseil', 'boost', 'ankerl')); bk = max(('abseil', 'boost', 'ankerl'), key=lambda k: d[k])
            rows.append((name, wl, f'Sept. 23, isolated harness, {lab}', h, mhz, b, d['v4'], bk, best))
        h, mhz, b = mach(f'{R}/results/2026-09_late-september/results_r3_{tag}/machine.txt'); d = prod(f'{R}/results/2026-09_late-september/results_r3_{tag}/prod_prefetch.csv', wl)
        sc = {k: d[f'{k}-sync'] for k in ('abseil', 'boost', 'ankerl')}; bk = max(sc, key=sc.get)
        rows.append((name, wl, 'Sept. 24, prod_prefetch harness, L1 keep', h, mhz, b, d['Inline-AMAC'], bk, sc[bk]))
w = csv.writer(open(f'{R}/results/2026-10_october/provenance_x86.csv', 'w'))
w.writerow(['core', 'workload', 'campaign', 'host', 'cpu_max_mhz', 'boost', 'inline_engine', 'best_scalar_map', 'best_scalar', 'speedup'])
for r in rows:
    w.writerow(list(r) + [f'{r[6]/r[8]:.2f}'])
    print(f'{r[0]:13s} {r[1]:4s} {r[2]:42s} {r[3]:28s} {r[4]:>9s} {r[5]:8s} inline {r[6]:6.1f}  {r[7]:7s} {r[8]:6.1f}  spd {r[6]/r[8]:.2f}')
