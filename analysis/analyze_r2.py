#!/usr/bin/env python3
"""September analysis: ablation2 (per build), prefetch sweep, iso both hints, lat2, filter cost/churn.
Within a layout, ratios are block-paired (same table, same block streams). Across layouts the tables are
separate runs, so the interaction bootstraps each layout's blocks independently.  -> results_r2_REPORT.md"""
import glob, os, sys, numpy as np, pandas as pd
rng = np.random.default_rng(2026); B = 10000
out = []
def P(s=""): out.append(s)

def within(d, a, b):
    """per-block ratio a/b (paired), returns array"""
    x = d[d.cell == a].set_index("block").mops; y = d[d.cell == b].set_index("block").mops
    k = x.index.intersection(y.index); return (x[k] / y[k]).values
def ci_med(r):
    bs = np.median(r[rng.integers(0, len(r), (B, len(r)))], 1); return np.median(r), *np.percentile(bs, [2.5, 97.5])
def inter(r1, r0):
    bs = np.median(r1[rng.integers(0, len(r1), (B, len(r1)))], 1) / np.median(r0[rng.integers(0, len(r0), (B, len(r0)))], 1)
    return np.median(r1) / np.median(r0), *np.percentile(bs, [2.5, 97.5])
def rd(fn, **k):
    try: return pd.read_csv(fn, **k)
    except Exception: return pd.DataFrame()
f = lambda t: f"{t[0]:.2f} [{t[1]:.2f}, {t[2]:.2f}]"

dirs = sorted(glob.glob("results_r2_*/"))
P("# September results (G5K, 2^27 keys, one pinned core)\n")
P("## 1. Ablation (ablation2.cc): medians (Mops/s) and paired speed-ups vs the layout's own sync\n")
P("Builds: `orig` = patched headers with the paper's hint (locality 1) for the original engines; NB the engines added later (SoA staged, Detached staged_v, coro) use locality 3 even in `orig`; `pf3` = every prefetch at locality 3 (matched). strong = `-DSTRONG_HASH=1` (Inline hashed with fmix64 like SoA/Detached).\n")
for d in dirs:
    tag = d.strip("/").replace("results_r2_", "")
    for fn in sorted(glob.glob(d + "ablation2_*.csv")):
        df = rd(fn)
        if df.empty: continue
        build = os.path.basename(fn)[10:-4]
        P(f"### {tag} - {build}\n"); P("| workload | cell | median | vs own sync [95% CI] |"); P("|---|---|---:|---:|")
        for wl, g in df.groupby("workload"):
            for c in g.cell.unique():
                med = g[g.cell == c].mops.median(); lay = c.split("-")[0]
                s = f(ci_med(within(g, c, lay + "-sync"))) if not c.endswith("sync") else "1"
                P(f"| {wl} | {c} | {med:.1f} | {s} |")
        P()
        for wl, g in df.groupby("workload"):
            ri = within(g, "Inline-AMAC", "Inline-sync")
            for ref in ("SoA-AMAC-staged-v", "Chunk-AMAC-staged-v", "SoA-AMAC-ptr", "Chunk-AMAC-ptr"):
                lay = ref.split("-")[0]
                if (g.cell == ref).any():
                    P(f"- {wl}: interaction (Inline-AMAC/Inline-sync) / ({ref}/{lay}-sync) = **{f(inter(ri, within(g, ref, lay + '-sync')))}**")
            if (g.cell == "SoA-sync").any():
                a = g[g.cell == "Inline-AMAC"].mops.median(); P(f"- {wl}: Inline-AMAC / SoA-sync = {a / g[g.cell == 'SoA-sync'].mops.median():.2f}; Inline-sync / SoA-sync = {g[g.cell == 'Inline-sync'].mops.median() / g[g.cell == 'SoA-sync'].mops.median():.2f}; Inline-AMAC / SoA-AMAC-staged-v = {a / g[g.cell == 'SoA-AMAC-staged-v'].mops.median():.2f}")
        P()

P("## 2. Prefetch hint x depth sweep (one shared Inline table), median Mops/s\n")
for d in dirs:
    fn = d + "prefetch_sweep.csv"
    if not os.path.exists(fn): continue
    df = rd(fn)
    if df.empty or "locality" not in df: continue
    P(f"### {d.strip('/').replace('results_r2_', '')}\n")
    t = df.groupby(["workload", "depth", "locality"]).mops.median().unstack("locality")
    P(t.round(1).to_markdown()); P()

P("## 3. Headline throughput (iso2.cc) per prefetch hint, median of 15 (orig = paper hint, locality 1; pf2 = locality 2, ARM only; pf3 = locality 3)\n")
for d in dirs:
    H = [(h, rd(d + fn)) for h, fn in (("orig", "iso_throughput.csv"), ("pf2", "iso_pf2_throughput.csv"), ("pf3", "iso_pf3_throughput.csv"))]
    H = [(h, t) for h, t in H if not t.empty]
    if not H: continue
    P(f"### {d.strip('/').replace('results_r2_', '')}\n")
    m = H[0][1][["workload", "variant"]].copy()
    for h, t in H: m = m.merge(t[["workload", "variant", "mops"]].rename(columns={"mops": h}), on=["workload", "variant"])
    P(m.round(1).to_markdown(index=False)); P()
    for wl in ("hit", "miss", "zipf"):
        for h, t in H:
            s_ = t[t.workload == wl].set_index("variant").mops; best = s_[[v for v in ("abseil", "boost", "ankerl", "f14") if v in s_]].max()
            P(f"- {wl} [{h}]: Inline {s_['v4']:.1f} / best production {best:.1f} = **{s_['v4'] / best:.2f}x**; Inline+F {s_['vf']:.1f} ({s_['vf'] / best:.2f}x)")
    P()

P("## 4. Latency three ways (lat2.cc), ns/lookup, median of 5\n")
for d in dirs:
    for fn in sorted(glob.glob(d + "lat2_*.csv")):
        df = rd(fn)
        if df.empty: continue
        P(f"### {d.strip('/').replace('results_r2_', '')} - {os.path.basename(fn)[5:-4]}\n")
        P(df.groupby(["kind", "label"]).ns_per_lookup.median().unstack("label").round(1).to_markdown()); P()

P("## 5. Filter fix (filter_cost.cc, paper hint, 9 blocks) and churn (mode 2)\n")
for d in dirs:
    fs = sorted(glob.glob(d + "filter_cost_m*.csv"))
    if not fs: continue
    df = pd.concat([rd(x) for x in fs]);
    if df.empty: continue
    P(f"### {d.strip('/').replace('results_r2_', '')}\n")
    t = df.groupby(["keys", "workload", "table", "filter_mode"]).agg(mops=("mops", "median"), chunks_neg=("avg_chunks_neg", "median")).round(3)
    P(t.to_markdown()); P()
    c = d + "filter_churn_m2.csv"
    if os.path.exists(c): P(rd(c).to_markdown(index=False)); P()

open("results_r2_REPORT.md", "w").write("\n".join(out)); print("\n".join(out))
