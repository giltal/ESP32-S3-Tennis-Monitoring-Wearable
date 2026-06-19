# Tennis Performance Wearable — Project Context

## What This Is

Wearable tennis performance analysis system built on the **Waveshare ESP32-S3-Touch-AMOLED-2.06** smart watch. Worn on the hitting hand. Detects hits using a dual-gate gyroscope algorithm, logs full-rate IMU data to SD card for offline analysis. Future: power/spin estimation, rally detection, scoring.

## Hardware — Waveshare ESP32-S3-Touch-AMOLED-2.06

| Component | Chip | Interface | Notes |
|-----------|------|-----------|-------|
| **MCU** | ESP32-S3R8 | — | Dual-core Xtensa LX7 @ 240MHz, 8MB PSRAM, 32MB Flash |
| **Display** | SH8601 AMOLED | QSPI | 2.06", 410x502, NOT round (rectangular with rounded corners) |
| **Touch** | FT3168 | I2C (0x38) | RST=GPIO9, INT=GPIO38 |
| **IMU** | QMI8658 | I2C (0x6B) | 6-axis accel+gyro, WHO_AM_I=0x05 |
| **PMIC** | AXP2101 | I2C (0x34) | Li-battery management |
| **RTC** | PCF85063A | I2C (0x51) | INT=GPIO39 |
| **Audio** | ES8311 | I2S | Speaker + mic |
| **SD Card** | — | SDMMC 1-bit | D0=GPIO3, CMD=GPIO1, CLK=GPIO2 |

### Pin Map

```
=== QSPI Display (SH8601) ===
LCD_SDIO0-3  = GPIO 4,5,6,7
LCD_SCLK     = GPIO 11
LCD_CS       = GPIO 12
LCD_RESET    = GPIO 8
LCD_TE       = GPIO 13

=== I2C Bus (shared: touch, IMU, PMIC, RTC) ===
I2C_SDA      = GPIO 15
I2C_SCL      = GPIO 14
I2C_FREQ     = 400 kHz

=== Touch (FT3168) ===
TP_RESET     = GPIO 9       (active low)
TP_INT       = GPIO 38
TP_ADDR      = 0x38

=== IMU (QMI8658) ===
IMU_ADDR     = 0x6B
IMU_INT      = GPIO 21

=== SD Card ===
SD_CLK       = GPIO 2
SD_CMD       = GPIO 1
SD_DATA      = GPIO 3
SD_CS        = GPIO 17

=== Buttons ===
BOOT_BUTTON  = GPIO 0       (used as "back" → Home)
PWR_BUTTON   = GPIO 10      (Home → Config; input+pull-up, active low)
```

## Development Environment

- **Framework**: ESP-IDF v5.5.2 (installed at `C:\Users\97254\esp\v5.5.2\esp-idf`)
- **Host OS**: Windows
- **Serial port**: COM9
- **BSP**: `waveshare/esp32_s3_touch_amoled_2_06` v1.0.7
- **LVGL**: 9.5.0 via `espressif/esp_lvgl_port` v2.8.0
- **Build/flash**: `idf.py -p COM9 flash`

## Git / GitHub

- **Repo**: https://github.com/giltal/ESP32-S3-Tennis-Monitoring-Wearable (branch `main`, HTTPS, creds in Windows Credential Manager).
- **Auto-push**: commit AND push after every change set (user preference). Use `git -C "<project path>" ...` (project is at `C:\ESPIDFprojects\ESP32_S3_TennisMonitoringWearableDevice`).
- **Not tracked** (`.gitignore`): `build/`, `managed_components/`, generated `sdkconfig`, raw IMU CSVs (`Data/ses_*.csv`, `Data/**/hits.csv`). Keep `sdkconfig.defaults`, `dependencies.lock`, `main/idf_component.yml`, and `Data/**/outcomes.txt`.
- **Never commit secrets** — WiFi creds live only in `wifi.txt` on the SD card, never in the repo.

## Key Paths

```
# Project root
C:\ESPIDFprojects\ESP32_S3_TennisMonitoringWearableDevice

main/main.c                     # Application entry — hit detection test/tune program
components/qmi8658/             # IMU driver (I2C register-level, burst read)
components/hit_detector/        # Hit detection algorithm (pure C, no HW deps)
HIT_DETECTION_SPEC.md           # Hit detection algorithm specification
DevelopmentLog.md               # Full session-by-session project history
```

## Current Firmware: Home Screen + Hit Detection Test & Tune

### Architecture

```
IMU Task (core 1, 500Hz)          LVGL Timer (20Hz)         Touch Timer (12Hz)
  ┌─ QMI8658 burst read            ┌─ Read telemetry          ┌─ BOOT(GPIO0) = back
  ├─ hit_detector_process()        ├─ Update active screen     ├─ FT3168 I2C read
  ├─ SD CSV logging                ├─ Update bar/labels        └─ Screen-aware tap
  └─ Write telemetry (mutex)       └─ Time/battery (5s)            dispatch

NTP Sync Task (one-shot at boot)
  ┌─ Read /sdcard/wifi.txt (SSID + password + UTC/summerTime)
  ├─ WiFi STA connect (15s timeout)
  ├─ SNTP sync from pool.ntp.org (10s timeout)
  ├─ Set PCF85063A RTC via I2C
  └─ WiFi stop + deinit (frees resources) + full-screen repaint
```

### Screen routing (4 screens)
- `app_screen_t {SCREEN_MAIN, SCREEN_PLAY, SCREEN_TEST, SCREEN_CONFIG}` + `nav_to()` → `lv_screen_load()`. **Boots to Home.**
- Each screen is a separate `lv_obj_create(NULL)`, styled by shared `style_screen()`. Builders: `create_main_screen()`, `create_play_screen()`, `create_config_screen()`, `create_test_screen()`. `create_ui()` builds all four + starts shared timers.
- **Home**: battery (top-center, icon+%, colored by level), large **PLAY** button (→ Play placeholder), dominant **clock** (m48, centered), large **TEST** button (→ Test & Tune). Clock/battery live via `update_main_screen()`.
- **Play**: live point-tagging mode (see Play Mode section below).
- **Config**: handedness radio (`RIGHT`/`LEFT`, persisted to `/sdcard/config.txt`, `g_left_handed`) + **Sync Clock (NTP)** button (on-demand) + **live RTC date/time** (`YYYY-MM-DD` / `HH:MM:SS`, ticks each second via `pcf85063a_read_full`, to verify the clock) + firmware version (`v` `FW_VERSION`, top-right). Reached from Home via **PWR button (GPIO10)**.
- **Test**: the existing hit-detection tuner (logic unchanged); session # stacked under the top-right clock.
- **Navigation buttons are physical** (rounded corners hide on-screen corner buttons), polled in `touch_poll_cb` (active-low, debounced; edge states init `true` so the power-on press isn't a false trigger):
  - **BOOT (GPIO0) = back** → Home. `handle_back()`: from Test, **stops+saves any active recording** first; from Config, saves config; from Play, just returns.
  - **PWR (GPIO10)** on Home → Config.
- Touch is **screen-aware**: `touch_poll_cb` dispatches taps by `current_screen` (`obj_hit()` for Home PLAY/TEST and Config radio/NTP; `buttons[]` for Test; none for Play).

### WiFi vs. single PSRAM draw buffer (display integrity)
- Panel uses a **single partial PSRAM draw buffer** (`BSP_LCD_DRAW_BUFF_DOUBLE=0`, `buff_spiram=true`). WiFi activity disturbs it; in-place repaints don't reliably recover.
- During an on-demand sync: `g_display_freeze` makes `update_display_cb`/`touch_poll_cb` early-return so nothing invalidates → panel holds the clean "Syncing…" frame.
- **After a successful sync the device reboots** (`esp_restart()`) — RTC is battery-backed so time persists, and the UI rebuilds pristine. (This is the reliable fix; a redraw-counter `g_redraw_frames` per `nav_to` handles ordinary screen switches.)

### IMU Configuration
- Accel: 16G scale, 500Hz ODR
- Gyro: 2048 DPS scale, 500Hz ODR
- LPF: disabled (CTRL5 = 0x00, widest bandwidth)
- Effective sample rate: ~470 Hz
- DT = 1.0/470.0 seconds

### Hit Detection Algorithm
- **Dual-gate**: omega (gyro magnitude √(gx²+gy²+gz²)) AND alpha (rate-of-change |Δω/dt|) must both exceed thresholds
- **Alpha lookback window**: alpha gate stays armed for 10 samples (~21ms) after crossing threshold, bridging the temporal gap between alpha peak (rising edge) and omega peak (top of impulse)
- **State machine**: IDLE → IN_HIT (track peak) → REFRACTORY (250ms cooldown) → IDLE
- **Gyro clipping**: detected when raw ADC = ±32767/32768
- **Delta instrumentation**: each hit logs `d=Xms` (time between alpha and omega threshold crossings) for window optimization
- **Current thresholds**: ω=500, α=40000 (court-tuned), alpha_window=10 samples
- **Threshold steps**: ω±50, α±5k

### Play Mode (Phase 1)
Reached from Home → PLAY. Reuses the hit detector + Test-mode HIT-logging unchanged; wraps them with a play/pause gate and 5-outcome tagging. Spec: `Play Mode — Phase 1 Implementation Plan.md`.
- **UI**: center dial (`PLAY`/`PAUSE`), ring of **6 colored slices** (60° each) as `lv_arc` background arcs (`arc_rounded=false`, straight edges). Clockwise from top with **icons** (no text) + large live count (m36): 1 Good hit ✓ green · 2 Out ↑ yellow · 3 Bad hit ⚠ orange · 4 Unforced error ✗ red · 5 First serve in ▶ blue · 6 Lost point ▼ white (dark icon). Top info row: battery · `Hits N` (session total) · clock. `BOOT`=back, `PWR`=play/pause.
- **Tap = angular hit-test**: `atan2(ty-CY, tx-CX)` + radius in `[R_IN,R_OUT]`, `slice=((ang+120)%360)/60` (slice 0 centered at top). Increments that in-memory counter (PLAY only). No LVGL indev — same custom-touch model as Test.
- **State (starts PAUSED)**: PLAY → resume IMU + `logging_active=true` (append File A); PAUSE → suspend IMU + stop append (file stays open). `play_toggle()` on PWR.
- **Session files** (`/sdcard/PlaySession_YYYY-MM-DD_HH-MM/`, datetime from RTC): `hits.csv` (File A, HIT-mode capture) + `outcomes.txt` (File B: 6 counts + `Total hits=N` + `hand=left|right`, written once at end) + `events.csv` (`ms,outcome` — each tagged outcome with a timestamp, for scoring). Files created **lazily on first PLAY** (`play_open_files`). Play does **not** touch the Test `ses_counter.txt`.
- **End triggers → single guarded `end_play_session()`** (runs once via `play_session_ended`): (1) BOOT back to Main, (2) battery ≤3%, (3) 30-min no-activity (hit OR slice tap OR PWR resets `play_last_activity_ms`; runs in PLAY *and* PAUSE). Battery/inactivity also `nav_to(MAIN)`. Finalize = write File B + flush/close File A.
- **RTC date**: `pcf85063a_write_datetime()` (NTP now writes full date) + `pcf85063a_read_date()` for the folder name. Before any NTP sync on this firmware the RTC date is unset → folders dated `2000-01-01`.
- **File timestamps**: FATFS stamps files from the ESP *system* clock (`time()`), not the RTC chip — so `sync_system_time_from_rtc()` seeds the system clock from the RTC at boot (TZ=UTC0 so localtime reproduces the RTC wall-clock). Without it, files got the 1980 FAT epoch.
- **Touch rate**: Play/Test poll touch at 30ms (responsive taps); Home/Config at 250ms (power).

### SD Card Logging
- **Starts IDLE** — user presses REC to begin recording
- **Two modes** (toggled by HIT/ALL button, locked during recording):
  - **HIT mode (default)**: ring buffer holds last 3s (~1600 samples); on hit, flushes pre-hit buffer + captures 3s post-hit. Multiple hits merge windows. Massive space savings.
  - **ALL mode**: continuous full-rate logging of every sample
- Files: `/sdcard/ses_NNN_<mode>.csv` where `<mode>` = `full` (ALL/continuous) or `hit` (HIT-capture) — the mode is in the filename so you know at a glance which recordings are replay-able by `scripts/analyze_hits.py` (need `full`).
- **Session counter persisted** in `/sdcard/ses_counter.txt` across reboots
- CSV columns: `session,ms,gx,gy,gz,ax,ay,az,omega,alpha,clip,hit`
- Gyro values ×10, accel values ×100, omega ×10 (one decimal), alpha /100
- Flush every 500 samples + on capture window close (HIT mode)
- REC/STOP toggle: each cycle creates a separate log file

### WiFi / NTP / RTC
- WiFi credentials + timezone: `/sdcard/wifi.txt`
  ```
  <SSID>          # line 1
  <password>      # line 2
  UTC=2           # optional: hours east of UTC (default 2)
  summerTime=true # optional: DST flag, adds +1h (default false)
  ```
  Extra lines are order-independent and case-tolerant; parsed into `g_utc_offset` / `g_summer_time`.
- **On-demand** from Config → Sync Clock (no longer auto-run at boot): connect WiFi → SNTP sync → set TZ → write PCF85063A RTC → WiFi off → **reboot on success**
- Sync task is re-runnable (one-time netif setup guarded; `s_ntp_busy` guards double-launch; `s_ntp_status` drives the Config status label)
- Timezone is computed, NOT hardcoded: `eff = UTC + (summerTime?1:0)`, POSIX `"UTC%+d"` with `-eff` (POSIX inverts sign). `UTC=2`+`summerTime=true` → `UTC-3` → UTC+3.
- WiFi connect timeout: 15s, NTP timeout: 10s
- WiFi fully disabled after sync (stop + deinit)
- **After deinit: full-screen repaint** (`lv_obj_invalidate(lv_screen_active())`) — see PSRAM framebuffer gotcha below.

### Battery (AXP2101)
- Fuel gauge enabled at boot (register 0x18, bit 3)
- Battery percentage read from register 0xA4 every 5 seconds
- Displayed in top-right of screen

### UI Layout (410×502 screen)

**Home screen (landing):**
```
            🔋 82%                Battery top-center, colored by level
   ┌────────────────────┐
   │     ▶  PLAY         │        Large green button → Play (placeholder)
   └────────────────────┘
          14:32                   Clock, dominant (m48), centered
   ┌────────────────────┐
   │     ⚙  TEST         │        Large blue button → Test & Tune
   └────────────────────┘
```
PLAY/TEST = m36 labels, 370×165. Back to Home from Play/Test = **BOOT button (GPIO0)**.

**Test & Tune screen** (reached via TEST; BOOT = back, saves recording first):
```
  #006              14:32 85%      Session (m32 cyan) + clock (m28 grey),
                                   pulled inward off the rounded corners
            12   HIT!              Hit counter (m48, centered) + state (m36)
  [ − ]     wT 500      [ + ]      Omega control: −/+ flank green value (m32)
  [ − ]     aT 40k      [ + ]      Alpha control (−/+ = 84×62)
  [ ======= RESET ======= ]        Wide reset, own row (390×52)
  [ HIT ] [ ===== REC ===== ]      Mode + record, 120×68 + 260×68 (unchanged)
  500Hz                            Status line (m24)
```
Power bar and live ω/peak readout were removed in the redesign. Threshold labels use
Latin `wT`/`aT` (Montserrat has no Greek ω/α glyphs). −/+ use `LV_SYMBOL_MINUS`/`PLUS`.

### Serial Logging
- Per-hit: `HIT #N peak=X clip=Y t=Z d=Dms`
- Per-second summary: `w=X wpk=Y a=Xk apk=Yk st=S hits=N [Hz]` (only while the IMU task runs — i.e. on the Test screen; silent on Home where the IMU is idled)

### Power management (Home = low power)
- **IMU task is cooperatively paused off the Test screen.** `apply_screen_power(screen)` (called from `nav_to()` + once at boot): on Test → resume IMU + fast timers (update 50ms / touch 80ms); on Home/Config/Play → `imu_paused_req=true` (the IMU loop self-`vTaskSuspend`s at a safe point, never mid-I2C) + slow timers (touch 250ms = 4Hz, clock ~1Hz).
- **DFS** via `esp_pm_configure({max=160, min=40})` — CPU downclocks when idle. Keeps USB-JTAG alive.
- **Automatic light sleep is OFF** (`light_sleep_enable=false`) on purpose — see gotcha below. One-line flip to re-enable for battery/production.
- **WiFi**: off (on-demand only; `esp_wifi_deinit` after each sync). **BLE/BT**: never initialized → radio/domain never powered.
- **Audio**: ES8311 codec + I²S never initialized; speaker amp held off via **GPIO46 (`PA_CTRL`) driven LOW** at boot.
- **Home screen-sleep** (AMOLED is emissive — no backlight; power ∝ lit pixels × brightness): after `HOME_SLEEP_TIMEOUT_MS` (30s) of no touch on Home, `home_sleep()` hides battery + PLAY/TEST buttons (→ black = ~0 power) and dims to `BRIGHT_DIM` (40%), leaving only the clock. Any touch → `home_wake()` (first touch is consumed as wake, no button action) restores full Home at `BRIGHT_FULL` (80%). Timer resets on every touch; entering Home wakes + restarts it; leaving Home forces full brightness (so Test/Config never inherit the dim).

### AXP2101 power-rail map (this board — from schematic, verified vs demos)
**The BSP does NOT configure the AXP2101** — it runs at hardware defaults; we only read battery %/fuel gauge. Rail→peripheral (I2C 0x34, LDO on/off reg 0x90):

| Rail | Net | V | Powers | Safe to cut? |
|------|-----|---|--------|--------------|
| DCDC1 | VCC3V3 | 3.3 | **SoC, PSRAM/flash, IMU, SD** | NO — bricks |
| ALDO1 (0x90 b0) | A3V3 | 3.3 | **display VCI/VDDIO + ES8311/amp + touch analog** | NO — kills display |
| ALDO3 (0x90 b2) | — | 3.0 | vibration motor **supply** (left ON); buzz gated by **GPIO18** | supply on, GPIO switches |
| ALDO2/4, BLDO1/2, DLDO1/2 | — | — | no load (likely NC) | — |

**Audio has no dedicated rail** — it shares ALDO1 with the display, so it's gated by **GPIO46**, not the PMIC. **Vibration motor**: schematic net is `MOTOR/GPIO18` — a transistor switched by **GPIO18**, supplied by ALDO3. So ALDO3 stays ON (supply) and GPIO18 HIGH=buzz/LOW=off (`motor_buzz`, non-blocking via one-shot timer). Toggling ALDO3 alone does NOT vibrate — the GPIO is the switch. Schematic: `files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06/ESP32-S3-Touch-AMOLED-2.06.pdf`.

## Critical Gotchas

### Touch Controller (FT3168)
BSP touch init FAILS on this board. The working approach:
1. `# CONFIG_BSP_ERROR_CHECK is not set` in sdkconfig.defaults (prevents abort on BSP failure)
2. Let `bsp_display_start()` return NULL (touch failure)
3. Recover display: `lv_display_get_default()` + manual `bsp_display_brightness_init()`
4. GPIO9 reset pulse AFTER BSP: 10ms low → 100ms high recovery
5. Add FT3168 as I2C device at 0x38, read registers directly

### Light Sleep Breaks USB-JTAG Flashing (recovery via BOOT-hold)
Enabling automatic light sleep (`esp_pm_configure` with `light_sleep_enable=true`) **drops the USB-Serial-JTAG console during sleep**. The device then can't be reached by `idf.py flash`/esptool (`Failed to connect: No serial data received`) and looks like it "keeps resetting". The build currently keeps DFS but `light_sleep_enable=false` to avoid this. If you re-enable light sleep and brick the flash path:
1. **Unplug USB → hold BOOT (GPIO0) → replug while holding → release.** Resets with BOOT held into a stable ROM download mode (no sleep).
2. Flash straight to the waiting ROM with `--before no_reset`:
   `python -m esptool --chip esp32s3 -p COM9 -b 460800 --before no_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB 0x0 build/bootloader/bootloader.bin 0x10000 build/tennis_monitor.bin 0x8000 build/partition_table/partition-table.bin`
Only enable light sleep for battery-only/production builds (no USB connected), ideally paired with screen-off.

### WiFi Corrupts PSRAM LCD Framebuffer (white stripes)
The LCD framebuffer is in octal PSRAM; the SH8601 is fed over QSPI by DMA reading it. While the WiFi stack runs, its bus/cache activity corrupts the PSRAM framebuffer, leaving **white horizontal stripes** in *static* (never-redrawn) regions — buttons, bar background. These persist long after WiFi stops because only label regions self-heal on update. System stays stable (no crash). **Fix**: after `esp_wifi_stop()`+`esp_wifi_deinit()`, settle ~150ms then `bsp_display_lock` → `lv_obj_invalidate(lv_screen_active())` → unlock to force a full repaint over the corruption. Stripes may flash during the WiFi-active window but clear permanently after sync. (Verified by A/B test: `if (0 && sd_available)` to disable the WiFi task → stripes gone.)

### LVGL Float Formatting
`lv_label_set_text_fmt()` does NOT support `%f` (newlib nano). Use integer math:
```c
int val_i = (int)(float_val * 10);
lv_label_set_text_fmt(lbl, "%d.%d", val_i/10, abs(val_i)%10);
```

### FreeRTOS Tick Rate
Must set `CONFIG_FREERTOS_HZ=1000` for 500Hz IMU polling. Default 100Hz tick makes `pdMS_TO_TICKS(2)` return 0 → assert crash.

### Serial Monitor Resets Device
Opening COM9 with DTR/RTS enabled resets the ESP32-S3 USB Serial JTAG. Use:
```powershell
$port = New-Object System.IO.Ports.SerialPort("COM9", 115200)
$port.DtrEnable = $false
$port.RtsEnable = $false
$port.Open()
```

### sdkconfig.defaults Key Settings
```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_SPIRAM=y / CONFIG_SPIRAM_MODE_OCT=y / CONFIG_SPIRAM_SPEED_80M=y
CONFIG_FREERTOS_HZ=1000
# CONFIG_BSP_ERROR_CHECK is not set
CONFIG_LV_FONT_MONTSERRAT_20=y / 24=y / 28=y / 32=y / 36=y / 48=y
CONFIG_FATFS_LFN_HEAP=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_BSP_I2C_NUM=1
```

**Important**: After changing sdkconfig.defaults, delete `sdkconfig` to force regeneration.

## Common Tasks

```powershell
# Build
cd C:\ESPIDFprojects\ESP32_S3_TennisMonitoringWearableDevice
C:\Users\97254\esp\v5.5.2\esp-idf\export.ps1
idf.py build

# Flash
idf.py -p COM9 flash

# Serial monitor (no device reset)
$port = New-Object System.IO.Ports.SerialPort("COM9", 115200)
$port.DtrEnable = $false; $port.RtsEnable = $false
$port.Open()
while ($true) { if ($port.BytesToRead) { Write-Host $port.ReadExisting() -NoNewline }; Start-Sleep -Milliseconds 50 }

# Clean build (after sdkconfig.defaults changes)
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py fullclean
idf.py build
```

## Threshold Tuning Reference

| Environment | ω thresh | α thresh | Window | Notes |
|-------------|----------|----------|--------|-------|
| Floor bouncing (racquet) | 150 | 20,000 | 10 samp | 11/11 with lookback fix. Old same-sample check: 5/30 |
| Court play | 500 | 35,000 | 10 samp | **Validated** (replay of 2026-06-16 full-data sessions). ω=500 is the sweet spot (higher drops soft contacts). α lowered 40k→35k to catch smooth high-ω strokes whose alpha peaks ~37-39k (just under 40k); enlarging the alpha window doesn't help (those strokes never cross 40k). α=35k adds only 0-2 detections/session → safe. |

Offline analysis tool: `scripts/analyze_hits.py` faithfully replays `hit_detector.c` against recorded CSVs to evaluate thresholds (reproduces on-device hit counts exactly). Note the detector emits on the **falling** edge (omega<wT), so the CSV `hit=1` sample shows ~threshold omega, not the peak (peak is in the serial `HIT peak=` log).

**Idle noise floor**: ω~8-35 dps, α~0-4k
**Floor bounce peaks**: ω=140-430 dps, α=8-85k
**Marginal soft bounces**: ω=110-150, α=7-15k

## Development Phases

1. ~~Phase 1 — Hardware bringup~~ ✓ (display, touch, IMU, SD card)
2. ~~Phase 2 — Hit detection algorithm~~ ✓ (dual-gate, state machine, alpha lookback window, tuned)
3. ~~Home screen + mode navigation~~ ✓ (Home/Play/Test routing, BOOT=back, battery/clock)
4. ~~Config screen~~ ✓ (PWR→Config: handedness radio, on-demand NTP clock sync)
5. **Court data collection** ← CURRENT (user testing with delta instrumentation)
6. ~~Play mode Phase 1~~ ✓ (play/pause + 5-outcome tagging + session files)
   - Later phases: stats/summary screens, scoring logic, stroke classification
7. Data analysis & threshold refinement
8. Power/spin estimation
9. Scoring & feedback UI
10. Personalization from user feedback

## Reference Repositories

- Official examples: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06
- Smartwatch firmware: https://github.com/joaquimorg/OLEDS3Watch
- Rust firmware (pin ref): https://github.com/infinition/waveshare-watch-rs
- Waveshare ESP32 components: https://github.com/waveshareteam/Waveshare-ESP32-components
