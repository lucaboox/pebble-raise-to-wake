#pragma once

// Shared settings and tuning values. Acceleration is in milli-g.
#define RTW_DETECTOR_REVISION 4
#define RTW_CONFIRMATION_PERSIST_KEY 11 // Key 7 was the old callback batch setting.
#define RTW_ACCEL_SAMPLING_RATE ACCEL_SAMPLING_25HZ
#define RTW_SAMPLES_PER_CALLBACK 1
#define RTW_DEFAULT_CONFIRM_SAMPLES 3
#define RTW_MIN_CONFIRM_SAMPLES 2
#define RTW_MAX_CONFIRM_SAMPLES 10

// Include the tilted/near-vertical poses observed in the Pebble Time logs.
// Negative Z is screen-up; positive Z is allowed only near vertical.
#define RTW_VIEW_X_MIN_MG (-550)
#define RTW_VIEW_X_MAX_MG 550
#define RTW_VIEW_Y_MIN_MG (-1150)
#define RTW_VIEW_Y_MAX_MG (-550) // Reject the shallow tilts seen while typing.
#define RTW_VIEW_Z_MIN_MG (-1100)
#define RTW_VIEW_Z_MAX_MG 450

// Wider outer boundary: leave this region before confirming lowering.
#define RTW_RETAIN_X_MIN_MG (-650)
#define RTW_RETAIN_X_MAX_MG 650
#define RTW_RETAIN_Y_MIN_MG (-1250)
#define RTW_RETAIN_Y_MAX_MG (-350)
#define RTW_RETAIN_Z_MIN_MG (-1200)
#define RTW_RETAIN_Z_MAX_MG 550

#define RTW_GRAVITY_FILTER_DIVISOR 2 // Short filter: about one sample of lag.
#define RTW_GRAVITY_MIN_MG 700
#define RTW_GRAVITY_MAX_MG 1300
// Ignore a short acceleration burst during a raise, but never confirm on it.
#define RTW_TRANSIENT_GRAVITY_MIN_MG 250
#define RTW_TRANSIENT_GRAVITY_MAX_MG 2500
#define RTW_TRANSIENT_MOTION_MAX_MS 400
#define RTW_ROTATION_START_MG 220
#define RTW_ROTATION_TOTAL_MG 350
#define RTW_VIEW_PROGRESS_MG_SQUARED 10000 // Decrease in distance to viewing region.
#define RTW_STABLE_DELTA_MG 140
#define RTW_STABLE_RESIDUAL_MG 180
#define RTW_LOWER_CONFIRM_SAMPLES 3
#define RTW_REST_REFRESH_SAMPLES 8 // Refresh only after ~320 ms in a lowered pose.
#define RTW_REST_REFRESH_DRIFT_MG 40 // Total drift from anchor, not per-sample drift.
#define RTW_ROTATION_TIMEOUT_MS 1500
#define RTW_COOLDOWN_MS 500
#define RTW_MAX_SAMPLE_GAP_MS 250
#define RTW_VIBRATION_HOLDOFF_MS 200

#define RTW_DEFAULT_LIGHT_DURATION_SECONDS 5
#define RTW_MAX_LIGHT_DURATION_SECONDS 60
#define RTW_LOG_XYZ 1
#define RTW_XYZ_LOG_INTERVAL_MS 1000
