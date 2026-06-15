# ESP32-S3 Tennis Monitoring Wearable

Tennis performance wearable firmware for the **Waveshare ESP32-S3-Touch-AMOLED-2.06** smartwatch. Worn on the hitting hand, it detects strokes with a dual-gate gyroscope algorithm, logs full-rate IMU data to SD for offline analysis, and lets you tag point outcomes live during a match.

Built with **ESP-IDF v5.5.2** + **LVGL 9**.

## Hardware

| Component | Chip | Bus |
|-----------|------|-----|
| MCU | ESP32-S3R8 (8 MB PSRAM) | — |
| Display | SH8601 AMOLED 410×502 | QSPI |
| Touch | FT3168 | I²C |
| IMU | QMI8658 (accel + gyro) | I²C |
| PMIC | AXP2101 | I²C |
| RTC | PCF85063A | I²C |
| Audio | ES8311 + NS4150B | I²S |
| Storage | microSD | SDMMC 1-bit |

## Features

- **Home** — battery, live clock, mode selection; AMOLED dim-to-clock screen-sleep.
- **Test & Tune** — live hit detection with on-device ω/α threshold tuning and SD recording (HIT-capture or ALL modes).
- **Play** — play/pause match logging with a 6-slice outcome dial (good hit, out, bad hit, unforced error, first serve in, lost point); per-session folder with hit log + outcome counts.
- **Config** — handedness, on-demand WiFi→NTP clock sync, live RTC date/time, firmware version.
- **Power management** — IMU idled off the tuning screen, DFS, screen-sleep.

## Hit detection

Dual-gate on gyro magnitude **ω** = √(gx²+gy²+gz²) and angular acceleration **α** = |Δω/dt|, with an alpha look-back window to bridge the rising-edge/peak gap. State machine: IDLE → IN_HIT → REFRACTORY. See [`HIT_DETECTION_SPEC.md`](HIT_DETECTION_SPEC.md).

## Build & flash

```bash
idf.py build
idf.py -p <PORT> flash
```

## Repository layout

```
main/                  Application (UI, screens, detection glue, logging, power)
components/qmi8658/     IMU driver
components/hit_detector/ Hit detection algorithm (pure C)
sdkconfig.defaults     Project config (sdkconfig is generated)
CLAUDE.md              Detailed project context / architecture notes
DevelopmentLog.md      Session-by-session development history
```

## Documentation

- [`CLAUDE.md`](CLAUDE.md) — architecture, pin map, gotchas, power notes, AXP2101 rail map.
- [`DevelopmentLog.md`](DevelopmentLog.md) — full build history.
- [`Play Mode — Phase 1 Implementation Plan.md`](Play%20Mode%20%E2%80%94%20Phase%201%20Implementation%20Plan.md) — Play mode spec.
