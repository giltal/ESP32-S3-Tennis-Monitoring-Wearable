#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Hit detection state machine per HIT_DETECTION_SPEC.md
 * Pure logic — no I/O, no globals, all thresholds injected.
 */

typedef enum {
    HIT_STATE_IDLE = 0,
    HIT_STATE_IN_HIT,
    HIT_STATE_REFRACTORY,
} hit_state_t;

typedef struct {
    uint32_t timestamp_ms;    // time of peak omega
    float    peak_omega;      // peak gyro magnitude (dps)
    bool     clipped;         // any sample in the hit was clipped
} hit_event_t;

typedef struct {
    /* Tunable thresholds */
    float    omega_thresh;         // dps — magnitude gate
    float    alpha_thresh;         // dps/s — sharpness gate
    float    dt;                   // seconds per sample (1/470 nominal)
    int      refractory_samples;   // samples to suppress after hit

    /* State machine */
    hit_state_t state;
    float    peak_omega;
    uint32_t peak_time;
    bool     peak_clipped;
    int      refractory_counter;

    /* Previous sample for alpha calculation */
    float    prev_omega;
    bool     first_sample;

    /* Alpha lookback window: alpha gate stays armed for this many samples
     * after alpha exceeds threshold, bridging the temporal gap between
     * alpha peak (rising edge) and omega peak (a few ms later). */
    int      alpha_window;           // samples (default 10 ≈ ~21ms at 470Hz)
    int      alpha_armed_countdown;  // counts down from alpha_window

    /* Live telemetry (read these for display) */
    float    cur_omega;            // current gyro magnitude
    float    cur_alpha;            // current rate-of-change

    /* Alpha-omega timing instrumentation */
    uint32_t last_alpha_cross_ms;  // timestamp when alpha last exceeded threshold
    uint32_t last_hit_delta_ms;    // ms between alpha crossing and omega crossing (per hit)

    /* Counters */
    uint32_t hit_count;
    hit_event_t last_hit;
} hit_detector_t;

/**
 * Initialize the detector with tuning parameters.
 * @param dt              Sample interval in seconds (e.g. 1.0/470.0)
 * @param omega_thresh    Magnitude threshold (dps), start with 800
 * @param alpha_thresh    Sharpness threshold (dps/s), start with 150000
 * @param refractory_ms   Minimum gap between hits (ms), start with 250
 */
void hit_detector_init(hit_detector_t *hd, float dt,
                       float omega_thresh, float alpha_thresh,
                       int refractory_ms);

/**
 * Process one gyro sample.
 * @param gx, gy, gz     Gyroscope readings in dps
 * @param timestamp_ms    Monotonic timestamp in ms
 * @param clipped         True if this sample was clipped at full-scale
 * @param out_event       If non-NULL and a hit was emitted, filled with event data
 * @return true if a hit event was emitted this call
 */
bool hit_detector_process(hit_detector_t *hd,
                          float gx, float gy, float gz,
                          uint32_t timestamp_ms, bool clipped,
                          hit_event_t *out_event);

/**
 * Reset state machine to IDLE, clear counters.
 */
void hit_detector_reset(hit_detector_t *hd);

/**
 * Update thresholds live (for tuning UI).
 */
void hit_detector_set_omega_thresh(hit_detector_t *hd, float val);
void hit_detector_set_alpha_thresh(hit_detector_t *hd, float val);
void hit_detector_set_refractory(hit_detector_t *hd, int ms);

#ifdef __cplusplus
}
#endif
