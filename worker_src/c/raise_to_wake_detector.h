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
  RTW_BLOCK_TIMEOUT, RTW_BLOCK_FILTER, RTW_BLOCK_REST_REFRESH, RTW_BLOCK_RESTING_HAND
} RtwBlockReason;
typedef struct { int32_t x, y, z; } RtwVector;
typedef struct {
  RtwState state;
  RtwBlockReason block_reason;
  RtwVector gravity, resting, previous, previous2, rest_anchor;
  uint64_t last_sample_ms, rotation_start_ms, last_raise_ms, last_vibration_ms;
  uint64_t view_entry_ms;
  uint64_t transient_start_ms;
  uint32_t view_to_raise_ms;
  int64_t tremor_sum;
  int32_t rotation_total_mg;
  uint8_t view_samples, lower_samples, required_view_samples, rest_samples;
  bool filter_ready, have_timestamp, cooldown, vibration_holdoff, have_view_entry;
  bool transient_motion;
  bool view_released;
  bool lying_view;
} RtwDetector;

// Portable core: only samples and millisecond timestamps, no Pebble APIs.
// Out-of-range values fall back to the defaults in raise_to_wake_config.h.
void rtw_detector_init(RtwDetector *d, unsigned confirmation_samples,
                       unsigned rotation_total_mg);
// Call before feeding samples again after sampling was paused or slowed while
// the watch was still. Keeps the lowered reference and VIEWING lockout, so the
// gap neither cancels the next raise nor unlocks a raised wrist.
void rtw_detector_resume(RtwDetector *d);
// Call once the raise's light has gone out. Leaving the viewing region then
// counts as lowering, so a partly lowered wrist rearms for the next raise.
void rtw_detector_release_view(RtwDetector *d);
// Also accept looking at the watch while lying on your back (screen facing
// down at you). Off by default; it needs a bigger turn and a longer hold.
void rtw_detector_set_lying_view(RtwDetector *d, bool enabled);
RtwEvent rtw_detector_update(RtwDetector *d, int16_t x, int16_t y, int16_t z,
                            uint64_t timestamp_ms, bool did_vibrate);
const char *rtw_state_name(RtwState state);
const char *rtw_block_reason_name(RtwBlockReason reason);
