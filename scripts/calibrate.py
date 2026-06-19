#!/usr/bin/env python3
"""Fit the forehand reference axis and spin classes from LABELED recordings,
and write scripts/calib.json (loaded automatically by session_report.py).

Two ways to provide labels:

A) One recording, hit in blocks separated by pauses:
   python calibrate.py <csv> --hand right \\
       --blocks "fh:flat:10,bh:flat:10,fh:topspin:10,fh:slice:10,bh:slice:10"

B) One file per stroke type (recommended):
   python calibrate.py --hand right \\
       --fh ses_012_full.csv,ses_013_full.csv --bh ses_014_full.csv \\
       --topspin ses_012_full.csv --flat ses_013_full.csv \\
       [--slice ses_0XX.csv] [--serve ses_0YY.csv]

Forehand/backhand needs --fh and --bh. Spin needs >=2 of topspin/flat/slice.
Serve needs --serve (a block of consecutive serves) plus --fh/--bh for the
serve-vs-groundstroke contrast. Paths resolve to the current dir then ../Data.
"""
import sys, os, json
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from session_report import load_csv, extract_strokes, spin_feature, serve_feature

HERE = os.path.dirname(os.path.abspath(__file__))
CALIB = os.path.join(HERE, "calib.json")
DATA = os.path.join(os.path.dirname(HERE), "Data")

def resolve(p):
    for c in (p, os.path.join(DATA, p)):
        if os.path.isfile(c): return c
    raise FileNotFoundError(p)

_cache = {}
def strokes_of(path):
    path = resolve(path)
    if path not in _cache:
        t, g, a, om, hit = load_csv(path)
        _cache[path] = extract_strokes(t, g, a, om, hit)
    return _cache[path]

# ── label collection ───────────────────────────────────────────────────────
def collect_blocks(path, blocks_str):
    def parse(s):
        out = []
        for b in s.split(','):
            p = b.split(':'); typ = p[0].strip().lower()
            spin = next((x for x in p[1:] if x.strip().lower() in ('flat', 'topspin', 'slice')), 'flat')
            out.append(('Forehand' if typ in ('fh', 'forehand') else 'Backhand', spin))
        return out
    blocks = parse(blocks_str)
    S = strokes_of(path)
    print(f"{os.path.basename(resolve(path))}: {len(S)} strokes, {len(blocks)} blocks")
    gaps = sorted(range(len(S) - 1), key=lambda i: S[i + 1]['ms'] - S[i]['ms'], reverse=True)
    cuts = sorted(gaps[:len(blocks) - 1]); segs = []; start = 0
    for c in cuts: segs.append(S[start:c + 1]); start = c + 1
    segs.append(S[start:])
    fh = bh = []; spin = {}
    fh = [s for seg, (ty, _) in zip(segs, blocks) if ty == 'Forehand' for s in seg]
    bh = [s for seg, (ty, _) in zip(segs, blocks) if ty == 'Backhand' for s in seg]
    for seg, (_, sp) in zip(segs, blocks):
        spin.setdefault(sp, []).extend(seg)
    for seg, (ty, sp) in zip(segs, blocks): print(f"  block {ty}/{sp}: {len(seg)}")
    return fh, bh, spin

def collect_files(opts):
    def grp(key):
        out = []
        for f in (opts.get(key) or '').split(','):
            if f.strip(): out += strokes_of(f.strip())
        return out
    fh, bh = grp('fh'), grp('bh')
    spin = {k: grp(k) for k in ('topspin', 'flat', 'slice') if opts.get(k)}
    serve = grp('serve') if opts.get('serve') else []
    print(f"forehand strokes: {len(fh)}   backhand strokes: {len(bh)}")
    for k, v in spin.items(): print(f"  {k}: {len(v)} strokes")
    if serve: print(f"  serve: {len(serve)} strokes")
    return fh, bh, spin, serve

# ── fit ─────────────────────────────────────────────────────────────────────
def unit(v): return v / (np.linalg.norm(v) + 1e-9)

def main():
    args = sys.argv[1:]; opts = {}; path = None; i = 0
    keys = ('hand', 'blocks', 'fh', 'bh', 'topspin', 'flat', 'slice', 'serve')
    while i < len(args):
        a = args[i]
        if a.startswith('--') and '=' in a:
            k, v = a[2:].split('=', 1); opts[k] = v
        elif a.startswith('--') and a[2:] in keys and i + 1 < len(args):
            opts[a[2:]] = args[i + 1]; i += 1
        elif not a.startswith('--'): path = a
        i += 1
    hand = opts.get('hand')

    if opts.get('blocks') and path:
        fh, bh, spin = collect_blocks(path, opts['blocks']); serve = []
    elif opts.get('fh') and opts.get('bh'):
        fh, bh, spin, serve = collect_files(opts)
    else:
        print(__doc__); return

    calib = {'hand': hand}

    # forehand/backhand reference axis
    if fh and bh:
        fh_ref = unit(np.mean([s['axis'] for s in fh], 0))
        bh_ref = unit(np.mean([s['axis'] for s in bh], 0))
        cos = float(np.dot(fh_ref, bh_ref))
        ok = sum(1 for s in fh if np.dot(s['axis'], fh_ref) > np.dot(s['axis'], bh_ref)) \
           + sum(1 for s in bh if np.dot(s['axis'], bh_ref) > np.dot(s['axis'], fh_ref))
        tot = len(fh) + len(bh)
        calib['fh_ref_axis'] = fh_ref.tolist()
        print(f"\nFH/BH: fh_ref={fh_ref.round(2).tolist()}  centroid cos={cos:+.2f}  "
              f"label agreement {ok}/{tot} ({100*ok/tot:.0f}%)")

    # spin: standardized spin-feature space, nearest class-mean classify
    present = [k for k in ('topspin', 'flat', 'slice') if spin.get(k)]
    if len(present) >= 2:
        labeled = [(spin_feature(s), c) for c in present for s in spin[c]]
        X = np.array([f for f, _ in labeled]); y = [c for _, c in labeled]
        mu = X.mean(0); sd = X.std(0) + 1e-9; Z = (X - mu) / sd
        idx = {c: [i for i, yy in enumerate(y) if yy == c] for c in present}
        # per-dim Fisher weight: between-class spread / within-class spread,
        # so a discriminating dim (directness) dominates the weak rotation dims
        cmeans = np.array([Z[idx[c]].mean(0) for c in present])
        within = np.mean([Z[idx[c]].std(0) for c in present], 0) + 1e-9
        w = (cmeans.std(0) / within) ** 2   # squared Fisher ratio: let the strong dim dominate
        w[w < 0.4 * w.max()] = 0.0           # drop weakly-discriminating (noise) dims
        cv = {c: Z[idx[c]].mean(0) for c in present}
        def dist(z, v): return np.linalg.norm(w * (z - v))
        correct = 0
        for i in range(len(Z)):
            cvi = {c: Z[[j for j in idx[c] if j != i]].mean(0) for c in present}
            pred = min(cvi, key=lambda c: dist(Z[i], cvi[c]))
            correct += (pred == y[i])
        calib['spin_feat_mean'] = mu.tolist(); calib['spin_feat_std'] = sd.tolist()
        calib['spin_feat_weight'] = w.tolist()
        calib['spin_class_vec'] = {c: v.tolist() for c, v in cv.items()}
        print(f"SPIN ({'/'.join(present)}): leave-one-out accuracy {100*correct/len(Z):.0f}% "
              f"on {len(Z)} labeled strokes  (dim weights {w.round(2).tolist()})")
        if 'slice' not in present:
            print("  (no slice block yet — add a slice session to classify slices)")
    else:
        print("SPIN: need >=2 spin classes to fit (skipped)")

    # serve vs groundstroke signature (serve = fast, overhead pronation axis)
    if serve and (fh or bh):
        ground = fh + bh
        labeled = [(serve_feature(s), 1) for s in serve] + [(serve_feature(s), 0) for s in ground]
        X = np.array([f for f, _ in labeled]); ya = np.array([l for _, l in labeled])
        mu = X.mean(0); sd = X.std(0) + 1e-9; Z = (X - mu) / sd
        sv = Z[ya == 1].mean(0); gr = Z[ya == 0].mean(0)
        ok = 0
        for i in range(len(Z)):
            m = np.ones(len(Z), bool); m[i] = False
            a = Z[m & (ya == 1)].mean(0); b = Z[m & (ya == 0)].mean(0)
            ok += (1 if np.linalg.norm(Z[i]-a) < np.linalg.norm(Z[i]-b) else 0) == ya[i]
        calib['serve_feat_mean'] = mu.tolist(); calib['serve_feat_std'] = sd.tolist()
        calib['serve_mean'] = sv.tolist(); calib['ground_mean'] = gr.tolist()
        print(f"SERVE: leave-one-out serve-vs-groundstroke accuracy {100*ok/len(Z):.0f}% "
              f"({len(serve)} serves vs {len(ground)} groundstrokes)")
    elif serve:
        print("SERVE: need --fh/--bh groundstrokes too for the serve-vs-ground contrast")

    json.dump(calib, open(CALIB, 'w'), indent=2)
    print(f"\nWrote {CALIB}")

if __name__ == "__main__":
    main()
