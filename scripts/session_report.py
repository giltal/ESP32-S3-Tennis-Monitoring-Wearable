#!/usr/bin/env python3
"""Tennis play-session report generator.

Reads a recorded session (a PlaySession_* folder with hits.csv [+ outcomes.txt],
or a single ses_NNN_full.csv / hits.csv) and writes a self-contained HTML
report: stroke count, rally breakdown, forehand/backhand split, swing-speed
estimate, and tagged outcomes.

Usage:
    python session_report.py <session_folder_or_csv> [out.html]

Exploratory: forehand/backhand is unsupervised (cluster->label is a heuristic
until a labeled calibration session exists); speed is a rotational proxy.
"""
import csv, sys, os, math, statistics, html, json
import numpy as np

# ── tunables ──────────────────────────────────────────────────────────────
MERGE_MS  = 300     # detections closer than this = one stroke (multi-contact)
# Inter-stroke gaps are bimodal: within-rally cycle ~2-4s, between-rally idle
# 8-25s, with a clear valley at 4-8s. 6s sits in the valley → splits rallies.
RALLY_GAP = 6000    # gap > this (ms) = "long idle" → new rally
WIN_MS    = 250     # +/- window around a stroke for feature extraction
RACKET_R  = 0.7     # m, wrist->sweet-spot radius for the speed proxy

# ── data ──────────────────────────────────────────────────────────────────
def load_csv(path):
    t=[]; g=[]; a=[]; om=[]; hit=[]
    for r in csv.reader(open(path)):
        if not r or r[0]=='session' or len(r)<12: continue
        try:
            t.append(int(r[1]))
            g.append((float(r[2])/10, float(r[3])/10, float(r[4])/10))
            a.append((float(r[5])/100, float(r[6])/100, float(r[7])/100))
            om.append(float(r[8])); hit.append(r[11]=='1')
        except ValueError: continue
    return np.array(t), np.array(g,dtype=float), np.array(a,dtype=float), np.array(om), np.array(hit)

def read_outcomes(folder):
    p=os.path.join(folder,"outcomes.txt")
    if not os.path.isfile(p): return None
    out={}
    for line in open(p):
        if '=' in line:
            k,v=line.rsplit('=',1); k=k.strip(); v=v.strip()
            try: out[k]=int(v)
            except ValueError: out[k]=v   # e.g. hand=left
    return out

def read_events(folder):
    """events.csv (ms,outcome): timestamped outcome tags (firmware v0.6+)."""
    p=os.path.join(folder,"events.csv")
    if not os.path.isfile(p): return []
    ev=[]
    for r in csv.reader(open(p)):
        if not r or r[0]=='ms' or len(r)<2: continue
        try: ev.append((int(r[0]), r[1].strip()))
        except ValueError: pass
    return ev

def load_calib():
    p=os.path.join(os.path.dirname(os.path.abspath(__file__)),"calib.json")
    return json.load(open(p)) if os.path.isfile(p) else None

# Points the wearer won vs lost (provisional mapping — adjust to your rules)
OUTCOME_SIGN={'Good hit':+1,'First serve in':0,'Out':-1,'Bad hit':-1,
              'Unforced error':-1,'Lost point':-1}

# ── stroke extraction ─────────────────────────────────────────────────────
def extract_strokes(t, g, a, om, hit):
    idx=np.where(hit)[0]
    if len(idx)==0: return []
    groups=[[idx[0]]]
    for i in idx[1:]:
        if t[i]-t[groups[-1][-1]]<=MERGE_MS: groups[-1].append(i)
        else: groups.append([i])
    strokes=[]
    for grp in groups:
        lo=max(0,grp[0]-150); hi=min(len(om),grp[-1]+150)
        pk=lo+int(np.argmax(om[lo:hi]))
        lo2=np.searchsorted(t,t[pk]-WIN_MS); hi2=np.searchsorted(t,t[pk]+WIN_MS)
        accmag=np.linalg.norm(a[lo2:hi2],axis=1)
        gv=g[pk]; n=np.linalg.norm(gv)
        peak_om=float(om[pk])
        # net rotation through the swing (gyro integral, deg) — encodes the
        # low->high (topspin) vs high->low (slice) brush direction.
        dts=np.diff(t[lo2:hi2])/1000.0 if hi2-lo2>1 else np.array([])
        rot=(g[lo2:hi2-1]*dts[:,None]).sum(0) if len(dts) else np.zeros(3)
        grav=a[lo2:hi2].mean(0); gn=np.linalg.norm(grav)
        strokes.append(dict(
            ms=int(t[pk]), peak_om=peak_om,
            peak_acc=float(accmag.max()) if len(accmag) else 0.0,
            axis=(gv/n if n>0 else gv),
            rot=rot, grav=(grav/gn if gn>0 else grav),
            kmh=math.radians(peak_om)*RACKET_R*3.6,
            dur_ms=int(np.sum(om[lo2:hi2]>0.4*peak_om)*2),
        ))
    return strokes

def detect_rallies(strokes):
    if not strokes: return []
    rallies=[[strokes[0]]]
    for s in strokes[1:]:
        if s['ms']-rallies[-1][-1]['ms']>RALLY_GAP: rallies.append([s])
        else: rallies[-1].append(s)
    return rallies

def kmeans2(X, iters=60):
    X=np.array(X)
    if len(X)<2: return np.zeros(len(X),int), X
    c=np.array([X[0],X[len(X)//2]],dtype=float)
    lab=np.zeros(len(X),int)
    for _ in range(iters):
        d=np.linalg.norm(X[:,None,:]-c[None,:,:],axis=2); lab=d.argmin(1)
        for k in range(2):
            if (lab==k).any(): c[k]=X[lab==k].mean(0)
    return lab, c

# Reference rotation-axis sign for a FOREHAND on a RIGHT hand. Placeholder until
# a labeled calibration session pins it; handedness then flips it for lefties.
FH_REF_AXIS_RIGHT = np.array([-1.0, 1.0, 0.0])

def classify_fh_bh(strokes, hand=None, calib=None):
    """Cluster strokes by contact rotation axis, then name the clusters using
    (in priority): a calibrated forehand reference axis (flipped if the session
    hand differs from the calibrated hand) → handedness + placeholder axis →
    larger cluster = forehand."""
    axes=np.array([s['axis'] for s in strokes])
    lab,cent=kmeans2(axes)
    ref=None
    if calib and 'fh_ref_axis' in calib:
        ref=np.array(calib['fh_ref_axis'],dtype=float)
        if hand and calib.get('hand') and hand!=calib['hand']: ref=-ref
    elif hand in ('right','left'):
        ref=FH_REF_AXIS_RIGHT * (1 if hand=='right' else -1)
    if ref is not None:
        fh_cluster=int(np.argmax([np.dot(cent[k], ref) for k in range(2)]))
    else:
        fh_cluster=0 if (lab==0).sum()>=(lab==1).sum() else 1
    names={fh_cluster:'Forehand', 1-fh_cluster:'Backhand'}
    for i,s in enumerate(strokes): s['type']=names[int(lab[i])]
    sep=float(np.dot(cent[0],cent[1])/(np.linalg.norm(cent[0])*np.linalg.norm(cent[1])+1e-9))
    return sep

def classify_spin(strokes, calib):
    """Per-stroke topspin/flat/slice from the calibrated spin axis+thresholds."""
    if not calib or 'spin_axis' not in calib:
        for s in strokes: s['spin']=None
        return False
    ax=np.array(calib['spin_axis'],dtype=float); hi=calib['spin_thr_hi']; lo=calib['spin_thr_lo']
    for s in strokes:
        v=float(np.dot(s['rot'],ax))
        s['spin']='topspin' if v>hi else ('slice' if v<lo else 'flat')
    return True

# ── HTML ───────────────────────────────────────────────────────────────────
def mmss(ms): return f"{ms//60000:d}:{(ms//1000)%60:02d}"

def bar_chart(pairs, w=320, h=150, color="#378ADD"):
    if not pairs: return ""
    mx=max(v for _,v in pairs) or 1; n=len(pairs); bw=w/n*0.6; gap=w/n
    bars=""
    for i,(lab,v) in enumerate(pairs):
        bh=(h-30)*v/mx; x=i*gap+gap*0.2; y=h-20-bh
        bars+=f'<rect x="{x:.0f}" y="{y:.0f}" width="{bw:.0f}" height="{bh:.0f}" rx="3" fill="{color}"/>'
        bars+=f'<text x="{x+bw/2:.0f}" y="{h-6:.0f}" font-size="11" fill="#666" text-anchor="middle">{html.escape(str(lab))}</text>'
        bars+=f'<text x="{x+bw/2:.0f}" y="{y-4:.0f}" font-size="10" fill="#222" text-anchor="middle">{v}</text>'
    return f'<svg viewBox="0 0 {w} {h}" width="100%">{bars}</svg>'

def render(meta, strokes, rallies, sep, outcomes, out_path,
           has_spin=False, events=None, rally_tag=None):
    events = events or []; rally_tag = rally_tag or [None]*len(rallies)
    rl=[len(r) for r in rallies]
    kmh=[s['kmh'] for s in strokes]; oms=[s['peak_om'] for s in strokes]
    fh=sum(1 for s in strokes if s['type']=='Forehand'); bh=len(strokes)-fh
    fh_pct=100*fh/len(strokes) if strokes else 0
    # rally length histogram
    rh={}
    for n in rl: rh[n]=rh.get(n,0)+1
    rhist=sorted(rh.items())
    # speed histogram (bins of 15 km/h)
    sh={}
    for v in kmh:
        b=int(v//15)*15; sh[b]=sh.get(b,0)+1
    shist=[(f"{b}-{b+15}", sh[b]) for b in sorted(sh)]

    def card(label,val,sub=""):
        return (f'<div class="card"><div class="lbl">{label}</div>'
                f'<div class="val">{val}<span class="unit">{sub}</span></div></div>')

    rally_rows=""
    for i,r in enumerate(rallies,1):
        fhc=sum(1 for s in r if s['type']=='Forehand')
        dur=(r[-1]['ms']-r[0]['ms'])/1000
        pk=max(s['kmh'] for s in r)
        t0=mmss(r[0]['ms']-meta['t0'])
        tag=rally_tag[i-1] if i-1<len(rally_tag) else None
        rally_rows+=(f"<tr><td>{i}</td><td>{t0}</td><td>{len(r)}</td>"
                     f"<td>{dur:.1f}s</td><td>{fhc} FH / {len(r)-fhc} BH</td><td>{pk:.0f} km/h</td>"
                     f"<td>{html.escape(tag) if tag else '—'}</td></tr>")

    # spin breakdown (only when calibrated)
    spin_html=""
    if has_spin:
        sc={k:sum(1 for s in strokes if s['spin']==k) for k in ('topspin','flat','slice')}
        tot=sum(sc.values()) or 1
        spin_html=("<div class='sec'><h2>Spin</h2><div class='split'>"
            f"<div style='width:{100*sc['topspin']/tot:.0f}%;background:#1D9E75'>Topspin · {sc['topspin']}</div>"
            f"<div style='width:{100*sc['flat']/tot:.0f}%;background:#888'>Flat · {sc['flat']}</div>"
            f"<div style='width:{100*sc['slice']/tot:.0f}%;background:#BA7517'>Slice · {sc['slice']}</div></div>"
            "<div class='note'>Calibrated from a labeled session.</div></div>")

    # score summary from tagged events
    score_html=""
    if events:
        won=sum(1 for _,oc in events if OUTCOME_SIGN.get(oc,0)>0)
        lost=sum(1 for _,oc in events if OUTCOME_SIGN.get(oc,0)<0)
        score_html=("<div class='sec'><h2>Points (from tagged events)</h2>"
            f"<div style='font-size:22px;font-weight:600'>{won} won · {lost} lost"
            f"<span style='font-size:13px;color:#999;font-weight:400'> of {len(events)} tagged</span></div>"
            "<div class='note'>Provisional mapping (Good hit/First serve = won; Out/Bad/Unforced/Lost = lost) — adjust to your scoring rules.</div></div>")

    out_rows=""
    if outcomes:
        total=sum(outcomes.values())
        for k in ["Good hit","Out","Bad hit","Unforced error","First serve in","Lost point","Total hits"]:
            if k in outcomes:
                out_rows+=f"<tr><td>{html.escape(k)}</td><td style='text-align:right'>{outcomes[k]}</td></tr>"

    H=f"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{html.escape(meta['name'])} — session report</title>
<style>
 body{{font-family:-apple-system,Segoe UI,Roboto,sans-serif;color:#1a1a1a;background:#f6f6f4;margin:0;padding:24px;}}
 .wrap{{max-width:860px;margin:0 auto;}}
 h1{{font-size:20px;font-weight:600;margin:0 0 2px;}} .sub{{color:#777;font-size:13px;margin-bottom:20px;}}
 .grid{{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:12px;margin-bottom:24px;}}
 .card{{background:#fff;border:1px solid #e7e7e2;border-radius:10px;padding:14px 16px;}}
 .lbl{{font-size:12px;color:#888;}} .val{{font-size:26px;font-weight:600;margin-top:2px;}} .unit{{font-size:13px;color:#999;font-weight:400;}}
 .sec{{background:#fff;border:1px solid #e7e7e2;border-radius:10px;padding:16px 18px;margin-bottom:18px;}}
 .sec h2{{font-size:15px;font-weight:600;margin:0 0 12px;}}
 .note{{font-size:12px;color:#999;margin-top:8px;}}
 .split{{display:flex;height:34px;border-radius:8px;overflow:hidden;font-size:13px;color:#fff;}}
 table{{width:100%;border-collapse:collapse;font-size:13px;}}
 td,th{{padding:6px 4px;border-bottom:1px solid #f0f0ec;text-align:left;}} th{{color:#888;font-weight:500;}}
 .two{{display:grid;grid-template-columns:1fr 1fr;gap:18px;}} @media(max-width:640px){{.two{{grid-template-columns:1fr;}}}}
</style></head><body><div class="wrap">
<h1>{html.escape(meta['name'])}</h1>
<div class="sub">{meta['dur']:.0f}s · {len(strokes)} strokes · {len(rallies)} rallies</div>
<div class="grid">
 {card("Strokes", len(strokes))}
 {card("Rallies", len(rallies))}
 {card("Avg / rally", f"{statistics.mean(rl):.1f}" if rl else "0")}
 {card("Median speed", f"{statistics.median(kmh):.0f}" if kmh else "0", " km/h")}
 {card("Top speed", f"{max(kmh):.0f}" if kmh else "0", " km/h")}
</div>

<div class="sec"><h2>Forehand vs backhand <span style="font-weight:400;color:#aaa;font-size:12px">— unsupervised, cluster→label heuristic</span></h2>
 <div class="split"><div style="width:{fh_pct:.0f}%;background:#378ADD;display:flex;align-items:center;justify-content:center">Forehand · {fh}</div>
 <div style="width:{100-fh_pct:.0f}%;background:#EF9F27;display:flex;align-items:center;justify-content:center">Backhand · {bh}</div></div>
 <div class="note">Two rotation-axis clusters, centroid cos = {sep:+.2f} (−1 = opposite axes ⇒ likely FH vs BH). Hand: {meta['hand']}{" · calibrated" if meta.get('calibrated') else ""}. Cluster→label uses the calibrated reference when present, else handedness / larger cluster.</div>
</div>

{spin_html}
{score_html}

<div class="two">
 <div class="sec"><h2>Strokes per rally</h2>{bar_chart(rhist)}<div class="note">Wearer's strokes only (full rally ≈ 2×).</div></div>
 <div class="sec"><h2>Swing speed (km/h)</h2>{bar_chart(shist, color="#1D9E75")}<div class="note">Rotational proxy, R={RACKET_R} m. Needs radar to calibrate absolute values.</div></div>
</div>

<div class="sec"><h2>Rally breakdown</h2>
 <table><tr><th>#</th><th>Start</th><th>Strokes</th><th>Duration</th><th>Types</th><th>Peak</th><th>Outcome</th></tr>{rally_rows}</table>
</div>

{"<div class='sec'><h2>Tagged outcomes</h2><table>"+out_rows+"</table></div>" if out_rows else ""}

<div class="note">Generated by scripts/session_report.py — exploratory analytics (no ground-truth labels yet).</div>
</div></body></html>"""
    open(out_path,"w",encoding="utf-8").write(H)

# ── main ─────────────────────────────────────────────────────────────────
def resolve(arg):
    if os.path.isdir(arg):
        csvp=os.path.join(arg,"hits.csv"); folder=arg; name=os.path.basename(arg)
    else:
        csvp=arg; folder=os.path.dirname(arg); name=os.path.basename(arg)
    return csvp, folder, name

def main():
    args=[x for x in sys.argv[1:]]
    hand=None
    for h in ('right','left'):
        if '--hand='+h in args: hand=h; args.remove('--hand='+h)
    if not args:
        print(__doc__); return
    csvp, folder, name = resolve(args[0])
    out = args[1] if len(args)>1 else os.path.join(folder,"report.html")
    t,g,a,om,hit = load_csv(csvp)
    strokes = extract_strokes(t,g,a,om,hit)
    if not strokes:
        print("No strokes found."); return
    outcomes = read_outcomes(folder)
    if hand is None and outcomes and 'hand' in outcomes:   # firmware-recorded
        hand = str(outcomes['hand'])
    calib = load_calib()
    sep = classify_fh_bh(strokes, hand, calib)
    has_spin = classify_spin(strokes, calib)
    rallies = detect_rallies(strokes)

    # link timestamped outcome tags (events.csv) to the rally they end
    events = read_events(folder)
    rally_tag=[None]*len(rallies)
    for ev_ms, oc in events:
        # the rally whose last stroke is closest before the tag
        best=None
        for i,r in enumerate(rallies):
            if r[0]['ms']-1000 <= ev_ms:
                best=i
        if best is not None: rally_tag[best]=oc

    meta = dict(name=name, dur=(t[-1]-t[0])/1000, t0=int(t[0]),
                hand=hand or "unknown", calibrated=bool(calib))
    render(meta, strokes, rallies, sep, outcomes, out,
           has_spin=has_spin, events=events, rally_tag=rally_tag)
    fh=sum(1 for s in strokes if s['type']=='Forehand')
    sp = (" spin: "+", ".join(f"{k} {sum(1 for s in strokes if s['spin']==k)}"
          for k in ('topspin','flat','slice'))) if has_spin else " spin: (no calib)"
    print(f"{name}: {len(strokes)} strokes, {len(rallies)} rallies, "
          f"{fh} FH / {len(strokes)-fh} BH (hand={hand or 'unknown'}), "
          f"median {statistics.median([s['kmh'] for s in strokes]):.0f} km/h.{sp}")
    if events: print(f"  {len(events)} tagged events linked to rallies")
    print(f"Report -> {out}")

if __name__=="__main__":
    main()
