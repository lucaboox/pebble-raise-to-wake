#include <pebble_worker.h>
#include "raise_to_wake_detector.h"

#define DURATION 6
#define LEGACY_CALLBACK_SAMPLES 7
#define CHARGING 8
#define PLUGGED 9
#define AMBIENT 10

// Preserve the declarations and worker light path tested on a real Pebble Time.
void light_enable_interaction(void);
void light_enable(bool val);

static RtwDetector s_detector;
static uint32_t s_duration_seconds;
static unsigned s_confirmation_samples;
static bool s_charging_enabled, s_plugged_enabled, s_ambient;
static bool s_power_light, s_forced_gesture_light;
static AppTimer *s_light_timer;
#if RTW_LOG_XYZ
static bool s_have_log_time;
static uint64_t s_last_log_ms;
#endif

static void cancel_light_timer(void) {
  if (s_light_timer) {
    app_timer_cancel(s_light_timer);
    s_light_timer = NULL;
  }
}

static void release_gesture_light(void) {
  cancel_light_timer();
  // Ambient interactions use the normal system timeout. Never cancel them.
  if (s_forced_gesture_light && !s_power_light) {
    light_enable(false); // Release our forced-on override to automatic control.
    APP_LOG(APP_LOG_LEVEL_INFO, "RTW released gesture backlight");
  }
  s_forced_gesture_light = false;
}

static void light_callback(void *context) {
  (void)context;
  s_light_timer = NULL;
  release_gesture_light();
  // A duration expiry does not unlock VIEWING. Lowering is still required.
}

static void activate_gesture_light(void) {
  cancel_light_timer();
  if (s_ambient) {
    light_enable_interaction();
  } else {
    light_enable(true);
    s_forced_gesture_light = true;
    if (s_duration_seconds) {
      s_light_timer = app_timer_register(s_duration_seconds * 1000, light_callback, NULL);
      if (!s_light_timer) {
        release_gesture_light();
        light_enable_interaction();
        APP_LOG(APP_LOG_LEVEL_WARNING, "RTW timer unavailable; using system light timeout");
      }
    }
  }
}

static void handle_accel(AccelData *data, uint32_t num_samples) {
  if (s_power_light) return;
  for (uint32_t i = 0; i < num_samples; ++i) {
    AccelData *sample = &data[i];
    RtwState before = s_detector.state;
    RtwEvent event = rtw_detector_update(&s_detector, sample->x, sample->y, sample->z,
                                        sample->timestamp, sample->did_vibrate);
    if (before != s_detector.state) {
      APP_LOG(APP_LOG_LEVEL_INFO, "RTW %s -> %s gate=%s xyz=%d,%d,%d", rtw_state_name(before),
              rtw_state_name(s_detector.state), rtw_block_reason_name(s_detector.block_reason),
              sample->x, sample->y, sample->z);
      if (s_detector.state == RTW_ARMED) {
        if (s_detector.block_reason == RTW_BLOCK_TIMEOUT) {
          APP_LOG(APP_LOG_LEVEL_INFO, "RTW attempt reset; lowered reference retained");
        } else {
          APP_LOG(APP_LOG_LEVEL_INFO, "RTW lowered confirmed; armed");
        }
      }
    }
#if RTW_LOG_XYZ
    if (!s_have_log_time || sample->timestamp < s_last_log_ms ||
        sample->timestamp - s_last_log_ms >= RTW_XYZ_LOG_INTERVAL_MS) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "RTW xyz=%d,%d,%d g=%ld,%ld,%ld gate=%s n=%u/%u vibe=%d",
              sample->x, sample->y, sample->z, (long)s_detector.gravity.x,
              (long)s_detector.gravity.y, (long)s_detector.gravity.z,
              rtw_block_reason_name(s_detector.block_reason), s_detector.view_samples,
              s_detector.required_view_samples, sample->did_vibrate);
      s_last_log_ms = sample->timestamp;
      s_have_log_time = true;
    }
#endif
    if (event == RTW_EVENT_RAISE) {
      activate_gesture_light();
      APP_LOG(APP_LOG_LEVEL_INFO, "RTW raise detected: %u samples, %lu ms from view entry",
              s_detector.required_view_samples, (unsigned long)s_detector.view_to_raise_ms);
    } else if (event == RTW_EVENT_LOWERED) {
      APP_LOG(APP_LOG_LEVEL_INFO, "RTW wrist lowered; waiting for rearm/cooldown");
      release_gesture_light();
    }
  }
}

static void battery_handler(BatteryChargeState charge) {
  bool power_light = (charge.is_charging && s_charging_enabled) ||
                     (charge.is_plugged && s_plugged_enabled);
  if (power_light == s_power_light) return;
  cancel_light_timer();
  s_forced_gesture_light = false;
  s_power_light = power_light;
  light_enable(power_light);
  rtw_detector_init(&s_detector, s_confirmation_samples);
  APP_LOG(APP_LOG_LEVEL_INFO, "RTW charging/powered override %s", power_light ? "on" : "released");
}

static void worker_init(void) {
  int32_t duration = persist_exists(DURATION) ? persist_read_int(DURATION) :
                     RTW_DEFAULT_LIGHT_DURATION_SECONDS;
  if (duration < 0 || duration > RTW_MAX_LIGHT_DURATION_SECONDS) {
    duration = RTW_DEFAULT_LIGHT_DURATION_SECONDS;
  }
  s_duration_seconds = (uint32_t)duration; // 0 = forced light lasts until lowering.
  int32_t confirm = persist_read_int(RTW_CONFIRMATION_PERSIST_KEY);
  if (confirm < RTW_MIN_CONFIRM_SAMPLES || confirm > RTW_MAX_CONFIRM_SAMPLES) {
    confirm = RTW_DEFAULT_CONFIRM_SAMPLES;
  }
  s_confirmation_samples = (unsigned)confirm;
  s_charging_enabled = persist_read_bool(CHARGING);
  s_plugged_enabled = persist_read_bool(PLUGGED);
  s_ambient = persist_read_bool(AMBIENT);
  s_power_light = s_forced_gesture_light = false;
#if RTW_LOG_XYZ
  s_have_log_time = false;
#endif
  rtw_detector_init(&s_detector, s_confirmation_samples);

  // Subscribe first: changing rate requires an active data subscription.
  accel_data_service_subscribe(RTW_SAMPLES_PER_CALLBACK, handle_accel);
  int result = accel_service_set_sampling_rate(RTW_ACCEL_SAMPLING_RATE);
  if (result != 0) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "RTW sampling rate failed: %d", result);
    accel_data_service_unsubscribe();
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "RTW rev=%u rate=%u Hz batch=%u confirm=%u duration=%lu s ambient=%d",
          (unsigned)RTW_DETECTOR_REVISION, (unsigned)RTW_ACCEL_SAMPLING_RATE,
          RTW_SAMPLES_PER_CALLBACK, s_confirmation_samples,
          (unsigned long)s_duration_seconds, s_ambient);
  if (persist_read_int(LEGACY_CALLBACK_SAMPLES) > 1) {
    APP_LOG(APP_LOG_LEVEL_INFO, "RTW legacy batching ignored; keeping one sample per callback");
  }
  if (s_charging_enabled || s_plugged_enabled) {
    battery_state_service_subscribe(battery_handler);
    battery_handler(battery_state_service_peek());
  }
}

static void worker_deinit(void) {
  accel_data_service_unsubscribe();
  if (s_charging_enabled || s_plugged_enabled) battery_state_service_unsubscribe();
  release_gesture_light();
  if (s_power_light) light_enable(false);
  s_power_light = false;
}

int main(void) {
  worker_init();
  worker_event_loop();
  worker_deinit();
  return 0;
}
