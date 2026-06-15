# Hit Detection Implementation Spec — QMI8658 Tennis Wearable

## Goal
Detect a tennis ball-contact event ("a hit") in real time from the QMI8658
gyroscope, robustly distinguishing it from setup motion, follow-through, and
idle hand movement.

---

## 1. Sensor configuration (do this first)

Configure the QMI8658 in 6DOF typical-sensor mode with these settings:

| Setting | Value | Why |
|---|---|---|
| Gyro full-scale | **±2048 dps** (gFS = 111) | Tennis impacts can exceed 2000 dps. Anything lower clips peaks and ruins detection. |
| Gyro ODR | **500 Hz** (gODR = 0100) | ~2 ms sampling. Note: in 6DOF mode the real rate is ~0.94×, i.e. ~470 Hz. |
| Gyro LPF | Wide bandwidth (least aggressive) | An aggressive LPF rounds off the impact peak you are trying to detect. |
| Accel | ±16 g, same ODR | For later stroke classification; not required for basic hit detection. |

**Effective sample interval:** `DT = 1.0 / 470.0` seconds (≈ 0.002128 s).
Make `DT` a named constant so it is easy to change if you re-tune ODR.

**Clipping flag:** read the gyro clipping status bits each sample. If a sample
is clipped, still treat it as a hit candidate (clipping means very high motion),
but mark it so it is excluded from any amplitude-based classification later.

---

## 2. Data pipeline overview

```
raw Gx,Gy,Gz (dps)
   -> [optional light smoothing]
   -> magnitude  omega = sqrt(Gx^2 + Gy^2 + Gz^2)
   -> rate-of-change  alpha = |omega[n] - omega[n-1]| / DT
   -> detection state machine (threshold + sharpness + refractory)
   -> emit "HIT" event with timestamp + peak value
```

Process **one sample at a time** in a streaming fashion. Do not buffer the whole
session. Keep only a small rolling history (a ring buffer of ~the last 50 samples
≈ 100 ms is plenty).

---

## 3. Core math (implement exactly)

For each new sample `n`:

1. **Magnitude**
   `omega[n] = sqrt(Gx[n]^2 + Gy[n]^2 + Gz[n]^2)`   (units: dps)

2. **Rate of change** (sharpness of the spike)
   `alpha[n] = abs(omega[n] - omega[n-1]) / DT`   (units: dps/s)

3. **Detection condition** — BOTH must hold:
   - `omega[n] > OMEGA_THRESH`   (the motion is large)
   - `alpha[n] > ALPHA_THRESH`   (the motion is sharp / impulsive)

The dual condition is what separates a real ball strike (large AND sharp) from a
big-but-smooth swing or follow-through (large but NOT sharp).

---

## 4. Detection state machine

Use a small state machine so one hit is counted once and peaks are reported
accurately.

States: `IDLE`, `IN_HIT`, `REFRACTORY`

```
constants (start here, tune later):
  OMEGA_THRESH   = 800.0    # dps   -- raise if false positives, lower if misses
  ALPHA_THRESH   = 150000.0 # dps/s -- impulsive-ness gate
  REFRACTORY_MS  = 250      # ms    -- minimum gap between two hits
  REFRACTORY_SAMPLES = round(REFRACTORY_MS / 1000 * 470)   # ~117

state = IDLE
peak_omega = 0
peak_time  = 0
refractory_counter = 0

on each sample n with timestamp t:
    omega = sqrt(gx*gx + gy*gy + gz*gz)
    alpha = abs(omega - prev_omega) / DT
    prev_omega = omega

    if state == REFRACTORY:
        refractory_counter -= 1
        if refractory_counter <= 0:
            state = IDLE
        # while in refractory, still track in case of a bigger nearby peak? No:
        # ignore — refractory deliberately suppresses double counts.
        return

    if state == IDLE:
        if omega > OMEGA_THRESH and alpha > ALPHA_THRESH:
            state = IN_HIT
            peak_omega = omega
            peak_time  = t
        return

    if state == IN_HIT:
        # we are riding the spike; track the true peak
        if omega > peak_omega:
            peak_omega = omega
            peak_time  = t
        # spike is over once magnitude falls back below threshold
        if omega < OMEGA_THRESH:
            emit_hit(time = peak_time, peak = peak_omega, clipped = was_clipped)
            state = REFRACTORY
            refractory_counter = REFRACTORY_SAMPLES
        return
```

`emit_hit(...)` is your event callback — increment a counter, log, send BLE, etc.
**Report `peak_time` (the time of maximum omega), not the threshold-crossing
time** — the peak is the actual moment of impact.

---

## 5. Optional smoothing (only if data is noisy)

If raw gyro noise causes spurious `alpha` spikes, apply a very light filter to
`omega` BEFORE computing `alpha`. Use a 3-tap moving average or a one-pole IIR:

```
omega_f[n] = (1 - a) * omega_f[n-1] + a * omega[n]   # a ~ 0.6
```

Keep `a` high (0.5–0.7). Heavy smoothing destroys the sharpness you need, so
prefer the hardware LPF set wide and only smooth lightly in software if required.
Validate that smoothing does not lower your measured peaks by more than ~5%.

---

## 6. Recommended code structure

```
HitDetector
  - constructor(dt, omega_thresh, alpha_thresh, refractory_ms)
  - process_sample(gx, gy, gz, timestamp, clipped) -> Optional<HitEvent>
  - reset()

HitEvent
  - timestamp        (time of peak)
  - peak_omega       (dps)
  - clipped          (bool)
```

Keep `HitDetector` pure: no I/O, no globals, all thresholds injected via the
constructor. This makes it trivially unit-testable and lets you replay recorded
sessions through it offline.

---

## 7. Validation / tuning procedure (important)

1. **Record raw sessions** at 500 Hz / ±2048 dps: include real hits (forehand,
   backhand, serve, volley), plus non-hits (walking, picking up the racquet,
   spinning the racquet, gesturing). Log `Gx,Gy,Gz,timestamp,clipped`.
2. **Hand-label** the true hit times.
3. **Replay offline** through `HitDetector` and sweep `OMEGA_THRESH` and
   `ALPHA_THRESH`. Plot detected vs. labeled to get precision/recall.
4. **Pick thresholds** at the knee of the precision/recall curve. The starting
   constants above are estimates; your real values depend on where the watch
   sits on the wrist and your players' stroke styles.
5. **Re-check refractory**: confirm fast exchanges (volleys) are not merged into
   one event and that a single stroke is never counted twice.

---

## 8. Unit tests to write

- Synthetic clean spike (Gaussian bump, peak ~1500 dps, ~15 ms wide) → exactly 1 hit.
- Two spikes 100 ms apart → 1 hit (within refractory) — verify, then space them
  300 ms apart → 2 hits.
- Slow large swing (ramp to 1200 dps over 300 ms, no sharp edge) → 0 hits
  (fails the alpha gate). This is the key test that proves the alpha term works.
- Pure noise below threshold → 0 hits.
- Clipped sample → hit still emitted with `clipped = true`.

---

## 9. Things NOT to do

- Do **not** threshold on a single axis (Gx alone, etc.) — orientation varies.
  Always use the magnitude.
- Do **not** use omega alone without the alpha gate — you will catch
  follow-through and big swings as false hits.
- Do **not** set the gyro range below ±2048 dps "to get more resolution" — peak
  clipping costs you far more than the resolution gains.
- Do **not** heavily low-pass the signal — it erases the impact you are detecting.
- Do **not** buffer the full session in RAM on the watch — stream sample-by-sample.
