#!/usr/bin/env python3
"""Offline replay of the on-device hit detector against recorded CSV sessions.

Faithfully reproduces components/hit_detector/hit_detector.c so threshold
choices can be evaluated against full-data (ALL-mode / continuous HIT-mode)
recordings. Run from anywhere; point it at a Data folder of ses_*.csv.

CSV columns: session,ms,gx,gy,gz,ax,ay,az,omega,alpha,clip,hit
  omega = actual dps (already gx,gy,gz magnitude); alpha column = alpha/100.

Usage:  python analyze_hits.py [data_dir]
"""
import csv, glob, os, sys

DATA = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))


def load(path):
    rows = []
    with open(path) as f:
        r = csv.reader(f); next(r, None)
        for x in r:
            if len(x) < 12:
                continue
            try:
                rows.append((int(x[1]), float(x[8]), int(x[9]) * 100, x[10] == '1'))
            except ValueError:
                continue
    return rows


def strong_peaks(rows, thr=700, refr=250, win=60):
    """Real 'contact' peaks: local omega maxima >= thr, merged within refr ms."""
    peaks = []; n = len(rows)
    for i in range(n):
        om = rows[i][1]
        if om >= thr:
            lo, hi = max(0, i - win), min(n, i + win)
            if om == max(rows[j][1] for j in range(lo, hi)):
                ms = rows[i][0]
                if not peaks or ms - peaks[-1][0] > refr:
                    peaks.append([ms, om])
                elif om > peaks[-1][1]:
                    peaks[-1] = [ms, om]
    return peaks


def simulate(rows, wT, aT=40000, refr_ms=250, win=10):
    """Replay hit_detector.c: emit on falling edge (omega<wT) with peak omega."""
    state = 'IDLE'; refr_end = 0; armed = 0; peak = 0; ptime = 0; det = []
    for ms, om, al, clip in rows:
        if al > aT:
            armed = win
        elif armed > 0:
            armed -= 1
        gate = armed > 0
        if state == 'REFRACTORY' and ms >= refr_end:
            state = 'IDLE'
        if state == 'IDLE':
            if om > wT and gate:
                state = 'IN_HIT'; peak = om; ptime = ms
        elif state == 'IN_HIT':
            if om > peak:
                peak = om; ptime = ms
            if om < wT:
                det.append((ptime, peak)); state = 'REFRACTORY'; refr_end = ms + refr_ms
    return det


def report(path):
    rows = load(path)
    if not rows:
        return
    sp = strong_peaks(rows, 700)
    print(f"\n=== {os.path.basename(path)}: {len(rows)} samples, "
          f"{len(sp)} real contact peaks (omega>=700) ===")
    print(f"{'config':<26}{'hits':>6}{'pk>=700':>9}{'pk<700':>8}{'missed':>8}")
    for wT, refr in [(500, 250), (600, 250), (700, 250), (800, 250), (500, 150)]:
        det = simulate(rows, wT, refr_ms=refr)
        strong = sum(1 for _, p in det if p >= 700)
        dts = [t for t, _ in det]
        missed = sum(1 for ms, _ in sp if not any(abs(ms - dt) < 200 for dt in dts))
        print(f"wT={wT} refr={refr}ms".ljust(26) +
              f"{len(det):>6}{strong:>9}{len(det)-strong:>8}{missed:>8}")


if __name__ == "__main__":
    for f in sorted(glob.glob(os.path.join(DATA, "ses_*.csv"))):
        report(f)
    for f in sorted(glob.glob(os.path.join(DATA, "PlaySession_*", "hits.csv"))):
        report(f)
