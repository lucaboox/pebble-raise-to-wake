#include <pebble_worker.h>
#include "raise_to_wake_detector.h"

// Preserve the declarations and worker light path tested on a real Pebble Time.
void light_enable_interaction(void);
void light_enable(bool val);

static RtwDetector s_detector;
static uint32_t s_duration_seconds;
static unsigned s_sensitivity;
static bool s_charging_enabled, s_plugged_enabled, s_ambient, s_logging;
static bool s_power_light, s_forced_gesture_light;
// A light-sensor (system interaction) light we started, and when.
static bool s_interaction_light;
static uint64_t s_interaction_light_ms;
static AppTimer *s_light_timer;
static bool s_have_log_time;
static uint64_t s_last_log_ms;
// Health counters: raises for the app's status line, and samples so a stalled
// motion stream can be noticed (per minute) and reported (per status request).
static uint16_t s_raise_count;
static uint32_t s_samples_since_tick, s_samples_since_status;

// Formatting and queueing log lines costs battery even with no phone listening.
#define RTW_LOG(level, ...) do { if (s_logging) APP_LOG(level, __VA_ARGS__); } while (0)

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
    RTW_LOG(APP_LOG_LEVEL_INFO, "RTW released gesture backlight");
  }
  s_forced_gesture_light = false;
}

// The user is pressing buttons: light_enable(false) would cut the light they
// are using, so give it to the system's normal interaction timeout instead.
static void hand_off_gesture_light(void) {
  bool forced = s_forced_gesture_light && !s_power_light;
  release_gesture_light();
  s_interaction_light = false; // Now the user's light: lowering must not cut it.
  if (forced) light_enable_interaction();
}

// Lowering soon after a light-sensor raise ends that light too, instead of
// leaving it on for the watch's own timeout. Much later, the light is more
// likely a button press or notification, so leave it alone.
static void release_interaction_light(uint64_t now) {
  if (s_interaction_light && !s_power_light &&
      now - s_interaction_light_ms <= RTW_INTERACTION_RELEASE_MS) {
    light_enable(false);
    RTW_LOG(APP_LOG_LEVEL_INFO, "RTW released light-sensor backlight");
  }
  s_interaction_light = false;
}

static void reset_detector(void) {
  rtw_detector_init(&s_detector, RTW_SENSITIVITY_CONFIRM_SAMPLES(s_sensitivity),
                    RTW_SENSITIVITY_ROTATION_MG(s_sensitivity));
}

static void light_callback(void *context) {
  (void)context;
  s_light_timer = NULL;
  release_gesture_light();
  // A duration expiry does not unlock VIEWING. Lowering is still required.
}

static void start_interaction_light(uint64_t now) {
  light_enable_interaction();
  s_interaction_light = true;
  s_interaction_light_ms = now;
}

static void activate_gesture_light(uint64_t now) {
  cancel_light_timer();
  if (s_ambient) {
    start_interaction_light(now);
  } else {
    light_enable(true);
    s_forced_gesture_light = true;
    if (s_duration_seconds) {
      s_light_timer = app_timer_register(s_duration_seconds * 1000, light_callback, NULL);
      if (!s_light_timer) {
        release_gesture_light();
        start_interaction_light(now);
        RTW_LOG(APP_LOG_LEVEL_WARNING, "RTW timer unavailable; using system light timeout");
      }
    }
  }
}

static void handle_accel(AccelData *data, uint32_t num_samples) {
  s_samples_since_tick += num_samples;
  s_samples_since_status += num_samples;
  if (s_power_light) return;
  uint64_t previous = 0;
  for (uint32_t i = 0; i < num_samples; ++i) {
    AccelData *sample = &data[i];
    // Samples in one batch are 40 ms apart. Should firmware ever stamp a batch
    // with one shared time, space them out rather than treating it as a clock
    // rollback. Rollbacks between batches still reach the detector unchanged.
    uint64_t timestamp = sample->timestamp;
    if (i && timestamp <= previous) timestamp = previous + RTW_SAMPLE_INTERVAL_MS;
    previous = timestamp;
    RtwState before = s_detector.state;
    RtwEvent event = rtw_detector_update(&s_detector, sample->x, sample->y, sample->z,
                                        timestamp, sample->did_vibrate);
    if (before != s_detector.state) {
      RTW_LOG(APP_LOG_LEVEL_INFO, "RTW %s -> %s gate=%s xyz=%d,%d,%d", rtw_state_name(before),
              rtw_state_name(s_detector.state), rtw_block_reason_name(s_detector.block_reason),
              sample->x, sample->y, sample->z);
      if (s_detector.state == RTW_ARMED) {
        if (s_detector.block_reason == RTW_BLOCK_TIMEOUT) {
          RTW_LOG(APP_LOG_LEVEL_INFO, "RTW attempt reset");
        } else {
          RTW_LOG(APP_LOG_LEVEL_INFO, "RTW lowered confirmed; armed");
        }
      }
    }
    if (s_logging && (!s_have_log_time || timestamp < s_last_log_ms ||
        timestamp - s_last_log_ms >= RTW_XYZ_LOG_INTERVAL_MS)) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "RTW xyz=%d,%d,%d g=%ld,%ld,%ld gate=%s n=%u/%u vibe=%d",
              sample->x, sample->y, sample->z, (long)s_detector.gravity.x,
              (long)s_detector.gravity.y, (long)s_detector.gravity.z,
              rtw_block_reason_name(s_detector.block_reason), s_detector.view_samples,
              s_detector.required_view_samples, sample->did_vibrate);
      s_last_log_ms = timestamp;
      s_have_log_time = true;
    }
    if (event == RTW_EVENT_RAISE) {
      ++s_raise_count;
      activate_gesture_light(timestamp);
      RTW_LOG(APP_LOG_LEVEL_INFO, "RTW raise detected: %u samples, %lu ms from view entry",
              s_detector.required_view_samples, (unsigned long)s_detector.view_to_raise_ms);
    } else if (event == RTW_EVENT_LOWERED) {
      RTW_LOG(APP_LOG_LEVEL_INFO, "RTW wrist lowered; waiting for rearm/cooldown");
      release_gesture_light();
      release_interaction_light(timestamp);
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
  reset_detector();
  RTW_LOG(APP_LOG_LEVEL_INFO, "RTW charging/powered override %s", power_light ? "on" : "released");
}

// Used at startup (from storage) and when the app changes a setting, so the
// worker never has to be restarted. Only a sensitivity change resets gesture
// tracking; an active gesture light keeps its current timer.
static void apply_settings(uint32_t duration, unsigned sensitivity, unsigned flags, bool startup) {
  // 0 = forced light lasts until lowering.
  s_duration_seconds = duration <= RTW_MAX_LIGHT_DURATION_SECONDS ? duration :
                       RTW_DEFAULT_LIGHT_DURATION_SECONDS;
  if (sensitivity > RTW_SENSITIVITY_LOW) sensitivity = RTW_DEFAULT_SENSITIVITY;
  if (startup || sensitivity != s_sensitivity) {
    s_sensitivity = sensitivity;
    reset_detector();
  }
  s_ambient = flags & RTW_SETTING_AMBIENT;
  s_logging = flags & RTW_SETTING_LOGGING;
  bool watched = !startup && (s_charging_enabled || s_plugged_enabled);
  s_charging_enabled = flags & RTW_SETTING_CHARGING;
  s_plugged_enabled = flags & RTW_SETTING_PLUGGED;
  bool watch = s_charging_enabled || s_plugged_enabled;
  if (watch && !watched) battery_state_service_subscribe(battery_handler);
  if (!watch && watched) battery_state_service_unsubscribe();
  // Switches a charger light on or off right away if the setting changed.
  battery_handler(battery_state_service_peek());
}

static void app_message_handler(uint16_t type, AppWorkerMessage *message) {
  if (type == RTW_MSG_HAND_OFF_LIGHT) {
    hand_off_gesture_light();
  } else if (type == RTW_MSG_SETTINGS) {
    apply_settings(message->data0, message->data1, message->data2, false);
    RTW_LOG(APP_LOG_LEVEL_INFO, "RTW settings: duration=%u s sensitivity=%u flags=%u",
            message->data0, message->data1, message->data2);
  } else if (type == RTW_MSG_STATUS) {
    AppWorkerMessage reply = {
      .data0 = s_raise_count,
      .data1 = s_samples_since_status > 0,
      .data2 = (uint16_t)s_detector.state,
    };
    s_samples_since_status = 0;
    app_worker_send_message(RTW_MSG_STATUS, &reply);
  }
}

static void subscribe_accel(void) {
  // Subscribe first: changing rate requires an active data subscription.
  accel_data_service_subscribe(RTW_SAMPLES_PER_CALLBACK, handle_accel);
  int result = accel_service_set_sampling_rate(RTW_ACCEL_SAMPLING_RATE);
  if (result != 0) {
    // Newer watches use different motion sensors. Keep the subscription at
    // the service default (also 25 Hz) rather than silently disabling raise
    // to wake on a watch that rejects the explicit rate.
    APP_LOG(APP_LOG_LEVEL_WARNING, "RTW sampling rate failed: %d; using default", result);
  }
}

// Motion data arrives about 12 times a second, so a whole minute without any
// means the stream has stalled (firmware can reconfigure the sensor, e.g. when
// Motion Backlight is switched). Reconnect rather than stay silently dead.
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  (void)tick_time;
  (void)units_changed;
  if (!s_samples_since_tick) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "RTW no motion data for a minute; reconnecting");
    accel_data_service_unsubscribe();
    subscribe_accel();
    reset_detector();
  }
  s_samples_since_tick = 0;
}

static void worker_init(void) {
  int32_t duration = persist_exists(RTW_DURATION_PERSIST_KEY) ?
                     persist_read_int(RTW_DURATION_PERSIST_KEY) : RTW_DEFAULT_LIGHT_DURATION_SECONDS;
  if (duration < 0) duration = RTW_DEFAULT_LIGHT_DURATION_SECONDS;
  int32_t sensitivity = persist_exists(RTW_SENSITIVITY_PERSIST_KEY) ?
                        persist_read_int(RTW_SENSITIVITY_PERSIST_KEY) : RTW_DEFAULT_SENSITIVITY;
  if (sensitivity < 0) sensitivity = RTW_DEFAULT_SENSITIVITY;
  unsigned flags = (persist_read_bool(RTW_CHARGING_PERSIST_KEY) ? RTW_SETTING_CHARGING : 0) |
                   (persist_read_bool(RTW_PLUGGED_PERSIST_KEY) ? RTW_SETTING_PLUGGED : 0) |
                   (persist_read_bool(RTW_AMBIENT_PERSIST_KEY) ? RTW_SETTING_AMBIENT : 0) |
                   (persist_read_bool(RTW_LOGGING_PERSIST_KEY) ? RTW_SETTING_LOGGING : 0);
  s_power_light = s_forced_gesture_light = s_interaction_light = false;
  s_have_log_time = false;
  s_raise_count = 0;
  s_samples_since_tick = s_samples_since_status = 0;
  apply_settings((uint32_t)duration, (unsigned)sensitivity, flags, true);

  subscribe_accel();
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  app_worker_message_subscribe(app_message_handler);
  // Always printed once, so a log session can confirm which build is running.
  APP_LOG(APP_LOG_LEVEL_INFO, "RTW rev=%u rate=%u Hz batch=%u sensitivity=%u duration=%lu s ambient=%d log=%d",
          (unsigned)RTW_DETECTOR_REVISION, (unsigned)RTW_ACCEL_SAMPLING_RATE,
          RTW_SAMPLES_PER_CALLBACK, s_sensitivity,
          (unsigned long)s_duration_seconds, s_ambient, s_logging);
}

static void worker_deinit(void) {
  accel_data_service_unsubscribe();
  tick_timer_service_unsubscribe();
  app_worker_message_unsubscribe();
  if (s_charging_enabled || s_plugged_enabled) battery_state_service_unsubscribe();
  // The app stops the worker while the user is changing settings.
  hand_off_gesture_light();
  if (s_power_light) light_enable(false);
  s_power_light = false;
}

int main(void) {
  worker_init();
  worker_event_loop();
  worker_deinit();
  return 0;
}
