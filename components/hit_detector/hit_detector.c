#include "hit_detector.h"
#include <math.h>
#include <string.h>

void hit_detector_init(hit_detector_t *hd, float dt,
                       float omega_thresh, float alpha_thresh,
                       int refractory_ms)
{
    memset(hd, 0, sizeof(*hd));
    hd->dt = dt;
    hd->omega_thresh = omega_thresh;
    hd->alpha_thresh = alpha_thresh;
    hd->refractory_samples = (int)(refractory_ms / 1000.0f * (1.0f / dt) + 0.5f);
    hd->state = HIT_STATE_IDLE;
    hd->first_sample = true;
}

bool hit_detector_process(hit_detector_t *hd,
                          float gx, float gy, float gz,
                          uint32_t timestamp_ms, bool clipped,
                          hit_event_t *out_event)
{
    /* 1. Magnitude */
    float omega = sqrtf(gx * gx + gy * gy + gz * gz);

    /* 2. Rate of change (sharpness) */
    float alpha = 0.0f;
    if (!hd->first_sample) {
        alpha = fabsf(omega - hd->prev_omega) / hd->dt;
    } else {
        hd->first_sample = false;
    }
    hd->prev_omega = omega;

    /* 3. Alpha lookback: if alpha exceeded threshold recently, keep gate armed.
     * This solves the timing problem where alpha peaks on the rising edge
     * but omega peaks a few ms later when alpha is near zero. */
    if (alpha > hd->alpha_thresh) {
        hd->alpha_armed_countdown = hd->alpha_window;
        hd->last_alpha_cross_ms = timestamp_ms;
    } else if (hd->alpha_armed_countdown > 0) {
        hd->alpha_armed_countdown--;
    }
    bool alpha_gate = (hd->alpha_armed_countdown > 0);

    /* Update live telemetry */
    hd->cur_omega = omega;
    hd->cur_alpha = alpha;

    /* 4. State machine */
    bool emitted = false;

    switch (hd->state) {
    case HIT_STATE_REFRACTORY:
        hd->refractory_counter--;
        if (hd->refractory_counter <= 0) {
            hd->state = HIT_STATE_IDLE;
        }
        break;

    case HIT_STATE_IDLE:
        if (omega > hd->omega_thresh && alpha_gate) {
            hd->state = HIT_STATE_IN_HIT;
            hd->peak_omega = omega;
            hd->peak_time = timestamp_ms;
            hd->peak_clipped = clipped;
            hd->last_hit_delta_ms = timestamp_ms - hd->last_alpha_cross_ms;
        }
        break;

    case HIT_STATE_IN_HIT:
        if (omega > hd->peak_omega) {
            hd->peak_omega = omega;
            hd->peak_time = timestamp_ms;
        }
        if (clipped) {
            hd->peak_clipped = true;
        }
        if (omega < hd->omega_thresh) {
            hd->hit_count++;
            hd->last_hit.timestamp_ms = hd->peak_time;
            hd->last_hit.peak_omega = hd->peak_omega;
            hd->last_hit.clipped = hd->peak_clipped;

            if (out_event) {
                *out_event = hd->last_hit;
            }
            emitted = true;

            hd->state = HIT_STATE_REFRACTORY;
            hd->refractory_counter = hd->refractory_samples;
        }
        break;
    }

    return emitted;
}

void hit_detector_reset(hit_detector_t *hd)
{
    float dt = hd->dt;
    float ot = hd->omega_thresh;
    float at = hd->alpha_thresh;
    int rs = hd->refractory_samples;
    int aw = hd->alpha_window;

    memset(hd, 0, sizeof(*hd));
    hd->dt = dt;
    hd->omega_thresh = ot;
    hd->alpha_thresh = at;
    hd->refractory_samples = rs;
    hd->alpha_window = aw;
    hd->state = HIT_STATE_IDLE;
    hd->first_sample = true;
}

void hit_detector_set_omega_thresh(hit_detector_t *hd, float val)
{
    hd->omega_thresh = val;
}

void hit_detector_set_alpha_thresh(hit_detector_t *hd, float val)
{
    hd->alpha_thresh = val;
}

void hit_detector_set_refractory(hit_detector_t *hd, int ms)
{
    hd->refractory_samples = (int)(ms / 1000.0f * (1.0f / hd->dt) + 0.5f);
}
