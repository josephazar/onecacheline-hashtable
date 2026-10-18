#!/usr/bin/env python3
"""Tables and figures for the feature-store workload, version 2: five repetitions per profile (fresh trace and
arrivals each), reported as the median across repetitions with the min-max range; service-time diagnostics."""
import glob, os, numpy as np, pandas as pd
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
files = [f for f in sorted(glob.glob("results/*.csv")) if "/v1/" not in f]
df = pd.concat([pd.read_csv(f) for f in files], ignore_index=True)
df["engine_tau"] = np.where(df.tau_us > 0, df.engine + " τ=" + df.tau_us.astype(int).astype(str) + "µs", df.engine)
STYLE = {"inline-batch": ("#0072B2", "o", "-"), "inline-coalesce τ=20µs": ("#009E73", "s", "-"), "inline-coalesce τ=100µs": ("#009E73", "^", "--"),
         "inline-sync": ("#56B4E9", "v", ":"), "boost": ("#D55E00", "D", "-"), "abseil": ("#CC79A7", "x", "-.")}
LABEL = {"inline-batch": "Inline, per-request batch", "inline-coalesce τ=20µs": "Inline, coalesced, τ=20 µs", "inline-coalesce τ=100µs": "Inline, coalesced, τ=100 µs",
         "inline-sync": "Inline, scalar", "boost": "Boost, scalar", "abseil": "Abseil, scalar"}
out = ["# Feature-store multi-get workload, version 2 (Apple M4 Max, 2^26 entities, 300 000 requests per repetition, 5 repetitions)\n",
       "Response time = queueing + batch accumulation + measured service, single FIFO server, Poisson arrivals. Each cell is the median",
       "across the five repetitions, with the min-max range in brackets. util > 0.95 means the engine is saturated at that load and the",
       "percentiles depend on the trace length. Closed-loop capacity is measured with the engine's own batching policy (the coalescing",
       "engine on 256-identifier batches of consecutive requests); the per-request capacity is given beside it.\n"]
agg = {}
for cfg, g in df.groupby("config", sort=False):
    out.append(f"\n## {cfg}  (timer overhead {g.timer_ns.iloc[0]:.0f} ns, mean k = {g.mean_k.median():.1f})\n")
    cl = g.groupby("engine_tau").agg(closed=("ids_per_s_closed", "median"), closed_req=("ids_per_s_closed_req", "median"))
    out.append("closed-loop capacity (M ids/s, policy-specific / per-request): " + ", ".join(f"{e} {r.closed/1e6:.0f}/{r.closed_req/1e6:.0f}" for e, r in cl.iterrows()) + "\n")
    out.append("| engine | λ (req/s) | offered ids/s | util | p50 µs | p99 µs [min, max] | p99.9 µs | svc p99 µs | svc max µs | samples > 100 µs |"); out.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    a = g.groupby(["engine_tau", "lambda_req_s"]).agg(off=("offered_ids_s", "median"), util=("util", "median"), p50=("p50_us", "median"), p99=("p99_us", "median"), p99lo=("p99_us", "min"), p99hi=("p99_us", "max"),
                                                       p999=("p999_us", "median"), sp99=("svc_p99_us", "median"), smax=("svc_max_us", "max"), over=("svc_over100us", "sum"), n=("rep", "count")).reset_index()
    agg[cfg] = a
    for _, r in a.iterrows():
        sat = " (sat.)" if r.util > 0.95 else ""
        out.append(f"| {r.engine_tau} | {r.lambda_req_s/1e6:.2f}M | {r.off/1e6:.0f}M | {r.util:.2f}{sat} | {r.p50:.2f} | {r.p99:.1f} [{r.p99lo:.1f}, {r.p99hi:.1f}] | {r.p999:.1f} | {r.sp99:.2f} | {r.smax:.0f} | {int(r.over)} |")
    fig, ax = plt.subplots(1, 2, figsize=(9.5, 3.6))
    for e, ge in a.groupby("engine_tau"):
        ge = ge.sort_values("lambda_req_s"); c, m, ls = STYLE.get(e, ("k", "o", "-"))
        ax[0].errorbar(ge.off / 1e6, ge.p99, yerr=[ge.p99 - ge.p99lo, ge.p99hi - ge.p99], color=c, marker=m, ls=ls, ms=5, lw=1.4, capsize=2, label=LABEL.get(e, e))
        ax[1].plot(ge.off / 1e6, ge.p50, color=c, marker=m, ls=ls, ms=5, lw=1.4, label=LABEL.get(e, e))
    for a_, t in zip(ax, ("p99 response time (µs)", "p50 response time (µs)")):
        a_.set_xlabel("offered load (M identifiers/s)"); a_.set_ylabel(t); a_.set_yscale("log"); a_.grid(alpha=.3, which="both", ls=":"); a_.tick_params(labelsize=8)
    h, l = ax[0].get_legend_handles_labels(); fig.legend(h, l, loc="lower center", ncol=3, fontsize=8, frameon=False, bbox_to_anchor=(0.5, -0.02))
    fig.tight_layout(rect=(0, 0.13, 1, 1)); fig.savefig(f"results/{cfg}.png", dpi=170); fig.savefig(f"results/{cfg}.pdf"); plt.close(fig)
open("results/REPORT.md", "w").write("\n".join(out)); print("\n".join(out))
