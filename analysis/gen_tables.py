#!/usr/bin/env python3
"""Generates every table body in tables/ from the raw CSVs in ../results_r2 and ../results/apple-m4_feature-store.
Run from paper_journal/.  Nothing in tables/ is hand-edited."""
import os, glob, numpy as np, pandas as pd
R2 = "../results/2026-09_september"; R3B = "../results/2026-09_late-september_aligned"; WL = "../results/apple-m4_feature-store"; os.makedirs("tables", exist_ok=True)
rng = np.random.default_rng(2026); B = 10000
NODES = [("lille_icelake", "Ice Lake"), ("nancy_cascadelake", "Cascade Lake"), ("lille_zen3", "Zen 3"),
         ("lyon_grace", "Neoverse V2"), ("grenoble_ampere", "Neoverse N1"), ("lyon_thunderx2", "ThunderX2")]
def med(v): return float(np.median(v))
def ratio_ci(a, b):  # independent bootstrap of two block sets (different table builds / streams)
    a = np.asarray(a, float); b = np.asarray(b, float)
    bs = np.median(a[rng.integers(0, len(a), (B, len(a)))], 1) / np.median(b[rng.integers(0, len(b), (B, len(b)))], 1)
    return med(a) / med(b), *np.percentile(bs, [2.5, 97.5])
def paired_ci(a, b):  # same table build, same block index = same streams
    a = np.asarray(a, float); b = np.asarray(b, float); idx = rng.integers(0, len(a), (B, len(a)))
    bs = np.median(a[idx], 1) / np.median(b[idx], 1); return med(a) / med(b), *np.percentile(bs, [2.5, 97.5])
def W(name, s):
    lines = s.split("\n")
    while lines and lines[-1].strip() == r"\addlinespace": lines.pop()   # booktabs: no spacer directly before \bottomrule
    if lines and lines[-1].rstrip().endswith("\\\\"): lines[-1] = lines[-1].rstrip()[:-2].rstrip()   # the section file supplies the final \\ after \input
    open(f"tables/{name}.tex", "w").write("\n".join(lines) + "\n"); print("wrote", name)

# ---- 1. ablation, x86, hint-matched (pf3), both hash builds --------------------------------------------------
CELLS = [("SoA-sync", "SoA, synchronous"), ("SoA-AMAC-ptr", "SoA, engine, first key line only"), ("SoA-AMAC-staged-v", "SoA, staged engine"),
         ("Chunk-sync", "Detached, synchronous"), ("Chunk-AMAC-staged-v", "Detached, staged engine"),
         ("Inline-sync", "Inline, synchronous"), ("Inline-AMAC", "Inline, engine"), ("Inline-coro", "Inline, coroutine ring")]
BASE = {"SoA": "SoA-sync", "Chunk": "Chunk-sync", "Inline": "Inline-sync"}
rows = []; inter = []
for tag, name in NODES[:3]:
    # late-September (aligned): the separate-array table's arrays 64-byte aligned (the r2 build left half of its 32-byte control groups
    # straddling two lines); the r2 files stay in ../results/2026-09_september as the unaligned history
    d0 = pd.read_csv(f"{R3B}/results_r3b_{tag}/ablation2_aligned_strong0.csv"); d1 = pd.read_csv(f"{R3B}/results_r3b_{tag}/ablation2_aligned_strong1.csv")
    for wl in ("hit", "miss"):
        for cell, label in CELLS:
            out = [f"{name if (wl=='hit' and cell=='SoA-sync') else ''}", wl if cell == "SoA-sync" else "", label]
            for d in (d0, d1):
                g = d[(d.workload == wl) & (d.cell == cell)]; b = d[(d.workload == wl) & (d.cell == BASE[cell.split('-')[0]])]
                if g.empty: out += ["--", "--"]; continue
                bb = g.sort_values("block").mops.values; ba = b.sort_values("block").mops.values
                r, lo, hi = paired_ci(bb, ba) if cell != BASE[cell.split('-')[0]] else (1.0, 1.0, 1.0)
                out += [f"{med(bb):.1f}", "1 (ref.)" if cell == BASE[cell.split('-')[0]] else f"{r:.2f} [{lo:.2f}, {hi:.2f}]"]
            rows.append(" & ".join(out) + r" \\")
        rows.append(r"\addlinespace")
        for d, hb in ((d0, "own hash"), (d1, "equalised")):
            g = lambda c: d[(d.workload == wl) & (d.cell == c)].sort_values("block").mops.values
            ia, is_, sa, ss = g("Inline-AMAC"), g("Inline-sync"), g("SoA-AMAC-staged-v"), g("SoA-sync")
            r = ia / is_; q = sa / ss; bs = np.median(r[rng.integers(0, 15, (B, 15))], 1) / np.median(q[rng.integers(0, 15, (B, 15))], 1)
            ca, cs = g("Chunk-AMAC-staged-v"), g("Chunk-sync"); q2 = ca / cs; bs2 = np.median(r[rng.integers(0, 15, (B, 15))], 1) / np.median(q2[rng.integers(0, 15, (B, 15))], 1)
            inter.append(f"{name} & {wl} & {hb} & {med(ia)/med(ss):.2f} & {med(is_)/med(ss):.2f} & {med(sa)/med(ss):.2f} & {med(r)/med(q):.2f} [{np.percentile(bs,2.5):.2f}, {np.percentile(bs,97.5):.2f}] & {med(r)/med(q2):.2f} [{np.percentile(bs2,2.5):.2f}, {np.percentile(bs2,97.5):.2f}] \\\\")
W("ablation_r2", "\n".join(rows)); W("interaction_r2", "\n".join(inter))

# ---- 2. prefetch-hint sweep, all six nodes ---------------------------------------------------------------------
HN = {-1: "none", 0: "L1 stream", 1: "L3 keep", 2: "L2 keep", 3: "L1 keep"}
rows = []
for tag, name in NODES:
    s = pd.read_csv(f"{R2}/results_r2_{tag}/prefetch_sweep.csv")
    for wl in ("hit", "miss"):
        sc = med(s[(s.workload == wl) & (s.depth == 0)].mops); g = s[(s.workload == wl) & (s.depth > 0)].groupby(["locality", "depth"]).mops.median()
        cells = [f"{name if wl=='hit' else ''}", wl, f"{sc:.1f}"] + [f"{g[(l, 32)]:.1f}" for l in (-1, 0, 1, 2, 3)] + [f"{g[l].max():.1f} ({g[l].idxmax()})" for l in (1, 2, 3)]
        rows.append(" & ".join(cells) + r" \\")
W("hint_sweep", "\n".join(rows))

# ---- 3. isolated protocol per hint on the ARM nodes: Inline and Inline+F vs best production map --------------
rows = []
for tag, name in NODES[3:]:
    for hint, label in (("", "L3 keep (paper, July)"), ("_pf2", "L2 keep"), ("_pf3", "L1 keep")):
        f = f"{R2}/results_r2_{tag}/iso{hint}_raw.csv"
        if not os.path.exists(f): continue
        r = pd.read_csv(f, header=None, names=["machine", "workload", "variant", "rep", "mops"]); m = r.groupby(["workload", "variant"]).mops.median()
        cells = [name if hint == "" else "", label]
        for wl in ("hit", "miss", "zipf"):
            best = max(m.get((wl, b), 0) for b in ("abseil", "boost", "ankerl")); cells += [f"{m[(wl,'v4')]:.0f}", f"{m[(wl,'vf')]:.0f}", f"{best:.0f}", f"{m[(wl,'v4')]/best:.2f}", f"{m[(wl,'vf')]/best:.2f}"]
        rows.append(" & ".join(cells) + r" \\")
W("iso_hint_arm", "\n".join(rows))

# ---- 4. latency, three metrics, six nodes ------------------------------------------------------------------------
rows = []
for tag, name in NODES:
    hint = "pf2" if tag in ("lyon_grace", "grenoble_ampere", "lyon_thunderx2") else "orig"
    r = pd.read_csv(f"{R2}/results_r2_{tag}/lat2_{hint}.csv"); m = r.groupby(["kind", "label"]).ns_per_lookup.median()
    for i, lab in enumerate(("Inline", "boost", "abseil")):
        g = lambda k: f"{m[(k, lab)]:.0f}" if (k, lab) in m else "--"
        rows.append(f"{name if i==0 else ''} & {'Inline (ours)' if lab=='Inline' else lab.capitalize()} & {g('scalar-stream')} & {g('serialized-chain')} & {g('batched-B1')} & {g('batched-B32')} & {g('batched-B256')} \\\\")
    rows.append(r"\addlinespace")
W("latency_r2", "\n".join(rows))

# ---- 5. filter fix on x86 ---------------------------------------------------------------------------------------
rows = []
for tag, name in NODES[:3]:
    d0 = pd.read_csv(f"{R2}/results_r2_{tag}/filter_cost_m0.csv"); d2 = pd.read_csv(f"{R2}/results_r2_{tag}/filter_cost_m2.csv")
    for keys, kl in (("random", "random"), ("aligned4096", "4 KiB-aligned")):
        cells = [name if keys == "random" else "", kl]
        for wl in ("hit", "miss"):
            t0 = d0.groupby(["keys", "table", "workload"]).mops.median(); t2 = d2.groupby(["keys", "table", "workload"]).mops.median()
            cells += [f"{t0[(keys,'Inline',wl)]:.0f}", f"{t0[(keys,'Inline+F',wl)]:.0f}", f"{t2[(keys,'Inline+F',wl)]:.0f}"]
        n0 = d0[(d0["keys"] == keys) & (d0.table == "Inline+F") & (d0.workload == "miss")].avg_chunks_neg.mean(); n2 = d2[(d2["keys"] == keys) & (d2.table == "Inline+F") & (d2.workload == "miss")].avg_chunks_neg.mean()
        cells += [f"{n0:.2f}", f"{n2:.2f}"]; rows.append(" & ".join(cells) + r" \\")
W("filter_r2", "\n".join(rows))

# ---- 6. churn (deterministic, one node suffices) ------------------------------------------------------------------
c = pd.read_csv(f"{R2}/results_r2_lille_icelake/filter_churn_m2.csv", dtype={"ops": str}); rows = []
for _, r in c.iterrows():
    o = r.ops
    if o.endswith("rebuild") or o in ("0", "5000000", "10000000", "20000000"):
        lab = {"0": "0", "5000000": "5 M", "10000000": "10 M", "20000000": "20 M", "15000000-before-rebuild": "15 M, before rebuild", "15000000-after-rebuild": "15 M, after rebuild"}[o]
        rows.append(f"{lab} & {r.tombstone_frac:.3f} & {r.avg_chunks_neg:.3f} & {r.consulted_frac:.2f} & {100*r.fp_rate:.2f}\\% & {r.avg_chunks_pos:.2f} & {r.check} \\\\")
W("churn_r2", "\n".join(rows))

# ---- 7. feature-store workload, version 2 (Apple M4 Max; five repetitions, median across them) -------------------
ENG = [("inline-batch", 0, "Inline, per-request batch"), ("inline-coalesce", 20, "Inline, coalesced, $\\tau{=}20$\\,\\textmu s"), ("inline-coalesce", 100, "Inline, coalesced, $\\tau{=}100$\\,\\textmu s"),
       ("inline-sync", 0, "Inline, scalar"), ("boost", 0, "Boost, scalar"), ("abseil", 0, "Abseil, scalar")]
rows = []; closed = []; spread = []
for cfg, label in (("recommender", "recommender"), ("iot-registry", "IoT registry"), ("batch-retrieval", "batch retrieval")):
    d = pd.read_csv(f"{WL}/{cfg}.csv")
    cl = d.groupby("engine").agg(c=("ids_per_s_closed", "median"), r=("ids_per_s_closed_req", "median"))
    closed.append(f"{label} & {d.mean_k.median():.0f} & " + " & ".join(f"{cl.c[e]/1e6:.0f}" for e in ("inline-batch", "inline-coalesce", "inline-sync", "boost", "abseil")) + f" & {cl.r['inline-coalesce']/1e6:.0f}" + r" \\")
    lams = sorted(d.lambda_req_s.unique()); pick = [lams[1], lams[3], lams[4]]
    for i, (eng, tau, name) in enumerate(ENG):
        g = d[(d.engine == eng) & (d.tau_us == tau)]; cells = [label if i == 0 else "", name]
        for lam in pick:
            r = g[g.lambda_req_s == lam]
            if r.empty: cells += ["--", "--", "--"]; continue
            u = r.util.median(); sat = u > 0.95
            cells += [f"{u:.2f}", "sat." if sat else f"{r.p50_us.median():.1f}", "sat." if sat else f"{r.p99_us.median():.1f}"]
            if not sat: spread.append((r.p99_us.max() - r.p99_us.min()) / r.p99_us.median())
        rows.append(" & ".join(cells) + r" \\")
    rows.append(r"\addlinespace")
W("featurestore", "\n".join(rows)); W("featurestore_closed", "\n".join(closed))
print(f"featurestore: p99 min-max spread across reps, unsaturated cells: median {100*np.median(spread):.0f}%, max {100*max(spread):.0f}%")

# ---- 8. ARM partial replication of the ablation (Inline and Detached cells; the SoA header is x86 only) ----------
CELLS_ARM = [("Chunk-sync", "Detached, synchronous"), ("Chunk-AMAC-staged-v", "Detached, staged engine"), ("Inline-sync", "Inline, synchronous"), ("Inline-AMAC", "Inline, engine"), ("Inline-coro", "Inline, coroutine ring")]
rows = []; inter = []
for tag, name in NODES[3:]:
    d0 = pd.read_csv(f"{R2}/results_r2_{tag}/ablation2_pf2_strong0.csv"); d1 = pd.read_csv(f"{R2}/results_r2_{tag}/ablation2_pf2_strong1.csv")
    for wl in ("hit", "miss"):
        for cell, label in CELLS_ARM:
            out = [f"{name if (wl=='hit' and cell=='Chunk-sync') else ''}", wl if cell == "Chunk-sync" else "", label]
            for d in (d0, d1):
                g = d[(d.workload == wl) & (d.cell == cell)]; b = d[(d.workload == wl) & (d.cell == BASE[cell.split('-')[0]])]
                bb = g.sort_values("block").mops.values; ba = b.sort_values("block").mops.values
                r, lo, hi = paired_ci(bb, ba) if cell != BASE[cell.split('-')[0]] else (1.0, 1.0, 1.0)
                out += [f"{med(bb):.1f}", "1 (ref.)" if cell == BASE[cell.split('-')[0]] else f"{r:.2f} [{lo:.2f}, {hi:.2f}]"]
            rows.append(" & ".join(out) + r" \\")
        rows.append(r"\addlinespace")
        for d, hb in ((d0, "own hash"), (d1, "equalised")):
            g = lambda c: d[(d.workload == wl) & (d.cell == c)].sort_values("block").mops.values
            ia, is_, ca, cs = g("Inline-AMAC"), g("Inline-sync"), g("Chunk-AMAC-staged-v"), g("Chunk-sync"); r = ia / is_; q = ca / cs
            bs = np.median(r[rng.integers(0, 15, (B, 15))], 1) / np.median(q[rng.integers(0, 15, (B, 15))], 1)
            inter.append(f"{name} & {wl} & {hb} & {med(is_)/med(cs):.2f} & {med(r):.2f} & {med(q):.2f} & {med(r)/med(q):.2f} [{np.percentile(bs,2.5):.2f}, {np.percentile(bs,97.5):.2f}] \\\\")
W("ablation_arm", "\n".join(rows)); W("interaction_arm", "\n".join(inter))

# ---- 9. overflowed chunks per load (deterministic; M4 run of overflow_count.cc) ---------------------------------
o = pd.read_csv("../results/apple-m4/overflow_count.csv"); rows = []
for _, r in o.iterrows():
    rows.append(f"{r['load']:.2f} & {100*r.full_frac:.1f}\\% & {100*r.overflowed_frac:.1f}\\% & {100*r.consulted_frac:.1f}\\% & {r.chunks_per_negative:.4f} \\\\")
W("overflow", "\n".join(rows))

# ---- 10. prefetch-capable production maps (prod_prefetch.cc); M4 now, Grid'5000 nodes when present ---------------
PP = [("../results/apple-m4/prod_prefetch.csv", "Apple M4 Max")] + [(f"../results/2026-09_late-september/results_r3_{t}/prod_prefetch.csv", n) for t, n in NODES]
PCELLS = [("abseil-sync", "Abseil, scalar"), ("abseil-prefetch8", "Abseil, prefetch 8 ahead"), ("abseil-prefetch16", "Abseil, prefetch 16 ahead"), ("abseil-prefetch32", "Abseil, prefetch 32 ahead"), ("abseil-prefetch64", "Abseil, prefetch 64 ahead"),
          ("f14-sync", "F14, scalar"), ("f14-token8", "F14, token 8 ahead"), ("f14-token16", "F14, token 16 ahead"), ("f14-token32", "F14, token 32 ahead"), ("f14-token64", "F14, token 64 ahead"),
          ("boost-sync", "Boost, scalar"), ("ankerl-sync", "ankerl, scalar"), ("Inline-sync", "Inline, synchronous"), ("Inline-AMAC", "Inline, engine $D{=}32$"), ("InlineF-sync", "Inline+F, synchronous"), ("InlineF-AMAC", "Inline+F, engine $D{=}32$")]
rows_x86 = []; rows_arm = []
for f, name in PP:
    if not os.path.exists(f): continue
    rows = rows_arm if name in ("Neoverse V2", "Neoverse N1", "ThunderX2") else rows_x86   # one float per page: x86 + M4, then ARM
    d = pd.read_csv(f); first = True
    for cell, label in PCELLS:
        if d[d.cell == cell].empty: continue
        out = [name if first else "", label]; first = False
        for wl in ("hit", "miss"):
            g = d[(d.workload == wl) & (d.cell == cell)].sort_values("block").mops.values; b = d[(d.workload == wl) & (d.cell == ("Inline-AMAC" if wl == "hit" else "InlineF-AMAC"))].sort_values("block").mops.values   # miss: Inline+F is the layout for negatives
            r, lo, hi = paired_ci(b, g); out += [f"{med(g):.1f}", f"{r:.2f}"]
        rows.append(" & ".join(out) + r" \\")
    rows.append(r"\addlinespace")
W("prod_prefetch", "\n".join(rows_x86)); W("prod_prefetch_arm", "\n".join(rows_arm))

# ---- 11. in-cache ablation on the M4 (compute cost of each cell with every line resident) -------------------------
rows = []
for L in (16, 20):
    f = f"../results/apple-m4/ablation2_L1_log2n{L}.csv"
    if not os.path.exists(f): continue
    d = pd.read_csv(f)
    for wl in ("hit", "miss"):
        cells = [f"$2^{{{L}}}$" if wl == "hit" else "", wl]
        for c in ("Chunk-sync", "Chunk-AMAC-staged-v", "Inline-sync", "Inline-AMAC", "Inline-coro"):
            g = d[(d.workload == wl) & (d.cell == c)].mops; cells.append(f"{med(g):.0f}" if len(g) else "--")
        rows.append(" & ".join(cells) + r" \\")
W("incache_m4", "\n".join(rows))

# ---- 12. ablation cells under hardware counters (abl_perf.cc, x86 nodes, pf3 hint) ---------------------------------
# SoA rows twice: as first run  and 64-byte aligned ;
# every other row from late-September (aligned).  Medians over the repetitions; LLC = generic last-level read-miss event
# (not exposed on Zen 3: "--").
R3 = "../results/2026-09_late-september"
ACELLS = [("r3", "SoA-sync", "SoA, synchronous, unaligned"), ("r3", "SoA-AMAC-staged-v", "SoA, staged engine, unaligned"),
          ("r3b", "SoA-sync", "SoA, synchronous"), ("r3b", "SoA-AMAC-staged-v", "SoA, staged engine"),
          ("r3b", "Chunk-sync", "Detached, synchronous"), ("r3b", "Chunk-AMAC-staged-v", "Detached, staged engine"),
          ("r3b", "Inline-sync", "Inline, synchronous"), ("r3b", "Inline-AMAC", "Inline, engine")]
rows = []
for tag, name in NODES[:3]:
    src = {"r3": f"{R3}/results_r3_{tag}/abl_perf_{{}}.csv", "r3b": f"{R3B}/results_r3b_{tag}/abl_perf_aligned_{{}}.csv"}
    if not all(os.path.exists(v.format(27)) for v in src.values()): continue
    D = {k: (pd.read_csv(v.format(27)), pd.read_csv(v.format(16))) for k, v in src.items()}
    for wl in ("hit", "miss"):
        for i, (run, cell, label) in enumerate(ACELLS):
            d, c = D[run]; g = d[(d.cell == cell) & (d.workload == wl)]; h = c[(c.cell == cell) & (c.workload == wl)]
            llc = g.llc_miss_per_lookup.median()
            rows.append(" & ".join([name if (wl == "hit" and i == 0) else "", wl if i == 0 else "", label,
                f"{h.mops.median():.0f}", f"{g.mops.median():.0f}", f"{g.instr_per_lookup.median():.0f}", f"{g.cycles_per_lookup.median():.0f}",
                "--" if llc < 0 else f"{llc:.2f}", f"{g.dtlb_miss_per_lookup.median():.2f}"]) + r" \\")
        rows.append(r"\addlinespace")
W("abl_perf", "\n".join(rows))

# ---- 12b. compact counter table for the main text (aligned build, positive lookups, the three x86 cores) --
rows = []
for tag, name in NODES[:3]:
    f27 = f"{R3B}/results_r3b_{tag}/abl_perf_aligned_27.csv"
    if not os.path.exists(f27): continue
    d = pd.read_csv(f27)
    for i, (run, cell, label) in enumerate(ACELLS[2:]):
        g = d[(d.cell == cell) & (d.workload == "hit")]; llc = g.llc_miss_per_lookup.median()
        rows.append(" & ".join([name if i == 0 else "", label, f"{g.mops.median():.0f}", f"{g.instr_per_lookup.median():.0f}",
            f"{g.cycles_per_lookup.median():.0f}", "--" if llc < 0 else f"{llc:.2f}", f"{g.dtlb_miss_per_lookup.median():.2f}"]) + r" \\")
    rows.append(r"\addlinespace")
W("abl_perf_main", "\n".join(rows))

# ---- 13. feature-store workload on the Grid'5000 x86 nodes , closed-loop capacity incl. Inline+F ----------
FE = ("inline-batch", "inlinef-batch", "inline-coalesce", "inlinef-coalesce", "inline-sync", "boost", "abseil")
rows = []
for cfg, label in (("recommender", "recommender"), ("iot-registry", "IoT registry"), ("batch-retrieval", "batch retrieval")):
    first = True
    for tag, name in NODES:
        f = f"{R3}/results_r3_{tag}/fs_{cfg}.csv"
        if not os.path.exists(f): continue
        d = pd.read_csv(f); cl = d.groupby("engine").ids_per_s_closed.median() / 1e6
        rows.append(" & ".join([label if first else "", name] + [f"{cl[e]:.0f}" for e in FE] + [f"{cl['inlinef-batch']/cl['boost']:.2f}"]) + r" \\"); first = False
    rows.append(r"\addlinespace")
W("featurestore_g5k", "\n".join(rows))

# ---- 14. compact production-prefetch summary for the main text (full tables go to the supplement) -------------------
def short(c):
    c = c.replace("abseil-prefetch", "Abseil, $k{=}").replace("f14-token", "F14 tokens, $k{=}")
    if "$k{=}" in c: c += "$"
    return {"abseil-sync": "Abseil", "boost-sync": "Boost", "ankerl-sync": "ankerl", "f14-sync": "F14"}.get(c, c)
rows = []
for f, name in PP:
    if not os.path.exists(f): continue
    d = pd.read_csv(f); m = d.groupby(["workload", "cell"]).mops.median(); cells = [name]
    prod = [c for c in m["hit"].index if not c.startswith("Inline")]
    bs = max([c for c in prod if c.endswith("sync")], key=lambda c: m[("hit", c)]); bp = max(prod, key=lambda c: m[("hit", c)])
    cells += [f"{short(bs)} {m[('hit', bs)]:.0f}", f"{short(bp)} {m[('hit', bp)]:.0f}", f"{m[('hit', 'Inline-AMAC')]:.0f}",
              f"{m[('hit', 'Inline-AMAC')] / m[('hit', bs)]:.2f}", f"{m[('hit', 'Inline-AMAC')] / m[('hit', bp)]:.2f}"]
    prodm = [c for c in m["miss"].index if not c.startswith("Inline")]; bpm = max(prodm, key=lambda c: m[("miss", c)])
    cells += [f"{short(bpm)} {m[('miss', bpm)]:.0f}", f"{m[('miss', 'InlineF-AMAC')]:.0f}", f"{m[('miss', 'InlineF-AMAC')] / m[('miss', bpm)]:.2f}"]
    rows.append(" & ".join(cells) + r" \\")
W("prod_summary", "\n".join(rows))

# ---- 15. server response-time percentiles, recommender profile (late-September feature-store runs) ------------------------
# p99 in microseconds, median over the five runs, at 0.25 and 1 million requests/s; "sat." when the median utilisation is
# >= 0.99 (the queue then grows over the finite 300 000-request horizon and the percentile only says "saturated").
# Zen 3 is left out: its service times show up to 198 samples above 100 us per run (Section 8).
rows = []
for tag, name in [("lille_icelake", "Ice Lake"), ("nancy_cascadelake", "Cascade Lake"), ("lyon_grace", "Neoverse V2"),
                  ("grenoble_ampere", "Neoverse N1"), ("lyon_thunderx2", "ThunderX2")]:
    d = pd.read_csv(f"{R3}/results_r3_{tag}/fs_recommender.csv"); d = d[d.tau_us.isin([0, 20])]
    cells = [name]
    for lam in (250000, 1000000):
        for eng in ("boost", "inlinef-batch", "inlinef-coalesce"):
            g = d[(d.engine == eng) & (d.lambda_req_s == lam)]
            cells.append("sat." if g.util.median() >= 0.99 else f"{g.p99_us.median():.1f}")
    rows.append(" & ".join(cells) + r" \\")
W("fs_pct_g5k", "\n".join(rows))

# ---- 16. new local runs (results_r4/) ----------------------------------------------------------------------
R4 = "../results/2026-10_october"
# 16a. NeighborHash against the engine on the M4 (nh_compare.cc, 9 blocks at 2^27 after a warm-up block, 5 at 2^16)
d27 = pd.read_csv(f"{R4}/m4/nh_compare_log2n27_v2.csv"); d16 = pd.read_csv(f"{R4}/m4/nh_compare_log2n16.csv")
NHC = [("Inline-sync", "Inline, synchronous", "4.29 GB, 0.67"), ("Inline-AMAC16", "Inline, engine $D{=}16$", ""), ("Inline-AMAC", "Inline, engine $D{=}32$", ""),
       ("Inline-AMAC64", "Inline, engine $D{=}64$", ""), ("InlineF-sync", "Inline+F, synchronous", "4.29 GB, 0.67"), ("InlineF-AMAC16", "Inline+F, engine $D{=}16$", ""),
       ("InlineF-AMAC", "Inline+F, engine $D{=}32$", ""), ("InlineF-AMAC64", "Inline+F, engine $D{=}64$", ""),
       ("NH-sync", "NeighborHash, scalar", "4.29 GB, 0.50"), ("NH-AMAC8", "NeighborHash, AMAC 8", ""), ("NH-AMAC16", "NeighborHash, AMAC 16", ""),
       ("NH-AMAC32", "NeighborHash, AMAC 32", ""), ("NH-AMAC64", "NeighborHash, AMAC 64", ""),
       ("NH79-sync", "NeighborHash at 0.79, scalar", "2.72 GB, 0.79"), ("NH79-AMAC32", "NeighborHash at 0.79, AMAC 32", ""), ("NH79-AMAC64", "NeighborHash at 0.79, AMAC 64", "")]
rows = []
for c, lab, fp in NHC:
    cells = [lab, fp]
    for d in (d16, d27):
        for wl in ("hit", "miss"):
            g = d[(d.cell == c) & (d.workload == wl)].mops; cells.append(f"{med(g):.0f}" if len(g) else "--")
    rows.append(" & ".join(cells) + r" \\")
    if c in ("InlineF-AMAC64", "NH-AMAC64"): rows.append(r"\addlinespace")
W("nh_m4", "\n".join(rows))
# 16b. capacities and loads at 2^27 (capacity_probe.cc, M4 build of the same library versions' policies)
c = pd.read_csv(f"{R4}/m4/capacity_probe_log2n27.csv")
W("capacity", "\n".join(f"{r.table} & {r.slots:,} & {r.load:.3f} & {r.bytes_table/1e9:.2f} \\\\".replace(",", "\\,") for r in c.itertuples()))
# 16c. cross-campaign provenance, x86 (provenance.py)
p = pd.read_csv(f"{R4}/provenance_x86.csv"); rows = []
for r in p[p.workload == "hit"].itertuples():
    rows.append(f"{r.core} & {r.campaign.replace(', prod_prefetch harness', ', prefetch harness').replace(' harness', '')} & {r.host.split('.')[0].replace('_', '\\_')} & {float(r.cpu_max_mhz)/1000:.1f} & {r.inline_engine:.1f} & {r.best_scalar_map} {r.best_scalar:.1f} & {r.speedup:.2f} \\\\")
W("provenance", "\n".join(rows))
# 16d. harness A/B on the M4 (harness_ab.cc)
h = pd.read_csv(f"{R4}/m4/harness_ab_log2n27.csv"); rows = []
for m in ("boost", "abseil", "ankerl", "Inline-AMAC"):
    cells = [{"boost": "Boost", "abseil": "Abseil", "ankerl": "ankerl", "Inline-AMAC": "Inline, engine"}[m]]
    for wl in ("hit", "miss"):
        for res in ("iso", "co"):
            for loop in ("sum", "store"):
                cells.append(f"{med(h[(h['map'] == m) & (h.workload == wl) & (h.residency == res) & (h.loop == loop)].mops):.0f}")
    rows.append(" & ".join(cells) + r" \\")
W("harness_ab", "\n".join(rows))
# 16e. depth rule against D=32 (model_check.py)
r = pd.read_csv(f"{R4}/model_depth_rule.csv")
W("depth_rule", "\n".join(f"{x.core} & {x.workload} & {x.Nm_prefetch:.1f} & {x.D_rule} & {x.D_best} & {x.T_best:.1f} & {x.rule_over_best:.3f} & {x.D32_over_best:.3f} \\\\" for x in r.itertuples()))
# 16f. termination regression (tombstone_test.cc) and churn cost (churn_cost.cc)
to = pd.read_csv(f"{R4}/m4/tombstone_test_orig.csv", comment=" ").dropna(); tf = pd.read_csv(f"{R4}/m4/tombstone_test_fix.csv")
to = to[to.test.isin(["trace_find_sync", "trace_find_batch", "churn"])]; tf = tf[tf.test.isin(["trace_find_sync", "trace_find_batch", "churn"])]
rows = []
for (a, b) in zip(to.itertuples(), tf.itertuples()):
    t = {"trace_find_sync": "trace, scalar lookup", "trace_find_batch": "trace, engine", "churn": "churn, 8192 keys"}[a.test]
    th = lambda v: f"{int(v):,}".replace(",", "\\,")
    rows.append(f"{t} & {a.table} & {th(a.ops)} & {a.status} & {th(b.ops)} & {b.status} & {int(b.rebuilds)} \\\\")
W("tombstone", "\n".join(rows))
cc = pd.concat([pd.read_csv(f"{R4}/m4/churn_cost_log2n24.csv"), pd.read_csv(f"{R4}/m4/churn_cost_log2n27.csv")]); rows = []
for x in cc.itertuples():
    share = x.rebuilds * x.rebuild_ms / 1e3 / (x.pairs * x.ns_per_pair / 1e9)
    rows.append(f"$2^{{{x.log2n}}}$ & {x.table} & {x.live_frac:.2f} & {x.pairs/2**x.log2n:.0f} & {x.ns_per_pair:.0f} & {x.rebuilds} & {x.rebuild_ms/1e3:.2f} & {100*share:.0f} \\\\")
W("churn_cost", "\n".join(rows))
# 16g. October Grid'5000 runs (results_r4/results_r4_<tag>/): NeighborHash on four server cores, harness A/B on three
G5K = [("lille_icelake", "Ice Lake"), ("nancy_cascadelake", "Cascade Lake"), ("lille_zen3", "Zen 3"), ("lyon_grace", "Grace")]
nh = {tag: pd.read_csv(f"{R4}/results_r4_{tag}/nh_compare.csv") for tag, _ in G5K}
NHG = [("Inline-sync", "Inline, synchronous"), ("Inline-AMAC16", "Inline, engine $D{=}16$"), ("Inline-AMAC", "Inline, engine $D{=}32$"), ("Inline-AMAC64", "Inline, engine $D{=}64$"),
       ("InlineF-sync", "Inline+F, synchronous"), ("InlineF-AMAC16", "Inline+F, engine $D{=}16$"), ("InlineF-AMAC", "Inline+F, engine $D{=}32$"), ("InlineF-AMAC64", "Inline+F, engine $D{=}64$"),
       ("NH-sync", "NeighborHash, scalar"), ("NH-vfind", "NeighborHash, AVX-512 find"), ("NH-SIMD", "NeighborHash, AVX-512 batch"),
       ("NH-AMAC8", "NeighborHash, AMAC 8"), ("NH-AMAC16", "NeighborHash, AMAC 16"), ("NH-AMAC32", "NeighborHash, AMAC 32"), ("NH-AMAC64", "NeighborHash, AMAC 64"),
       ("NH-SIMDAMAC8", "NeighborHash, SIMD-AMAC 8"), ("NH-SIMDAMAC16", "NeighborHash, SIMD-AMAC 16"), ("NH-SIMDAMAC32", "NeighborHash, SIMD-AMAC 32"), ("NH-SIMDAMAC64", "NeighborHash, SIMD-AMAC 64"),
       ("NH79-sync", "NeighborHash at 0.79, scalar"), ("NH79-AMAC16", "NeighborHash at 0.79, AMAC 16"), ("NH79-AMAC32", "NeighborHash at 0.79, AMAC 32"), ("NH79-AMAC64", "NeighborHash at 0.79, AMAC 64"),
       ("NH79-SIMDAMAC8", "NeighborHash at 0.79, SIMD-AMAC 8"), ("NH79-SIMDAMAC16", "NeighborHash at 0.79, SIMD-AMAC 16")]
rows = []
for c, lab in NHG:
    cells = [lab]
    for tag, _ in G5K:
        for wl in ("hit", "miss"):
            g = nh[tag][(nh[tag].cell == c) & (nh[tag].workload == wl)].mops; cells.append(f"{med(g):.0f}" if len(g) else "--")
    rows.append(" & ".join(cells) + r" \\")
    if c in ("InlineF-AMAC64", "NH-SIMDAMAC64"): rows.append(r"\addlinespace")
W("nh_g5k", "\n".join(rows))
rows = []
for tag, core in G5K[:3]:
    h = pd.read_csv(f"{R4}/results_r4_{tag}/harness_ab.csv")
    for m in ("boost", "abseil", "ankerl", "Inline-AMAC"):
        cells = [core if m == "boost" else "", {"boost": "Boost", "abseil": "Abseil", "ankerl": "ankerl", "Inline-AMAC": "Inline, engine"}[m]]
        for wl in ("hit", "miss"):
            for res in ("iso", "co"):
                for loop in ("sum", "store"):
                    cells.append(f"{med(h[(h['map'] == m) & (h.workload == wl) & (h.residency == res) & (h.loop == loop)].mops):.0f}")
        rows.append(" & ".join(cells) + r" \\")
    if tag != G5K[2][0]: rows.append(r"\addlinespace")
W("harness_ab_g5k", "\n".join(rows))
