#pragma once

// Shared settings and tuning values. Acceleration is in milli-g.
#define RTW_DETECTOR_REVISION 11
// Persistent keys shared by the app and worker. Keys 7 (old callback batch)
// and 11 (old viewing samples) are retired; Sensitivity replaces both.
#define RTW_DURATION_PERSIST_KEY 6
#define RTW_CHARGING_PERSIST_KEY 8
#define RTW_PLUGGED_PERSIST_KEY 9
#define RTW_AMBIENT_PERSIST_KEY 10
#define RTW_LOGGING_PERSIST_KEY 12 // Debug logging; off unless enabled in the app.
#define RTW_SENSITIVITY_PERSIST_KEY 13
#define RTW_SLEEP_PAUSE_PERSIST_KEY 16 // Keys 14 and 15 are the app's schedule and first run.
#define RTW_ACCEL_SAMPLING_RATE ACCEL_SAMPLING_25HZ
#define RTW_SAMPLE_INTERVAL_MS 40 // Must match RTW_ACCEL_SAMPLING_RATE.
// Two samples per callback halves worker wake-ups for at most 40 ms delivery delay.
#define RTW_SAMPLES_PER_CALLBACK 2

// App -> worker messages. Settings arrive as data0 = light seconds,
// data1 = sensitivity, data2 = RTW_SETTING_* flags, so no restart is needed.
#define RTW_MSG_HAND_OFF_LIGHT 1 // The user is in the app: give the light to the system.
#define RTW_MSG_SETTINGS 2
#define RTW_SETTING_CHARGING 1
#define RTW_SETTING_PLUGGED 2
#define RTW_SETTING_AMBIENT 4
#define RTW_SETTING_LOGGING 8
// Bits 4-5 of the flags: pause while Pebble Health says you are asleep.
#define RTW_SETTING_SLEEP_SHIFT 4
#define RTW_SETTING_SLEEP_MASK (3 << RTW_SETTING_SLEEP_SHIFT)
#define RTW_SLEEP_PAUSE_OFF 0
#define RTW_SLEEP_PAUSE_DEEP 1 // Only during restful (deep) sleep.
#define RTW_SLEEP_PAUSE_ANY 2  // During any sleep.

// Sensitivity: how far the wrist must turn, and how long the view must hold.
#define RTW_SENSITIVITY_HIGH 0   // ~19 degrees, 3 samples
#define RTW_SENSITIVITY_NORMAL 1 // ~24 degrees, 3 samples
#define RTW_SENSITIVITY_LOW 2    // ~30 degrees, 4 samples
#define RTW_DEFAULT_SENSITIVITY RTW_SENSITIVITY_NORMAL
#define RTW_SENSITIVITY_ROTATION_MG(level) \
  ((level) == RTW_SENSITIVITY_HIGH ? 330 : (level) == RTW_SENSITIVITY_LOW ? 520 : 420)
#define RTW_SENSITIVITY_CONFIRM_SAMPLES(level) ((level) == RTW_SENSITIVITY_LOW ? 4 : 3)

#define RTW_DEFAULT_CONFIRM_SAMPLES 3
#define RTW_MIN_CONFIRM_SAMPLES 2
#define RTW_MAX_CONFIRM_SAMPLES 10

// Include the tilted/near-vertical poses observed in the Pebble Time logs.
// Negative Z is screen-up; positive Z is allowed only near vertical.
#define RTW_VIEW_X_MIN_MG (-550)
#define RTW_VIEW_X_MAX_MG 550
#define RTW_VIEW_Y_MIN_MG (-1150)
// Typing tilts in the logs reach Y -410; real looks ended at -525 to -680,
// so -550 (rev 4-6) split real looks. -530 (~32 degrees) was the best
// trade in simulated raises vs. keyboard fidgets; below ~-525 fidgets wake.
#define RTW_VIEW_Y_MAX_MG (-530)
#define RTW_VIEW_Z_MIN_MG (-1100)
#define RTW_VIEW_Z_MAX_MG 450

// Wider outer boundary: leave this region before confirming lowering.
#define RTW_RETAIN_X_MIN_MG (-650)
#define RTW_RETAIN_X_MAX_MG 650
#define RTW_RETAIN_Y_MIN_MG (-1250)
#define RTW_RETAIN_Y_MAX_MG (-350)
#define RTW_RETAIN_Z_MIN_MG (-1200)
#define RTW_RETAIN_Z_MAX_MG 550

// Lying on your back, the screen faces down at you (positive Z). Tighter than
// the upright region and only used when enabled (at night), since a palm
// flipped up also faces the screen down.
#define RTW_LYING_VIEW_X_MIN_MG (-550)
#define RTW_LYING_VIEW_X_MAX_MG 550
#define RTW_LYING_VIEW_Y_MIN_MG (-450)
#define RTW_LYING_VIEW_Y_MAX_MG 600
#define RTW_LYING_VIEW_Z_MIN_MG 600 // Screen tilted at least ~37 degrees toward the floor.
#define RTW_LYING_VIEW_Z_MAX_MG 1100
#define RTW_LYING_RETAIN_X_MIN_MG (-650)
#define RTW_LYING_RETAIN_X_MAX_MG 650
#define RTW_LYING_RETAIN_Y_MIN_MG (-600)
#define RTW_LYING_RETAIN_Y_MAX_MG 750
#define RTW_LYING_RETAIN_Z_MIN_MG 450
#define RTW_LYING_RETAIN_Z_MAX_MG 1200
#define RTW_LYING_ROTATION_MG 600 // ~35 degree turn into the lying view.
#define RTW_LYING_EXTRA_CONFIRM_SAMPLES 8
// A watch held up to look trembles with the hand; one resting palm-up on a
// bed or lap is nearly still. RMS of the sample-to-sample change in motion
// (second difference, which ignores smooth settling) over the hold.
#define RTW_LYING_MIN_TREMOR_MG 25
#define RTW_LYING_TREMOR_SKIP_SAMPLES 2 // Skip the arrival, which still carries the turn.
#define RTW_LYING_NIGHT_START_HOUR 20 // Lying view from 8 pm...
#define RTW_LYING_NIGHT_END_HOUR 8     // ...to 8 am.

#define RTW_GRAVITY_FILTER_DIVISOR 2 // Short filter: about one sample of lag.
#define RTW_GRAVITY_MIN_MG 700
#define RTW_GRAVITY_MAX_MG 1300
// Ignore a short acceleration burst during a raise, but never confirm on it.
#define RTW_TRANSIENT_GRAVITY_MIN_MG 250
#define RTW_TRANSIENT_GRAVITY_MAX_MG 2500
#define RTW_TRANSIENT_MOTION_MAX_MS 400
#define RTW_ROTATION_START_MG 220
// Default total rotation; Sensitivity overrides it per level.
#define RTW_ROTATION_TOTAL_MG 420 // About a 24 degree wrist turn.
#define RTW_MIN_ROTATION_TOTAL_MG 250
#define RTW_MAX_ROTATION_TOTAL_MG 900
#define RTW_VIEW_PROGRESS_MG_SQUARED 10000 // Decrease in distance to viewing region.
#define RTW_STABLE_DELTA_MG 140
#define RTW_STABLE_RESIDUAL_MG 180
#define RTW_LOWER_CONFIRM_SAMPLES 3
#define RTW_REST_REFRESH_SAMPLES 8 // Refresh only after ~320 ms in a lowered pose.
#define RTW_REST_REFRESH_DRIFT_MG 40 // Total drift from anchor, not per-sample drift.
// Settling this far outside the viewing region (e.g. back at a keyboard)
// also ends a look and rearms, without needing the arm fully lowered.
#define RTW_REARM_VIEW_MARGIN_MG 75
#define RTW_ROTATION_TIMEOUT_MS 2000
#define RTW_COOLDOWN_MS 500
#define RTW_MAX_SAMPLE_GAP_MS 250
#define RTW_VIBRATION_HOLDOFF_MS 200

// Light sensor mode: lowering within this long of the raise ends the light.
#define RTW_INTERACTION_RELEASE_MS 10000
#define RTW_DEFAULT_LIGHT_DURATION_SECONDS 5
#define RTW_MAX_LIGHT_DURATION_SECONDS 60
#define RTW_XYZ_LOG_INTERVAL_MS 1000 // Only when debug logging is enabled.
