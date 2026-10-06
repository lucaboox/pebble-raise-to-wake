#include "raise_to_wake_detector.h"

static int64_t magnitude_squared(RtwVector v) {
  return (int64_t)v.x * v.x + (int64_t)v.y * v.y + (int64_t)v.z * v.z;
}
static int64_t distance_squared(RtwVector a, RtwVector b) {
  return magnitude_squared((RtwVector){a.x - b.x, a.y - b.y, a.z - b.z});
}
static bool gravity_valid(RtwVector v) {
  int64_t magnitude = magnitude_squared(v);
  return magnitude >= RTW_GRAVITY_MIN_MG * RTW_GRAVITY_MIN_MG &&
         magnitude <= RTW_GRAVITY_MAX_MG * RTW_GRAVITY_MAX_MG;
}
static bool in_view(RtwVector v) {
  return v.x >= RTW_VIEW_X_MIN_MG && v.x <= RTW_VIEW_X_MAX_MG &&
         v.y >= RTW_VIEW_Y_MIN_MG && v.y <= RTW_VIEW_Y_MAX_MG &&
         v.z >= RTW_VIEW_Z_MIN_MG && v.z <= RTW_VIEW_Z_MAX_MG;
}
static bool outside_retained_view(RtwVector v) {
  return v.x < RTW_RETAIN_X_MIN_MG || v.x > RTW_RETAIN_X_MAX_MG ||
         v.y < RTW_RETAIN_Y_MIN_MG || v.y > RTW_RETAIN_Y_MAX_MG ||
         v.z < RTW_RETAIN_Z_MIN_MG || v.z > RTW_RETAIN_Z_MAX_MG;
}
static int32_t bound(int32_t value, int32_t minimum, int32_t maximum) {
  return value < minimum ? minimum : (value > maximum ? maximum : value);
}
static int64_t distance_to_view_squared(RtwVector v) {
  RtwVector closest = {
    bound(v.x, RTW_VIEW_X_MIN_MG, RTW_VIEW_X_MAX_MG),
    bound(v.y, RTW_VIEW_Y_MIN_MG, RTW_VIEW_Y_MAX_MG),
    bound(v.z, RTW_VIEW_Z_MIN_MG, RTW_VIEW_Z_MAX_MG)
  };
  return distance_squared(v, closest);
}
static void invalidate(RtwDetector *d, RtwBlockReason reason) {
  // Noise and gaps cannot unlock a wrist that has already triggered.
  if (d->state != RTW_VIEWING) d->state = RTW_WAIT_LOWERED;
  d->filter_ready = false;
  d->view_samples = d->lower_samples = 0;
  d->have_view_entry = false;
  d->transient_motion = false;
  d->block_reason = reason;
}

void rtw_detector_init(RtwDetector *d, unsigned confirmation_samples) {
  *d = (RtwDetector){0};
  d->state = RTW_WAIT_LOWERED;
  if (confirmation_samples < RTW_MIN_CONFIRM_SAMPLES ||
      confirmation_samples > RTW_MAX_CONFIRM_SAMPLES) {
    confirmation_samples = RTW_DEFAULT_CONFIRM_SAMPLES;
  }
  d->required_view_samples = (uint8_t)confirmation_samples;
}

RtwEvent rtw_detector_update(RtwDetector *d, int16_t x, int16_t y, int16_t z,
                            uint64_t now, bool did_vibrate) {
  RtwVector raw = {x, y, z};
  d->block_reason = RTW_BLOCK_NONE;
  if (d->have_timestamp && now <= d->last_sample_ms) {
    invalidate(d, RTW_BLOCK_TIMESTAMP);
    d->last_raise_ms = now;
    d->last_vibration_ms = now;
  } else if (d->have_timestamp && now - d->last_sample_ms > RTW_MAX_SAMPLE_GAP_MS) {
    invalidate(d, RTW_BLOCK_GAP);
  }
  d->last_sample_ms = now;
  d->have_timestamp = true;
  if (did_vibrate) {
    invalidate(d, RTW_BLOCK_VIBRATION);
    d->last_vibration_ms = now;
    d->vibration_holdoff = true;
    return RTW_EVENT_NONE;
  }
  if (d->vibration_holdoff) {
    if (now - d->last_vibration_ms < RTW_VIBRATION_HOLDOFF_MS) {
      d->block_reason = RTW_BLOCK_VIBRATION;
      return RTW_EVENT_NONE;
    }
    d->vibration_holdoff = false;
  }
  if (!gravity_valid(raw)) {
    int64_t magnitude = magnitude_squared(raw);
    bool candidate = d->state == RTW_ARMED || d->state == RTW_ROTATING ||
                     d->state == RTW_CONFIRMING_VIEW;
    bool modest_burst = magnitude >= RTW_TRANSIENT_GRAVITY_MIN_MG * RTW_TRANSIENT_GRAVITY_MIN_MG &&
                        magnitude <= RTW_TRANSIENT_GRAVITY_MAX_MG * RTW_TRANSIENT_GRAVITY_MAX_MG;
    if (candidate && modest_burst) {
      if (!d->transient_motion) {
        d->transient_start_ms = now;
        d->transient_motion = true;
      }
      if (now - d->transient_start_ms < RTW_TRANSIENT_MOTION_MAX_MS) {
        // Preserve the lowered reference, not partial viewing confirmation.
        // The next valid sample reseeds the filter; settling is still required.
        d->filter_ready = false;
        d->view_samples = 0;
        d->have_view_entry = false;
        if (d->state == RTW_CONFIRMING_VIEW) d->state = RTW_ROTATING;
        d->block_reason = RTW_BLOCK_GRAVITY;
        return RTW_EVENT_NONE;
      }
    }
    invalidate(d, RTW_BLOCK_GRAVITY);
    return RTW_EVENT_NONE;
  }
  if (d->transient_motion && now - d->transient_start_ms >= RTW_TRANSIENT_MOTION_MAX_MS) {
    invalidate(d, RTW_BLOCK_GRAVITY);
  }
  d->transient_motion = false;
  if (!d->filter_ready) {
    d->gravity = d->previous = raw;
    d->filter_ready = true;
    if (d->block_reason == RTW_BLOCK_NONE) d->block_reason = RTW_BLOCK_FILTER;
    return RTW_EVENT_NONE;
  }
  d->gravity.x += (raw.x - d->gravity.x) / RTW_GRAVITY_FILTER_DIVISOR;
  d->gravity.y += (raw.y - d->gravity.y) / RTW_GRAVITY_FILTER_DIVISOR;
  d->gravity.z += (raw.z - d->gravity.z) / RTW_GRAVITY_FILTER_DIVISOR;
  bool stable = distance_squared(raw, d->previous) <= RTW_STABLE_DELTA_MG * RTW_STABLE_DELTA_MG &&
                distance_squared(raw, d->gravity) <= RTW_STABLE_RESIDUAL_MG * RTW_STABLE_RESIDUAL_MG;
  d->previous = raw;
  if (!gravity_valid(d->gravity)) {
    invalidate(d, RTW_BLOCK_GRAVITY);
    return RTW_EVENT_NONE;
  }
  bool is_lowered = outside_retained_view(raw) && outside_retained_view(d->gravity);
  bool is_view = in_view(raw) && in_view(d->gravity);
  if (d->cooldown && now - d->last_raise_ms >= RTW_COOLDOWN_MS) d->cooldown = false;

  if (d->state == RTW_WAIT_LOWERED || d->state == RTW_VIEWING) {
    d->block_reason = RTW_BLOCK_LOWERED;
    if (is_lowered && stable) {
      if (d->lower_samples < RTW_LOWER_CONFIRM_SAMPLES) ++d->lower_samples;
    } else {
      d->lower_samples = 0;
    }
    if (d->lower_samples >= RTW_LOWER_CONFIRM_SAMPLES) {
      if (d->state == RTW_VIEWING) {
        // Lowering is reported promptly even if a short cooldown is still running.
        d->state = RTW_WAIT_LOWERED;
        d->have_view_entry = false;
        return RTW_EVENT_LOWERED;
      }
      if (!d->cooldown) {
        d->resting = d->gravity;
        d->lower_samples = 0;
        d->state = RTW_ARMED;
        d->block_reason = RTW_BLOCK_NONE;
      } else {
        d->block_reason = RTW_BLOCK_COOLDOWN;
      }
    }
    return RTW_EVENT_NONE;
  }

  int64_t rotation = distance_squared(d->resting, d->gravity);
  // A fixed dot product with one ideal viewing angle rejects flat-to-vertical
  // raises: Z becomes less negative as Y becomes more negative. Measure
  // progress toward the whole accepted region instead, on all three axes.
  bool toward_view = distance_to_view_squared(d->resting) -
                     distance_to_view_squared(d->gravity) >= RTW_VIEW_PROGRESS_MG_SQUARED;
  if (d->state == RTW_ARMED) {
    // Freeze the lowered reference; following every sample would swallow slow raises.
    if (rotation >= RTW_ROTATION_START_MG * RTW_ROTATION_START_MG && toward_view) {
      d->rotation_start_ms = now;
      d->state = RTW_ROTATING;
    } else {
      d->block_reason = toward_view ? RTW_BLOCK_ROTATION : RTW_BLOCK_DIRECTION;
      return RTW_EVENT_NONE;
    }
  }
  if (now - d->rotation_start_ms > RTW_ROTATION_TIMEOUT_MS) {
    d->state = RTW_WAIT_LOWERED;
    d->view_samples = d->lower_samples = 0;
    d->have_view_entry = false;
    d->block_reason = RTW_BLOCK_TIMEOUT;
    return RTW_EVENT_NONE;
  }
  if (in_view(raw)) {
    if (!d->have_view_entry) {
      d->view_entry_ms = now;
      d->have_view_entry = true;
    }
  } else {
    d->have_view_entry = false;
  }
  if (!is_view || !stable || !toward_view ||
      rotation < RTW_ROTATION_TOTAL_MG * RTW_ROTATION_TOTAL_MG) {
    d->block_reason = !is_view ? RTW_BLOCK_VIEW : (!stable ? RTW_BLOCK_MOTION :
                      (!toward_view ? RTW_BLOCK_DIRECTION : RTW_BLOCK_ROTATION));
    d->view_samples = 0;
    d->state = RTW_ROTATING;
    return RTW_EVENT_NONE;
  }
  d->state = RTW_CONFIRMING_VIEW;
  d->block_reason = RTW_BLOCK_CONFIRMATION;
  if (++d->view_samples >= d->required_view_samples) {
    d->state = RTW_VIEWING;
    d->block_reason = RTW_BLOCK_NONE;
    d->view_to_raise_ms = (uint32_t)(now - d->view_entry_ms);
    d->last_raise_ms = now;
    d->cooldown = true;
    d->lower_samples = 0;
    return RTW_EVENT_RAISE;
  }
  return RTW_EVENT_NONE;
}

const char *rtw_state_name(RtwState state) {
  switch (state) {
    case RTW_WAIT_LOWERED: return "WAIT_LOWERED";
    case RTW_ARMED: return "ARMED";
    case RTW_ROTATING: return "ROTATING";
    case RTW_CONFIRMING_VIEW: return "CONFIRMING_VIEW";
    case RTW_VIEWING: return "VIEWING";
  }
  return "UNKNOWN";
}

const char *rtw_block_reason_name(RtwBlockReason reason) {
  switch (reason) {
    case RTW_BLOCK_NONE: return "none";
    case RTW_BLOCK_LOWERED: return "need-lowering";
    case RTW_BLOCK_COOLDOWN: return "cooldown";
    case RTW_BLOCK_ROTATION: return "small-rotation";
    case RTW_BLOCK_DIRECTION: return "no-view-progress";
    case RTW_BLOCK_VIEW: return "outside-view";
    case RTW_BLOCK_MOTION: return "unsettled";
    case RTW_BLOCK_CONFIRMATION: return "confirming";
    case RTW_BLOCK_GAP: return "sample-gap";
    case RTW_BLOCK_TIMESTAMP: return "timestamp";
    case RTW_BLOCK_VIBRATION: return "vibration";
    case RTW_BLOCK_GRAVITY: return "accel-magnitude";
    case RTW_BLOCK_TIMEOUT: return "rotation-timeout";
    case RTW_BLOCK_FILTER: return "filter-start";
  }
  return "unknown";
}
