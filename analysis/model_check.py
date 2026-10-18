#!/usr/bin/env python3
"""test the model against every existing depth sweep, without fitting anything.
Inputs: Table tab:mlppf (Lm and the two N_M estimators, September campaign + held-out Sapphire Rapids), the September
depth sweeps (x86: prefetch_sweep.csv, Inline, L1-keep hint, D in {16,32,64,128} and the synchronous loop as D=0;
ARM and Sapphire Rapids: depth_ext.csv, Inline and Paired, D 4..384, L2-keep on ARM).
Outputs (results_r4/): model_concurrency.csv  effective lookups in flight T*Lm per depth (the quantity the software
term D/(C Lm) claims to bound by D); model_paired.csv  the line cost a Paired lookup can have at most given its
plateau and the gather ceiling; model_depth_rule.csv  the depth rule (smallest power of two >= 2 N_M), unclamped,
against fixed D=32 and against the best depth of the sweep."""
import csv, os, statistics as st, collections, math
R = os.path.dirname(os.path.abspath(__file__)) + '/..'
# core: (tag, Lm ns, N_M chase, N_M prefetch)   -- Table tab:mlppf
CORES = {'Zen 3': ('lille_zen3', 102, 21.9, 20.2), 'Ice Lake': ('lille_icelake', 90, 16.5, 15.7),
         'Cascade Lake': ('nancy_cascadelake', 86, 9.4, 8.8), 'Sapphire Rapids': ('grenoble_spr', 123, 13.2, 12.1),
         'ThunderX2': ('lyon_thunderx2', 117, 14.1, 17.1), 'Neoverse N1': ('grenoble_ampere', 100, 9.6, 9.5),
         'Neoverse V2': ('lyon_grace', 216, 20.0, 40.1)}
DEPTH2 = {'ThunderX2': 'lyon_pyxis_thunderx2', 'Neoverse N1': 'grenoble_sasquatch_ampere', 'Neoverse V2': 'lyon_hydra_grace'}
def sweeps(core):
    """{(layout, workload): {D: median Mops}}; D=0 is the synchronous loop."""
    tag = CORES[core][0]; out = collections.defaultdict(lambda: collections.defaultdict(list))
    ps = f'{R}/results/2026-09_september/results_r2_{tag}/prefetch_sweep.csv'
    if os.path.exists(ps):
        hint = '3' if core in ('Zen 3', 'Ice Lake', 'Cascade Lake') else '2'
        for r in csv.DictReader(open(ps)):
            d = int(r['depth'])
            if d == 0 or r['locality'] == hint: out[('Inline', r['workload'])][d].append(float(r['mops']))
    de = f'{R}/results/2026-09_september/results_depth2_{DEPTH2[core]}/depth_ext.csv' if core in DEPTH2 else (
         f'{R}/results/2026-09_held-out-sapphire-rapids/results_ho_grenoble_spr/depth_ext.csv' if core == 'Sapphire Rapids' else None)
    if de:
        if core in DEPTH2:   # the dense ARM sweep replaces the 4-point one for positive lookups (same campaign, same hint)
            out[('Inline', 'hit')] = collections.defaultdict(list, {0: out[('Inline', 'hit')][0]})
        for r in csv.DictReader(open(de)): out[(r['layout'], 'hit')][int(r['depth'])].append(float(r['mops']))
    return {k: {d: st.median(v) for d, v in sorted(dd.items()) if v} for k, dd in out.items()}
conc, paired, rule = [], [], []
for core, (tag, Lm, nch, npf) in CORES.items():
    sw = sweeps(core)
    for (lay, wl), s in sorted(sw.items()):
        for d, t in s.items():
            conc.append([core, lay, wl, d, f'{t:.1f}', f'{t*Lm/1e3:.2f}', d, f'{npf:.1f}'])
    if ('Paired', 'hit') in sw:
        s = sw[('Paired', 'hit')]; dmax = max(s, key=s.get); t = s[dmax]
        paired.append([core, dmax, f'{t:.1f}', f'{npf/(2*Lm)*1e3:.1f}', f'{npf/(t*Lm/1e3):.2f}'])
    for wl in ('hit', 'miss'):
        s = {d: t for d, t in sw.get(('Inline', wl), {}).items() if d > 0}
        if not s: continue
        drule = 2 ** math.ceil(math.log2(2 * npf)); dopt = max(s, key=s.get)
        near = lambda D: s[D] if D in s else s[min(s, key=lambda x: abs(math.log2(x) - math.log2(D)))]
        rule.append([core, wl, f'{npf:.1f}', drule, dopt, f'{s[dopt]:.1f}', f'{near(drule):.1f}', f'{near(32):.1f}',
                     f'{near(drule)/s[dopt]:.3f}', f'{near(32)/s[dopt]:.3f}', ' '.join(str(d) for d in s)])
def W(name, hdr, rows):
    with open(f'{R}/results/2026-10_october/{name}', 'w') as f: w = csv.writer(f); w.writerow(hdr); w.writerows(rows)
    print(f'== {name}'); print(','.join(hdr)); [print(','.join(map(str, r))) for r in rows]
W('model_concurrency.csv', ['core', 'layout', 'workload', 'D', 'mops', 'lookups_in_flight_T_Lm', 'D_ref', 'Nm_prefetch'], conc)
W('model_paired.csv', ['core', 'D_best', 'plateau_mops', 'ceiling_C2_mops', 'max_lines_per_lookup'], paired)
W('model_depth_rule.csv', ['core', 'workload', 'Nm_prefetch', 'D_rule', 'D_best', 'T_best', 'T_rule', 'T_32', 'rule_over_best', 'D32_over_best', 'depths_swept'], rule)
