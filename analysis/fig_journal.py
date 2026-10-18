#!/usr/bin/env python3
"""Regenerate the journal manuscript's data figures (vector PDF) in figures/.

Run from paper_journal/:   python3 fig_journal.py

Overwrites (names kept so the LaTeX needs no change):
  figures/v2_depth.pdf     depth sweep, Inline, hit, September campaign (results_r4/model_concurrency.csv)
  figures/v2_phase.pdf     Inline / Paired / Detached vs hit ratio, Ice Lake
  figures/v2_speedup.pdf   single-thread Inline speedup over best baseline, ten nodes, with intervals
  figures/v2_ablation.pdf  layout x engine ablation , bootstrap intervals
  figures/v2_latency.pdf   batched engine vs dependent chain / scalar stream, Ice Lake (Sept. lat2)
  figures/v2_scaling.pdf   multi-thread positive lookups, Grace and Ice Lake

Data:
  ../results/2026-07_july/results_<node>/{depth.txt,throughput.csv,mt_hit.csv}
  ../results/2026-07_july/results_hitratio_lille_icelake/hitratio.csv   (layout,h,mops)
  ../results/2026-09_september/results_r2_<node>/ablation2_pf3_strong0.csv        (cell,workload,log2n,block,order,mops)
  ../results/2026-09_september/results_r2_lille_icelake/lat2_orig.csv             (kind,label,rep,ns_per_lookup)
"""
import csv
import os
import re
import statistics

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FixedFormatter, NullLocator
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
R1 = os.path.join(HERE, '..', 'results', '2026-07_july')
R2 = os.path.join(HERE, '..', 'results', '2026-09_september')
R3B = os.path.join(HERE, '..', 'results', '2026-09_late-september_aligned')
OUT = os.path.join(HERE, 'figures')
os.makedirs(OUT, exist_ok=True)

# Okabe-Ito colour-blind-safe palette
OI = dict(blue='#0072B2', vermillion='#D55E00', green='#009E73', orange='#E69F00',
          sky='#56B4E9', purple='#CC79A7', yellow='#F0E442', black='#000000')
INK = '0.2'      # annotation text
GUIDE = '0.45'   # guide lines

plt.rcParams.update({
    'font.size': 8.5,
    'axes.labelsize': 9,
    'axes.titlesize': 9,
    'xtick.labelsize': 8,
    'ytick.labelsize': 8,
    'legend.fontsize': 8,
    'axes.linewidth': 0.7,
    'grid.color': '0.85',
    'grid.linewidth': 0.5,
    'grid.linestyle': '-',
    'pdf.fonttype': 42,
    'savefig.bbox': 'tight',
    'savefig.pad_inches': 0.02,
})

SOTA = ['abseil', 'boost', 'ankerl']
NODES10 = [
    ('results_chiclet_zen1', 'Zen 1'),
    ('results_chuc_zen3', 'Zen 3'),
    ('results_nancy_grele_broadwell', 'Broadwell'),
    ('results_nancy_grvingt_skylake', 'Skylake'),
    ('results_nancy_gros_cascadelake', 'Cascade Lake'),
    ('results_lille_chirop_icelake', 'Ice Lake'),
    ('results_grenoble_spr', 'Sapphire Rapids'),
    ('results_lyon_pyxis_thunderx2', 'ThunderX2'),
    ('results_grenoble_sasquatch_ampere', 'Neoverse N1'),
    ('results_lyon_hydra_grace', 'Neoverse V2'),
]


def save(fig, name):
    p = os.path.join(OUT, name)
    fig.savefig(p)
    plt.close(fig)
    print(f'wrote figures/{name}')


def log2_ticks(ax, axis, vals):
    a = ax.xaxis if axis == 'x' else ax.yaxis
    a.set_major_locator(FixedLocator(vals))
    a.set_major_formatter(FixedFormatter([str(v) for v in vals]))
    a.set_minor_locator(NullLocator())


# ------------------------------------------------------------------ FIG 1: depth sweep
def parse_depth(node):
    out = []
    p = os.path.join(R1, node, 'depth.txt')
    for ln in open(p):
        m = re.match(r'\s*DEPTH=\s*(\d+)\s*:\s*([\d.]+)', ln)
        if m:
            out.append((int(m.group(1)), float(m.group(2))))
    return out


def fig_depth():
    """Inline (the layout Table tab:mlppf tests), September campaign, positive lookups.  (a) throughput against
    the depth, with the synchronous loop at the left and each core's gather ceiling N_M^pf/Lm (C=1) as a dotted line;
    (b) the same points as lookups in flight, T*Lm, against the line T*Lm = D and the gather's N_M^pf.  Nothing fitted."""
    import csv as _csv
    CK = os.path.join(HERE, '..', 'results', '2026-10_october', 'model_concurrency.csv')
    rows = [r for r in _csv.DictReader(open(CK)) if r['layout'] == 'Inline' and r['workload'] == 'hit']
    sel = [('Neoverse V2', OI['purple'], 'D', (0, (4, 1.5, 1, 1.5, 1, 1.5)), 216),
           ('Sapphire Rapids', OI['vermillion'], 's', '--', 123),
           ('Neoverse N1', OI['green'], '^', '-.', 100),
           ('ThunderX2', OI['blue'], 'o', '-', 117)]
    SYNC = 2.0   # x position of the synchronous loop on the log axis
    fig, (ax, bx) = plt.subplots(1, 2, figsize=(7.2, 2.9))
    print('\n[depth] Inline, hit, 2^27, September campaign (Mops/s; lookups in flight)')
    for core, col, mk, ls, Lm in sel:
        pts = sorted((int(r['D']), float(r['mops']), float(r['lookups_in_flight_T_Lm']), float(r['Nm_prefetch'])) for r in rows if r['core'] == core)
        pts = [p for p in pts if p[0] <= 384]
        npf = pts[0][3]
        sw = [p for p in pts if p[0] > 0]; sy = [p for p in pts if p[0] == 0]
        ax.plot([p[0] for p in sw], [p[1] for p in sw], marker=mk, ms=3.8, lw=1.3, ls=ls, color=col, label=core, zorder=3)
        bx.plot([p[0] for p in sw], [p[2] for p in sw], marker=mk, ms=3.8, lw=1.3, ls=ls, color=col, label=core, zorder=3)
        if sy:
            ax.plot([SYNC], [sy[0][1]], marker=mk, ms=4.5, color=col, mfc='white', zorder=3)
            bx.plot([SYNC], [sy[0][2]], marker=mk, ms=4.5, color=col, mfc='white', zorder=3)
        ceil = npf / Lm * 1e3
        ax.hlines(ceil, 4, 384, color=col, ls=':', lw=1.0, zorder=2)
        bx.hlines(npf, 4, 384, color=col, ls=':', lw=1.0, zorder=2)
        print(f'  {core:16s} ceiling {ceil:.0f} Mops/s, N_M^pf {npf:.1f}; ' + ' '.join(f'D{p[0]}={p[1]:.1f}/{p[2]:.1f}' for p in pts))
    dd = np.array([2.6, 4, 8, 16, 32, 42])
    bx.plot(dd, dd, color=GUIDE, ls='--', lw=1.0, zorder=1)
    bx.text(12, 22, r'$T\,L_m = D$', fontsize=8, color=INK, ha='right')
    for a in (ax, bx):
        a.set_xscale('log', base=2)
        a.xaxis.set_major_locator(FixedLocator([SYNC, 4, 8, 16, 32, 64, 128, 384]))
        a.xaxis.set_major_formatter(FixedFormatter(['sync', '4', '8', '16', '32', '64', '128', '384']))
        a.xaxis.set_minor_locator(NullLocator())
        a.set_xlim(1.7, 450)
        a.axvline(32, color=GUIDE, ls=':', lw=0.8, zorder=1)
        a.set_xlabel('ring depth $D$ (lanes)')
        a.grid(True, which='major'); a.set_axisbelow(True)
    ax.set_ylim(0, 200); bx.set_ylim(0, 45)
    ax.set_ylabel('throughput (Mops/s)')
    bx.set_ylabel(r'lookups in flight, $T\,L_m$')
    ax.legend(frameon=False, loc='upper left', ncol=2, fontsize=7.5)
    ax.set_title(r'(a) throughput; dotted: $N_M^{\mathrm{pf}}/L_m$', fontsize=8.5)
    bx.set_title(r"(b) Little's law; dotted: $N_M^{\mathrm{pf}}$", fontsize=8.5)
    fig.tight_layout()
    save(fig, 'v2_depth.pdf')


# ------------------------------------------------------------------ FIG 2: phase diagram
def fig_phase():
    hr = {}
    for r in csv.DictReader(open(os.path.join(R1, 'results_hitratio_lille_icelake', 'hitratio.csv'))):
        hr.setdefault(r['layout'], {})[float(r['h'])] = float(r['mops'])
    lays = [('Inline', OI['blue'], 'o', '-'), ('Paired', OI['vermillion'], 's', '--'),
            ('Detached', OI['green'], '^', '-.')]
    col = {l: c for l, c, _, _ in lays}
    hs = sorted(hr['Inline'])
    winners = []
    print('\n[phase] Ice Lake, winner per hit ratio (Mops/s)')
    for h in hs:
        vals = {l: hr[l][h] for l, _, _, _ in lays}
        w = max(vals, key=vals.get)
        second = sorted(vals.values())[-2]
        winners.append(w)
        print(f'  h={h:.1f}  winner={w:8s}  ' + '  '.join(f'{l}={v:.1f}' for l, v in vals.items())
              + f'  margin over 2nd={100 * (vals[w] / second - 1):.1f}%')
    # piecewise regions between midpoints of consecutive measured h
    edges = [hs[0]] + [(hs[i] + hs[i + 1]) / 2 for i in range(len(hs) - 1)] + [hs[-1]]
    regions = []  # (lo, hi, winner) merged runs
    for i, w in enumerate(winners):
        if regions and regions[-1][2] == w:
            regions[-1][1] = edges[i + 1]
        else:
            regions.append([edges[i], edges[i + 1], w])
    print('  regions: ' + ', '.join(f'[{a:.2f},{b:.2f}] {w}' for a, b, w in regions))

    # empirical Inline-vs-Paired crossover (linear interpolation of the first sign change)
    cross = None
    for i in range(len(hs) - 1):
        di = hr['Inline'][hs[i]] - hr['Paired'][hs[i]]
        dj = hr['Inline'][hs[i + 1]] - hr['Paired'][hs[i + 1]]
        if di <= 0 < dj:
            cross = hs[i] + (hs[i + 1] - hs[i]) * (-di) / (dj - di)
            break
    m = hr['Inline'][1.0] / hr['Inline'][0.0]
    hstar = 1 - 1 / m
    print(f'  Inline/Paired empirical crossover h={cross:.3f}; model h*=1-1/m={hstar:.3f} (m={m:.3f})')

    fig, ax = plt.subplots(figsize=(4.4, 3.0))
    ytop = 128
    for lo, hi, w in regions:
        ax.axvspan(lo, hi, color=col[w], alpha=0.12, lw=0, zorder=0)
        mid = (lo + hi) / 2
        if hi - lo >= 0.14:
            ax.text(mid, ytop - 3, w, ha='center', va='top', fontsize=8, color=INK)
        else:
            ax.text(mid, ytop - 3, w, ha='center', va='top', fontsize=8, color=INK, rotation=90)
    for l, c, mk, ls in lays:
        ax.plot(hs, [hr[l][h] for h in hs], marker=mk, ms=4.5, lw=1.4, ls=ls, color=c, label=l, zorder=3)
    ax.axvline(cross, color=INK, ls='-', lw=0.9, zorder=2)
    ax.text(cross + 0.012, 30, f'Inline/Paired\ncrossover\n$h{{=}}{cross:.2f}$', fontsize=8,
            color=INK, va='center')
    ax.axvline(hstar, color=INK, ls=':', lw=1.1, zorder=2)
    ax.text(hstar - 0.012, 30, f'model\n$h^*{{=}}{hstar:.2f}$', fontsize=8, color=INK,
            ha='right', va='center')
    ax.set_xlim(hs[0], hs[-1])
    ax.set_ylim(0, ytop)
    ax.set_xticks(hs)
    ax.set_xticklabels([f'{h:.1f}' for h in hs])
    ax.set_xlabel('hit ratio $h$')
    ax.set_ylabel('throughput (Mops/s)')
    ax.legend(frameon=True, framealpha=0.9, edgecolor='none', loc='lower left', ncol=3,
              bbox_to_anchor=(0.0, 0.0), handlelength=2.4, columnspacing=1.0)
    ax.grid(True)
    ax.set_axisbelow(True)
    fig.tight_layout()
    save(fig, 'v2_phase.pdf')


# ------------------------------------------------------------------ FIG 3: ten-node speedup
def load_thr(node):
    out = {}
    for r in csv.DictReader(open(os.path.join(R1, node, 'throughput.csv'))):
        if r['log2n'] == '27':
            out[(r['workload'], r['variant'])] = (float(r['mops']), float(r['ci_lo']), float(r['ci_hi']))
    return out


def fig_speedup():
    labels, S = [], {'hit': [], 'zipf': []}
    print('\n[speedup] Inline (v4) / best baseline, 2^27, single thread: median [conservative interval]')
    for node, lab in NODES10:
        t = load_thr(node)
        labels.append(lab)
        row = []
        for wl in ('hit', 'zipf'):
            v, vlo, vhi = t[(wl, 'v4')]
            best = max((s for s in SOTA if (wl, s) in t), key=lambda s: t[(wl, s)][0])
            b, blo, bhi = t[(wl, best)]
            sp, lo, hi = v / b, vlo / bhi, vhi / blo
            S[wl].append((sp, lo, hi))
            row.append(f'{wl} {sp:.2f} [{lo:.2f}, {hi:.2f}] (best={best})')
        print(f'  {lab:16s} ' + ' | '.join(row))
    x = np.arange(len(labels))
    w = 0.38
    fig, ax = plt.subplots(figsize=(7.0, 3.0))
    for off, wl, c, hatch in [(-w / 2, 'hit', OI['blue'], None), (w / 2, 'zipf', OI['orange'], '////')]:
        sp = np.array([s for s, _, _ in S[wl]])
        lo = np.array([l for _, l, _ in S[wl]])
        hi = np.array([h for _, _, h in S[wl]])
        ax.bar(x + off, sp, w * 0.94, color=c, hatch=hatch, edgecolor='white', lw=0.0,
               label={'hit': 'positive (hit)', 'zipf': 'skewed (zipf 0.99)'}[wl], zorder=2)
        ax.errorbar(x + off, sp, yerr=[sp - lo, hi - sp], fmt='none', ecolor='0.1',
                    elinewidth=0.9, capsize=2.2, capthick=0.9, zorder=3)
    ax.axhline(1.0, color='0.1', lw=0.8, ls='--', zorder=2)
    ax.text(len(labels) - 0.38, 1.0, ' parity', fontsize=8, ha='left', va='center', color=INK,
            clip_on=False)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=30, ha='right')
    ax.set_xlim(-0.6, len(labels) - 0.4)
    top = max(h for wl in S for _, _, h in S[wl])
    ax.set_ylim(0, np.ceil(top * 1.15 * 2) / 2)
    ax.set_ylabel('speedup over best of\nAbseil / Boost / ankerl')
    ax.legend(frameon=False, loc='upper left', ncol=2)
    ax.grid(True, axis='y')
    ax.set_axisbelow(True)
    fig.tight_layout()
    save(fig, 'v2_speedup.pdf')


# ------------------------------------------------------------------ FIG 4: ablation
ABL_CELLS = [
    ('SoA-AMAC-staged-v', 'Separate arrays, staged engine', OI['sky'], None),
    ('Inline-sync', 'Inline, sync', OI['green'], '////'),
    ('Inline-AMAC', 'Inline, AMAC', OI['blue'], None),
    ('Chunk-sync', 'Detached, sync', OI['orange'], '////'),
    ('Chunk-AMAC-staged-v', 'Detached, staged engine', OI['vermillion'], None),
    ('Inline-coro', 'Inline, coroutines', OI['purple'], '....'),
]
ABL_NODES = [('lille_icelake', 'Ice Lake'), ('nancy_cascadelake', 'Cascade Lake'), ('lille_zen3', 'Zen 3')]


def fig_ablation():
    B = 10000
    res = {}
    print('\n[ablation] hit, 2^27, pf3/strong0, SoA aligned (r3b): median of 15 block ratios to SoA-sync '
          '[95% bootstrap CI, 10000 resamples, default_rng(2026) per cell]')
    for node, lab in ABL_NODES:
        blk = {}
        for r in csv.DictReader(open(os.path.join(R3B, f'results_r3b_{node}', 'ablation2_aligned_strong0.csv'))):
            if r['workload'] == 'hit':
                blk.setdefault(r['cell'], {})[int(r['block'])] = float(r['mops'])
        base = blk['SoA-sync']
        parts = []
        for cell, name, _, _ in ABL_CELLS:
            ks = sorted(set(blk[cell]) & set(base))
            assert len(ks) == 15, (node, cell, len(ks))
            rat = np.array([blk[cell][k] / base[k] for k in ks])
            rng = np.random.default_rng(2026)
            bs = np.median(rat[rng.integers(0, len(rat), (B, len(rat)))], axis=1)
            lo, hi = np.percentile(bs, [2.5, 97.5])
            med = float(np.median(rat))
            rom = statistics.median(blk[cell].values()) / statistics.median(base.values())
            res[(node, cell)] = (med, lo, hi)
            parts.append(f'{name}: {med:.2f} [{lo:.2f}, {hi:.2f}] (ratio of medians {rom:.2f})')
        print(f'  {lab} (SoA-sync median {statistics.median(base.values()):.1f} Mops/s)')
        for p in parts:
            print('     ' + p)

    n = len(ABL_CELLS)
    w = 0.8 / n
    x = np.arange(len(ABL_NODES))
    fig, ax = plt.subplots(figsize=(4.7, 3.3))
    for j, (cell, name, c, hatch) in enumerate(ABL_CELLS):
        med = np.array([res[(nd, cell)][0] for nd, _ in ABL_NODES])
        lo = np.array([res[(nd, cell)][1] for nd, _ in ABL_NODES])
        hi = np.array([res[(nd, cell)][2] for nd, _ in ABL_NODES])
        xs = x + (j - (n - 1) / 2) * w
        ax.bar(xs, med, w * 0.9, color=c, hatch=hatch, edgecolor='white', lw=0.0, label=name, zorder=2)
        ax.errorbar(xs, med, yerr=[med - lo, hi - med], fmt='none', ecolor='0.1',
                    elinewidth=0.9, capsize=1.8, capthick=0.9, zorder=3)
    ax.axhline(1.0, color='0.1', lw=0.8, ls='--', zorder=2)
    ax.set_xticks(x)
    ax.set_xticklabels([lab for _, lab in ABL_NODES])
    top = max(v[2] for v in res.values())
    ax.set_ylim(0, np.ceil(top * 1.08 * 2) / 2)
    ax.set_ylabel('speedup over SwissTable\n(separate arrays, synchronous)')
    ax.legend(frameon=False, loc='lower center', bbox_to_anchor=(0.5, 1.0), ncol=2,
              handlelength=1.6, columnspacing=1.2)
    ax.grid(True, axis='y')
    ax.set_axisbelow(True)
    fig.tight_layout()
    save(fig, 'v2_ablation.pdf')


# ------------------------------------------------------------------ FIG 5: latency
def fig_latency():
    agg = {}
    for r in csv.DictReader(open(os.path.join(R2, 'results_r2_lille_icelake', 'lat2_orig.csv'))):
        agg.setdefault((r['kind'], r['label']), []).append(float(r['ns_per_lookup']))
    L = {k: statistics.median(v) for k, v in agg.items()}
    bs = [1, 8, 32, 256]
    ys = [L[(f'batched-B{b}', 'Inline')] for b in bs]
    chI, chB, scI = L[('serialized-chain', 'Inline')], L[('serialized-chain', 'boost')], L[('scalar-stream', 'Inline')]
    print('\n[latency] Ice Lake lat2_orig, medians over reps (ns/lookup)')
    print('  batched: ' + ', '.join(f'B={b}: {y:.1f}' for b, y in zip(bs, ys)))
    print(f'  dependent chain Inline {chI:.1f}, Boost {chB:.1f}; scalar stream Inline {scI:.1f}')

    fig, ax = plt.subplots(figsize=(4.2, 3.4))
    ax.axhline(chB, color=OI['vermillion'], ls='-.', lw=1.3, label=f'dependent chain, Boost ({chB:.0f} ns)')
    ax.axhline(chI, color=OI['orange'], ls='--', lw=1.3, label=f'dependent chain, Inline ({chI:.0f} ns)')
    ax.axhline(scI, color=OI['sky'], ls=':', lw=1.6, label=f'scalar stream, Inline ({scI:.0f} ns)')
    ax.plot(bs, ys, marker='o', ms=5, lw=1.6, color=OI['blue'],
            label='batched engine, completion time per lookup', zorder=3)
    ax.set_xscale('log', base=2)
    log2_ticks(ax, 'x', bs)
    ax.set_xlim(0.75, 340)
    ax.set_yscale('log')
    yt = [10, 20, 50, 100, 200]
    ax.yaxis.set_major_locator(FixedLocator(yt))
    ax.yaxis.set_major_formatter(FixedFormatter([str(v) for v in yt]))
    ax.yaxis.set_minor_locator(NullLocator())
    ax.set_ylim(8, 260)
    ax.set_xlabel('batch size (independent lookups)')
    ax.set_ylabel('ns per lookup')
    ax.legend(frameon=False, loc='upper left', bbox_to_anchor=(-0.02, -0.18), ncol=1)
    ax.grid(True)
    ax.set_axisbelow(True)
    fig.tight_layout()
    save(fig, 'v2_latency.pdf')


# ------------------------------------------------------------------ FIG 6: multi-thread scaling
def load_mt(node):
    pts = {}
    for r in csv.DictReader(open(os.path.join(R1, node, 'mt_hit.csv'))):
        pts.setdefault(r['variant'], {})[int(r['threads'])] = float(r['mops'])
    return pts


def fig_scaling():
    sel = [('results_lyon_hydra_grace', 'Neoverse V2', OI['blue'], 'o'),
           ('results_lille_chirop_icelake', 'Ice Lake', OI['vermillion'], 's')]
    fig, ax = plt.subplots(figsize=(4.2, 3.4))
    print('\n[scaling] mt_hit.csv, Inline vs best of abseil/boost/ankerl (Mops/s)')
    allt = set()
    for node, name, c, mk in sel:
        mt = load_mt(node)
        th = sorted(mt['v4'])
        allt |= set(th)
        inl = [mt['v4'][t] for t in th]
        best = [max(mt[s][t] for s in SOTA if s in mt and t in mt[s]) for t in th]
        ax.plot(th, inl, marker=mk, ms=4.5, lw=1.5, ls='-', color=c, label=f'{name}, Inline', zorder=3)
        ax.plot(th, best, marker=mk, ms=4.5, lw=1.2, ls='--', color=c, mfc='white', mew=1.1,
                label=f'{name}, best baseline', zorder=3)
        print(f'  {name}: ' + ' '.join(f't{t}={a:.0f}/{b:.0f}' for t, a, b in zip(th, inl, best)))
    ax.set_xscale('log', base=2)
    ax.set_yscale('log', base=2)
    xt = [1, 2, 4, 8, 16, 32, 64, 128]
    log2_ticks(ax, 'x', xt)
    ax.set_xlim(0.85, 150)
    yt = [32, 64, 128, 256, 512, 1024, 2048, 4096]
    log2_ticks(ax, 'y', yt)
    ax.set_ylim(24, 4600)
    ax.set_xlabel('threads')
    ax.set_ylabel('throughput (Mops/s)')
    ax.legend(frameon=False, loc='upper center', bbox_to_anchor=(0.45, -0.17), ncol=2,
              columnspacing=1.2, handlelength=2.4)
    ax.grid(True)
    ax.set_axisbelow(True)
    fig.tight_layout()
    save(fig, 'v2_scaling.pdf')


if __name__ == '__main__':
    fig_depth()
    fig_phase()
    fig_speedup()
    fig_ablation()
    fig_latency()
    fig_scaling()
