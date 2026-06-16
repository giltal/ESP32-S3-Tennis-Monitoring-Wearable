#!/usr/bin/env python3
"""First-pass analytics for a recorded play/test session.

Extracts per-stroke features from the IMU capture, detects rallies, estimates
swing speed, and does an unsupervised 2-way split of the swing rotation axis
(forehand vs backhand hypothesis). Exploratory — no ground-truth labels yet.

CSV cols: session,ms,gx,gy,gz,ax,ay,az,omega,alpha,clip,hit
  gyro = raw*10 dps, accel = raw*100 (g*100?), omega = dps, alpha = /100.

Usage: python play_analysis.py <path-to-hits.csv-or-ses_full.csv>
"""
import csv, sys, math, statistics
import numpy as np

MERGE_MS   = 300     # detections closer than this = one stroke (multi-contact)
RALLY_GAP  = 8000    # gap > this (ms) starts a new rally
WIN_MS     = 250     # +/- window around a stroke for feature extraction
RACKET_R   = 0.7     # m, effective wrist->sweet-spot radius for speed estimate

def load(path):
    t=[]; g=[]; a=[]; om=[]; hit=[]
    for r in csv.reader(open(path)):
        if not r or r[0]=='session' or len(r)<12: continue
        try:
            t.append(int(r[1]))
            g.append((float(r[2])/10, float(r[3])/10, float(r[4])/10))   # dps
            a.append((float(r[5])/100, float(r[6])/100, float(r[7])/100))# g
            om.append(float(r[8])); hit.append(r[11]=='1')
        except ValueError: continue
    return (np.array(t), np.array(g), np.array(a), np.array(om), np.array(hit))

def strokes(t, om, hit):
    """Merge hit=1 events within MERGE_MS; return list of stroke indices (peak)."""
    idx = np.where(hit)[0]
    if len(idx)==0: return []
    groups=[[idx[0]]]
    for i in idx[1:]:
        if t[i]-t[groups[-1][-1]] <= MERGE_MS: groups[-1].append(i)
        else: groups.append([i])
    # within each merged group, the stroke = sample of max omega in a window
    peaks=[]
    for grp in groups:
        lo=max(0,grp[0]-150); hi=min(len(om),grp[-1]+150)
        pk=lo+int(np.argmax(om[lo:hi]))
        peaks.append(pk)
    return peaks

def features(t, g, a, om, pk):
    lo=np.searchsorted(t, t[pk]-WIN_MS); hi=np.searchsorted(t, t[pk]+WIN_MS)
    seg_g=g[lo:hi]; seg_a=a[lo:hi]; seg_om=om[lo:hi]
    peak_om=om[pk]
    accmag=np.linalg.norm(seg_a,axis=1); peak_acc=float(accmag.max()) if len(accmag) else 0
    # rotation axis at contact (unit gyro vector at peak)
    gv=g[pk]; n=np.linalg.norm(gv); axis=gv/n if n>0 else gv
    # swing duration: omega above 40% of peak
    thr=0.4*peak_om
    dur = (np.sum(seg_om>thr))*2 if peak_om>0 else 0   # ~2ms/sample
    # estimated racket-head speed from rotation
    v = math.radians(peak_om)*RACKET_R           # m/s
    kmh = v*3.6
    rot_lin = peak_om/(peak_acc+1e-6)            # rotational vs linear ratio
    return dict(ms=int(t[pk]), peak_om=peak_om, peak_acc=peak_acc,
                axis=axis, dur_ms=dur, kmh=kmh, rot_lin=rot_lin)

def kmeans2(X, iters=50):
    X=np.array(X)
    c=np.array([X[0], X[len(X)//2]])  # deterministic seed
    for _ in range(iters):
        d=np.linalg.norm(X[:,None,:]-c[None,:,:],axis=2)
        lab=d.argmin(1)
        for k in range(2):
            if (lab==k).any(): c[k]=X[lab==k].mean(0)
    return lab, c

def main(path):
    t,g,a,om,hit=load(path)
    print(f"\n=== {path} ===")
    print(f"samples={len(t)}  dur={(t[-1]-t[0])/1000:.0f}s  raw hit=1 events={int(hit.sum())}")
    pk=strokes(t,om,hit)
    print(f"strokes after merging <{MERGE_MS}ms multi-contacts: {len(pk)}")
    feats=[features(t,g,a,om,p) for p in pk]

    # ---- rallies ----
    rallies=[]; cur=[feats[0]]
    for f in feats[1:]:
        if f['ms']-cur[-1]['ms']>RALLY_GAP: rallies.append(cur); cur=[f]
        else: cur.append(f)
    rallies.append(cur)
    rl=[len(r) for r in rallies]
    print(f"\nRALLIES (gap>{RALLY_GAP/1000:.0f}s): {len(rallies)}  "
          f"strokes/rally: min {min(rl)} med {statistics.median(rl):.0f} max {max(rl)} mean {statistics.mean(rl):.1f}")
    print("  (wearer's strokes only; full rally ~2x)")
    hist={}
    for n in rl: hist[n]=hist.get(n,0)+1
    print("  strokes-per-rally histogram:", dict(sorted(hist.items())))

    # ---- speed ----
    kmh=[f['kmh'] for f in feats]; oms=[f['peak_om'] for f in feats]
    print(f"\nSWING SPEED (rotational proxy, R={RACKET_R}m):")
    print(f"  peak omega dps: min {min(oms):.0f} med {statistics.median(oms):.0f} max {max(oms):.0f}")
    print(f"  est racket-head km/h: med {statistics.median(kmh):.0f} max {max(kmh):.0f}")

    # ---- forehand/backhand hypothesis: 2-cluster on rotation axis ----
    axes=np.array([f['axis'] for f in feats])
    lab,cent=kmeans2(axes)
    print(f"\nROTATION-AXIS 2-CLUSTER (forehand/backhand hypothesis):")
    for k in range(2):
        m=lab==k
        print(f"  cluster {k}: {m.sum():>3} strokes  axis~[{cent[k][0]:+.2f},{cent[k][1]:+.2f},{cent[k][2]:+.2f}]  "
              f"med speed {statistics.median([feats[i]['kmh'] for i in range(len(feats)) if m[i]]):.0f}km/h")
    cos=float(np.dot(cent[0],cent[1])/(np.linalg.norm(cent[0])*np.linalg.norm(cent[1])+1e-9))
    print(f"  cluster separation: centroids cos={cos:+.2f} (-1=opposite axes => likely FH vs BH)")

    # ---- spin proxy ----
    rl2=[f['rot_lin'] for f in feats]
    print(f"\nSPIN PROXY (rotation/linear ratio): med {statistics.median(rl2):.1f}  "
          f"(higher=more brushing/topspin-like; needs validation)")

if __name__=="__main__":
    main(sys.argv[1] if len(sys.argv)>1 else
         "Data/PlaySession_2026-06-16_08-50/hits.csv")
