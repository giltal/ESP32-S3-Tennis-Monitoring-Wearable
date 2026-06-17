#!/usr/bin/env python3
"""Fit the forehand reference axis and spin boundaries from a LABELED
calibration recording, and write scripts/calib.json (loaded by session_report.py).

Record a Test-mode `full` session hitting in clean blocks with a pause between
each, then describe the blocks in order. Each block = TYPE[:SPIN]:COUNT where
TYPE is fh|bh and SPIN is flat|topspin|slice (COUNT is advisory).

Example:
  python calibrate.py Data/ses_011_full.csv --hand=right \\
      --blocks "fh:flat:10,bh:flat:10,fh:topspin:10,fh:slice:10,bh:slice:10"

The recording is split into len(blocks) segments by the largest inter-stroke
gaps (the pauses); strokes inherit their block's label, in order.
"""
import sys, os, json
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from session_report import load_csv, extract_strokes

CALIB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "calib.json")

def parse_blocks(s):
    blocks=[]
    for b in s.split(','):
        p=b.split(':')
        typ=p[0].strip().lower()
        spin=p[1].strip().lower() if len(p)>2 else (p[1].strip().lower() if len(p)==2 and p[1].strip().lower() in ('flat','topspin','slice') else 'flat')
        blocks.append(('Forehand' if typ in ('fh','forehand') else 'Backhand', spin))
    return blocks

def split_into_blocks(strokes, nblocks):
    """Split the stroke sequence into nblocks segments at the largest gaps."""
    if nblocks<=1: return [strokes]
    gaps=sorted(range(len(strokes)-1), key=lambda i: strokes[i+1]['ms']-strokes[i]['ms'], reverse=True)
    cuts=sorted(gaps[:nblocks-1])
    segs=[]; start=0
    for c in cuts:
        segs.append(strokes[start:c+1]); start=c+1
    segs.append(strokes[start:])
    return segs

def main():
    args=sys.argv[1:]; hand=None; blocks_str=None; path=None; i=0
    while i < len(args):
        a=args[i]
        if a.startswith('--hand='): hand=a.split('=',1)[1]
        elif a.startswith('--blocks='): blocks_str=a.split('=',1)[1]
        elif a=='--hand' and i+1<len(args): hand=args[i+1]; i+=1
        elif a=='--blocks' and i+1<len(args): blocks_str=args[i+1]; i+=1
        elif not a.startswith('--'): path=a
        i+=1
    if not path or not blocks_str:
        print(__doc__); return
    blocks=parse_blocks(blocks_str)
    t,g,a,om,hit=load_csv(path)
    strokes=extract_strokes(t,g,a,om,hit)
    print(f"{os.path.basename(path)}: {len(strokes)} strokes, expecting {len(blocks)} blocks")
    segs=split_into_blocks(strokes, len(blocks))
    labeled=[]
    for seg,(typ,spin) in zip(segs,blocks):
        for s in seg: s['L_type']=typ; s['L_spin']=spin
        labeled+=seg
        print(f"  block {typ}/{spin}: {len(seg)} strokes")

    # ---- forehand reference axis = mean contact rotation axis of forehands ----
    fh=[s for s in labeled if s['L_type']=='Forehand']
    bh=[s for s in labeled if s['L_type']=='Backhand']
    fh_ref=np.mean([s['axis'] for s in fh],0); fh_ref/=np.linalg.norm(fh_ref)+1e-9
    bh_ref=np.mean([s['axis'] for s in bh],0); bh_ref/=np.linalg.norm(bh_ref)+1e-9
    sep=float(np.dot(fh_ref,bh_ref))
    # FH/BH check: how many labeled strokes are nearer the correct ref
    ok=sum(1 for s in labeled
           if (np.dot(s['axis'],fh_ref)>np.dot(s['axis'],bh_ref))==(s['L_type']=='Forehand'))
    print(f"\nFH/BH: fh_ref={fh_ref.round(2).tolist()}  centroid cos={sep:+.2f}  "
          f"label agreement {ok}/{len(labeled)} ({100*ok/len(labeled):.0f}%)")

    # ---- spin discriminant: topspin vs slice net-rotation direction ----
    tops=[s for s in labeled if s['L_spin']=='topspin']
    slic=[s for s in labeled if s['L_spin']=='slice']
    calib=dict(hand=hand, fh_ref_axis=fh_ref.tolist())
    if tops and slic:
        mt=np.mean([s['rot'] for s in tops],0); msl=np.mean([s['rot'] for s in slic],0)
        spin_axis=mt-msl; spin_axis/=np.linalg.norm(spin_axis)+1e-9
        def score(s): return float(np.dot(s['rot'],spin_axis))
        st=[score(s) for s in tops]; ss=[score(s) for s in slic]
        flat=[s for s in labeled if s['L_spin']=='flat']
        sf=[score(s) for s in flat] if flat else []
        # thresholds: midpoints between class means along the axis
        thr_hi=(np.mean(st)+ (np.mean(sf) if sf else (np.mean(st)+np.mean(ss))/2))/2
        thr_lo=(np.mean(ss)+ (np.mean(sf) if sf else (np.mean(st)+np.mean(ss))/2))/2
        thr_hi,thr_lo=max(thr_hi,thr_lo),min(thr_hi,thr_lo)
        # accuracy on labeled
        def cls(v): return 'topspin' if v>thr_hi else ('slice' if v<thr_lo else 'flat')
        acc=sum(1 for s in labeled if s['L_spin'] and cls(score(s))==s['L_spin'])/len(labeled)
        calib.update(spin_axis=spin_axis.tolist(), spin_thr_hi=float(thr_hi), spin_thr_lo=float(thr_lo))
        print(f"SPIN: topspin score med {np.median(st):.1f}, flat {np.median(sf) if sf else float('nan'):.1f}, "
              f"slice {np.median(ss):.1f}  -> thr [{thr_lo:.1f}, {thr_hi:.1f}]  "
              f"labeled accuracy {100*acc:.0f}%")
    else:
        print("SPIN: need both topspin and slice blocks to fit spin axis (skipped)")

    json.dump(calib, open(CALIB,'w'), indent=2)
    print(f"\nWrote {CALIB}  -> session_report.py will use it automatically.")

if __name__=="__main__":
    main()
