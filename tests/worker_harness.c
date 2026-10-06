#include <pebble_worker.h>

// Include the exact worker to exercise its private lifecycle and callbacks.
#define main backlight_worker_main
#include "../worker_src/c/backlight_worker.c"
#undef main

struct AppTimer {
  AppTimerCallback callback;
  void *context;
  uint32_t duration;
  bool active;
};
static struct AppTimer s_mock_timers[16];
static int s_timer_count, s_timer_cancellations, s_timer_failure;
static int s_on_calls, s_off_calls, s_interaction_calls, s_logs;
static int s_mock_rate, s_mock_batch, s_rate_error;
static bool s_rate_after_subscribe;
static AccelDataHandler s_mock_accel_handler;
static BatteryStateHandler s_mock_battery_handler;
static BatteryChargeState s_mock_battery;
static int s_persist_values[12];
static bool s_persist_exists[12];
static RtwDetector s_core;

void *memset(void *destination, int value, size_t count) {
  unsigned char *bytes = destination;
  for (size_t i = 0; i < count; ++i) bytes[i] = (unsigned char)value;
  return destination;
}
void accel_data_service_subscribe(uint32_t count, AccelDataHandler handler) {
  s_mock_batch = (int)count;
  s_mock_accel_handler = handler;
}
void accel_data_service_unsubscribe(void) { s_mock_accel_handler = NULL; }
int accel_service_set_sampling_rate(AccelSamplingRate rate) {
  s_mock_rate = rate;
  s_rate_after_subscribe = s_mock_accel_handler != NULL;
  return s_rate_error;
}
void battery_state_service_subscribe(BatteryStateHandler handler) { s_mock_battery_handler = handler; }
void battery_state_service_unsubscribe(void) { s_mock_battery_handler = NULL; }
BatteryChargeState battery_state_service_peek(void) { return s_mock_battery; }
AppTimer *app_timer_register(uint32_t ms, AppTimerCallback callback, void *context) {
  if (s_timer_failure || s_timer_count >= 16) return NULL;
  AppTimer *timer = &s_mock_timers[s_timer_count++];
  *timer = (AppTimer){callback, context, ms, true};
  return timer;
}
bool app_timer_cancel(AppTimer *timer) {
  bool active = timer->active;
  timer->active = false;
  ++s_timer_cancellations;
  return active;
}
bool persist_exists(uint32_t key) { return key < 12 && s_persist_exists[key]; }
int32_t persist_read_int(uint32_t key) { return persist_exists(key) ? s_persist_values[key] : 0; }
bool persist_read_bool(uint32_t key) { return persist_read_int(key) != 0; }
void worker_event_loop(void) {}
void light_enable_interaction(void) { ++s_interaction_calls; }
void light_enable(bool value) { if (value) ++s_on_calls; else ++s_off_calls; }
void test_app_log(int level, const char *format, ...) {
  (void)level;
  (void)format;
  ++s_logs;
}

void core_init(int confirmations) { rtw_detector_init(&s_core, confirmations); }
int core_feed(int x, int y, int z, uint32_t timestamp, int vibrated) {
  return rtw_detector_update(&s_core, x, y, z, timestamp, vibrated != 0);
}
int core_state(void) { return s_core.state; }
int core_latency(void) { return (int)s_core.view_to_raise_ms; }
void mock_reset(void) {
  worker_deinit();
  s_timer_count = s_timer_cancellations = s_timer_failure = 0;
  s_on_calls = s_off_calls = s_interaction_calls = s_logs = 0;
  s_rate_error = 0;
  s_mock_battery = (BatteryChargeState){0};
  for (unsigned i = 0; i < 12; ++i) s_persist_exists[i] = false;
  for (unsigned i = 0; i < 16; ++i) s_mock_timers[i].active = false;
}
void mock_setting(int key, int value) { s_persist_values[key] = value; s_persist_exists[key] = true; }
void mock_start(void) { worker_init(); }
void mock_stop(void) { worker_deinit(); }
void mock_feed(int x, int y, int z, uint32_t timestamp, int vibrated) {
  if (s_mock_accel_handler) {
    AccelData sample = {.x = x, .y = y, .z = z, .timestamp = timestamp, .did_vibrate = vibrated != 0};
    s_mock_accel_handler(&sample, 1);
  }
}
void mock_battery(int charging, int plugged) {
  s_mock_battery.is_charging = charging != 0;
  s_mock_battery.is_plugged = plugged != 0;
  if (s_mock_battery_handler) s_mock_battery_handler(s_mock_battery);
}
void mock_expire(int index) {
  AppTimer *timer = &s_mock_timers[index];
  if (timer->active) {
    timer->active = false;
    timer->callback(timer->context);
  }
}
void mock_fail_timer(int fail) { s_timer_failure = fail; }
void mock_fail_rate(int fail) { s_rate_error = fail; }
int mock_on_calls(void) { return s_on_calls; }
int mock_off_calls(void) { return s_off_calls; }
int mock_interaction_calls(void) { return s_interaction_calls; }
int mock_timer_count(void) { return s_timer_count; }
int mock_timer_duration(int index) { return s_mock_timers[index].duration; }
int mock_timer_cancelled(void) { return s_timer_cancellations; }
int mock_rate(void) { return s_mock_rate; }
int mock_batch(void) { return s_mock_batch; }
int mock_rate_order(void) { return s_rate_after_subscribe; }
int mock_has_subscription(void) { return s_mock_accel_handler != NULL; }
int mock_state(void) { return s_detector.state; }
int mock_logs(void) { return s_logs; }
