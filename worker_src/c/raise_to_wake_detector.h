#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../../src/c/raise_to_wake_config.h"

typedef enum {
  RTW_WAIT_LOWERED, RTW_ARMED, RTW_ROTATING, RTW_CONFIRMING_VIEW, RTW_VIEWING
} RtwState;
typedef enum { RTW_EVENT_NONE, RTW_EVENT_RAISE, RTW_EVENT_LOWERED } RtwEvent;
typedef enum {
  RTW_BLOCK_NONE, RTW_BLOCK_LOWERED, RTW_BLOCK_COOLDOWN, RTW_BLOCK_ROTATION,
  RTW_BLOCK_DIRECTION, RTW_BLOCK_VIEW, RTW_BLOCK_MOTION, RTW_BLOCK_CONFIRMATION,
  RTW_BLOCK_GAP, RTW_BLOCK_TIMESTAMP, RTW_BLOCK_VIBRATION, RTW_BLOCK_GRAVITY,
  RTW_BLOCK_TIMEOUT, RTW_BLOCK_FILTER, RTW_BLOCK_REST_REFRESH
} RtwBlockReason;
typedef struct { int32_t x, y, z; } RtwVector;
typedef struct {
  RtwState state;
  RtwBlockReason block_reason;
  RtwVector gravity, resting, previous, rest_anchor;
  uint64_t last_sample_ms, rotation_start_ms, last_raise_ms, last_vibration_ms;
  uint64_t view_entry_ms;
  uint64_t transient_start_ms;
  uint32_t view_to_raise_ms;
  int32_t rotation_total_mg;
  uint8_t view_samples, lower_samples, required_view_samples, rest_samples;
  bool filter_ready, have_timestamp, cooldown, vibration_holdoff, have_view_entry;
  bool transient_motion;
} RtwDetector;

// Portable core: only samples and millisecond timestamps, no Pebble APIs.
// Out-of-range values fall back to the defaults in raise_to_wake_config.h.
void rtw_detector_init(RtwDetector *d, unsigned confirmation_samples,
                       unsigned rotation_total_mg);
RtwEvent rtw_detector_update(RtwDetector *d, int16_t x, int16_t y, int16_t z,
                            uint64_t timestamp_ms, bool did_vibrate);
const char *rtw_state_name(RtwState state);
const char *rtw_block_reason_name(RtwBlockReason reason);
