#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../../src/c/raise_to_wake_config.h"

typedef enum {
  RTW_WAIT_LOWERED, RTW_ARMED, RTW_ROTATING, RTW_CONFIRMING_VIEW, RTW_VIEWING
} RtwState;
typedef enum { RTW_EVENT_NONE, RTW_EVENT_RAISE, RTW_EVENT_LOWERED } RtwEvent;
typedef struct { int32_t x, y, z; } RtwVector;
typedef struct {
  RtwState state;
  RtwVector gravity, resting, previous;
  uint64_t last_sample_ms, rotation_start_ms, last_raise_ms, last_vibration_ms;
  uint64_t view_entry_ms;
  uint32_t view_to_raise_ms;
  uint8_t view_samples, lower_samples, required_view_samples;
  bool filter_ready, have_timestamp, cooldown, vibration_holdoff, have_view_entry;
} RtwDetector;

// Portable core: only samples and millisecond timestamps, no Pebble APIs.
void rtw_detector_init(RtwDetector *d, unsigned confirmation_samples);
RtwEvent rtw_detector_update(RtwDetector *d, int16_t x, int16_t y, int16_t z,
                            uint64_t timestamp_ms, bool did_vibrate);
const char *rtw_state_name(RtwState state);
