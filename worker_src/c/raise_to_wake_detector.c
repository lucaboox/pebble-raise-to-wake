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
static bool in_box(RtwVector v, int32_t x0, int32_t x1, int32_t y0, int32_t y1, int32_t z0,
                   int32_t z1) {
  return v.x >= x0 && v.x <= x1 && v.y >= y0 && v.y <= y1 && v.z >= z0 && v.z <= z1;
}
static int32_t bound(int32_t value, int32_t minimum, int32_t maximum) {
  return value < minimum ? minimum : (value > maximum ? maximum : value);
}
static int64_t distance_to_box_squared(RtwVector v, int32_t x0, int32_t x1, int32_t y0,
                                       int32_t y1, int32_t z0, int32_t z1) {
  RtwVector closest = {bound(v.x, x0, x1), bound(v.y, y0, y1), bound(v.z, z0, z1)};
  return distance_squared(v, closest);
}
#define UPRIGHT_VIEW RTW_VIEW_X_MIN_MG, RTW_VIEW_X_MAX_MG, RTW_VIEW_Y_MIN_MG,                      RTW_VIEW_Y_MAX_MG, RTW_VIEW_Z_MIN_MG, RTW_VIEW_Z_MAX_MG
#define UPRIGHT_RETAIN RTW_RETAIN_X_MIN_MG, RTW_RETAIN_X_MAX_MG, RTW_RETAIN_Y_MIN_MG,                        RTW_RETAIN_Y_MAX_MG, RTW_RETAIN_Z_MIN_MG, RTW_RETAIN_Z_MAX_MG
#define LYING_VIEW RTW_LYING_VIEW_X_MIN_MG, RTW_LYING_VIEW_X_MAX_MG, RTW_LYING_VIEW_Y_MIN_MG,                    RTW_LYING_VIEW_Y_MAX_MG, RTW_LYING_VIEW_Z_MIN_MG, RTW_LYING_VIEW_Z_MAX_MG
#define LYING_RETAIN RTW_LYING_RETAIN_X_MIN_MG, RTW_LYING_RETAIN_X_MAX_MG,                      RTW_LYING_RETAIN_Y_MIN_MG, RTW_LYING_RETAIN_Y_MAX_MG,                      RTW_LYING_RETAIN_Z_MIN_MG, RTW_LYING_RETAIN_Z_MAX_MG

// The viewing region is the upright one, plus the lying-on-your-back one when enabled.
static bool in_lying_view(const RtwDetector *d, RtwVector v) {
  return d->lying_view && in_box(v, LYING_VIEW);
}
static bool in_view(const RtwDetector *d, RtwVector v) {
  return in_box(v, UPRIGHT_VIEW) || in_lying_view(d, v);
}
static bool outside_retained_view(const RtwDetector *d, RtwVector v) {
  return !in_box(v, UPRIGHT_RETAIN) && !(d->lying_view && in_box(v, LYING_RETAIN));
}
static int64_t distance_to_view_squared(const RtwDetector *d, RtwVector v) {
  int64_t distance = distance_to_box_squared(v, UPRIGHT_VIEW);
  if (d->lying_view) {
    int64_t lying = distance_to_box_squared(v, LYING_VIEW);
    if (lying < distance) distance = lying;
  }
  return distance;
}
static void invalidate(RtwDetector *d, RtwBlockReason reason) {
  // Noise and gaps cannot unlock a wrist that has already triggered.
  if (d->state != RTW_VIEWING) d->state = RTW_WAIT_LOWERED;
  d->filter_ready = false;
  d->view_samples = d->lower_samples = d->rest_samples = 0;
  d->have_view_entry = false;
  d->transient_motion = false;
  d->block_reason = reason;
}

void rtw_detector_init(RtwDetector *d, unsigned confirmation_samples,
                       unsigned rotation_total_mg) {
  *d = (RtwDetector){0};
  d->state = RTW_WAIT_LOWERED;
  if (confirmation_samples < RTW_MIN_CONFIRM_SAMPLES ||
      confirmation_samples > RTW_MAX_CONFIRM_SAMPLES) {
    confirmation_samples = RTW_DEFAULT_CONFIRM_SAMPLES;
  }
  if (rotation_total_mg < RTW_MIN_ROTATION_TOTAL_MG ||
      rotation_total_mg > RTW_MAX_ROTATION_TOTAL_MG) {
    rotation_total_mg = RTW_ROTATION_TOTAL_MG;
  }
  d->required_view_samples = (uint8_t)confirmation_samples;
  d->rotation_total_mg = (int32_t)rotation_total_mg;
}

void rtw_detector_resume(RtwDetector *d) {
  d->have_timestamp = false; // No gap check against the last sample before the pause.
  d->filter_ready = false;
  d->transient_motion = false;
  d->view_samples = d->lower_samples = d->rest_samples = 0;
  d->have_view_entry = false;
  if (d->state == RTW_ROTATING || d->state == RTW_CONFIRMING_VIEW) d->state = RTW_ARMED;
}

void rtw_detector_release_view(RtwDetector *d) { d->view_released = true; }

void rtw_detector_set_lying_view(RtwDetector *d, bool enabled) { d->lying_view = enabled; }

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
    if (modest_burst && (d->state == RTW_WAIT_LOWERED || d->state == RTW_VIEWING)) {
      // Footsteps and arm swing while walking: skip the sample but keep any
      // lowering already counted, or a moving arm could never rearm.
      d->filter_ready = false;
      d->block_reason = RTW_BLOCK_GRAVITY;
      return RTW_EVENT_NONE;
    }
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
        d->rest_samples = 0;
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
    d->gravity = d->previous = d->previous2 = raw;
    d->filter_ready = true;
    if (d->block_reason == RTW_BLOCK_NONE) d->block_reason = RTW_BLOCK_FILTER;
    return RTW_EVENT_NONE;
  }
  d->gravity.x += (raw.x - d->gravity.x) / RTW_GRAVITY_FILTER_DIVISOR;
  d->gravity.y += (raw.y - d->gravity.y) / RTW_GRAVITY_FILTER_DIVISOR;
  d->gravity.z += (raw.z - d->gravity.z) / RTW_GRAVITY_FILTER_DIVISOR;
  const RtwVector change = {raw.x - 2 * d->previous.x + d->previous2.x,
                            raw.y - 2 * d->previous.y + d->previous2.y,
                            raw.z - 2 * d->previous.z + d->previous2.z};
  const int64_t tremor = magnitude_squared(change);
  bool stable = distance_squared(raw, d->previous) <= RTW_STABLE_DELTA_MG * RTW_STABLE_DELTA_MG &&
                distance_squared(raw, d->gravity) <= RTW_STABLE_RESIDUAL_MG * RTW_STABLE_RESIDUAL_MG;
  d->previous2 = d->previous;
  d->previous = raw;
  if (!gravity_valid(d->gravity)) {
    // Opposing valid gravity directions can average to nearly zero while the
    // wrist rotates. That is a filter artefact, not loss of the lowered pose.
    // Reseed only from a valid raw sample, and require fresh confirmation.
    d->gravity = d->previous = d->previous2 = raw;
    d->view_samples = d->lower_samples = d->rest_samples = 0;
    d->have_view_entry = false;
    if (d->state == RTW_CONFIRMING_VIEW) d->state = RTW_ROTATING;
    d->block_reason = RTW_BLOCK_FILTER;
    return RTW_EVENT_NONE;
  }
  bool is_lowered = outside_retained_view(d, raw) && outside_retained_view(d, d->gravity);
  bool is_view = in_view(d, raw) && in_view(d, d->gravity);
  if (d->cooldown && now - d->last_raise_ms >= RTW_COOLDOWN_MS) d->cooldown = false;

  if (d->state == RTW_WAIT_LOWERED || d->state == RTW_VIEWING) {
    d->block_reason = RTW_BLOCK_LOWERED;
    // Lowering does not need a steady arm: requiring one meant a wrist
    // swinging at your side while walking never rearmed, so every later raise
    // was ignored until the arm was held still. A raise still has to settle.
    // Once the light is out there is no shallow look to keep lit, so leaving
    // the viewing region is enough; the wide exit region otherwise stranded a
    // partly lowered wrist in VIEWING.
    bool released_out = d->view_released && !in_view(d, raw) && !in_view(d, d->gravity);
    if (is_lowered || released_out) {
      if (d->lower_samples < RTW_LOWER_CONFIRM_SAMPLES) ++d->lower_samples;
    } else {
      d->lower_samples = 0;
    }
    // Resting at a keyboard or desk often stays inside the wide exit region,
    // which used to leave VIEWING stuck so the next raise was ignored. A pose
    // settled clearly outside the viewing region for a refresh window counts
    // as lowered too; the margin keeps a slightly shallow look lit.
    bool away = stable &&
                distance_to_view_squared(d, raw) >= RTW_REARM_VIEW_MARGIN_MG * RTW_REARM_VIEW_MARGIN_MG &&
                distance_to_view_squared(d, d->gravity) >= RTW_REARM_VIEW_MARGIN_MG * RTW_REARM_VIEW_MARGIN_MG;
    if (!away) {
      d->rest_samples = 0;
    } else if (!d->rest_samples || distance_squared(d->gravity, d->rest_anchor) >
               RTW_REST_REFRESH_DRIFT_MG * RTW_REST_REFRESH_DRIFT_MG) {
      d->rest_anchor = d->gravity;
      d->rest_samples = 1;
    } else if (d->rest_samples < RTW_REST_REFRESH_SAMPLES) {
      ++d->rest_samples;
    }
    if (d->lower_samples >= RTW_LOWER_CONFIRM_SAMPLES ||
        d->rest_samples >= RTW_REST_REFRESH_SAMPLES) {
      if (d->state == RTW_VIEWING) {
        // Lowering is reported promptly even if a short cooldown is still running.
        // A small dip (settled just outside the viewing region, or leaving it
        // after the light) is not a full lowering; the worker then must not
        // cut a light someone may still be using.
        d->lowered_fully = is_lowered;
        d->state = RTW_WAIT_LOWERED;
        d->have_view_entry = false;
        return RTW_EVENT_LOWERED;
      }
      if (!d->cooldown) {
        d->resting = d->gravity;
        d->view_samples = d->lower_samples = d->rest_samples = 0;
        d->state = RTW_ARMED;
        d->block_reason = RTW_BLOCK_NONE;
      } else {
        d->block_reason = RTW_BLOCK_COOLDOWN;
      }
    }
    return RTW_EVENT_NONE;
  }

  // A reference held from an earlier arm position exaggerates later typing
  // tilts. Refresh after any settled window outside the viewing region (not
  // only fully lowered: typing poses sit between the two boundaries), measured
  // against one anchor so a slow continuous raise cannot accumulate as rest.
  // Arming from WAIT_LOWERED above still requires a genuinely lowered pose.
  if (!in_view(d, raw) && !in_view(d, d->gravity) && stable) {
    if (!d->rest_samples || distance_squared(d->gravity, d->rest_anchor) >
        RTW_REST_REFRESH_DRIFT_MG * RTW_REST_REFRESH_DRIFT_MG) {
      d->rest_anchor = d->gravity;
      d->rest_samples = 1;
    } else {
      ++d->rest_samples;
    }
    if (d->rest_samples >= RTW_REST_REFRESH_SAMPLES) {
      d->resting = d->gravity;
      d->view_samples = d->lower_samples = d->rest_samples = 0;
      d->have_view_entry = false;
      d->state = RTW_ARMED;
      d->block_reason = RTW_BLOCK_REST_REFRESH;
      return RTW_EVENT_NONE;
    }
  } else {
    d->rest_samples = 0;
  }

  int64_t rotation = distance_squared(d->resting, d->gravity);
  // A fixed dot product with one ideal viewing angle rejects flat-to-vertical
  // raises: Z becomes less negative as Y becomes more negative. Measure
  // progress toward the whole accepted region instead, on all three axes.
  // A reference resting just outside the region can never shrink its
  // distance by the full progress amount, which blocked every raise from a
  // wrist parked near the viewing angle. Reaching the region counts as
  // progress then; the rotation threshold still demands a real turn.
  int64_t rest_distance = distance_to_view_squared(d, d->resting);
  int64_t view_distance = distance_to_view_squared(d, d->gravity);
  bool toward_view = rest_distance - view_distance >= RTW_VIEW_PROGRESS_MG_SQUARED ||
                     (view_distance == 0 && rest_distance > 0);
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
    // This attempt has not lit the screen. Keeping the old lowered reference
    // would let ARMED re-enter ROTATING on the very next sample, so a wrist
    // parked near the viewing region (typing, reading a desk) would stay one
    // small tilt away from waking. Re-anchor to the current pose instead: a
    // later raise must again cover the full rotation from here.
    // VIEWING is handled above and can only unlock after actual lowering.
    // Only ever anchor outside the region: a reference inside it can never
    // make progress, so raises would stay blocked until a full lowering.
    if (!in_view(d, d->gravity)) d->resting = d->gravity;
    d->state = RTW_ARMED;
    d->view_samples = d->lower_samples = 0;
    d->have_view_entry = false;
    d->block_reason = RTW_BLOCK_TIMEOUT;
    return RTW_EVENT_NONE;
  }
  if (in_view(d, raw)) {
    if (!d->have_view_entry) {
      d->view_entry_ms = now;
      d->have_view_entry = true;
    }
  } else {
    d->have_view_entry = false;
  }
  // A palm flipped up also faces the screen down, so the lying view asks more.
  const bool lying = in_lying_view(d, d->gravity) && !in_box(d->gravity, UPRIGHT_VIEW);
  int32_t required_rotation = d->rotation_total_mg;
  if (lying && required_rotation < RTW_LYING_ROTATION_MG) required_rotation = RTW_LYING_ROTATION_MG;
  const unsigned required_samples =
      d->required_view_samples + (lying ? RTW_LYING_EXTRA_CONFIRM_SAMPLES : 0);
  if (!is_view || !stable || !toward_view ||
      rotation < (int64_t)required_rotation * required_rotation) {
    d->block_reason = !is_view ? RTW_BLOCK_VIEW : (!stable ? RTW_BLOCK_MOTION :
                      (!toward_view ? RTW_BLOCK_DIRECTION : RTW_BLOCK_ROTATION));
    d->view_samples = 0;
    d->state = RTW_ROTATING;
    return RTW_EVENT_NONE;
  }
  d->state = RTW_CONFIRMING_VIEW;
  d->block_reason = RTW_BLOCK_CONFIRMATION;
  if (d->view_samples == 0) d->tremor_sum = 0;
  if (d->view_samples >= RTW_LYING_TREMOR_SKIP_SAMPLES) d->tremor_sum += tremor;
  if (++d->view_samples >= required_samples) {
    const int64_t tremor_samples = required_samples - RTW_LYING_TREMOR_SKIP_SAMPLES;
    if (lying && d->tremor_sum < (int64_t)RTW_LYING_MIN_TREMOR_MG * RTW_LYING_MIN_TREMOR_MG *
                                     tremor_samples) {
      // A hand resting palm-up, not one held up to look. Start over from this
      // pose, so twitches while it rests cannot complete the turn later.
      d->resting = d->gravity;
      d->view_samples = 0;
      d->have_view_entry = false;
      d->state = RTW_ARMED;
      d->block_reason = RTW_BLOCK_RESTING_HAND;
      return RTW_EVENT_NONE;
    }
    d->state = RTW_VIEWING;
    d->block_reason = RTW_BLOCK_NONE;
    d->view_to_raise_ms = (uint32_t)(now - d->view_entry_ms);
    d->view_released = false;
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
    case RTW_BLOCK_REST_REFRESH: return "rest-refreshed";
    case RTW_BLOCK_RESTING_HAND: return "resting-hand";
  }
  return "unknown";
}
