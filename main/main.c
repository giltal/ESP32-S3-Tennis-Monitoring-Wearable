/**
 * Tennis Hit Detection — Testing & Tuning Program
 *
 * Architecture:
 *   - High-priority FreeRTOS task polls IMU at ~500 Hz, feeds hit_detector
 *   - LVGL timer updates display at ~20 Hz with live telemetry
 *   - Touch buttons adjust thresholds in real time
 *   - SD card logging: user-triggered via RECORD/STOP toggle
 *   - Two recording modes:
 *       HIT (default): ring buffer keeps last 3s; on hit, flushes -3s..+3s window
 *       ALL: continuous logging of every sample (original behavior)
 *   - Session counter persisted to SD card across reboots
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "qmi8658.h"
#include "hit_detector.h"

static const char *TAG = "tennis_test";

/* ── Hardware config ── */
#define TOUCH_RST_GPIO      GPIO_NUM_9
#define BOOT_BTN_GPIO       GPIO_NUM_0   /* BOOT button — used as "back" */
#define PWR_BTN_GPIO        GPIO_NUM_10  /* PWR button — Home → Config */
#define AUDIO_PA_GPIO       GPIO_NUM_46  /* ES8311/NS4150B PA_CTRL: HIGH=on */

/* Home screen-sleep: after timeout show only the clock, dimmed; touch wakes */
#define HOME_SLEEP_TIMEOUT_MS  30000
#define BRIGHT_FULL            80
#define BRIGHT_DIM             40

/* Play mode: center dial + ring of 5 tappable slices (screen center 205,251) */
#define PLAY_CX                205
#define PLAY_CY                251
#define PLAY_R_IN              74     /* center circle radius */
#define PLAY_R_OUT             195    /* outer ring radius */
#define PLAY_NUM_SLICES        6
#define PLAY_INACT_MS          (30u * 60u * 1000u)  /* 30 min no-activity end */
#define PLAY_BAT_CUTOFF        3      /* % → end session */

#define FW_VERSION             "0.7"  /* firmware revision (shown on Config) */
#define FT3168_ADDR         0x38
#define FT_REG_NUM_TOUCHES  0x02

/* AXP2101 PMIC */
#define AXP2101_ADDR        0x34
#define AXP_REG_FUEL_GAUGE  0x18
#define AXP_REG_BAT_PERCENT 0xA4
#define AXP_REG_LDO_EN0     0x90   /* LDO on/off (ALDO3 = bit 2 = vibration motor) */
#define AXP_REG_ALDO3_VOL   0x94   /* ALDO3 voltage: 0.5V + 0.1V*n */
#define MOTOR_ALDO3_BIT     2
#define MOTOR_BUZZ_MS       120    /* haptic pulse on a Play tag */

/* PCF85063A RTC */
#define PCF85063A_ADDR      0x51
#define PCF_REG_SECONDS     0x04
#define PCF_REG_MINUTES     0x05
#define PCF_REG_HOURS       0x06

/* WiFi credentials file */
#define WIFI_CRED_FILE      BSP_SD_MOUNT_POINT "/wifi.txt"

/* ── IMU / detection constants ── */
#define IMU_SAMPLE_HZ       470
#define DT                  (1.0f / IMU_SAMPLE_HZ)

/* Court-tuned from 5 recorded sessions: real hits 550-2600 dps, arm noise <500.
 * ω=500 α=40k cleanly separates hits from arm movements on court. */
#define OMEGA_THRESH_INIT   500.0f
#define ALPHA_THRESH_INIT   35000.0f   /* 40k→35k: catches smooth strokes (alpha ~37-39k), validated on court data */
#define REFRACTORY_MS_INIT  250

/* Threshold adjustment steps */
#define OMEGA_STEP          50.0f
#define ALPHA_STEP          5000.0f

/* Session counter persistence */
#define SESSION_COUNTER_FILE  BSP_SD_MOUNT_POINT "/ses_counter.txt"
#define CONFIG_FILE           BSP_SD_MOUNT_POINT "/config.txt"

/* ── Ring buffer for HIT-only recording ── */
#define RING_BUF_SECS       3
#define RING_BUF_CAP        (500 * RING_BUF_SECS + 100)  /* ~1600 samples */
#define POST_HIT_MS         3000                          /* capture 3s after hit */

typedef struct {
    uint32_t ms;
    int16_t  gx10, gy10, gz10;       /* gyro × 10 */
    int16_t  ax100, ay100, az100;     /* accel × 100 */
    int16_t  omega10;                 /* omega × 10 */
    int16_t  alpha_d100;              /* alpha / 100 */
    uint8_t  flags;                   /* bit0 = clip, bit1 = hit */
} ring_sample_t;

static ring_sample_t ring_buf[RING_BUF_CAP];
static int ring_head  = 0;       /* next write position */
static int ring_count = 0;       /* number of valid samples (≤ CAP) */
static bool capture_active = false;
static uint32_t capture_deadline_ms = 0;

/* ── Shared state (IMU task → display) ── */
static SemaphoreHandle_t data_mutex;

typedef struct {
    float    omega;
    float    alpha;
    hit_state_t state;
    uint32_t hit_count;
    float    last_peak;
    bool     last_clipped;
    float    omega_thresh;
    float    alpha_thresh;
    bool     clipped;
    uint32_t samples_per_sec;
} telemetry_t;

static volatile telemetry_t g_telem;

/* ── IMU + detector ── */
static qmi8658_handle_t imu;
static hit_detector_t detector;

/* ── Touch ── */
static i2c_master_dev_handle_t touch_i2c_dev = NULL;

/* ── PMIC + RTC ── */
static i2c_master_dev_handle_t axp_i2c_dev = NULL;
static i2c_master_dev_handle_t rtc_i2c_dev = NULL;
static int g_battery_percent = -1;
static int g_rtc_hour = -1, g_rtc_min = -1;
static int g_utc_offset = 2;       /* hours east of UTC (from wifi.txt UTC=) */
static bool g_summer_time = false; /* DST flag (from wifi.txt summerTime=) */

/* ── SD logging ── */
static FILE *log_file = NULL;
static bool logging_active = false;
static bool sd_available = false;
static bool hit_only_mode = true;    /* default: HIT mode */
static uint32_t log_sample_count = 0;
static int log_session_num = 0;

/* ── UI elements ── */
static lv_obj_t *lbl_session;
static lv_obj_t *lbl_hits;
static lv_obj_t *lbl_state;
static lv_obj_t *lbl_thresh;     /* omega threshold value (wT) */
static lv_obj_t *lbl_thresh_a;   /* alpha threshold value (aT) */
static lv_obj_t *lbl_status;

static lv_obj_t *lbl_time_bat;

static lv_obj_t *btn_omega_up, *btn_omega_dn;
static lv_obj_t *btn_alpha_up, *btn_alpha_dn;
static lv_obj_t *btn_rec, *btn_reset, *btn_mode;
static lv_obj_t *lbl_rec;    /* dynamic label inside REC button */
static lv_obj_t *lbl_mode;   /* dynamic label inside MODE button */

/* ── Screen routing ── */
typedef enum { SCREEN_MAIN, SCREEN_PLAY, SCREEN_TEST, SCREEN_CONFIG } app_screen_t;
static app_screen_t current_screen = SCREEN_MAIN;
static lv_obj_t *scr_main, *scr_play, *scr_test, *scr_config;
static lv_obj_t *lbl_main_time, *lbl_main_bat;          /* live values on home */
static lv_obj_t *main_btn_play, *main_btn_test;          /* home mode buttons */
/* Back-to-home is the physical BOOT button (GPIO0), not a touch target. */

/* ── Play mode ── */
typedef enum { PLAY_PAUSED, PLAY_RUNNING } play_state_t;
static play_state_t play_state = PLAY_PAUSED;
static lv_obj_t *play_lbl_state;                 /* center PLAY/PAUSE text */
static lv_obj_t *play_lbl_slice[PLAY_NUM_SLICES];/* per-slice name + count */
static lv_obj_t *play_lbl_bat, *play_lbl_time, *play_lbl_hits; /* top info row */
static uint32_t  play_counts[PLAY_NUM_SLICES];
static bool      play_session_active = false;
static bool      play_session_ended  = false;    /* end_play_session() guard */
static bool      play_files_open     = false;    /* folder+hits.csv created (first PLAY) */
static char      play_dir[80];                   /* /sdcard/PlaySession_... */
static FILE     *play_events = NULL;             /* timestamped outcome tags (events.csv) */
static volatile uint32_t play_last_activity_ms = 0;  /* hit / tap / PWR */
/* Slice order clockwise from the top (used in outcomes.txt) */
static const char *play_names[PLAY_NUM_SLICES] = {
    "Good hit", "Out", "Bad hit", "Unforced error", "First serve in", "Lost point" };

/* Forward decls used across the file */
static void nav_to(app_screen_t s);
static void imu_set_paused(bool pause);

/* ── Power management (Home = low-power; Test = full rate) ── */
static TaskHandle_t imu_handle = NULL;       /* 500Hz IMU task */
static volatile bool imu_paused_req = false; /* cooperative pause flag */
static lv_timer_t *tmr_update, *tmr_touch;   /* re-paced per screen */
static bool home_asleep = false;             /* Home dimmed clock-only mode */
static uint32_t home_activity_ms = 0;        /* last touch/wake on Home */

/* Config screen (PWR/GPIO10 from Home; BOOT/GPIO0 = back) */
static lv_obj_t *cfg_btn_right, *cfg_btn_left;   /* handedness radio */
static lv_obj_t *cfg_btn_ntp, *lbl_cfg_ntp;       /* NTP sync button + status */
static lv_obj_t *lbl_cfg_datetime;                /* live RTC date+time (verify) */
static bool g_left_handed = false;                /* persisted to config.txt */

/* On-demand NTP sync state (no longer auto-run at boot) */
static volatile bool s_ntp_busy = false;
static volatile int  s_ntp_status = 0;            /* 0 idle 1 syncing 2 ok 3 fail */
/* While WiFi runs it corrupts the PSRAM framebuffer; freeze UI redraws so the
 * panel holds the last clean frame instead of flushing corrupted pixels. */
static volatile bool g_display_freeze = false;

static void update_cfg_ntp_status(void);   /* config NTP status label */

/* On a screen switch, invalidate the whole new screen for a few refresh cycles
 * so BOTH double-buffers get a full render (else a partial update later flushes
 * a buffer still holding the previous screen → "dirty" artifacts). */
static volatile int g_redraw_frames = 0;

/* ── AXP2101: read battery percentage ── */
static int axp2101_read_battery_percent(void)
{
    if (!axp_i2c_dev) return -1;
    uint8_t reg = AXP_REG_BAT_PERCENT;
    uint8_t val = 0;
    esp_err_t ret = i2c_master_transmit_receive(axp_i2c_dev, &reg, 1, &val, 1, 100);
    if (ret != ESP_OK) return -1;
    if (val > 100) val = 100;
    return (int)val;
}

static void axp2101_enable_fuel_gauge(void)
{
    if (!axp_i2c_dev) return;
    uint8_t reg = AXP_REG_FUEL_GAUGE;
    uint8_t val = 0;
    i2c_master_transmit_receive(axp_i2c_dev, &reg, 1, &val, 1, 100);
    val |= (1 << 3);
    uint8_t cmd[2] = { AXP_REG_FUEL_GAUGE, val };
    i2c_master_transmit(axp_i2c_dev, cmd, 2, 100);
    ESP_LOGI(TAG, "AXP2101 fuel gauge enabled (reg 0x18 = 0x%02x)", val);
}

/* ── Vibration motor (AXP2101 ALDO3 rail — its own rail, safe to toggle) ── */
static esp_timer_handle_t motor_timer = NULL;

static void axp2101_set_ldo(uint8_t bit, bool on)
{
    if (!axp_i2c_dev) return;
    uint8_t reg = AXP_REG_LDO_EN0, val = 0;
    i2c_master_transmit_receive(axp_i2c_dev, &reg, 1, &val, 1, 100);
    if (on) val |= (1 << bit); else val &= ~(1 << bit);
    uint8_t cmd[2] = { AXP_REG_LDO_EN0, val };
    i2c_master_transmit(axp_i2c_dev, cmd, 2, 100);
}

static void motor_off_cb(void *arg) { axp2101_set_ldo(MOTOR_ALDO3_BIT, false); }

static void motor_init(void)
{
    if (axp_i2c_dev) {                       /* ALDO3 = 3.0V (0.5 + 0.1*25) */
        uint8_t cmd[2] = { AXP_REG_ALDO3_VOL, 25 };
        i2c_master_transmit(axp_i2c_dev, cmd, 2, 100);
    }
    axp2101_set_ldo(MOTOR_ALDO3_BIT, false); /* ensure off at boot */
    const esp_timer_create_args_t a = { .callback = motor_off_cb, .name = "motor" };
    esp_timer_create(&a, &motor_timer);
}

/* Non-blocking haptic pulse: motor on now, one-shot timer turns it off. */
static void motor_buzz(uint32_t ms)
{
    if (!motor_timer) return;
    axp2101_set_ldo(MOTOR_ALDO3_BIT, true);
    esp_timer_stop(motor_timer);
    esp_timer_start_once(motor_timer, (uint64_t)ms * 1000);
}

/* ── PCF85063A: read/write time ── */
static inline uint8_t bcd2bin(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static inline uint8_t bin2bcd(uint8_t bin) { return ((bin / 10) << 4) | (bin % 10); }

static void pcf85063a_read_time(int *hour, int *minute)
{
    if (!rtc_i2c_dev) { *hour = -1; *minute = -1; return; }
    uint8_t reg = PCF_REG_SECONDS;
    uint8_t buf[3];
    esp_err_t ret = i2c_master_transmit_receive(rtc_i2c_dev, &reg, 1, buf, 3, 100);
    if (ret != ESP_OK) { *hour = -1; *minute = -1; return; }
    *minute = bcd2bin(buf[1] & 0x7F);
    *hour   = bcd2bin(buf[2] & 0x3F);
}

static void pcf85063a_write_time(int hour, int minute, int second)
{
    if (!rtc_i2c_dev) return;
    uint8_t cmd[4] = {
        PCF_REG_SECONDS,
        bin2bcd(second) & 0x7F,
        bin2bcd(minute),
        bin2bcd(hour),
    };
    i2c_master_transmit(rtc_i2c_dev, cmd, 4, 100);
    ESP_LOGI(TAG, "RTC set to %02d:%02d:%02d", hour, minute, second);
}

/* Full date+time (regs 0x04..0x0A: sec,min,hr,day,wday,month,year) */
static void pcf85063a_write_datetime(const struct tm *t)
{
    if (!rtc_i2c_dev) return;
    uint8_t cmd[8] = {
        PCF_REG_SECONDS,
        bin2bcd(t->tm_sec) & 0x7F,
        bin2bcd(t->tm_min),
        bin2bcd(t->tm_hour),
        bin2bcd(t->tm_mday),
        bin2bcd(t->tm_wday),
        bin2bcd(t->tm_mon + 1),
        bin2bcd((uint8_t)((t->tm_year + 1900) % 100)),
    };
    i2c_master_transmit(rtc_i2c_dev, cmd, 8, 100);
}

static void pcf85063a_read_date(int *year, int *month, int *day)
{
    *year = -1; *month = 1; *day = 1;
    if (!rtc_i2c_dev) return;
    uint8_t reg = PCF_REG_SECONDS;
    uint8_t b[7];
    if (i2c_master_transmit_receive(rtc_i2c_dev, &reg, 1, b, 7, 100) != ESP_OK) return;
    *day   = bcd2bin(b[3] & 0x3F);
    *month = bcd2bin(b[5] & 0x1F);
    *year  = 2000 + bcd2bin(b[6]);
}

/* Full date+time in one read (for the Config screen verification display) */
static bool pcf85063a_read_full(int *yr, int *mo, int *da, int *hh, int *mi, int *ss)
{
    if (!rtc_i2c_dev) return false;
    uint8_t reg = PCF_REG_SECONDS;
    uint8_t b[7];
    if (i2c_master_transmit_receive(rtc_i2c_dev, &reg, 1, b, 7, 100) != ESP_OK) return false;
    *ss = bcd2bin(b[0] & 0x7F);
    *mi = bcd2bin(b[1] & 0x7F);
    *hh = bcd2bin(b[2] & 0x3F);
    *da = bcd2bin(b[3] & 0x3F);
    *mo = bcd2bin(b[5] & 0x1F);
    *yr = 2000 + bcd2bin(b[6]);
    return true;
}

/* Seed the ESP system clock from the RTC so FATFS file timestamps match the
 * real date/time (FATFS stamps via time()/localtime, not the RTC chip).
 * RTC holds local wall-clock; using TZ=UTC0 makes localtime() reproduce those
 * exact values, so file attributes show the same date/time as the RTC. */
static void sync_system_time_from_rtc(void)
{
    int yr, mo, da, hh, mi, ss;
    if (!pcf85063a_read_full(&yr, &mo, &da, &hh, &mi, &ss)) return;
    if (yr < 2020) {                 /* RTC not set yet (no NTP) — leave default */
        ESP_LOGW(TAG, "RTC date unset (%04d) — file timestamps stay at default", yr);
        return;
    }
    setenv("TZ", "UTC0", 1);
    tzset();
    struct tm t = {0};
    t.tm_year = yr - 1900; t.tm_mon = mo - 1; t.tm_mday = da;
    t.tm_hour = hh; t.tm_min = mi; t.tm_sec = ss; t.tm_isdst = 0;
    struct timeval tv = { .tv_sec = mktime(&t), .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "System clock seeded from RTC: %04d-%02d-%02d %02d:%02d:%02d",
             yr, mo, da, hh, mi, ss);
}

/* ── WiFi + NTP → RTC sync ── */
static SemaphoreHandle_t s_wifi_connected;
static bool s_ntp_synced = false;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi connected, got IP");
        xSemaphoreGive(s_wifi_connected);
    }
}

static void ntp_sync_callback(struct timeval *tv)
{
    ESP_LOGI(TAG, "NTP time synced");
    s_ntp_synced = true;
}

static bool read_wifi_credentials(char *ssid, size_t ssid_len,
                                  char *pass, size_t pass_len)
{
    FILE *f = fopen(WIFI_CRED_FILE, "r");
    if (!f) {
        ESP_LOGW(TAG, "No wifi.txt on SD card");
        return false;
    }
    ssid[0] = pass[0] = '\0';
    if (!fgets(ssid, ssid_len, f)) { fclose(f); return false; }
    fgets(pass, pass_len, f);

    /* Optional line 3: UTC=<offset>, line 4: summerTime=<true|false> */
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "UTC=%d", &v) == 1 || sscanf(line, "utc=%d", &v) == 1) {
            g_utc_offset = v;
        } else if (strstr(line, "summerTime") || strstr(line, "summertime")) {
            g_summer_time = (strstr(line, "true") || strstr(line, "True") ||
                             strstr(line, "1")) ? true : false;
        }
    }
    fclose(f);

    /* Strip newlines/CR */
    char *p;
    if ((p = strchr(ssid, '\r'))) *p = '\0';
    if ((p = strchr(ssid, '\n'))) *p = '\0';
    if ((p = strchr(pass, '\r'))) *p = '\0';
    if ((p = strchr(pass, '\n'))) *p = '\0';

    if (strlen(ssid) == 0) return false;
    ESP_LOGI(TAG, "WiFi SSID: %s  UTC=%d  summerTime=%d",
             ssid, g_utc_offset, g_summer_time);
    return true;
}

static void wifi_ntp_sync_task(void *arg)
{
    /* Re-runnable: one-time stack setup is guarded so it persists across syncs */
    static bool netif_ready = false;

    /* Paint a clean "Syncing…" frame and flush it NOW (under lock), before any
     * WiFi radio activity can corrupt the framebuffer. Redraws stay frozen
     * (g_display_freeze) for the rest of the sync. */
    bsp_display_lock(0);
    update_cfg_ntp_status();
    lv_refr_now(NULL);
    bsp_display_unlock();

    char ssid[64], pass[64];
    if (!read_wifi_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "No WiFi credentials — skipping NTP sync");
        s_ntp_status = 3;          /* fail */
        g_display_freeze = false;
        bsp_display_lock(0);
        lv_obj_invalidate(lv_screen_active());
        bsp_display_unlock();
        s_ntp_busy = false;
        vTaskDelete(NULL);
        return;
    }

    /* Fresh sync: clear prior result and drain any stale connect signal */
    s_ntp_synced = false;
    while (xSemaphoreTake(s_wifi_connected, 0) == pdTRUE) { }

    /* NVS (required by WiFi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Network interface + event loop + default STA netif — create once only */
    if (!netif_ready) {
        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();
        esp_event_handler_instance_t inst_any, inst_ip;
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_event_handler, NULL, &inst_any);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            wifi_event_handler, NULL, &inst_ip);
        netif_ready = true;
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_cfg);

    wifi_config_t sta_cfg = { 0 };
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "Connecting to WiFi...");
    if (xSemaphoreTake(s_wifi_connected, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "WiFi connect timeout — shutting down");
        s_ntp_status = 3;          /* fail */
        goto cleanup;
    }

    /* SNTP sync */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(ntp_sync_callback);
    esp_sntp_init();

    /* Wait for NTP (up to 10s) */
    for (int i = 0; i < 50 && !s_ntp_synced; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (s_ntp_synced) {
        /* Build POSIX TZ from wifi.txt: effective east offset = UTC + DST.
         * POSIX inverts the sign (east of UTC is negative in the TZ string). */
        int eff = g_utc_offset + (g_summer_time ? 1 : 0);
        char tz[16];
        snprintf(tz, sizeof(tz), "UTC%+d", -eff);
        setenv("TZ", tz, 1);
        tzset();
        ESP_LOGI(TAG, "Timezone: %s (UTC+%d, DST=%d)", tz, eff, g_summer_time);
        time_t now;
        time(&now);
        struct tm t;
        localtime_r(&now, &t);
        ESP_LOGI(TAG, "NTP %04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);

        /* Write full date+time to RTC (date needed for Play session folders) */
        pcf85063a_write_datetime(&t);
        s_ntp_status = 2;          /* ok */
    } else {
        ESP_LOGW(TAG, "NTP sync timeout");
        s_ntp_status = 3;          /* fail */
    }

    esp_sntp_stop();

cleanup:
    esp_wifi_stop();
    esp_wifi_deinit();
    ESP_LOGI(TAG, "WiFi disabled");

    /*
     * WiFi disturbs the single PSRAM LCD draw buffer; in-place repaints don't
     * reliably recover it. Since the RTC (battery-backed) now holds the correct
     * time, a clean reboot is the bulletproof way to get a pristine UI.
     */
    if (s_ntp_status == 2) {
        bsp_display_lock(0);
        if (lbl_cfg_ntp) {
            lv_label_set_text(lbl_cfg_ntp, LV_SYMBOL_OK " Synced — restarting…");
            lv_obj_set_style_text_color(lbl_cfg_ntp, lv_color_make(0, 200, 80), 0);
        }
        lv_refr_now(NULL);
        bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(900));
        esp_restart();             /* RTC retains time; UI rebuilds clean */
    }

    /* Sync failed — no clock change. Unfreeze and repaint so the user sees it. */
    vTaskDelay(pdMS_TO_TICKS(150));
    g_display_freeze = false;
    bsp_display_lock(0);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    bsp_display_unlock();

    s_ntp_busy = false;
    vTaskDelete(NULL);
}

/* Launch a one-shot NTP sync (from the Config screen). Ignored if already busy.
 * Freezes UI redraws up front; the sync task paints a clean "Syncing…" frame and
 * unfreezes + repaints when done, so WiFi never flushes corrupted pixels. */
static void start_ntp_sync(void)
{
    if (s_ntp_busy) return;
    s_ntp_busy = true;
    s_ntp_status = 1;              /* syncing */
    g_display_freeze = true;
    xTaskCreate(wifi_ntp_sync_task, "ntp", 8192, NULL, 3, NULL);
}

/* ── Touch reading ── */
static esp_err_t ft3168_read_touch(int *num_points, int *x, int *y)
{
    uint8_t reg = FT_REG_NUM_TOUCHES;
    uint8_t buf[7];
    esp_err_t ret = i2c_master_transmit_receive(touch_i2c_dev, &reg, 1, buf, 7, 100);
    if (ret != ESP_OK) return ret;
    *num_points = buf[0] & 0x0F;
    if (*num_points > 0) {
        *x = ((buf[1] & 0x0F) << 8) | buf[2];
        *y = ((buf[3] & 0x0F) << 8) | buf[4];
    }
    return ESP_OK;
}

/* ── Session counter persistence ── */
static void load_session_counter(void)
{
    FILE *f = fopen(SESSION_COUNTER_FILE, "r");
    if (f) {
        int n = 0;
        if (fscanf(f, "%d", &n) == 1 && n > 0)
            log_session_num = n;
        fclose(f);
        ESP_LOGI(TAG, "Loaded session counter: %d", log_session_num);
    } else {
        DIR *dir = opendir(BSP_SD_MOUNT_POINT);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                int n;
                if (sscanf(entry->d_name, "ses_%d.csv", &n) == 1)
                    if (n > log_session_num) log_session_num = n;
            }
            closedir(dir);
        }
        ESP_LOGI(TAG, "Scanned files, highest session: %d", log_session_num);
    }
}

static void save_session_counter(void)
{
    FILE *f = fopen(SESSION_COUNTER_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", log_session_num);
        fclose(f);
    }
}

/* ── Config persistence (handedness, etc.) ── */
static void load_config(void)
{
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "left_handed=%d", &v) == 1)
            g_left_handed = (v != 0);
    }
    fclose(f);
    ESP_LOGI(TAG, "Config loaded: left_handed=%d", g_left_handed);
}

static void save_config(void)
{
    if (!sd_available) return;
    FILE *f = fopen(CONFIG_FILE, "w");
    if (f) {
        fprintf(f, "left_handed=%d\n", g_left_handed ? 1 : 0);
        fclose(f);
        ESP_LOGI(TAG, "Config saved: left_handed=%d", g_left_handed);
    }
}

/* ── CSV writing helpers ── */
static void write_csv_header(FILE *fp)
{
    fprintf(fp, "session,ms,gx,gy,gz,ax,ay,az,omega,alpha,clip,hit\n");
    fflush(fp);
}

static void write_sample_to_csv(FILE *fp, const ring_sample_t *s, int session)
{
    fprintf(fp, "%d,%lu,%d,%d,%d,%d,%d,%d,%d.%d,%d,%d,%d\n",
            session,
            (unsigned long)s->ms,
            (int)s->gx10, (int)s->gy10, (int)s->gz10,
            (int)s->ax100, (int)s->ay100, (int)s->az100,
            s->omega10 / 10, abs(s->omega10) % 10,
            (int)s->alpha_d100,
            (s->flags & 0x01) ? 1 : 0,
            (s->flags & 0x02) ? 1 : 0);
}

static void flush_ring_to_csv(FILE *fp, int session)
{
    /* Write ring buffer contents in chronological order */
    if (ring_count == 0) return;
    int start;
    if (ring_count < RING_BUF_CAP)
        start = 0;
    else
        start = ring_head;  /* oldest sample */

    for (int i = 0; i < ring_count; i++) {
        int idx = (start + i) % RING_BUF_CAP;
        write_sample_to_csv(fp, &ring_buf[idx], session);
    }
    log_sample_count += ring_count;
    ESP_LOGI(TAG, "Flushed %d ring samples (pre-hit)", ring_count);
}

/* ── Button visual updates ── */
static void update_rec_button_visual(void)
{
    bsp_display_lock(0);
    if (logging_active) {
        lv_obj_set_style_bg_color(btn_rec, lv_color_make(80, 80, 80), 0);
        lv_label_set_text(lbl_rec, "STOP");
    } else {
        lv_obj_set_style_bg_color(btn_rec, lv_color_make(200, 30, 30), 0);
        lv_label_set_text(lbl_rec, "REC");
    }
    bsp_display_unlock();
}

static void update_mode_button_visual(void)
{
    bsp_display_lock(0);
    if (hit_only_mode) {
        lv_obj_set_style_bg_color(btn_mode, lv_color_make(200, 140, 0), 0);
        lv_label_set_text(lbl_mode, "HIT");
    } else {
        lv_obj_set_style_bg_color(btn_mode, lv_color_make(30, 100, 180), 0);
        lv_label_set_text(lbl_mode, "ALL");
    }
    bsp_display_unlock();
}

/* ── SD logging ── */
static void start_logging(void)
{
    if (logging_active) return;
    if (!sd_available) {
        ESP_LOGW(TAG, "No SD card");
        return;
    }

    log_session_num++;
    save_session_counter();

    char path[64];
    snprintf(path, sizeof(path), "%s/ses_%03d_%s.csv",
             BSP_SD_MOUNT_POINT, log_session_num,
             hit_only_mode ? "hit" : "full");   /* mode in the filename */
    log_file = fopen(path, "w");
    if (!log_file) {
        ESP_LOGE(TAG, "Cant open %s: %s", path, strerror(errno));
        log_session_num--;
        save_session_counter();
        return;
    }
    write_csv_header(log_file);
    logging_active = true;
    log_sample_count = 0;
    capture_active = false;
    capture_deadline_ms = 0;

    hit_detector_reset(&detector);

    ESP_LOGI(TAG, "REC #%03d [%s mode] → %s",
             log_session_num, hit_only_mode ? "HIT" : "ALL", path);
    update_rec_button_visual();
}

static void stop_logging(void)
{
    if (!logging_active) return;
    logging_active = false;
    capture_active = false;
    if (log_file) { fclose(log_file); log_file = NULL; }
    ESP_LOGI(TAG, "STOP #%03d (%lu samples)",
             log_session_num, (unsigned long)log_sample_count);
    update_rec_button_visual();
}

/* ── Ring buffer push (always, regardless of recording state) ── */
static inline void ring_push(const ring_sample_t *s)
{
    ring_buf[ring_head] = *s;
    ring_head = (ring_head + 1) % RING_BUF_CAP;
    if (ring_count < RING_BUF_CAP) ring_count++;
}

/* ── IMU polling task (500 Hz) ── */
static void imu_task(void *arg)
{
    qmi8658_data_t d;
    hit_event_t evt;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t sec_samples = 0, sec_counter = 0;
    TickType_t last_sec_tick = xTaskGetTickCount();
    static float sec_peak_omega = 0, sec_peak_alpha = 0;

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(2));

        /* Cooperative pause: self-suspend OUTSIDE any I2C transaction so we
         * never hold the shared bus mutex while suspended (would deadlock the
         * UI's touch/RTC/battery reads). Used to idle the IMU on Home. */
        if (imu_paused_req) {
            vTaskSuspend(NULL);                 /* blocks until vTaskResume */
            last_wake = xTaskGetTickCount();    /* reset cadence after resume */
            continue;
        }

        if (qmi8658_read_data(&imu, &d) != ESP_OK) continue;

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool hit = hit_detector_process(&detector,
                        d.gyro.x, d.gyro.y, d.gyro.z,
                        now_ms, d.gyro_clipped, &evt);

        if (hit) {
            if (play_session_active) play_last_activity_ms = now_ms;  /* activity */
            ESP_LOGI(TAG, "HIT #%lu peak=%d clip=%d t=%lu d=%lums",
                     (unsigned long)detector.hit_count,
                     (int)evt.peak_omega, evt.clipped ? 1 : 0,
                     (unsigned long)evt.timestamp_ms,
                     (unsigned long)detector.last_hit_delta_ms);
        }

        /* Per-second stats */
        if (detector.cur_omega > sec_peak_omega) sec_peak_omega = detector.cur_omega;
        if (detector.cur_alpha > sec_peak_alpha) sec_peak_alpha = detector.cur_alpha;
        sec_samples++;
        if ((xTaskGetTickCount() - last_sec_tick) >= pdMS_TO_TICKS(1000)) {
            sec_counter = sec_samples;
            ESP_LOGI(TAG, "w=%d wpk=%d a=%dk apk=%dk st=%d hits=%lu  [%lu Hz]",
                     (int)detector.cur_omega, (int)sec_peak_omega,
                     (int)(detector.cur_alpha / 1000), (int)(sec_peak_alpha / 1000),
                     (int)detector.state, (unsigned long)detector.hit_count,
                     (unsigned long)sec_counter);
            sec_peak_omega = 0; sec_peak_alpha = 0;
            sec_samples = 0;
            last_sec_tick = xTaskGetTickCount();
        }

        /* Build compact sample for ring buffer + CSV */
        ring_sample_t samp = {
            .ms       = now_ms,
            .gx10     = (int16_t)(d.gyro.x * 10),
            .gy10     = (int16_t)(d.gyro.y * 10),
            .gz10     = (int16_t)(d.gyro.z * 10),
            .ax100    = (int16_t)(d.acc.x * 100),
            .ay100    = (int16_t)(d.acc.y * 100),
            .az100    = (int16_t)(d.acc.z * 100),
            .omega10  = (int16_t)(detector.cur_omega * 10),
            .alpha_d100 = (int16_t)(detector.cur_alpha / 100),
            .flags    = (uint8_t)((d.gyro_clipped ? 0x01 : 0) |
                                  (hit ? 0x02 : 0)),
        };

        /* Always push to ring buffer (needed for HIT mode pre-capture) */
        ring_push(&samp);

        /* ── SD logging ── */
        if (logging_active && log_file) {
            if (!hit_only_mode) {
                /* ALL mode: write every sample */
                write_sample_to_csv(log_file, &samp, log_session_num);
                log_sample_count++;
                if ((log_sample_count % 500) == 0) fflush(log_file);
            } else {
                /* HIT mode: capture windows around hits */
                if (hit) {
                    if (!capture_active) {
                        /* First hit in this window — flush ring buffer
                         * (contains the pre-hit context) */
                        flush_ring_to_csv(log_file, log_session_num);
                        capture_active = true;
                    }
                    /* Extend (or set) the post-hit deadline */
                    capture_deadline_ms = now_ms + POST_HIT_MS;
                }

                if (capture_active) {
                    /* Write the current sample (post-hit live data) */
                    write_sample_to_csv(log_file, &samp, log_session_num);
                    log_sample_count++;

                    /* Check if post-hit window expired */
                    if (now_ms > capture_deadline_ms) {
                        capture_active = false;
                        fflush(log_file);
                        ESP_LOGI(TAG, "Capture window closed (%lu samples total)",
                                 (unsigned long)log_sample_count);
                    } else if ((log_sample_count % 500) == 0) {
                        fflush(log_file);
                    }
                }
            }
        }

        /* Telemetry */
        if (xSemaphoreTake(data_mutex, 0) == pdTRUE) {
            telemetry_t *t = (telemetry_t *)&g_telem;
            t->omega = detector.cur_omega;
            t->alpha = detector.cur_alpha;
            t->state = detector.state;
            t->hit_count = detector.hit_count;
            t->last_peak = detector.last_hit.peak_omega;
            t->last_clipped = detector.last_hit.clipped;
            t->omega_thresh = detector.omega_thresh;
            t->alpha_thresh = detector.alpha_thresh;
            t->clipped = d.gyro_clipped;
            t->samples_per_sec = sec_counter;
            xSemaphoreGive(data_mutex);
        }
    }
}

/* Battery icon + percentage, colored by charge level. pct<0 = unknown. */
static void set_battery_label(lv_obj_t *lbl, int pct)
{
    if (!lbl) return;
    if (pct < 0) {
        lv_label_set_text(lbl, LV_SYMBOL_BATTERY_EMPTY " --%");
        lv_obj_set_style_text_color(lbl, lv_color_make(120, 120, 120), 0);
        return;
    }
    const char *icon = pct >= 80 ? LV_SYMBOL_BATTERY_FULL
                     : pct >= 60 ? LV_SYMBOL_BATTERY_3
                     : pct >= 40 ? LV_SYMBOL_BATTERY_2
                     : pct >= 20 ? LV_SYMBOL_BATTERY_1
                     :             LV_SYMBOL_BATTERY_EMPTY;
    lv_color_t col = pct >= 50 ? lv_color_make(0, 200, 80)
                   : pct >= 20 ? lv_color_make(255, 180, 0)
                   :             lv_color_make(255, 60, 60);
    lv_label_set_text_fmt(lbl, "%s %d%%", icon, pct);
    lv_obj_set_style_text_color(lbl, col, 0);
}

/* Home screen: live clock + battery (cheap, runs every tick) */
static void update_main_screen(void)
{
    if (lbl_main_time) {
        if (g_rtc_hour >= 0)
            lv_label_set_text_fmt(lbl_main_time, "%02d:%02d", g_rtc_hour, g_rtc_min);
        else
            lv_label_set_text(lbl_main_time, "--:--");
    }
    set_battery_label(lbl_main_bat, g_battery_percent);
}

static void update_cfg_ntp_status(void);   /* defined in the touch section */

/* Home screen-sleep: show only the clock at reduced brightness (AMOLED → the
 * hidden buttons/battery go black = ~no power). Touch wakes to the full Home. */
static void home_sleep(void)
{
    if (home_asleep) return;
    if (lbl_main_bat)  lv_obj_add_flag(lbl_main_bat,  LV_OBJ_FLAG_HIDDEN);
    if (main_btn_play) lv_obj_add_flag(main_btn_play, LV_OBJ_FLAG_HIDDEN);
    if (main_btn_test) lv_obj_add_flag(main_btn_test, LV_OBJ_FLAG_HIDDEN);
    bsp_display_brightness_set(BRIGHT_DIM);
    home_asleep = true;
    g_redraw_frames = 3;                 /* clear the now-hidden button areas */
}

static void home_wake(void)
{
    if (lbl_main_bat)  lv_obj_remove_flag(lbl_main_bat,  LV_OBJ_FLAG_HIDDEN);
    if (main_btn_play) lv_obj_remove_flag(main_btn_play, LV_OBJ_FLAG_HIDDEN);
    if (main_btn_test) lv_obj_remove_flag(main_btn_test, LV_OBJ_FLAG_HIDDEN);
    bsp_display_brightness_set(BRIGHT_FULL);
    home_asleep = false;
    home_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    g_redraw_frames = 3;
}

/* ── Play mode helpers ── */
static uint32_t now_ms_u32(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void play_set_slice_label(int i)
{
    if (i >= 0 && i < PLAY_NUM_SLICES && play_lbl_slice[i])
        lv_label_set_text_fmt(play_lbl_slice[i], "%lu", (unsigned long)play_counts[i]);
}

static void play_refresh_state_label(void)
{
    if (!play_lbl_state) return;
    bool run = (play_state == PLAY_RUNNING);
    lv_label_set_text(play_lbl_state, run ? "PLAY" : "PAUSE");
    lv_obj_set_style_text_color(play_lbl_state,
        run ? lv_color_make(0, 200, 80) : lv_color_make(190, 190, 190), 0);
}

/* Enter Play mode: reset state, start PAUSED. No disk work and no session-counter
 * change here — the folder + hits.csv are created lazily on the first PLAY, so
 * just opening and leaving Play creates nothing. */
static void start_play_session(void)
{
    for (int i = 0; i < PLAY_NUM_SLICES; i++) { play_counts[i] = 0; play_set_slice_label(i); }
    play_state = PLAY_PAUSED;
    play_session_ended = false;
    play_files_open = false;
    play_dir[0] = '\0';
    log_file = NULL;
    play_events = NULL;
    logging_active = false;
    hit_only_mode = true;          /* HIT-mode capture (same as Test recording) */
    hit_detector_reset(&detector);
    play_last_activity_ms = now_ms_u32();
    play_refresh_state_label();
    play_session_active = true;    /* logical session; files appear on first PLAY */
}

/* Create the session folder + File A on first PLAY (does NOT touch the Test
 * session counter — Play files live in their own datetime-named folder). */
static void play_open_files(void)
{
    if (play_files_open || !sd_available) return;
    int yr, mo, da;
    pcf85063a_read_date(&yr, &mo, &da);
    if (yr < 2000) { yr = 2000; mo = 1; da = 1; }   /* RTC date unset (no NTP yet) */
    int hh = (g_rtc_hour >= 0) ? g_rtc_hour : 0;
    int mm = (g_rtc_min  >= 0) ? g_rtc_min  : 0;
    snprintf(play_dir, sizeof(play_dir), "%s/PlaySession_%04d-%02d-%02d_%02d-%02d",
             BSP_SD_MOUNT_POINT, yr, mo, da, hh, mm);
    mkdir(play_dir, 0775);

    char path[128];
    snprintf(path, sizeof(path), "%s/hits.csv", play_dir);
    log_file = fopen(path, "w");
    if (!log_file) {
        ESP_LOGE(TAG, "Play: cant open %s: %s", path, strerror(errno));
        return;
    }
    write_csv_header(log_file);
    log_sample_count = 0;
    capture_active = false;
    capture_deadline_ms = 0;

    /* events.csv — each tagged outcome with its timestamp (for scoring) */
    snprintf(path, sizeof(path), "%s/events.csv", play_dir);
    play_events = fopen(path, "w");
    if (play_events) { fprintf(play_events, "ms,outcome\n"); fflush(play_events); }

    play_files_open = true;
    ESP_LOGI(TAG, "Play files opened → %s", play_dir);
}

/* Single finalize path for all 3 end triggers; guarded to run once. */
static void end_play_session(void)
{
    if (!play_session_active || play_session_ended) return;
    play_session_ended = true;
    play_state = PLAY_PAUSED;
    logging_active = false;
    capture_active = false;

    /* Only finalize files if the user actually played (files were opened).
     * Entering Play and leaving without pressing PLAY writes nothing. */
    if (play_files_open) {
        /* File B — outcome counters, written once at end */
        if (sd_available && play_dir[0]) {
            char path[128];
            snprintf(path, sizeof(path), "%s/outcomes.txt", play_dir);
            FILE *fb = fopen(path, "w");
            if (fb) {
                for (int i = 0; i < PLAY_NUM_SLICES; i++)
                    fprintf(fb, "%s=%lu\n", play_names[i], (unsigned long)play_counts[i]);
                fprintf(fb, "Total hits=%lu\n", (unsigned long)detector.hit_count);
                fprintf(fb, "hand=%s\n", g_left_handed ? "left" : "right");
                fflush(fb);
                fclose(fb);
            }
        }
        /* File A — flush + close cleanly */
        if (log_file) { fflush(log_file); fclose(log_file); log_file = NULL; }
        if (play_events) { fflush(play_events); fclose(play_events); play_events = NULL; }
        ESP_LOGI(TAG, "Play session saved → %s", play_dir);
    }
    play_files_open = false;
    play_session_active = false;
}

/* Live top-row info on the Play screen */
static void update_play_screen(void)
{
    if (play_lbl_time) {
        if (g_rtc_hour >= 0)
            lv_label_set_text_fmt(play_lbl_time, "%02d:%02d", g_rtc_hour, g_rtc_min);
        else
            lv_label_set_text(play_lbl_time, "--:--");
    }
    set_battery_label(play_lbl_bat, g_battery_percent);
    if (play_lbl_hits)
        lv_label_set_text_fmt(play_lbl_hits, "Hits %lu",
                              (unsigned long)detector.hit_count);
}

/* PWR/GPIO10 in Play = toggle PLAY/PAUSE (gates IMU + File A append) */
static void play_toggle(void)
{
    if (!play_session_active) return;
    play_last_activity_ms = now_ms_u32();
    if (play_state == PLAY_RUNNING) {
        play_state = PLAY_PAUSED;
        logging_active = false;
        imu_set_paused(true);
    } else {
        play_open_files();                 /* create folder + hits.csv on 1st PLAY */
        play_state = PLAY_RUNNING;
        logging_active = (log_file != NULL);
        imu_set_paused(false);
    }
    play_refresh_state_label();
}

/* ── Display update (~20 Hz) ── */
static void update_display_cb(lv_timer_t *timer)
{
    /* Frozen during WiFi sync — no invalidations, panel holds the clean frame */
    if (g_display_freeze) return;

    /* Play mode: battery + inactivity end triggers run regardless of pause */
    if (current_screen == SCREEN_PLAY && play_session_active && !play_session_ended) {
        uint32_t now = now_ms_u32();
        if ((g_battery_percent >= 0 && g_battery_percent <= PLAY_BAT_CUTOFF) ||
            (now - play_last_activity_ms >= PLAY_INACT_MS)) {
            end_play_session();
            nav_to(SCREEN_MAIN);
            return;
        }
        update_play_screen();      /* live battery / clock / session hit count */
    }

    /* Home inactivity → dim to clock-only after the timeout */
    if (current_screen == SCREEN_MAIN && !home_asleep) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - home_activity_ms >= HOME_SLEEP_TIMEOUT_MS)
            home_sleep();
    }

    /* After a screen switch, force a full redraw for a few cycles (both buffers) */
    if (g_redraw_frames > 0) {
        lv_obj_invalidate(lv_screen_active());
        g_redraw_frames--;
    }

    /* Home-screen values come from globals, not telemetry — always refresh */
    update_main_screen();
    if (current_screen == SCREEN_CONFIG) {
        update_cfg_ntp_status();
        int yr, mo, da, hh, mi, ss;
        if (lbl_cfg_datetime && pcf85063a_read_full(&yr, &mo, &da, &hh, &mi, &ss))
            lv_label_set_text_fmt(lbl_cfg_datetime, "%04d-%02d-%02d\n%02d:%02d:%02d",
                                  yr, mo, da, hh, mi, ss);
    }

    telemetry_t snap;
    if (xSemaphoreTake(data_mutex, 0) == pdTRUE) {
        memcpy(&snap, (void *)&g_telem, sizeof(snap));
        xSemaphoreGive(data_mutex);
    } else return;

    /* Time + battery (top line) */
    if (g_rtc_hour >= 0 && g_battery_percent >= 0)
        lv_label_set_text_fmt(lbl_time_bat, "%02d:%02d  %d%%",
                              g_rtc_hour, g_rtc_min, g_battery_percent);
    else if (g_rtc_hour >= 0)
        lv_label_set_text_fmt(lbl_time_bat, "%02d:%02d", g_rtc_hour, g_rtc_min);
    else if (g_battery_percent >= 0)
        lv_label_set_text_fmt(lbl_time_bat, "%d%%", g_battery_percent);

    /* Session number */
    if (log_session_num > 0)
        lv_label_set_text_fmt(lbl_session, "#%03d", log_session_num);
    else
        lv_label_set_text(lbl_session, "---");

    /* Big hit counter */
    lv_label_set_text_fmt(lbl_hits, "%lu", (unsigned long)snap.hit_count);

    /* State — HIT! flash */
    if (snap.state == HIT_STATE_IN_HIT) {
        lv_obj_set_style_text_color(lbl_hits, lv_color_make(255, 50, 50), 0);
        lv_label_set_text(lbl_state, "HIT!");
        lv_obj_set_style_text_color(lbl_state, lv_color_make(255, 50, 50), 0);
    } else if (snap.state == HIT_STATE_REFRACTORY) {
        lv_obj_set_style_text_color(lbl_hits, lv_color_white(), 0);
        lv_label_set_text(lbl_state, "...");
        lv_obj_set_style_text_color(lbl_state, lv_color_make(120, 120, 120), 0);
    } else {
        lv_obj_set_style_text_color(lbl_hits, lv_color_white(), 0);
        lv_label_set_text(lbl_state, "");
    }

    /* Threshold values (shown between the −/+ buttons) */
    lv_label_set_text_fmt(lbl_thresh,   "wT %d",  (int)snap.omega_thresh);
    lv_label_set_text_fmt(lbl_thresh_a, "aT %dk", (int)(snap.alpha_thresh / 1000));

    /* Status line */
    if (logging_active) {
        int k = (int)(log_sample_count / 1000);
        if (hit_only_mode && capture_active) {
            lv_label_set_text_fmt(lbl_status, "CAP #%03d %dk %luHz",
                                  log_session_num, k,
                                  (unsigned long)snap.samples_per_sec);
            lv_obj_set_style_text_color(lbl_status, lv_color_make(255, 200, 0), 0);
        } else if (logging_active) {
            lv_label_set_text_fmt(lbl_status, "REC #%03d %dk %luHz",
                                  log_session_num, k,
                                  (unsigned long)snap.samples_per_sec);
            lv_obj_set_style_text_color(lbl_status, lv_color_make(255, 80, 80), 0);
        }
    } else {
        lv_label_set_text_fmt(lbl_status, "%luHz",
                              (unsigned long)snap.samples_per_sec);
        lv_obj_set_style_text_color(lbl_status, lv_color_make(100, 100, 100), 0);
    }
}

/* ── Touch button handling ── */
typedef enum {
    BTN_NONE = 0,
    BTN_OMEGA_UP, BTN_OMEGA_DN,
    BTN_ALPHA_UP, BTN_ALPHA_DN,
    BTN_REC, BTN_RESET, BTN_MODE,
    BTN_PLAY, BTN_TEST, BTN_BACK,   /* navigation */
} button_id_t;

typedef struct { lv_obj_t *obj; button_id_t id; } touch_button_t;

#define NUM_BUTTONS 7               /* test controls (back is the BOOT button) */
static touch_button_t buttons[NUM_BUTTONS];

/* Point-in-object hit test (absolute display coords) */
static bool obj_hit(lv_obj_t *obj, int tx, int ty)
{
    if (!obj) return false;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return (tx >= a.x1 && tx <= a.x2 && ty >= a.y1 && ty <= a.y2);
}

static button_id_t check_button_hit(int tx, int ty)
{
    for (int i = 0; i < NUM_BUTTONS; i++) {
        if (obj_hit(buttons[i].obj, tx, ty))
            return buttons[i].id;
    }
    return BTN_NONE;
}

/* Pause/resume the 500Hz IMU task (cooperative, suspends at a safe point). */
static void imu_set_paused(bool pause)
{
    if (!imu_handle) return;
    if (pause) {
        imu_paused_req = true;              /* task self-suspends next loop */
    } else {
        imu_paused_req = false;
        vTaskResume(imu_handle);
    }
}

/* Per-screen power policy: full rate only on Test (live hit detection + UI);
 * everywhere else idle the IMU and poll controls at 4Hz / clock at ~1Hz. */
static void apply_screen_power(app_screen_t s)
{
    bool full = (s == SCREEN_TEST);
    bool interactive = (s == SCREEN_TEST || s == SCREEN_PLAY);
    imu_set_paused(!full);                       /* Play IMU is gated by play_state */
    /* Interactive screens need responsive touch; idle screens save power */
    if (tmr_touch)  lv_timer_set_period(tmr_touch, interactive ? 30 : 250);
    if (tmr_update) lv_timer_set_period(tmr_update,
                        full ? 50 : (s == SCREEN_PLAY ? 200 : 1000));

    /* Entering Home → start awake + restart the inactivity timer.
     * Any other screen → ensure full brightness (clears a dimmed Home). */
    if (s == SCREEN_MAIN) {
        home_wake();
    } else {
        home_asleep = false;
        bsp_display_brightness_set(BRIGHT_FULL);
    }
}

/* Switch the active screen (called from the LVGL timer context). */
static void nav_to(app_screen_t s)
{
    current_screen = s;
    lv_obj_t *t = (s == SCREEN_PLAY)   ? scr_play
                : (s == SCREEN_TEST)   ? scr_test
                : (s == SCREEN_CONFIG) ? scr_config
                : scr_main;
    lv_screen_load(t);
    g_redraw_frames = 3;          /* full-redraw both buffers over next frames */
    apply_screen_power(s);        /* idle IMU / slow polling off the Test screen */
    if (s == SCREEN_PLAY)
        start_play_session();     /* open folder + File A, start PAUSED */
}

/* Highlight the selected handedness radio option */
static void update_hand_radio(void)
{
    lv_color_t on  = lv_color_make(0, 150, 200);
    lv_color_t off = lv_color_make(45, 45, 55);
    if (cfg_btn_right)
        lv_obj_set_style_bg_color(cfg_btn_right, g_left_handed ? off : on, 0);
    if (cfg_btn_left)
        lv_obj_set_style_bg_color(cfg_btn_left,  g_left_handed ? on : off, 0);
}

/* Reflect on-demand NTP sync state on the config screen */
static void update_cfg_ntp_status(void)
{
    if (!lbl_cfg_ntp) return;
    switch (s_ntp_status) {
    case 1:
        lv_label_set_text(lbl_cfg_ntp, "Syncing… (WiFi)");
        lv_obj_set_style_text_color(lbl_cfg_ntp, lv_color_make(255, 200, 0), 0);
        break;
    case 2:
        lv_label_set_text(lbl_cfg_ntp, LV_SYMBOL_OK " Clock synced");
        lv_obj_set_style_text_color(lbl_cfg_ntp, lv_color_make(0, 200, 80), 0);
        break;
    case 3:
        lv_label_set_text(lbl_cfg_ntp, LV_SYMBOL_WARNING " Sync failed");
        lv_obj_set_style_text_color(lbl_cfg_ntp, lv_color_make(255, 80, 80), 0);
        break;
    default:
        lv_label_set_text(lbl_cfg_ntp, "");
        break;
    }
}

/* BOOT button = "back". From Test, stop+save any recording first. */
static void handle_back(void)
{
    switch (current_screen) {
    case SCREEN_TEST:
        if (logging_active) {
            stop_logging();   /* closes & flushes the CSV before leaving */
            ESP_LOGI(TAG, "Recording stopped & saved (BOOT back)");
        }
        nav_to(SCREEN_MAIN);
        break;
    case SCREEN_PLAY:
        end_play_session();   /* trigger #1: back to main finalizes the session */
        nav_to(SCREEN_MAIN);
        break;
    case SCREEN_CONFIG:
        save_config();    /* persist handedness on exit */
        nav_to(SCREEN_MAIN);
        break;
    case SCREEN_MAIN:
    default:
        break;            /* already home */
    }
}

static void touch_poll_cb(lv_timer_t *timer)
{
    static bool was_pressed = false;

    /* Ignore all input while the display is frozen for a WiFi sync */
    if (g_display_freeze) return;

    /* Buttons start "down" so a press held during power-on (the user turning
     * the watch on with PWR) isn't mistaken for a fresh press — wait for release. */
    static bool boot_down = true;
    int boot_lvl = gpio_get_level(BOOT_BTN_GPIO);
    if (boot_lvl == 0 && !boot_down) {
        boot_down = true;
        handle_back();
    } else if (boot_lvl != 0) {
        boot_down = false;
    }

    /* PWR button (GPIO10) = Home → Config */
    static bool pwr_down = true;
    int pwr_lvl = gpio_get_level(PWR_BTN_GPIO);
    if (pwr_lvl == 0 && !pwr_down) {
        pwr_down = true;
        if (current_screen == SCREEN_MAIN)
            nav_to(SCREEN_CONFIG);
        else if (current_screen == SCREEN_PLAY)
            play_toggle();              /* PWR toggles PLAY/PAUSE in Play mode */
    } else if (pwr_lvl != 0) {
        pwr_down = false;
    }

    if (!touch_i2c_dev) return;
    int npts = 0, tx = 0, ty = 0;
    if (ft3168_read_touch(&npts, &tx, &ty) != ESP_OK) return;

    if (npts > 0 && !was_pressed) {
        was_pressed = true;

        /* Resolve which control was tapped, scoped to the active screen.
         * Play has no touch targets (BOOT button is its only control). */
        button_id_t btn = BTN_NONE;
        switch (current_screen) {
        case SCREEN_MAIN:
            home_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (home_asleep) {
                home_wake();             /* first touch only wakes, no action */
            } else {
                if (obj_hit(main_btn_play, tx, ty))      btn = BTN_PLAY;
                else if (obj_hit(main_btn_test, tx, ty)) btn = BTN_TEST;
            }
            break;
        case SCREEN_PLAY: {
            /* Angular hit-test of the 5-slice ring (atan2 of the touch vector) */
            int dx = tx - PLAY_CX, dy = ty - PLAY_CY;
            int d2 = dx * dx + dy * dy;
            if (d2 > PLAY_R_IN * PLAY_R_IN && d2 <= PLAY_R_OUT * PLAY_R_OUT) {
                play_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
                float ang = atan2f((float)dy, (float)dx) * 57.29578f;   /* deg */
                if (ang < 0.0f) ang += 360.0f;                         /* [0,360) */
                /* 6 slices of 60°, slice 0 centered at top (270°) */
                int sl = (((int)ang + 120) % 360) / 60;
                if (sl < 0) sl = 0;
                if (sl > 5) sl = 5;
                if (play_state == PLAY_RUNNING) {        /* count only while PLAY */
                    play_counts[sl]++;
                    play_set_slice_label(sl);
                    motor_buzz(MOTOR_BUZZ_MS);           /* haptic feedback on tag */
                    if (play_events) {                   /* timestamped tag → scoring */
                        fprintf(play_events, "%lu,%s\n",
                                (unsigned long)(esp_timer_get_time() / 1000),
                                play_names[sl]);
                        fflush(play_events);
                    }
                }
            }
            break;
        }
        case SCREEN_CONFIG:
            if (obj_hit(cfg_btn_right, tx, ty)) {
                g_left_handed = false; save_config(); update_hand_radio();
            } else if (obj_hit(cfg_btn_left, tx, ty)) {
                g_left_handed = true;  save_config(); update_hand_radio();
            } else if (obj_hit(cfg_btn_ntp, tx, ty)) {
                start_ntp_sync();
            }
            break;
        case SCREEN_TEST:
            btn = check_button_hit(tx, ty);
            break;
        }

        switch (btn) {
        case BTN_PLAY:  nav_to(SCREEN_PLAY); break;
        case BTN_TEST:  nav_to(SCREEN_TEST); break;
        case BTN_OMEGA_UP:
            hit_detector_set_omega_thresh(&detector,
                detector.omega_thresh + OMEGA_STEP); break;
        case BTN_OMEGA_DN:
            if (detector.omega_thresh > OMEGA_STEP)
                hit_detector_set_omega_thresh(&detector,
                    detector.omega_thresh - OMEGA_STEP); break;
        case BTN_ALPHA_UP:
            hit_detector_set_alpha_thresh(&detector,
                detector.alpha_thresh + ALPHA_STEP); break;
        case BTN_ALPHA_DN:
            if (detector.alpha_thresh > ALPHA_STEP)
                hit_detector_set_alpha_thresh(&detector,
                    detector.alpha_thresh - ALPHA_STEP); break;
        case BTN_REC:
            if (logging_active) stop_logging(); else start_logging();
            break;
        case BTN_RESET:
            hit_detector_reset(&detector); break;
        case BTN_MODE:
            if (!logging_active) {
                hit_only_mode = !hit_only_mode;
                update_mode_button_visual();
                ESP_LOGI(TAG, "Mode → %s", hit_only_mode ? "HIT" : "ALL");
            } else {
                ESP_LOGW(TAG, "Stop recording before changing mode");
            }
            break;
        default: break;
        }
    } else if (npts == 0) {
        was_pressed = false;
    }
}

/* ── Slow peripherals timer (RTC + battery, every 5s) ── */
static void slow_peripherals_cb(lv_timer_t *timer)
{
    pcf85063a_read_time(&g_rtc_hour, &g_rtc_min);
    g_battery_percent = axp2101_read_battery_percent();
}

/* ── Helper: button with stored label ref ── */
static lv_obj_t *make_btn_ex(lv_obj_t *parent, const char *text,
                              int x, int y, int w, int h,
                              lv_color_t bg, const lv_font_t *font,
                              lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_center(lbl);

    if (out_label) *out_label = lbl;
    return btn;
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text,
                           int x, int y, int w, int h, lv_color_t bg)
{
    return make_btn_ex(parent, text, x, y, w, h, bg,
                       &lv_font_montserrat_24, NULL);
}

/* ── Shared screen base styling ── */
static void style_screen(lv_obj_t *scr)
{
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_outline_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

/*
 * Home screen (410 × 502, top → bottom):
 *   battery       top-center, icon + %, colored by level
 *   [  PLAY  ]    large button, upper half  → Play (placeholder)
 *   14:32         dominant clock, centered between the buttons
 *   [  TEST  ]    large button, lower half  → existing Test & Tune
 */
static void create_main_screen(void)
{
    scr_main = lv_obj_create(NULL);
    style_screen(scr_main);

    lbl_main_bat = lv_label_create(scr_main);
    lv_obj_set_style_text_font(lbl_main_bat, &lv_font_montserrat_28, 0);
    set_battery_label(lbl_main_bat, -1);
    lv_obj_align(lbl_main_bat, LV_ALIGN_TOP_MID, 0, 12);

    main_btn_play = make_btn_ex(scr_main, LV_SYMBOL_PLAY "  PLAY",
                                20, 55, 370, 165,
                                lv_color_make(0, 150, 70),
                                &lv_font_montserrat_36, NULL);

    lbl_main_time = lv_label_create(scr_main);
    lv_obj_set_style_text_color(lbl_main_time, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_main_time, &lv_font_montserrat_48, 0);
    lv_label_set_text(lbl_main_time, "--:--");
    lv_obj_align(lbl_main_time, LV_ALIGN_CENTER, 0, 0);

    main_btn_test = make_btn_ex(scr_main, LV_SYMBOL_SETTINGS "  TEST",
                                20, 295, 370, 165,
                                lv_color_make(30, 90, 170),
                                &lv_font_montserrat_36, NULL);
}

/* Play screen: center PLAY/PAUSE dial + ring of 5 colored, tappable slices.
 * Slices map clockwise from the top: Good hit, Out, Unforced, First serve,
 * Lost pt. Tap = increment (PLAY only); PWR = play/pause; BOOT = back. */
static void create_play_screen(void)
{
    scr_play = lv_obj_create(NULL);
    style_screen(scr_play);

    /* 6 slices of 60°, clockwise from top. LVGL arc 0°=3 o'clock, CW.
     * Slice 0 centered at top → bg from 240° spanning 60° each. */
    const int   starts[PLAY_NUM_SLICES] = { 240, 300, 0, 60, 120, 180 };
    const lv_color_t cols[PLAY_NUM_SLICES] = {
        lv_color_make(29, 158, 85),    /* 1 Good hit       — green  */
        lv_color_make(225, 200, 20),   /* 2 Out            — yellow */
        lv_color_make(235, 120, 20),   /* 3 Bad hit        — orange */
        lv_color_make(200, 45, 45),    /* 4 Unforced error — red    */
        lv_color_make(45, 106, 216),   /* 5 First serve in — blue   */
        lv_color_make(225, 225, 225) };/* 6 Lost point     — white  */
    const char *icons[PLAY_NUM_SLICES] = {
        LV_SYMBOL_OK, LV_SYMBOL_UP, LV_SYMBOL_WARNING,
        LV_SYMBOL_CLOSE, LV_SYMBOL_PLAY, LV_SYMBOL_DOWN };
    /* label offsets from screen center at each slice mid-angle (radius ~116) */
    const int lx[PLAY_NUM_SLICES] = {   0, 100, 100,   0, -100, -100 };
    const int ly[PLAY_NUM_SLICES] = { -116, -58,  58, 116,   58,  -58 };

    /* Colored ring segments (lv_arc background arc = the slice) */
    for (int i = 0; i < PLAY_NUM_SLICES; i++) {
        lv_obj_t *a = lv_arc_create(scr_play);
        lv_obj_set_size(a, PLAY_R_OUT * 2, PLAY_R_OUT * 2);
        lv_obj_align(a, LV_ALIGN_CENTER, 0, 0);
        lv_arc_set_rotation(a, 0);
        lv_arc_set_bg_angles(a, starts[i], starts[i] + 60);
        lv_arc_set_angles(a, starts[i], starts[i]);          /* no indicator */
        lv_obj_set_style_arc_color(a, cols[i], LV_PART_MAIN);
        lv_obj_set_style_arc_width(a, PLAY_R_OUT - PLAY_R_IN, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);  /* straight radial edges */
        lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Slice labels: icon (top) + large live count (below). White-band slice
     * (Lost point) uses dark text for contrast. */
    for (int i = 0; i < PLAY_NUM_SLICES; i++) {
        lv_color_t fg = (i == 5) ? lv_color_make(30, 30, 30) : lv_color_white();

        lv_obj_t *ic = lv_label_create(scr_play);
        lv_obj_set_style_text_color(ic, fg, 0);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_36, 0);
        lv_label_set_text(ic, icons[i]);
        lv_obj_align(ic, LV_ALIGN_CENTER, lx[i], ly[i] - 18);

        lv_obj_t *cnt = lv_label_create(scr_play);
        lv_obj_set_style_text_color(cnt, fg, 0);
        lv_obj_set_style_text_font(cnt, &lv_font_montserrat_36, 0);
        lv_obj_set_style_text_align(cnt, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(cnt, "0");
        lv_obj_align(cnt, LV_ALIGN_CENTER, lx[i], ly[i] + 18);
        play_lbl_slice[i] = cnt;     /* the count is what updates live */
    }

    /* Center dial */
    lv_obj_t *circ = lv_obj_create(scr_play);
    lv_obj_remove_style_all(circ);
    lv_obj_set_size(circ, PLAY_R_IN * 2, PLAY_R_IN * 2);
    lv_obj_align(circ, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(circ, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circ, lv_color_make(22, 22, 22), 0);
    lv_obj_set_style_bg_opa(circ, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(circ, lv_color_make(70, 70, 70), 0);
    lv_obj_set_style_border_width(circ, 2, 0);
    lv_obj_remove_flag(circ, LV_OBJ_FLAG_SCROLLABLE);

    play_lbl_state = lv_label_create(circ);
    lv_obj_set_style_text_font(play_lbl_state, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(play_lbl_state, lv_color_make(190, 190, 190), 0);
    lv_label_set_text(play_lbl_state, "PAUSE");
    lv_obj_center(play_lbl_state);

    lv_obj_t *hint = lv_label_create(scr_play);
    lv_obj_set_style_text_color(hint, lv_color_make(110, 110, 110), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
    lv_label_set_text(hint, "BOOT = back    PWR = play/pause");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    /* Top info row: battery (left) · session hits (center) · clock (right),
     * inset from the rounded corners */
    play_lbl_bat = lv_label_create(scr_play);
    lv_obj_set_style_text_font(play_lbl_bat, &lv_font_montserrat_20, 0);
    set_battery_label(play_lbl_bat, -1);
    lv_obj_align(play_lbl_bat, LV_ALIGN_TOP_LEFT, 45, 12);

    play_lbl_hits = lv_label_create(scr_play);
    lv_obj_set_style_text_color(play_lbl_hits, lv_color_make(0, 200, 255), 0);
    lv_obj_set_style_text_font(play_lbl_hits, &lv_font_montserrat_24, 0);
    lv_label_set_text(play_lbl_hits, "Hits 0");
    lv_obj_align(play_lbl_hits, LV_ALIGN_TOP_MID, 0, 8);

    play_lbl_time = lv_label_create(scr_play);
    lv_obj_set_style_text_color(play_lbl_time, lv_color_make(190, 190, 190), 0);
    lv_obj_set_style_text_font(play_lbl_time, &lv_font_montserrat_20, 0);
    lv_label_set_text(play_lbl_time, "--:--");
    lv_obj_align(play_lbl_time, LV_ALIGN_TOP_RIGHT, -45, 12);
}

/*
 * Config screen (reached via PWR/GPIO10 from Home; BOOT/GPIO0 = back):
 *   Settings title
 *   Player hand:  [ RIGHT ] [ LEFT ]   (radio; selected = cyan)
 *   [  Sync Clock (NTP)  ]             (on-demand WiFi → NTP → RTC)
 *   <ntp status>
 *   BOOT = back hint
 */
static void create_config_screen(void)
{
    scr_config = lv_obj_create(NULL);
    style_screen(scr_config);

    lv_obj_t *title = lv_label_create(scr_config);
    lv_obj_set_style_text_color(title, lv_color_make(0, 200, 255), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_36, 0);
    lv_label_set_text(title, "Settings");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *hand = lv_label_create(scr_config);
    lv_obj_set_style_text_color(hand, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(hand, &lv_font_montserrat_28, 0);
    lv_label_set_text(hand, "Player hand:");
    lv_obj_set_pos(hand, 20, 95);

    /* Handedness radio — two tappable option boxes (custom hit-tested) */
    cfg_btn_right = make_btn_ex(scr_config, "RIGHT", 20, 135, 175, 64,
                                lv_color_make(45, 45, 55),
                                &lv_font_montserrat_28, NULL);
    cfg_btn_left  = make_btn_ex(scr_config, "LEFT", 215, 135, 175, 64,
                                lv_color_make(45, 45, 55),
                                &lv_font_montserrat_28, NULL);
    update_hand_radio();

    /* On-demand NTP clock sync */
    cfg_btn_ntp = make_btn_ex(scr_config, LV_SYMBOL_REFRESH "  Sync Clock (NTP)",
                              20, 235, 370, 80,
                              lv_color_make(30, 90, 170),
                              &lv_font_montserrat_28, NULL);

    lbl_cfg_ntp = lv_label_create(scr_config);
    lv_obj_set_style_text_font(lbl_cfg_ntp, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl_cfg_ntp, "");
    lv_obj_align(lbl_cfg_ntp, LV_ALIGN_TOP_MID, 0, 330);

    /* Live RTC date + time (to verify the clock/date are correct) */
    lbl_cfg_datetime = lv_label_create(scr_config);
    lv_obj_set_style_text_color(lbl_cfg_datetime, lv_color_make(0, 200, 255), 0);
    lv_obj_set_style_text_font(lbl_cfg_datetime, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(lbl_cfg_datetime, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_cfg_datetime, "----:--:--\n--:--:--");
    lv_obj_align(lbl_cfg_datetime, LV_ALIGN_TOP_MID, 0, 372);

    lv_obj_t *hint = lv_label_create(scr_config);
    lv_obj_set_style_text_color(hint, lv_color_make(110, 110, 110), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_24, 0);
    lv_label_set_text(hint, LV_SYMBOL_LEFT "  BOOT = back");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -30);

    /* Firmware version — top-right, inset off the rounded corner */
    lv_obj_t *ver = lv_label_create(scr_config);
    lv_obj_set_style_text_color(ver, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_20, 0);
    lv_label_set_text(ver, "v" FW_VERSION);
    lv_obj_align(ver, LV_ALIGN_TOP_RIGHT, -45, 22);
}

/* ── Test & Tune screen (existing hit-detection UI, unchanged logic) ── */
static void create_test_screen(void)
{
    scr_test = lv_obj_create(NULL);
    style_screen(scr_test);
    lv_obj_t *scr = scr_test;

    /*
     * Screen: 410 × 502 (redesigned)
     *
     *  #006                 14:32 85%   session (m32) + clock (m28), enlarged
     *
     *            12   HIT!               hit counter (m48), lowered & centered
     *
     *  [ − ]      wT 500       [ + ]     omega control: −/+ flank the value
     *  [ − ]      aT 40k       [ + ]     alpha control
     *  [ ======== RESET ======== ]       wide reset, own row
     *  [ HIT ] [ ====== REC ====== ]     mode + record (unchanged)
     *  500Hz                             status
     */

    int LM = 10;

    /* ── Top row: Session (left) + Time/Battery (right), both enlarged ── */
    lbl_session = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_session, lv_color_make(0, 200, 255), 0);
    lv_obj_set_style_text_font(lbl_session, &lv_font_montserrat_32, 0);
    if (log_session_num > 0)
        lv_label_set_text_fmt(lbl_session, "#%03d", log_session_num);
    else
        lv_label_set_text(lbl_session, "---");
    /* Pulled inward from the rounded top corners */
    lv_obj_set_pos(lbl_session, 70, 8);

    lbl_time_bat = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_time_bat, lv_color_make(190, 190, 190), 0);
    lv_obj_set_style_text_font(lbl_time_bat, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_time_bat, "");
    lv_obj_align(lbl_time_bat, LV_ALIGN_TOP_RIGHT, -55, 12);

    /* ── Hit counter (lowered, centered, large) + HIT! state ── */
    lbl_hits = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_hits, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_hits, &lv_font_montserrat_48, 0);
    lv_label_set_text(lbl_hits, "0");
    lv_obj_align(lbl_hits, LV_ALIGN_TOP_MID, 0, 96);

    lbl_state = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_state, lv_color_make(255, 50, 50), 0);
    lv_obj_set_style_text_font(lbl_state, &lv_font_montserrat_36, 0);
    lv_label_set_text(lbl_state, "");
    lv_obj_set_pos(lbl_state, 250, 104);

    /* ── Threshold controls: [ − ]  value  [ + ] ── */
    lv_color_t cbl = lv_color_make(40, 50, 100);   /* −/+ buttons */
    lv_color_t cbu = lv_color_make(30, 80, 160);    /* reset */
    int sbw = 84, sbh = 62, rx = 410 - 10 - 84;     /* side-button w/h, right x */

    btn_omega_dn = make_btn_ex(scr, LV_SYMBOL_MINUS, LM, 196, sbw, sbh, cbl, &lv_font_montserrat_36, NULL);
    btn_omega_up = make_btn_ex(scr, LV_SYMBOL_PLUS,  rx, 196, sbw, sbh, cbl, &lv_font_montserrat_36, NULL);
    lbl_thresh = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_thresh, lv_color_make(0, 255, 128), 0);
    lv_obj_set_style_text_font(lbl_thresh, &lv_font_montserrat_32, 0);
    lv_label_set_text_fmt(lbl_thresh, "wT %d", (int)OMEGA_THRESH_INIT);
    lv_obj_align(lbl_thresh, LV_ALIGN_TOP_MID, 0, 211);

    btn_alpha_dn = make_btn_ex(scr, LV_SYMBOL_MINUS, LM, 268, sbw, sbh, cbl, &lv_font_montserrat_36, NULL);
    btn_alpha_up = make_btn_ex(scr, LV_SYMBOL_PLUS,  rx, 268, sbw, sbh, cbl, &lv_font_montserrat_36, NULL);
    lbl_thresh_a = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_thresh_a, lv_color_make(0, 255, 128), 0);
    lv_obj_set_style_text_font(lbl_thresh_a, &lv_font_montserrat_32, 0);
    lv_label_set_text_fmt(lbl_thresh_a, "aT %dk", (int)(ALPHA_THRESH_INIT / 1000));
    lv_obj_align(lbl_thresh_a, LV_ALIGN_TOP_MID, 0, 283);

    /* ── Wide RESET on its own row ── */
    btn_reset = make_btn_ex(scr, "RESET", LM, 340, 390, 52, cbu, &lv_font_montserrat_28, NULL);

    /* ── MODE toggle + REC/STOP (unchanged) ── */
    lv_color_t mode_color = lv_color_make(200, 140, 0);
    btn_mode = make_btn_ex(scr, "HIT", LM, 402, 120, 68,
                           mode_color, &lv_font_montserrat_28, &lbl_mode);
    lv_color_t rec_color = lv_color_make(200, 30, 30);
    btn_rec = make_btn_ex(scr, "REC", LM + 130, 402, 260, 68,
                          rec_color, &lv_font_montserrat_32, &lbl_rec);

    /* Register all touch buttons (test screen) */
    buttons[0] = (touch_button_t){btn_omega_up, BTN_OMEGA_UP};
    buttons[1] = (touch_button_t){btn_omega_dn, BTN_OMEGA_DN};
    buttons[2] = (touch_button_t){btn_alpha_up, BTN_ALPHA_UP};
    buttons[3] = (touch_button_t){btn_alpha_dn, BTN_ALPHA_DN};
    buttons[4] = (touch_button_t){btn_rec, BTN_REC};
    buttons[5] = (touch_button_t){btn_reset, BTN_RESET};
    buttons[6] = (touch_button_t){btn_mode, BTN_MODE};

    /* ── Status line ── */
    lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_status, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl_status, "---Hz");
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 476);
}

/* ── Build all screens, land on Home, start shared timers ── */
static void create_ui(void)
{
    bsp_display_lock(0);

    create_main_screen();
    create_play_screen();
    create_config_screen();
    create_test_screen();

    /* Land on the home screen at startup (not Test, not auto-recording) */
    current_screen = SCREEN_MAIN;
    lv_screen_load(scr_main);

    /* Timers are shared across all screens (re-paced per screen for power) */
    tmr_update = lv_timer_create(update_display_cb, 50, NULL);
    tmr_touch  = lv_timer_create(touch_poll_cb, 80, NULL);
    lv_timer_create(slow_peripherals_cb, 5000, NULL);

    /* Immediate first read so home shows real values ASAP */
    slow_peripherals_cb(NULL);

    bsp_display_unlock();
}

/* ── SD card init ── */
static bool init_sd(void)
{
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "SD card mounted");
    load_session_counter();
    load_config();
    return true;
}

/* ── app_main ── */
void app_main(void)
{
    ESP_LOGI(TAG, "=== Tennis Hit Detection — Test & Tune ===");
    data_mutex = xSemaphoreCreateMutex();

    /* Power management: dynamic frequency scaling (160↔40MHz) when idle.
     * DFS keeps the SoC always-on (USB-JTAG stays reachable for flashing).
     *
     * NOTE: automatic light_sleep_enable=true gives deeper savings but the
     * USB-Serial-JTAG console drops during light sleep, making the device
     * unflashable over USB without manually forcing download mode (hold BOOT).
     * Re-enable it for battery-only / production builds (pair with Stage 3
     * screen-off), where USB isn't connected. */
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = false,
    };
    esp_pm_configure(&pm_cfg);

    /* Display (let BSP touch fail) */
    bsp_i2c_init();
    lv_display_t *disp = bsp_display_start();
    if (!disp) disp = lv_display_get_default();
    if (!disp) { ESP_LOGE(TAG, "No display!"); return; }
    bsp_display_brightness_init();
    bsp_display_brightness_set(80);

    /* Touch */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(TOUCH_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* BOOT (GPIO0, back) + PWR (GPIO10, Home→Config) as inputs, active low */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BTN_GPIO) | (1ULL << PWR_BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_cfg);

    /* Audio not used → hold the speaker amp (PA_CTRL/GPIO46) in shutdown.
     * On this board the ES8311/NS4150B share ALDO1 with the display, so there's
     * no audio rail to cut — this GPIO is the only safe gate. */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << AUDIO_PA_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(AUDIO_PA_GPIO, 0);

    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    i2c_device_config_t touch_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FT3168_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_bus_add_device(i2c_bus, &touch_cfg, &touch_i2c_dev);

    /* AXP2101 PMIC */
    i2c_device_config_t axp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_bus_add_device(i2c_bus, &axp_cfg, &axp_i2c_dev);
    axp2101_enable_fuel_gauge();
    motor_init();

    /* PCF85063A RTC */
    i2c_device_config_t rtc_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063A_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_bus_add_device(i2c_bus, &rtc_cfg, &rtc_i2c_dev);

    /* Seed system clock from RTC → correct FATFS file timestamps */
    sync_system_time_from_rtc();

    /* IMU: 16G, 2048dps, 500Hz, no LPF */
    qmi8658_config_t imu_cfg = {
        .acc_scale  = QMI8658_ACC_SCALE_16G,
        .acc_odr    = QMI8658_ACC_ODR_500,
        .gyro_scale = QMI8658_GYRO_SCALE_2048DPS,
        .gyro_odr   = QMI8658_GYRO_ODR_500,
    };
    ESP_ERROR_CHECK(qmi8658_init(&imu, i2c_bus, &imu_cfg));

    /* Hit detector */
    hit_detector_init(&detector, DT,
                      OMEGA_THRESH_INIT, ALPHA_THRESH_INIT,
                      REFRACTORY_MS_INIT);
    detector.alpha_window = 10;  /* ~21ms at 470Hz — bridges alpha/omega peak gap */

    /* SD card — mount but start IDLE */
    sd_available = init_sd();
    if (sd_available)
        ESP_LOGI(TAG, "SD ready — HIT mode — press REC to record (next=#%03d)",
                 log_session_num + 1);
    else
        ESP_LOGW(TAG, "No SD — recording disabled");

    /* WiFi/NTP clock sync is now ON-DEMAND from the Config screen
     * (Home → PWR button → "Sync Clock (NTP)"), not auto-run at boot.
     * The connect semaphore is created up-front so the sync task can run. */
    s_wifi_connected = xSemaphoreCreateBinary();

    /* UI */
    create_ui();

    /* IMU task on core 1 */
    xTaskCreatePinnedToCore(imu_task, "imu", 8192, NULL,
                            configMAX_PRIORITIES - 2, &imu_handle, 1);

    /* We boot on Home → apply the low-power policy (idle IMU, 4Hz polling) */
    bsp_display_lock(0);
    apply_screen_power(current_screen);
    bsp_display_unlock();

    ESP_LOGI(TAG, "Running IDLE [HIT mode] — wT=%d aT=%dk",
             (int)OMEGA_THRESH_INIT, (int)(ALPHA_THRESH_INIT/1000));
}
