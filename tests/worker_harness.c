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
static AppWorkerMessageHandler s_mock_message_handler;
static TickHandler s_mock_tick_handler;
static AppWorkerMessage s_mock_sent;
static int s_mock_sent_type = -1, s_mock_subscribes;
static BatteryStateHandler s_mock_battery_handler;
static BatteryChargeState s_mock_battery;
static AccelData s_mock_pending[8];
static uint32_t s_mock_pending_count;
static bool s_mock_shared_stamp;
static int s_persist_values[16];
static bool s_persist_exists[16];
static RtwDetector s_core;
static int s_mock_hour = 12;

void *memset(void *destination, int value, size_t count) {
  unsigned char *bytes = destination;
  for (size_t i = 0; i < count; ++i) bytes[i] = (unsigned char)value;
  return destination;
}
void accel_data_service_subscribe(uint32_t count, AccelDataHandler handler) {
  ++s_mock_subscribes;
  s_mock_batch = (int)count;
  s_mock_accel_handler = handler;
}
void accel_data_service_unsubscribe(void) { s_mock_accel_handler = NULL; s_mock_pending_count = 0; }
bool app_worker_message_subscribe(AppWorkerMessageHandler handler) {
  s_mock_message_handler = handler;
  return true;
}
bool app_worker_message_unsubscribe(void) { s_mock_message_handler = NULL; return true; }
void app_worker_send_message(uint8_t type, AppWorkerMessage *data) {
  s_mock_sent_type = type;
  s_mock_sent = *data;
}
void tick_timer_service_subscribe(TimeUnits units, TickHandler handler) {
  (void)units;
  s_mock_tick_handler = handler;
}
void tick_timer_service_unsubscribe(void) { s_mock_tick_handler = NULL; }
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
bool persist_exists(uint32_t key) { return key < 16 && s_persist_exists[key]; }
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

void core_init(int confirmations, int rotation_mg) {
  rtw_detector_init(&s_core, confirmations, rotation_mg);
}
void core_resume(void) { rtw_detector_resume(&s_core); }
void core_set_lying(int on) { rtw_detector_set_lying_view(&s_core, on != 0); }
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
  s_mock_shared_stamp = false;
  s_mock_hour = 12;
  s_mock_sent_type = -1;
  s_mock_subscribes = 0;
  s_mock_pending_count = 0;
  s_mock_battery = (BatteryChargeState){0};
  for (unsigned i = 0; i < 16; ++i) s_persist_exists[i] = false;
  for (unsigned i = 0; i < 16; ++i) s_mock_timers[i].active = false;
}
void mock_setting(int key, int value) { s_persist_values[key] = value; s_persist_exists[key] = true; }
void mock_start(void) { worker_init(); }
void mock_stop(void) { worker_deinit(); }
// Like the firmware, deliver only once a full batch has been collected.
// shared_stamp mimics firmware that stamps every sample in a batch alike.
void mock_shared_stamp(int shared) { s_mock_shared_stamp = shared != 0; }
void mock_feed(int x, int y, int z, uint32_t timestamp, int vibrated) {
  if (!s_mock_accel_handler) return;
  AccelData *sample = &s_mock_pending[s_mock_pending_count++];
  *sample = (AccelData){.x = x, .y = y, .z = z, .timestamp = timestamp, .did_vibrate = vibrated != 0};
  if (s_mock_shared_stamp) sample->timestamp = s_mock_pending[0].timestamp;
  if (s_mock_pending_count >= (uint32_t)s_mock_batch) {
    s_mock_pending_count = 0;
    s_mock_accel_handler(s_mock_pending, (uint32_t)s_mock_batch);
  }
}
void mock_app_message(int type, int data0, int data1, int data2) {
  AppWorkerMessage message = {(uint16_t)data0, (uint16_t)data1, (uint16_t)data2};
  if (s_mock_message_handler) s_mock_message_handler((uint16_t)type, &message);
}
int mock_battery_subscribed(void) { return s_mock_battery_handler != NULL; }
time_t time(time_t *t) { if (t) *t = 0; return 0; }
struct tm *localtime(const time_t *t) {
  static struct tm now;
  (void)t;
  now = (struct tm){0};
  now.tm_hour = s_mock_hour;
  return &now;
}
void mock_set_hour(int hour) { s_mock_hour = hour; }
void mock_tick(void) { if (s_mock_tick_handler) s_mock_tick_handler(NULL, MINUTE_UNIT); }
int mock_subscribes(void) { return s_mock_subscribes; }
int mock_sent_type(void) { return s_mock_sent_type; }
int mock_sent_data(int field) {
  return field == 0 ? s_mock_sent.data0 : field == 1 ? s_mock_sent.data1 : s_mock_sent.data2;
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
