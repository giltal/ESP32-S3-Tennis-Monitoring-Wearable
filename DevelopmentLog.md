# Development Log — Tennis Performance Wearable

## Session 1 — 2026-06-08 — Project Initialization & Hardware Selection

### What happened
- Created project directory at `C:\ESPIDFprojects\ESP32_S3_TennisMonitoringWearableDevice`
- Wrote project specification (`tennis_smart_watch_project_spec.md`)
- Selected hardware: **Waveshare ESP32-S3-Touch-AMOLED-2.06** smart watch dev board
- Researched and documented complete hardware specs and pin map from official Waveshare repo, community projects, and reference firmware
- Created `CLAUDE.md` with full hardware details, pin assignments, I2C addresses, and architecture
- Created this development log

### Hardware summary (Waveshare ESP32-S3-Touch-AMOLED-2.06)

| Component | Chip | Interface |
|-----------|------|-----------|
| MCU | ESP32-S3R8 (240MHz, 8MB PSRAM, 32MB Flash) | — |
| Display | SH8601 AMOLED (NOT CO5300) | QSPI (GPIO 4,5,6,7,11,12) |
| Touch | FT3168 | I2C 0x38 (GPIO 14/15) |
| IMU | QMI8658 6-axis | I2C 0x6B (GPIO 14/15), INT=GPIO21 |
| PMIC | AXP2101 | I2C 0x34 |
| RTC | PCF85063A | I2C 0x51 |
| Audio | ES8311 | I2S (GPIO 16,40,41,42,45) |
| SD Card | TF slot | SD 1-bit (GPIO 1,2,3) |

### Reference projects found
- [waveshareteam/ESP32-S3-Touch-AMOLED-2.06](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06) — Official Arduino examples
- [joaquimorg/OLEDS3Watch](https://github.com/joaquimorg/OLEDS3Watch) — ESP-IDF + LVGL smartwatch firmware
- [infinition/waveshare-watch-rs](https://github.com/infinition/waveshare-watch-rs) — Rust firmware (complete pin map source)

---

## Session 2 — 2026-06-08 — Display, Touch, IMU, SD Card Bringup

### What happened
- Scaffolded full ESP-IDF project with Waveshare BSP component (`waveshare/esp32_s3_touch_amoled_2_06` v1.0.7)
- BSP uses LVGL 9.5.0 via `espressif/esp_lvgl_port` v2.8.0
- Display: SH8601 AMOLED via QSPI, resolution 410x502, NOT round (has rounded corners)
- Wrote custom QMI8658 driver component from scratch (I2C register-level)
- Got all four subsystems working: display, touch, IMU, SD card

### Key discoveries & gotchas

**Touch controller (FT3168) — the hardest part:**
- BSP touch init FAILS on this board (wrong config or timing)
- With `CONFIG_BSP_ERROR_CHECK=y` (default), the failure calls `assert()` → `abort()` → hard crash
- **Solution**: Disable BSP error check (`# CONFIG_BSP_ERROR_CHECK is not set` in sdkconfig.defaults), let BSP touch fail gracefully, then:
  1. GPIO9 reset pulse AFTER BSP init (10ms low, 100ms recovery)
  2. Direct I2C reads to FT3168 at 0x38 (register 0x02 = num touches, followed by XY data)
- `bsp_display_start()` returns NULL when touch fails — recover display via `lv_display_get_default()`
- Must call `bsp_display_brightness_init()` manually after recovery

**Display:**
- Display driver is SH8601 (not CO5300 as originally assumed)
- 410x502 pixels, not round (rectangular with rounded corners)

**IMU (QMI8658):**
- Works reliably on shared I2C bus at 400kHz
- WHO_AM_I = 0x05 at address 0x6B
- Burst read: 14 bytes from register 0x33 (temp + accel + gyro)

**SD Card:**
- SDMMC 1-bit mode: D0=GPIO3, CMD=GPIO1, CLK=GPIO2
- BSP `bsp_sdcard_mount()` works, mount point is `BSP_SD_MOUNT_POINT`

**LVGL:**
- `%f` float format NOT supported (newlib nano) — must use integer math for `lv_label_set_text_fmt()`
- `bsp_display_lock(0)` means "block indefinitely" per BSP docs

---

## Session 3 — 2026-06-09 — Hit Detection Algorithm & Testing Program

### What happened
- Read and implemented `HIT_DETECTION_SPEC.md` as a modular component
- Built complete hit detection testing & tuning program
- Created `components/hit_detector/` — pure C module, no hardware dependencies
- Created real-time testing UI with live telemetry display and touch threshold controls
- Added serial logging (per-hit events + 1-second summaries) for data-driven tuning
- Added SD card CSV logging (toggled by REC button)

### Hit detector architecture
- **Dual-gate detection**: omega (gyro magnitude) AND alpha (rate-of-change) must both exceed thresholds
- **State machine**: IDLE → IN_HIT (tracks peak) → REFRACTORY (cooldown) → IDLE
- **Per-frame math**: omega = sqrt(gx²+gy²+gz²), alpha = |omega - prev_omega| / dt
- **Gyro clipping detection**: checks if raw ADC values hit ±32767/32768

### FreeRTOS architecture
- **IMU task** (core 1, priority MAX-2): polls QMI8658 at ~500Hz via `vTaskDelayUntil(2ms)`
- **LVGL timer** (20Hz): reads shared telemetry struct, updates display
- **Touch timer** (12Hz): reads FT3168 directly, checks button hit areas
- **Mutex**: protects telemetry struct between IMU task and display timer

### Critical fix: FreeRTOS tick rate
- `pdMS_TO_TICKS(2)` returned 0 with default 100Hz tick (10ms period)
- Caused `assert failed: xTaskDelayUntil tasks.c:1499 (xTimeIncrement > 0U)` → crash
- **Fix**: `CONFIG_FREERTOS_HZ=1000` in sdkconfig.defaults + delete sdkconfig to force regeneration

### IMU configuration for hit detection
- Accel: 16G scale, 500Hz ODR
- Gyro: 2048 DPS scale, 500Hz ODR
- LPF: disabled (CTRL5 = 0x00) for widest bandwidth
- Effective 6DOF sample rate: ~470 Hz (measured)
- DT = 1.0/470.0 seconds

---

## Session 4 — 2026-06-09 — Live Threshold Tuning

### What happened
- Connected serial monitor while user bounced a real tennis ball with racquet on the floor
- Iteratively tuned detection thresholds using live data analysis
- Achieved 10/10 hit detection accuracy

### Tuning progression

| Round | ω thresh | α thresh | Result | Notes |
|-------|----------|----------|--------|-------|
| Spec default | 800 | 150,000 | 0/10 | Way too high for floor bouncing |
| Round 1 | 150 | 15,000 | 8-9/10 | Some soft bounces had α=8-15k (below threshold) |
| Round 2 | 120 | 7,000 | 8/10 | Some hits peaked at only ω=123 (barely above) |
| Round 3 | 100 | 7,000 | **10/10** | Perfect detection |

### Live data observations (floor bouncing with racquet)
- **Idle noise floor**: ω~8-35 dps, α~0-4k
- **Bounce peaks**: ω=140-430 dps, α=8-85k
- **Soft bounces**: ω=110-150, α=7-15k (these are the marginal cases)
- **Conclusion**: Spec thresholds (800/150k) are calibrated for full-court play, not floor bouncing. Floor bouncing produces ~3-5x lower values.

### Button step sizes tuned
- Original ALPHA_STEP=10000 was too coarse for tuning around 7-15k range
- Changed to: OMEGA_STEP=20, ALPHA_STEP=2000

---

## Session 5 — 2026-06-09/10 — UI Enlargement & Auto-Recording

### What happened
- User reported UI elements too small for big fingers
- Redesigned entire UI with larger fonts and much bigger touch buttons
- Made SD card recording automatic (starts on boot when SD present)
- Simplified layout for better readability during play

### UI changes
- **Fonts**: montserrat_28 for hit counter/state, montserrat_24 for data + button labels, montserrat_20 for thresholds/status
- **Buttons**: 120×56 pixels (was 55×36), arranged in 2 rows of 3
- **Layout**: hit count → omega/peak → bar → thresholds → buttons → status
- **Button grid**: Row 1: ω-/ω+/RST, Row 2: α-/α+/REC

### SD card changes
- **Auto-start recording** when SD card is present at boot
- Session files: `ses_001.csv`, `ses_002.csv`, etc.
- Resumes numbering across reboots (scans existing files on mount)
- REC button now toggles pause/resume (instead of start/stop)
- CSV columns: ms, gx, gy, gz, ax, ay, az, omega, alpha, clip, hit
- Flush every 500 samples for crash resilience

### Current state
- Firmware running with ω=100, α=7k thresholds (tuned for floor bouncing)
- Auto-records full-rate IMU data to SD card
- Ready for real court play testing

---

## Session 6 — 2026-06-10 — Recording Overhaul & UI Polish

### What happened
1. Removed auto-start recording — app now boots in IDLE state
2. Added RECORD/STOP toggle button with clear visual states
3. Added persistent session counter displayed on screen
4. Added HIT-only recording mode with 3-second ring buffer
5. Iteratively enlarged all buttons to be finger-friendly on the 2" screen

### Recording architecture: two modes

**HIT mode (default):**
- A circular ring buffer continuously holds the last ~3 seconds of IMU data (~1600 samples, ~32KB)
- When REC is pressed, recording starts but nothing is written to SD — buffer keeps rolling
- On hit detection: flush the entire ring buffer to CSV (pre-hit context), then continue writing live samples for 3 seconds post-hit
- If another hit occurs during the post-hit window, the deadline extends (+3s) — nearby hits merge into one continuous capture
- Between hits, no data written → massive SD space savings during idle periods
- Status line shows "CAP" (yellow) during active capture windows, "REC" (red) when armed but idle

**ALL mode (toggle):**
- Every IMU sample written to CSV continuously (original behavior)
- Good for debugging or capturing full sessions without relying on hit detection

### Ring buffer implementation
```c
#define RING_BUF_CAP  1600       // ~3.4s at 470Hz
#define POST_HIT_MS   3000       // capture window after each hit

typedef struct {
    uint32_t ms;
    int16_t  gx10, gy10, gz10;   // gyro × 10
    int16_t  ax100, ay100, az100; // accel × 100
    int16_t  omega10;             // omega × 10
    int16_t  alpha_d100;          // alpha / 100
    uint8_t  flags;               // bit0=clip, bit1=hit
} ring_sample_t;                  // 21 bytes × 1600 = ~33KB
```

### Session counter persistence
- Counter stored in `/sdcard/ses_counter.txt` (plain integer)
- Incremented on each REC press, saved immediately
- Displayed as `#001`, `#002`, etc. at top of screen in cyan montserrat_32
- Filenames match: `ses_001.csv`, `ses_002.csv`
- CSV includes `session` column as first field for cross-reference
- Fallback on first boot: scans existing `ses_XXX.csv` filenames to find highest number

### REC button toggle
- **IDLE**: Red button, text "REC" — starts new log, increments session counter, resets hit count
- **Recording**: Grey button, text "STOP" — closes CSV file
- Multiple start/stop cycles per power-on, each creating a separate log file

### MODE toggle button
- **HIT** (amber, default): ring-buffer recording, only captures around detected hits
- **ALL** (blue): continuous full-rate recording
- Locked during active recording — must stop first to switch

### UI enlargement (iterative)
- User reported buttons too small multiple times — went through 3 rounds of enlargement
- Final layout uses 3 rows of large buttons, all 70px tall:

```
Row 1: [ w -  ] [ w +  ] [ RST  ]     3 × 120×70px
Row 2: [     a -     ] [     a +     ] 2 × 190×70px
Row 3: [ HIT ] [======== REC ========] 120×70 + 260×70px
```

- All button text: montserrat_28
- Threshold label (`wT:100 aT:7k`): montserrat_28
- Hit counter: montserrat_48
- Session number: montserrat_32
- HIT! indicator: montserrat_36

### Font sizes enabled in sdkconfig.defaults
Added `CONFIG_LV_FONT_MONTSERRAT_32=y`, `_36=y`, `_48=y` (binary grew from 764KB to 955KB — plenty of room on 32MB flash)

### Current state
- Firmware running, boots IDLE, HIT mode default
- All buttons large and finger-friendly
- Session counter persists across reboots
- Ready for court testing with efficient hit-windowed recording

### Next steps
- **Court testing**: User will play real tennis and bring back SD card data
- **Data analysis**: Load CSV files, analyze hit signatures at full court intensity
- **Threshold re-tuning**: Court hits will be stronger — likely need higher thresholds
- **Feature extraction**: After reliable detection, add power/spin estimation

---

## Session 7 — 2026-06-10 — Alpha Lookback Window Fix

### Problem: dual-gate timing mismatch

The dual-gate hit detection (`omega > thresh AND alpha > thresh`) checked both conditions on the **exact same sample** (line 51 of `hit_detector.c`). This caused poor detection of weaker hits (floor bounces):

- **Alpha** = rate-of-change of omega = peaks on the **rising/falling edge** (when omega is changing fastest)
- **Omega** = gyro magnitude = peaks at the **top of the impulse** (when omega is momentarily constant → alpha ≈ 0)
- For weak floor bounces where omega barely exceeds threshold for a few samples, there was almost no temporal overlap between both gates being true simultaneously
- Result: only 5/30 floor bounces detected (ω=150, α=20k)

### Fix: alpha lookback window

Added `alpha_window` field to `hit_detector_t` — when alpha exceeds its threshold, the alpha gate stays "armed" for the next N samples. This bridges the temporal gap between the alpha peak (rising edge) and the omega peak (a few ms later).

**Implementation** (`hit_detector.c`):
```c
if (alpha > hd->alpha_thresh) {
    hd->alpha_armed_countdown = hd->alpha_window;
    hd->last_alpha_cross_ms = timestamp_ms;
} else if (hd->alpha_armed_countdown > 0) {
    hd->alpha_armed_countdown--;
}
bool alpha_gate = (hd->alpha_armed_countdown > 0);

// In IDLE state:
if (omega > hd->omega_thresh && alpha_gate) {  // was: && alpha > hd->alpha_thresh
```

**Window size**: 10 samples (~21ms at 470Hz), set in `app_main()` after `hit_detector_init()`.

### Delta timing instrumentation

Added `last_hit_delta_ms` field — logs the time between alpha threshold crossing and omega threshold crossing for each hit. Serial output now shows `d=Xms` per hit.

### Test results

**Floor bounce detection** (ω=150, α=20k):
- Before fix: **5/30** bounces detected (17%)
- After fix: **11/11** in first test, then **66+ hits** in extended session

**Delta timing data** (11 hits analyzed):

| Delta (ms) | Count | Notes |
|------------|-------|-------|
| 0 | 4 | Alpha and omega cross same sample |
| 2 | 5 | 1 sample gap (~2ms) |
| 10-11 | 2 | ~5 sample gap (outliers) |

Most deltas are 0-2ms. Max observed = 11ms. Window of 21ms has good margin.

**False positives**: Zero during ~20 seconds of idle after bouncing stopped.

### Files changed
- `components/hit_detector/hit_detector.c` — alpha lookback logic + delta timing
- `components/hit_detector/include/hit_detector.h` — new fields: `alpha_window`, `alpha_armed_countdown`, `last_alpha_cross_ms`, `last_hit_delta_ms`
- `main/main.c` — set `detector.alpha_window = 10` after init, added `d=%lums` to HIT log

### Current state
- Firmware flashed, ω=150, α=20k, alpha window=10 samples (~21ms)
- Delta timing logged per hit for window optimization
- Ready for court testing — user will tune ω/α on the watch during play
- Court data + delta logs will inform final window size

---

## Session 8 — 2026-06-11 — Court Data Analysis & Court Threshold Tuning

### What happened
1. User brought back 5 court recording sessions on SD card
2. Analyzed all sessions — discovered court hits are 10x stronger than floor bounces
3. Found detection was almost completely failing: only 2/65 big court hits detected
4. Root cause: ω=150 threshold too low — detector triggered on pre-hit arm swings, sat in REFRACTORY during actual impact
5. Simulated detection with various thresholds against recorded data
6. Found ω=500 α=40k as optimal court thresholds — stable across all threshold ranges tested
7. Updated firmware defaults and threshold step sizes

### Court session data

| Session | Description | Duration | Samples | Hits (w=500 a=40k) | Peak ω |
|---------|-------------|----------|---------|---------------------|--------|
| ses_001 | Warmup light-med | 194s | 109,760 | 66 | 1723 |
| ses_002 | Hard hits (HIT mode) | 155s | 40,347 | 32 | 2605 |
| ses_003 | Hard hits (ALL mode) | 46s | 23,184 | 14 | 2375 |
| ses_004 | Backline game (ALL) | 122s | 60,843 | 23 | 2181 |
| ses_005 | Game play (ALL) | 122s | 61,149 | 30 | 2241 |

### Signal ranges on court

| Signal | Omega (dps) | Alpha (dps/s) |
|--------|-------------|---------------|
| Idle noise | 8-35 | 0-4k |
| Arm movements | 100-500 | 1-20k |
| Real court hits | 550-2600 | 70k-530k |

### Threshold simulation results
Detection count was extremely stable across tested thresholds (400-800 ω, 30-70k α) — the gap between arm motion (<500 dps) and real hits (>550 dps) is clean. Selected ω=500 α=40k with step sizes ω±50 α±5k.

### Files changed
- `main/main.c` — OMEGA_THRESH_INIT=500, ALPHA_THRESH_INIT=40000, OMEGA_STEP=50, ALPHA_STEP=5000

---

## Session 9 — 2026-06-11 — WiFi/NTP Time Sync, Battery Level, Clock Display

### What happened
1. Added WiFi credentials from SD card (`/sdcard/wifi.txt`: SSID line 1, password line 2)
2. One-shot WiFi → NTP → RTC sync at boot, then WiFi disabled
3. Added PCF85063A RTC read/write (I2C 0x51, BCD format)
4. Added AXP2101 fuel gauge battery percentage (I2C 0x34, register 0xA4)
5. Added time (HH:MM) and battery (%) display in top-right of screen
6. Rearranged top row: session# left, time+battery right

### New peripherals

**AXP2101 PMIC (0x34):**
- Fuel gauge enabled at boot: register 0x18, set bit 3
- Battery percentage: register 0xA4, direct read (0-100)
- Read every 5 seconds via LVGL slow timer

**PCF85063A RTC (0x51):**
- Time registers: 0x04 (seconds), 0x05 (minutes), 0x06 (hours) — BCD format
- Written once after NTP sync
- Read every 5 seconds for display update

### WiFi/NTP architecture
- Background FreeRTOS task, runs once at boot then self-deletes
- Reads SSID/password from `/sdcard/wifi.txt`
- WiFi STA mode, 15s connect timeout
- SNTP from pool.ntp.org, 10s sync timeout
- Timezone: `IST-2` (Israel, UTC+2)
- After RTC write: `esp_sntp_stop()` → `esp_wifi_stop()` → `esp_wifi_deinit()`
- No WiFi resources held after sync completes

### UI changes
- Top row split: session# (m24, cyan, left) + time/battery (m20, grey, right)
- Format: `14:30  85%`
- Session label shrunk from m32 to m24 to share row

### Build changes
- `main/CMakeLists.txt`: added `esp_wifi esp_netif esp_event nvs_flash` to PRIV_REQUIRES
- Binary size: 955KB → 1.4MB (WiFi stack)

### Files changed
- `main/main.c` — WiFi/NTP task, AXP2101 battery, PCF85063A RTC, time/battery UI
- `main/CMakeLists.txt` — WiFi dependencies

### Current state
- Court-tuned thresholds: ω=500 α=40k
- Time + battery displayed, RTC synced via NTP at boot
- WiFi disabled after sync — no ongoing power drain
- Ready for next court session

## Session 10 — 2026-06-11 — White-Stripe Framebuffer Fix, Clock Placement, Configurable TZ/DST

### What happened
1. Diagnosed and fixed "big white stripes" on the AMOLED introduced by Session 9's WiFi addition
2. Moved the clock/battery readout inward to clear the rounded display corner
3. Made the timezone configurable from `wifi.txt` (UTC offset + summer-time flag) instead of hardcoded `IST-2`

### White-stripe root cause (PSRAM framebuffer corruption)
- **Symptom**: persistent white horizontal stripes after the WiFi/battery/time changes; system otherwise stable (500Hz IMU, no crash/reboot — confirmed via serial).
- **First hypotheses (wrong)**: LVGL 9 default-theme borders on the bar/screen. Stripping all theme styles (`lv_obj_remove_style_all` on `bar_omega` and the screen, explicit border/outline/pad = 0) did NOT fix it. Those style cleanups were kept anyway (harmless hardening).
- **A/B test (decisive)**: disabled the WiFi task (`if (0 && sd_available)`) → stripes gone. Re-enabled → stripes back.
- **Root cause**: the LCD framebuffer lives in octal PSRAM; the SH8601 is fed over QSPI by DMA reading that framebuffer. While the WiFi stack runs (~15–25s at boot) its bus/cache activity corrupts the PSRAM framebuffer. The corruption lands in **static** screen regions (buttons, bar background) which LVGL never redraws, so it persists long after WiFi shuts down — only label regions self-heal on update.
- **Fix**: after `esp_wifi_stop()` + `esp_wifi_deinit()`, settle 150ms then `bsp_display_lock` → `lv_obj_invalidate(lv_screen_active())` → unlock, forcing a full repaint over the corrupted static areas. Stripes may flash briefly during the WiFi-active window but clear permanently once sync completes.

### Clock/battery placement
- Rounded corners were clipping the top-right time/battery label.
- Changed `lv_obj_align(lbl_time_bat, LV_ALIGN_TOP_RIGHT, -10, y)` → `(-50, y + 4)` to pull it inward toward center.

### Configurable timezone + DST (wifi.txt)
- `wifi.txt` now accepts two optional extra lines (order-independent, case-tolerant):
  ```
  <SSID>
  <password>
  UTC=2
  summerTime=true
  ```
- Parsed in `read_wifi_credentials()` into globals `g_utc_offset` (default 2) and `g_summer_time` (default false).
- TZ built at sync time: `eff = UTC + (summerTime ? 1 : 0)`, POSIX string `snprintf(tz, "UTC%+d", -eff)` (POSIX inverts the sign for east-of-UTC). `UTC=2` + `summerTime=true` → `UTC-3` → displayed UTC+3.
- Replaced hardcoded `setenv("TZ","IST-2",1)`. Serial logs parsed values and resulting TZ.

### Files changed
- `main/main.c` — framebuffer repaint after WiFi deinit; screen/bar style hardening; clock reposition; UTC/DST parsing + dynamic TZ

### Current state
- Display clean; clock shows summer time when `summerTime=true` in wifi.txt
- All Session 9 features intact (NTP sync, battery, WiFi one-shot)

## Session 11 — 2026-06-11 — Home Screen + Mode Navigation (multi-screen routing)

### What happened
Added a landing **Home screen** with mode selection in front of the existing Test & Tune UI, turning the single-screen app into a 3-screen routed app. Existing hit-detection / recording logic untouched — only wrapped and routed.

### Screen architecture
- Three real LVGL screens, each `lv_obj_create(NULL)` + shared `style_screen()` (black bg, no border/outline/pad, no scroll):
  - `scr_main` (Home), `scr_play` (placeholder), `scr_test` (existing Test & Tune)
- `app_screen_t current_screen` + `nav_to(screen)` → `lv_screen_load()`. Boots to `SCREEN_MAIN`.
- Builders: `create_main_screen()`, `create_play_screen()`, `create_test_screen()` (was the old `create_ui` body, now parented to `scr_test`). New `create_ui()` builds all three, loads Home, starts the shared timers.
- Timers (`update_display_cb` 50ms, `touch_poll_cb` 80ms, `slow_peripherals_cb` 5s) are global — they run regardless of active screen; updating labels on an inactive screen is harmless.

### Home screen layout (410×502, top→bottom)
- Battery: top-center, icon + %, colored by level (green ≥50 / amber ≥20 / red <20). Helper `set_battery_label()` picks `LV_SYMBOL_BATTERY_*` glyph + color.
- PLAY button: large (370×165) green, upper half, m36 label, `LV_SYMBOL_PLAY`.
- Clock: dominant element, m48 white, centered between the buttons (HH:MM, `--:--` until NTP).
- TEST button: large (370×165) blue, lower half, m36 label, `LV_SYMBOL_SETTINGS`.
- `update_main_screen()` refreshes clock + battery every tick from globals (not telemetry).

### Touch routing (screen-aware)
- `touch_poll_cb` resolves taps scoped to `current_screen`: Home hit-tests PLAY/TEST; Test uses the existing `buttons[]` array; Play has no touch targets.
- Generalized hit testing into `obj_hit(obj,tx,ty)`; `check_button_hit()` now loops via it.

### Back navigation = physical BOOT button (GPIO0)
- Rounded display corners hid an on-screen top-corner back button → replaced with the **BOOT button (GPIO0)** as universal "back".
- GPIO0 configured input + internal pull-up in `app_main`; polled in `touch_poll_cb` (active-low, edge-detect + debounce, fires once/press).
- `handle_back()`: from **Test**, if `logging_active` it calls `stop_logging()` (flush+close CSV) **before** returning Home — no truncated files; from **Play**, just returns Home; on Home, no-op.
- Play screen shows a `← BOOT = back` hint. (Reading GPIO0 at runtime is safe; it only straps at reset.)
- Removed on-screen back buttons; `NUM_BUTTONS` back to 7.

### Tweaks
- Test screen: session # was clipped by the rounded top-left corner → moved to stack right-aligned under the top-right clock (`#006` under `14:32 85%`).

### Files changed
- `main/main.c` — screen routing, Home/Play builders, `create_test_screen`, GPIO0 back button, battery-icon helper, session-# reposition

### Current state
- Boots to Home; PLAY → placeholder (BOOT back), TEST → existing tuner (BOOT back, saves recording first)
- Battery/clock live on Home; all hit-detection/recording behavior unchanged

## Session 12 — 2026-06-11 — Config Screen (handedness + on-demand NTP) + WiFi/Display Fixes

### What happened
Added a 4th screen — **Config/Settings** — reached from Home via the **PWR button (GPIO10)**. Holds a handedness radio and an on-demand NTP clock sync, replacing the boot-time auto-sync. Plus several display-integrity fixes around WiFi.

### Config screen
- `SCREEN_CONFIG` added to the screen enum; `create_config_screen()` builds it. Reached: Home + **PWR (GPIO10)** → Config. Back: **BOOT (GPIO0)** → Home (saves config).
- Custom hit-tested controls (no LVGL indev in this app):
  - **Handedness radio**: `RIGHT` / `LEFT` boxes; selected = cyan via `update_hand_radio()`. Tapping sets `g_left_handed` + `save_config()`.
  - **Sync Clock (NTP)** button → `start_ntp_sync()`.
- **Persistence**: `g_left_handed` ↔ `/sdcard/config.txt` (`left_handed=0/1`), `load_config()` at boot (in `init_sd`), `save_config()` on toggle and on Config exit.

### GPIO10 (PWR) as Config button
- Configured input + pull-up alongside GPIO0 (single `gpio_config` covering both pin masks).
- Polled in `touch_poll_cb`; on Home, a press → Config.
- **Power-on artifact fix**: both `boot_down`/`pwr_down` edge states init to `true`, so the button press used to power the watch on (or a held BOOT) isn't read as a fresh press — was making it boot straight into Config.

### On-demand NTP (removed boot auto-sync)
- Removed `xTaskCreate(wifi_ntp_sync_task,...)` from `app_main`; semaphore still created up-front. WiFi/NTP now only runs when the user taps Sync Clock.
- `wifi_ntp_sync_task` made **re-runnable**: one-time netif/event-loop/STA-netif setup guarded by `static netif_ready`; `s_ntp_synced` reset and the connect semaphore drained at each run; `esp_wifi_init`/`deinit` cycled per run. Guarded against double-launch via `s_ntp_busy`.
- Status surfaced on Config: `s_ntp_status` (idle/syncing/ok/fail) → `update_cfg_ntp_status()` (Syncing… / ✓ synced / ⚠ failed).

### WiFi vs. display — three fixes
1. **Stripes during sync**: WiFi disturbs the LCD framebuffer while running. Added `g_display_freeze` — set before launching the sync; `update_display_cb` and `touch_poll_cb` early-return while frozen, so nothing invalidates and the panel holds the clean "Syncing…" frame painted at task start (`lv_refr_now` under `bsp_display_lock`).
2. **Boots into Config** — see power-on artifact fix above.
3. **Dirty Home after returning from a sync** — the panel uses a **single partial PSRAM draw buffer** (confirmed in BSP: `BSP_LCD_DRAW_BUFF_DOUBLE=0`, `buff_spiram=true`); in-place repaints + a per-`nav_to` redraw-counter (`g_redraw_frames`, kept for normal switches) did NOT reliably recover the WiFi disturbance. **Resolution: reboot after a successful sync** — the PCF85063A RTC is battery-backed so time persists; `esp_restart()` rebuilds a pristine UI. On failure: no reboot, unfreeze + repaint + show "⚠ Sync failed".

### Files changed
- `main/main.c` — config screen + builder, GPIO10 polling, handedness persistence, re-runnable on-demand NTP, display freeze, reboot-on-sync; `#include "esp_system.h"`

### Current state
- Home → PWR → Config (handedness radio + Sync Clock); BOOT = back
- Clock sync is on-demand, clean (no stripes), reboots to a pristine Home with correct RTC time
- Handedness persisted to config.txt (consumed later by Play mode / analysis)

## Session 13 — 2026-06-11 — Test Screen Redesign

### What happened
Reworked the Test & Tune screen layout (no logic changes):
- **Removed** the omega power bar and the live ω/peak readout (`bar_omega`, `lbl_omega` deleted, plus their `update_display_cb` blocks).
- **Threshold controls** restyled to `[ − ]  value  [ + ]`: a green threshold value (m32) centered between minus (left) and plus (right) buttons (84×62, `LV_SYMBOL_MINUS`/`PLUS`). Two labels now — `lbl_thresh` (wT) and `lbl_thresh_a` (aT) — replacing the single combined label.
- **RESET** moved to its own full-width row (390×52), much wider than the old 120-wide button.
- **Hit counter** lowered to upper-mid and centered (`LV_ALIGN_TOP_MID`, m48), `HIT!` state beside it.
- **Clock + session enlarged** (session m24→m32, clock m20→m28) and **pulled inward** off the rounded corners (session x=70, clock right offset −55) — recurring rounded-corner clipping issue.
- HIT/REC unchanged.
- Labels use Latin `wT`/`aT` (Montserrat lacks Greek ω/α glyphs → would render as empty boxes).

### Files changed
- `main/main.c` — `create_test_screen` rewrite, `update_display_cb` threshold split, removed bar/omega globals

### Current state
- Test screen redesigned per spec; all hit-detection/recording/threshold-tuning behavior unchanged

## Session 14 — 2026-06-12 — Home Power Management (Stage 1 + DFS; light sleep deferred)

### Goal
Cut Home-screen power. Always-on clock retained (user choice). Staged: 1 = safe (no sleep), 2 = chip light sleep.

### Stage 1 — IMU idle + slow polling on Home (LANDED, working)
- The 500Hz IMU task is the dominant load and is **useless off the Test screen**. Made it **cooperatively pausable**: `imu_paused_req` flag checked at the top of the IMU loop; the task `vTaskSuspend(NULL)`s itself there — OUTSIDE any I2C transaction, so it never holds the shared bus mutex while suspended (would deadlock the UI's touch/RTC/battery reads).
- `apply_screen_power(screen)`: full rate only on Test (resume IMU, `update`=50ms `touch`=80ms); everywhere else (Home/Config/Play) idle the IMU and re-pace timers to `touch`=250ms (4Hz) / `update`=1000ms via `lv_timer_set_period` on stored handles `tmr_update`/`tmr_touch`.
- Wired into `nav_to()` (every screen switch) and called once at boot (Home) after the IMU task is created (handle captured into `imu_handle`; the app_main call wrapped in `bsp_display_lock`).
- Verified: boot log goes silent after `Returned from app_main()` (no per-second IMU logs) = IMU suspended on Home; logs resume on Test.

### Stage 2 — automatic light sleep (ENABLED, then REVERTED to DFS-only)
- Added `CONFIG_PM_ENABLE`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE`, `CONFIG_PM_DFS_INIT_AUTO` to `sdkconfig.defaults`; `esp_pm_configure({160, 40, light_sleep_enable})`.
- With `light_sleep_enable=true` it booted (`pm: ... Light sleep: ENABLED`) but **broke the dev workflow**: the **USB-Serial-JTAG console drops during light sleep**, so esptool/`idf.py flash` could no longer reach the chip (`Failed to connect: No serial data received`), and the watch appeared to "keep resetting".
- **Recovery** (device unflashable while asleep): unplug USB → **hold BOOT (GPIO0)** → replug while holding → release. That resets with BOOT held into a stable ROM **download mode** (no sleep). Then flash with esptool `--before no_reset` directly to the waiting ROM:
  `python -m esptool --chip esp32s3 -p COM9 -b 460800 --before no_reset --after hard_reset write_flash ... 0x0 bootloader.bin 0x10000 tennis_monitor.bin 0x8000 partition-table.bin`
- **Resolution**: kept DFS (`max=160, min=40`) which downclocks when idle and **keeps USB-JTAG alive**; set `light_sleep_enable=false`. Light sleep is a one-line flip — re-enable it for battery-only / production (pair with Stage 3 screen-off, where USB isn't connected).

### Files changed
- `main/main.c` — IMU cooperative pause, `apply_screen_power`/`imu_set_paused`, timer handles, `esp_pm_configure` (DFS, light sleep off), `#include "esp_pm.h"`
- `sdkconfig.defaults` — `CONFIG_PM_ENABLE` / tickless idle / DFS auto

### Current state
- Home: IMU idle + 4Hz control polling + CPU DFS down to 40MHz when idle; clock always-on; USB-JTAG reliable
- Test/Play/Config: full rate restored on entry
- User measuring real run-time gain. Stage 2 light sleep + Stage 3 screen-off are the next levers.

### Radio/audio audit (same session)
- **WiFi** off (on-demand + `esp_wifi_deinit`); **BLE/BT** never initialized → radio domain never powered.
- Researched the AXP2101 rail map (schematic + Waveshare ES8311 demo + Rust firmware). **Finding: audio has no dedicated PMIC rail** — ES8311/NS4150B share **ALDO1 (A3V3)** with the display, so cutting an AXP rail would kill the screen. Audio is gated by **GPIO46 (`PA_CTRL`)** instead. Added `gpio_set_level(GPIO46, 0)` at boot to hold the speaker amp off (codec/I²S already never init'd). Only separate unused rail is ALDO3 = vibration motor (negligible idle draw, left alone). Full rail table now in CLAUDE.md.

### Home screen-sleep (AMOLED dim-to-clock) — LANDED, working
- **AMOLED has no backlight**: emissive, power ∝ lit pixels × brightness, black ≈ free. So the big lit cost on Home is the filled PLAY/TEST buttons.
- After 30s idle on Home, `home_sleep()` hides battery + PLAY/TEST (→ black) and drops brightness to 40% (`bsp_display_brightness_set`), leaving only the centered clock. `home_wake()` on any touch restores full Home at 80% (first touch consumed as wake). Inactivity tracked via `home_activity_ms` (touch-down edge) + checked in `update_display_cb`; `apply_screen_power()` wakes on entering Home and forces full brightness on any other screen. `g_redraw_frames=3` keeps the single-buffer transitions clean.

## Session 15 — 2026-06-13 — Play Mode Phase 1

Implemented per `Play Mode — Phase 1 Implementation Plan.md`. Hit detector + Test HIT-logging reused unchanged; wrapped with play/pause + outcome tagging.

### UI (segmented-ring dial)
- `create_play_screen()`: center dial (`PLAY`/`PAUSE`) + **5 colored slices** as `lv_arc` background arcs. First render looked like "banana" sausages → fixed with `arc_rounded=false` (straight radial edges, matches mockup). Slices clockwise from top: Good hit / Out / Unforced / First serve / Lost pt, each a name+count label at its band mid-angle.
- Top info row added on request: battery (left) · `Hits N` session total (center) · clock (right), inset off the rounded corners, refreshed by `update_play_screen()`.
- **Tap = angular hit-test**: `atan2(dy,dx)` + radius in `[R_IN=74, R_OUT=195]` → `slice = (ang+90)/72`. Counts only in PLAY.

### State / files / triggers
- `play_state` starts PAUSED. `play_toggle()` (PWR/GPIO10, dispatched by screen in `touch_poll_cb`): PLAY → `imu_set_paused(false)` + `logging_active=true`; PAUSE → suspend + stop append. File A stays open across pauses.
- `start_play_session()` (on `nav_to(PLAY)`): mkdir `/sdcard/PlaySession_YYYY-MM-DD_HH-MM/`, open `hits.csv`, HIT-mode, reset detector + counters, PAUSED.
- `end_play_session()` — single guarded finalize (`play_session_ended`): writes `outcomes.txt` (5 counts, once) + flush/close `hits.csv`. Called by all 3 triggers: BOOT-back (`handle_back`), battery ≤3% and 30-min inactivity (both in `update_display_cb`, then `nav_to(MAIN)`). Activity = hit (set in `imu_task`) OR slice tap OR PWR.
- **RTC date**: extended `pcf85063a_write_datetime()` (NTP writes full date now) + `pcf85063a_read_date()`. Pre-NTP folders date as `2000-01-01`.

### Touch responsiveness
- Slice taps felt insensitive at the non-Test 250ms poll, then still at 80ms → set **interactive (Test/Play) touch poll to 30ms** in `apply_screen_power` (Home/Config stay 250ms for power). User confirmed "better".

### Files changed
- `main/main.c` — Play screen/state/session/triggers, RTC datetime, per-screen touch rate, Play info row

### Bug fix — no empty sessions / counter bumps
- Original `start_play_session()` created the folder + `hits.csv` and did `log_session_num++; save_session_counter()` on every Play *entry* → empty PlaySession folders + the Test counter creeping each time you opened Play.
- Fix: `start_play_session()` now only resets state (no disk, no counter). Folder + `hits.csv` are created **lazily on first PLAY** via new `play_open_files()` (which does NOT touch the Test counter). `end_play_session()` gated on `play_files_open` — writes `outcomes.txt`/closes only if the user actually played. Enter→leave Play = nothing written.

### Config additions
- **Live RTC date/time** on Config (`YYYY-MM-DD` / `HH:MM:SS`, `pcf85063a_read_full`, 1Hz) to verify the clock/date are right.
- **Firmware version** `FW_VERSION "0.1"` (single `#define`, bump per revision) shown top-right of Config (moved off the rounded corner).

### Current state
- Play Phase 1 working: Home→PLAY → dial+ring, PWR play/pause, slice tagging, lazy session folder with hits.csv + outcomes.txt, 3 end triggers. Later phases: stats/scoring/classification.
- Firmware **v0.1** (revision tracking started).

## Session 16 — 2026-06-15 — Court Data Round 1: file-timestamp fix + 6-icon Play slices (v0.2)

First court data in (`Data/PlaySession_2026-06-14_*`): 3 sessions, 94/97/344 detected hits vs 21/28/60 tagged points.

### Bug — wrong file timestamps (FIXED)
- SD files had the **1980 FAT epoch** as their date attribute (folder/file *names* were right — those come from the RTC). Cause: FATFS stamps via the ESP **system clock** (`time()`), which we never seeded from the RTC.
- Fix: `sync_system_time_from_rtc()` at boot — reads RTC full datetime, `setenv("TZ","UTC0")` + `mktime` + `settimeofday` so `localtime()` reproduces the RTC wall-clock → FATFS stamps match the RTC. Verified: boot logs `System clock seeded from RTC: 2026-06-15 ...`. (Old files keep their bad stamps; new ones are correct.)

### Play tweaks
- **Total hits → outcomes.txt**: appended `Total hits=N` (`detector.hit_count`) line after the outcome counts.
- **5 → 6 slices, icons not text** (text was hard to find mid-game). Clockwise from top, 60° each: 1 Good hit ✓ green · 2 Out ↑ yellow · 3 Bad hit ⚠ orange · 4 Unforced error ✗ red · 5 First serve in ▶ blue · 6 Lost point ▼ white. Icon (m36) + count (m36) per slice; LVGL built-in symbols. Hit-test → `((ang+120)%360)/60`. (Custom tennis icons would need a generated icon font — deferred.)

### Files changed
- `main/main.c` — `sync_system_time_from_rtc`, 6-slice icon ring, total-hits line, `FW_VERSION 0.2`

### Current state
- Firmware **v0.2**. File timestamps correct, Play uses 6 icon slices + total-hits in outcomes.

### Published to GitHub
- Repo: https://github.com/giltal/ESP32-S3-Tennis-Monitoring-Wearable (branch `main`). Added `.gitignore` (excludes build/managed_components/sdkconfig + raw IMU CSVs, keeps outcomes.txt) and `README.md`.
- **Workflow going forward: auto commit + push after every change set** (user preference).

## Session 17 — 2026-06-16 — Threshold validation from court data

New full-data court sessions (`ses_006`/`ses_007`, ALL/continuous) + 3 HIT-mode sessions + a Play session. Goal: evaluate ω=500/α=40k.

### Lesson — verify with an algorithm replay, not a proxy metric
- First-pass script flagged dozens of "missed strong strokes" (omega 700–2600 undetected) → looked alarming. **It was a measurement bug**: it matched a hit to a peak only within ±60ms, but the detector **emits on the falling edge** (omega<wT), which for a hard stroke is 100–150ms *after* the peak. So real detections were mis-counted as misses.
- Correct method: `scripts/analyze_hits.py` replays `hit_detector.c` exactly (reproduces the on-device hit counts 61/31 to the number).

### Result — ω=500/α=40k is good
- Replay: ses_006 caught **58/61** contacts (3 missed), ses_007 **30/30**. ~95–100%.
- Raising wT hurts: ω=700 → ses_006 misses 13 real (softer) contacts; ω=800 → 19. 500 is the sweet spot (lower = arm noise, higher = drops soft shots).
- Only real limits: a few contacts within the 250ms refractory get merged (the 3 ses_006 misses); ~12 soft 500–700 detections in ses_006 (likely real soft rallying).
- **Decision: keep ω=500 / α=40k. No firmware change.**

### Added
- `scripts/analyze_hits.py` — reusable offline threshold-evaluation tool.

### Follow-up — alpha threshold 40k → 35k (v0.3)
- The 3 ses_006 misses are **not** omega/refractory/window — they're the **alpha gate**. Those strokes are smooth (omega ramps gradually), so alpha (Δω/dt) peaks at only ~10.9k / 36.7k / 38.9k, under the 40k gate.
- Enlarging the alpha lookback window (20ms → 200ms) does **nothing** (58/61) — the window only extends arming *after* a crossing, and these never cross 40k.
- Lowering the **threshold** is the only lever: two of the three peak at 36.7k/38.9k (just under 40k) → α=35k catches them (60/61). The third (alpha 10.9k, omega 714) was the user gently handing the ball to the coach — not a real stroke — so 60/61 = all real strokes.
- Validated α=35k vs 40k across all sessions: +0..+2 detections each → negligible false-positive cost.
- **Change: `ALPHA_THRESH_INIT` 40000 → 35000, FW v0.3.**

## Session 18 — 2026-06-16 — Play data analytics (first pass) + filename mode (v0.4)

### v0.4
- Test recordings now named `ses_NNN_full.csv` / `ses_NNN_hit.csv` (mode in filename → know which are replay-able).

### Play analytics — `scripts/play_analysis.py` (exploratory, no labels yet)
First pass on `PlaySession_2026-06-16` (746s, 161 raw hit=1):
- **Multi-contact merge**: raw hit=1 events double-log across overlapping HIT-mode capture windows. Merging detections <300ms apart → **87 strokes, exactly matching the device `Total hits=87`**. Use merged strokes, not raw rows.
- **Rallies** (gap>8s): 33 rallies, median 2 wearer-strokes/rally (≈2× total), max 7.
- **Swing speed** (rotational proxy, ω→rad/s × R=0.7m): peak ω med 1429 / max 2639 dps → racket-head **med 63 / max 116 km/h** (plausible).
- **Forehand/backhand**: unsupervised 2-means on the contact rotation axis → **52 vs 35 strokes, centroids nearly opposite (cos=-0.77)** = strong FH/BH signature. Needs a labeled swing set to assign which cluster is which.
- **Spin**: rotation/linear ratio computed; needs labeled topspin/flat to calibrate.
- **For scoring** (final-app target): firmware currently logs only *aggregate* outcome counts. Will need **per-tag timestamped outcome events** to reconstruct points/score.

Target: `scripts/play_analysis.py` grows into a session-processing app (stats + score).

### Session report app — `scripts/session_report.py`
- `python scripts/session_report.py <folder_or_csv> [out.html]` → self-contained **HTML report**: metric cards (strokes/rallies/avg/speed), forehand-vs-backhand split bar, strokes-per-rally + swing-speed histograms (inline SVG), per-rally breakdown table, and tagged outcomes. Works on both PlaySession folders and `ses_*_full.csv`. numpy-only (no matplotlib/sklewn).
- Verified: Play 06-16 → 87 strokes / 33 rallies / 52 FH·35 BH / 63 km/h median; ses_006 → 61 strokes.
- Generated `report.html` files are gitignored (outputs, not source).
- Still heuristic: FH/BH label assignment (needs calibration session); speed is a rotational proxy (needs radar); scoring needs timestamped outcome events from firmware.

## Session 19 — 2026-06-16 — Analytics: rally model, handedness, spin research (v0.5)

### Rally model (data-driven)
- Inter-stroke gap distribution is **bimodal**: within-rally cycle 2–4s (46 gaps), between-rally idle 8–25s (32 gaps), clear **valley at 4–8s**. → `RALLY_GAP` 8s→**6s** (sits in the valley). Confirms user's model: rally = strokes at a regular cycle bounded by long idles; outcome tags land at rally ends (linking needs timestamped events — future).

### Handedness for FH/BH
- Firmware now records `hand=left|right` in `outcomes.txt` (from `g_left_handed`), FW v0.5.
- `session_report.py`: reads `hand` from outcomes (or `--hand=right|left`); `classify_fh_bh()` aligns clusters to a forehand reference axis flipped by handedness (deterministic flip verified: right→52FH/35BH, left→35/52). The reference axis (`FH_REF_AXIS_RIGHT`) is a placeholder until a labeled calibration session pins it — handedness then generalizes it to all players.

### Spin research (web)
- IMU literature: accelerometer→stroke detection, **gyroscope (angular velocity)→stroke/spin classification** (~93% with ML). Biomechanics: **topspin = low→high brush** (tip below→above, face closed), **slice = high→low** (tip above→below, face open), **flat = straight/neutral**. Wrist-sensor mapping: net rotation direction about the horizontal brush axis (gyro integral through contact) + wrist tilt at contact (accel gravity vector → face angle).
- Spin (and final FH/BH naming) is a **supervised** problem → needs a short **calibration session** (e.g. 10 topspin / 10 slice / 10 flat, and 10 FH / 10 BH labeled). Then thresholds/boundaries can be set and validated.

### Files changed
- `main/main.c` (hand in outcomes, v0.5); `scripts/session_report.py` (rally 6s, handedness-aware FH/BH)
- Sources: ambientintelligence.aalto.fi tennis IMU paper; topspinpro.com / eliteclubs.com swing-path biomechanics

## Session 20 — 2026-06-16 — Calibration tool + timestamped events (v0.6)

### Firmware — events.csv (v0.6)
- Play mode now writes `<play_dir>/events.csv` (`ms,outcome`): each tagged outcome with its timestamp (opened in `play_open_files`, appended on each slice tap during PLAY, closed in `end_play_session`). Enables reconstructing points/score and linking outcomes → rallies.

### Calibration tool — `scripts/calibrate.py`
- Fits the **forehand reference axis** and **spin (topspin/flat/slice) boundaries** from a *labeled* recording. Record a Test `full` session in clean blocks with pauses, then: `python calibrate.py <csv> --hand right --blocks "fh:flat:10,bh:flat:10,fh:topspin:10,fh:slice:10,bh:slice:10"`. Splits at the largest gaps → labels by order → writes `scripts/calib.json` (fh_ref_axis, spin_axis, thresholds) + reports FH/BH label agreement and spin accuracy.
- Spin discriminant = direction of (mean topspin net-rotation − mean slice net-rotation); thresholds at class-mean midpoints. Net rotation = gyro integral over the swing (encodes low→high vs high→low brush).

### Report integration — `scripts/session_report.py`
- Loads `calib.json` if present → calibrated FH/BH (ref flipped by handedness) + per-stroke **spin** classification (new Spin section).
- Reads `events.csv` → links each tag to its rally (new Outcome column in the rally table) + a provisional **Points won/lost** score (mapping documented, user-adjustable).
- Pipeline validated end-to-end (graceful when calib/events absent).

## Session 21 — 2026-06-18 — First real calibration (FH/BH 100%, spin 89%)

Labeled sessions today: 11 warmup, **12 FH topspin**, **13 FH flat**, **14 backhand**, 15 baseline practice (all `ses_NNN_full` except 15 `_hit`).

### calibrate.py — multi-file labeled mode
- Added per-stroke-type file groups (cleaner than blocks-in-one-file): `--fh a,b --bh c --topspin a --flat b [--slice d]`. Resolves paths against `Data/`. Keeps the `--blocks` single-file mode.

### Results
- **Forehand/backhand: 100% (40/40)**, fh_ref centroid cos −0.87. Classifier locked. ✓
- **Spin (topspin vs flat): 89% LOO.** Investigated features: **peak linear accel is the discriminator** (flat = direct/high accel, topspin = brushy/low; d-prime 1.47, 89%); net-rotation was weak (≤70%). Rebuilt spin as a standardized feature vector `[peak_acc, peak_acc/peak_om, rot_unit(3)]` with **squared-Fisher dim weights + weak-dim dropping** → calibrator auto-picks peak_acc (weights [2.17, 0.88, 0,0,0]). Nearest weighted class-mean classify; generalizes to slice when a slice block is added.

### Session 15 (baseline practice) with calibration
- 114 strokes, 18 rallies (up to **16** strokes — real baseline rallies vs the choppy match session), **57 FH / 57 BH** balanced, 72–76 km/h.
- **Caveat: spin was forehand-only calibration** → backhand spin is extrapolated (BH 49 topspin/8 flat is unreliable). Need backhand topspin + flat blocks to calibrate BH spin.

### Files changed
- `scripts/calibrate.py` (multi-file mode, Fisher-weighted spin), `scripts/session_report.py` (`spin_feature`, weighted nearest-mean `classify_spin`). `calib.json` gitignored (user-specific).

### Serve detection — characterized + tooling ready (session_report + calibrate)
- Court data (June 17, full games): the **"First serve in"** tag (12 across 3 sessions) gives partial serve ground-truth. Found the serve signature: **always first stroke of a rally** (12/12); **fast** (~82 km/h vs ~64 groundstroke); **distinct overhead-pronation axis** (ax_x −0.63 vs −0.33). Serve-like first-strokes **cluster into service games** (temporal confirmation).
- Limitation: only first-serves-in are tagged, so most serves are unlabeled → tags alone can't train a clean serve-vs-return classifier (63% on noisy labels). The IMU can, with a **dedicated serve calibration block**.
- **Tagging model (from user)**: `First serve in` = serve marker (not a point); the following tag is the point outcome. No marker before an outcome = 2nd-serve point. `Bad hit` = double fault OR bad shot (context). Win = `Good hit`; loss = Out/Unforced/Lost/Bad hit.
- **Added serve support (ready, pending a serve block)**: `calibrate.py --serve <block>` fits a serve-vs-groundstroke signature `[peak_om, peak_acc, axis_x, axis_y]` (standardized nearest-mean, LOO reported) → `serve_mean`/`ground_mean` in calib.json. `session_report.classify_serve` marks first-of-rally strokes matching the serve signature; summary reports serve count, avg serve speed, and first-serve-in % (from tags). Graceful when no serve calib.

### Haptic feedback on Play tags (v0.7)
- Court-usability: a short vibration confirms each point tag. Called on each Play slice tap; **non-blocking** (set the pin, arm a one-shot esp_timer to release after `MOTOR_BUZZ_MS`=120ms — no UI freeze).
- **First attempt (wrong):** pulsed only the AXP2101 **ALDO3** rail — motor did **not** vibrate.
- **Root cause (from schematic):** the motor net is `MOTOR/GPIO18` — a transistor **switched by GPIO18**, *supplied* by ALDO3. ALDO3 alone is just the rail; GPIO18 is the actual switch. Found by extracting the schematic PDF text layer (pypdf): net labels `MOTORGPIO18`, `MOTOR`, `QMI_INT1 MOTOR GND`.
- **Fix:** `motor_init()` enables ALDO3 (3.0V, left ON as supply) + configures GPIO18 as output-low; `motor_buzz()` drives GPIO18 high, the one-shot timer (`motor_off_cb`) drives it low. GPIO18 is otherwise unused in our pin map.

### Report reshaped for real matches (session_report.py)
- A full match would make the per-rally table huge, so: **tagged points only** — when `events.csv` exists, untagged rallies (warm-up / noise) are dropped from all stats; untagged sessions still keep everything.
- **Removed** the long rally-breakdown table. Kept the **strokes-per-rally** bars; replaced the speed histogram with a **swing-speed range** box (min / IQR / median / max).
- Added a **Game summary** section at the end — `summarize()` writes plain-language, data-driven bullets (volume, FH/BH balance + speeds, dominant spin, provisional score + most-frequent error, and a first-vs-second-half win-rate trend / fatigue flag).
