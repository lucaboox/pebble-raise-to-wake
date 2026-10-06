#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Minimal worker API shim for host tests; not the real Pebble SDK.
typedef struct {
  int16_t x, y, z;
  bool did_vibrate;
  uint64_t timestamp;
} AccelData;
typedef enum { ACCEL_SAMPLING_10HZ = 10, ACCEL_SAMPLING_25HZ = 25 } AccelSamplingRate;
typedef void (*AccelDataHandler)(AccelData *, uint32_t);
typedef struct {
  uint8_t charge_percent;
  bool is_charging, is_plugged;
} BatteryChargeState;
typedef void (*BatteryStateHandler)(BatteryChargeState);
typedef struct AppTimer AppTimer;
typedef void (*AppTimerCallback)(void *);

void accel_data_service_subscribe(uint32_t count, AccelDataHandler handler);
void accel_data_service_unsubscribe(void);
int accel_service_set_sampling_rate(AccelSamplingRate rate);
void battery_state_service_subscribe(BatteryStateHandler handler);
void battery_state_service_unsubscribe(void);
BatteryChargeState battery_state_service_peek(void);
AppTimer *app_timer_register(uint32_t ms, AppTimerCallback callback, void *context);
bool app_timer_cancel(AppTimer *timer);
bool persist_exists(uint32_t key);
int32_t persist_read_int(uint32_t key);
bool persist_read_bool(uint32_t key);
void worker_event_loop(void);
void test_app_log(int level, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define APP_LOG_LEVEL_DEBUG 0
#define APP_LOG_LEVEL_INFO 1
#define APP_LOG_LEVEL_WARNING 2
#define APP_LOG_LEVEL_ERROR 3
#define APP_LOG(...) test_app_log(__VA_ARGS__)
