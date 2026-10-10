#include <pebble.h>
#include "raise_to_wake_config.h"

// Persistent keys used only by the app. Shared keys are in the config header.
#define START_HOUR    0
#define START_MINUTE  1
#define STOP_HOUR     2
#define STOP_MINUTE   3
#define SCHEDULE      14
#define STARTED_ONCE  15 // Set after the first launch has started the worker.
#define POSITION_COUNT 19 // Number of recorded positions, then the positions themselves.
#define POSITION_FIRST 20
#define MAX_POSITIONS  8

#define TIME_START 0 // Wakeup cookies; the worker is started / stopped at these.
#define TIME_STOP  1

#define MINUTES 60
#define HOURS   (60 * MINUTES)
#define DAYS    (24 * HOURS)

#ifdef PBL_COLOR
#define ACCENT GColorChromeYellow
#define ACCENT_TEXT GColorBlack
#else
#define ACCENT GColorBlack
#define ACCENT_TEXT GColorWhite
#endif

static const uint8_t s_durations[] = {3, 5, 8, 10, 15, 30, 0}; // 0 = until lowered
static const char *s_sensitivity_names[] = {"High", "Normal", "Low"};
static const char *s_charger_names[] = {"Off", "While charging", "Plugged in"};
static const char *s_sleep_pause_names[] = {"Off", "Deep sleep", "Any sleep"};
// Sleep pause needs Pebble Health, which not every watch has.
#if defined(PBL_HEALTH)
#define HAS_SLEEP_PAUSE 1
#else
#define HAS_SLEEP_PAUSE 0
#endif

static Window *s_window;
static MenuLayer *s_menu;
static AppTimer *s_refresh_timer;

static bool s_worker_on, s_ambient, s_logging, s_charging, s_plugged, s_schedule;
static int s_duration = RTW_DEFAULT_LIGHT_DURATION_SECONDS;
static int s_sensitivity = RTW_DEFAULT_SENSITIVITY;
static int s_sleep_pause = RTW_SLEEP_PAUSE_OFF;
static int s_start_hour = 7, s_start_min = 0, s_stop_hour = 23, s_stop_min = 0;


/****************************************************************************
 * Settings and the background worker
 ****************************************************************************/

static int read_int(uint32_t key, int fallback) {
  return persist_exists(key) ? (int)persist_read_int(key) : fallback;
}

static void load_settings(void) {
  s_duration = read_int(RTW_DURATION_PERSIST_KEY, RTW_DEFAULT_LIGHT_DURATION_SECONDS);
  if (s_duration < 0 || s_duration > RTW_MAX_LIGHT_DURATION_SECONDS) {
    s_duration = RTW_DEFAULT_LIGHT_DURATION_SECONDS;
  }
  s_sensitivity = read_int(RTW_SENSITIVITY_PERSIST_KEY, RTW_DEFAULT_SENSITIVITY);
  if (s_sensitivity < RTW_SENSITIVITY_HIGH || s_sensitivity > RTW_SENSITIVITY_LOW) {
    s_sensitivity = RTW_DEFAULT_SENSITIVITY;
  }
  s_charging = persist_read_bool(RTW_CHARGING_PERSIST_KEY);
  s_plugged = persist_read_bool(RTW_PLUGGED_PERSIST_KEY);
  s_ambient = persist_read_bool(RTW_AMBIENT_PERSIST_KEY);
  s_logging = persist_read_bool(RTW_LOGGING_PERSIST_KEY);
  s_schedule = persist_read_bool(SCHEDULE);
  s_sleep_pause = read_int(RTW_SLEEP_PAUSE_PERSIST_KEY, RTW_SLEEP_PAUSE_OFF);
  if (s_sleep_pause < RTW_SLEEP_PAUSE_OFF || s_sleep_pause > RTW_SLEEP_PAUSE_ANY) {
    s_sleep_pause = RTW_SLEEP_PAUSE_OFF;
  }
  s_start_hour = read_int(START_HOUR, 7) % 24;
  s_start_min = read_int(START_MINUTE, 0) % 60;
  s_stop_hour = read_int(STOP_HOUR, 23) % 24;
  s_stop_min = read_int(STOP_MINUTE, 0) % 60;
}

// A running worker picks settings up immediately; a stopped one reads storage.
static void send_settings(void) {
  if (!app_worker_is_running()) return;
  AppWorkerMessage message = {
    .data0 = (uint16_t)s_duration,
    .data1 = (uint16_t)s_sensitivity,
    .data2 = (s_charging ? RTW_SETTING_CHARGING : 0) | (s_plugged ? RTW_SETTING_PLUGGED : 0) |
             (s_ambient ? RTW_SETTING_AMBIENT : 0) | (s_logging ? RTW_SETTING_LOGGING : 0) |
             ((s_sleep_pause << RTW_SETTING_SLEEP_SHIFT) & RTW_SETTING_SLEEP_MASK),
  };
  app_worker_send_message(RTW_MSG_SETTINGS, &message);
}

// The user is pressing buttons here, so the worker must not switch off a
// raise-to-wake light on its own timer; it hands the light to the system.
static void hand_off_light(void) {
  if (!app_worker_is_running()) return;
  AppWorkerMessage message = {0};
  app_worker_send_message(RTW_MSG_HAND_OFF_LIGHT, &message);
}

static void set_worker(bool on) {
  if (on) {
    AppWorkerResult result = app_worker_launch();
    // The watch may ask to confirm switching background apps; assume yes and
    // let the refresh on return to this window correct it.
    s_worker_on = result == APP_WORKER_RESULT_SUCCESS ||
                  result == APP_WORKER_RESULT_ALREADY_RUNNING ||
                  result == APP_WORKER_RESULT_ASKING_CONFIRMATION;
  } else {
    app_worker_kill();
    s_worker_on = false;
  }
}


/****************************************************************************
 * Schedule: start the worker at one time of day and stop it at another
 ****************************************************************************/

static WakeupId schedule_daily(int hour, int min, int cookie) {
  time_t now = time(NULL);
  struct tm *local = localtime(&now);
  int32_t delta = (hour - local->tm_hour) * HOURS + (min - local->tm_min) * MINUTES -
                  local->tm_sec;
  if (delta <= 0) delta += DAYS;
  time_t when = now + delta;
  WakeupId id = wakeup_schedule(when, cookie, true);
  // Another wakeup within a minute of this one: shift a minute earlier.
  for (int tries = 0; id == E_RANGE && tries < 5; ++tries) {
    when -= MINUTES;
    id = wakeup_schedule(when, cookie, true);
  }
  return id;
}

static bool inside_schedule(void) {
  time_t now = time(NULL);
  struct tm *local = localtime(&now);
  int t = local->tm_hour * 60 + local->tm_min;
  int start = s_start_hour * 60 + s_start_min, stop = s_stop_hour * 60 + s_stop_min;
  if (start == stop) return true;
  return start < stop ? (t >= start && t < stop) : (t >= start || t < stop);
}

// Before the schedule switch existed, both times were always scheduled at
// 00:00, so a worker turned off in the app came back on every midnight.
static void update_schedule(bool apply_now) {
  wakeup_cancel_all();
  if (!s_schedule) return;
  schedule_daily(s_start_hour, s_start_min, TIME_START);
  schedule_daily(s_stop_hour, s_stop_min, TIME_STOP);
  if (apply_now) set_worker(inside_schedule());
}


/****************************************************************************
 * Time picker: boxed fields like the built-in Alarms app
 ****************************************************************************/

static Window *s_picker_window;
static Layer *s_picker_layer;
static int s_picker_which, s_picker_field, s_picker_hour, s_picker_min;

static int picker_fields(void) { return clock_is_24h_style() ? 2 : 3; }

static void picker_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  bool h24 = clock_is_24h_style();
  int count = picker_fields(), width = h24 ? 50 : 40, gap = 10, height = 44;
  int x = (bounds.size.w - (count * width + (count - 1) * gap)) / 2;
  int y = bounds.size.h / 2 - height / 2 + 8;
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);

  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, s_picker_which == TIME_START ? "Turn on at" : "Turn off at",
                     fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(0, y - 46, bounds.size.w, 30), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  char text[3][4];
  snprintf(text[0], sizeof(text[0]), h24 ? "%02d" : "%d",
           h24 ? s_picker_hour : (s_picker_hour + 11) % 12 + 1);
  snprintf(text[1], sizeof(text[1]), "%02d", s_picker_min);
  snprintf(text[2], sizeof(text[2]), "%s", s_picker_hour < 12 ? "AM" : "PM");

  for (int i = 0; i < count; ++i) {
    GRect box = GRect(x + i * (width + gap), y, width, height);
    if (i == s_picker_field) {
      graphics_context_set_fill_color(ctx, ACCENT);
      graphics_fill_rect(ctx, box, 6, GCornersAll);
      graphics_context_set_text_color(ctx, ACCENT_TEXT);
    } else {
      graphics_context_set_stroke_color(ctx, GColorBlack);
      graphics_draw_round_rect(ctx, box, 6);
      graphics_context_set_text_color(ctx, GColorBlack);
    }
    graphics_draw_text(ctx, text[i], font, GRect(box.origin.x, box.origin.y + 3, width, height - 3),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, ":", font, GRect(x + width, y + 1, gap, height), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);
}

static void picker_change(int step) {
  if (s_picker_field == 0) {
    if (clock_is_24h_style()) {
      s_picker_hour = (s_picker_hour + step + 24) % 24;
    } else { // Stay within AM or PM; that is its own field.
      int half = s_picker_hour >= 12 ? 12 : 0;
      s_picker_hour = half + (s_picker_hour % 12 + step + 12) % 12;
    }
  } else if (s_picker_field == 1) {
    s_picker_min = (s_picker_min + step + 60) % 60;
  } else {
    s_picker_hour = (s_picker_hour + 12) % 24;
  }
  layer_mark_dirty(s_picker_layer);
}

static void picker_up(ClickRecognizerRef recognizer, void *context) { picker_change(1); }
static void picker_down(ClickRecognizerRef recognizer, void *context) { picker_change(-1); }

static void picker_select(ClickRecognizerRef recognizer, void *context) {
  if (++s_picker_field < picker_fields()) {
    layer_mark_dirty(s_picker_layer);
    return;
  }
  if (s_picker_which == TIME_START) {
    s_start_hour = s_picker_hour;
    s_start_min = s_picker_min;
    persist_write_int(START_HOUR, s_start_hour);
    persist_write_int(START_MINUTE, s_start_min);
  } else {
    s_stop_hour = s_picker_hour;
    s_stop_min = s_picker_min;
    persist_write_int(STOP_HOUR, s_stop_hour);
    persist_write_int(STOP_MINUTE, s_stop_min);
  }
  update_schedule(true);
  window_stack_pop(true);
}

static void picker_back(ClickRecognizerRef recognizer, void *context) {
  if (s_picker_field > 0) {
    --s_picker_field;
    layer_mark_dirty(s_picker_layer);
  } else {
    window_stack_pop(true); // Cancel without saving.
  }
}

static void picker_click_config(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, picker_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, picker_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, picker_select);
  window_single_click_subscribe(BUTTON_ID_BACK, picker_back);
}

static void picker_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_picker_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_picker_layer, picker_update);
  layer_add_child(root, s_picker_layer);
}

static void picker_unload(Window *window) {
  layer_destroy(s_picker_layer);
  window_destroy(s_picker_window);
  s_picker_window = NULL;
}

static void open_picker(int which) {
  s_picker_which = which;
  s_picker_field = 0;
  s_picker_hour = which == TIME_START ? s_start_hour : s_stop_hour;
  s_picker_min = which == TIME_START ? s_start_min : s_stop_min;
  s_picker_window = window_create();
  window_set_background_color(s_picker_window, GColorWhite);
  window_set_click_config_provider(s_picker_window, picker_click_config);
  window_set_window_handlers(s_picker_window, (WindowHandlers) {
    .load = picker_load,
    .unload = picker_unload,
  });
  window_stack_push(s_picker_window, true);
}


/****************************************************************************
 * Record position: capture where the wrist is, to tune what counts as looking
 ****************************************************************************/

typedef struct { int16_t x, y, z; } Position;

#define COUNTDOWN_SECONDS 3
#define RECORD_SAMPLES 25 // One second at 25 Hz, averaged.

static Window *s_record_window;
static TextLayer *s_record_text;
static AppTimer *s_record_timer;
static int s_countdown;
static int32_t s_sum_x, s_sum_y, s_sum_z;
static int s_sample_count;
static char s_record_buffer[96];

static bool in_box(Position p, int x0, int x1, int y0, int y1, int z0, int z1) {
  return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1 && p.z >= z0 && p.z <= z1;
}

// Same regions as the detector: viewing, lowered (outside the wider exit region), or between.
static const char *classify(Position p) {
  if (in_box(p, RTW_VIEW_X_MIN_MG, RTW_VIEW_X_MAX_MG, RTW_VIEW_Y_MIN_MG, RTW_VIEW_Y_MAX_MG,
             RTW_VIEW_Z_MIN_MG, RTW_VIEW_Z_MAX_MG)) {
    return "viewing";
  }
  if (in_box(p, RTW_LYING_VIEW_X_MIN_MG, RTW_LYING_VIEW_X_MAX_MG, RTW_LYING_VIEW_Y_MIN_MG,
             RTW_LYING_VIEW_Y_MAX_MG, RTW_LYING_VIEW_Z_MIN_MG, RTW_LYING_VIEW_Z_MAX_MG)) {
    return "bed view";
  }
  if (in_box(p, RTW_RETAIN_X_MIN_MG, RTW_RETAIN_X_MAX_MG, RTW_RETAIN_Y_MIN_MG, RTW_RETAIN_Y_MAX_MG,
             RTW_RETAIN_Z_MIN_MG, RTW_RETAIN_Z_MAX_MG)) {
    return "between";
  }
  return "lowered";
}

static int position_count(void) {
  int count = persist_exists(POSITION_COUNT) ? (int)persist_read_int(POSITION_COUNT) : 0;
  return (count < 0 || count > MAX_POSITIONS) ? 0 : count;
}

static void save_position(Position p) {
  int count = position_count();
  if (count == MAX_POSITIONS) { // Drop the oldest.
    for (int i = 1; i < MAX_POSITIONS; ++i) {
      Position older;
      persist_read_data(POSITION_FIRST + i, &older, sizeof(older));
      persist_write_data(POSITION_FIRST + i - 1, &older, sizeof(older));
    }
    count--;
  }
  persist_write_data(POSITION_FIRST + count, &p, sizeof(p));
  persist_write_int(POSITION_COUNT, count + 1);
}

static void record_accel(AccelData *data, uint32_t num_samples) {
  for (uint32_t i = 0; i < num_samples && s_sample_count < RECORD_SAMPLES; ++i) {
    s_sum_x += data[i].x;
    s_sum_y += data[i].y;
    s_sum_z += data[i].z;
    s_sample_count++;
  }
  if (s_sample_count < RECORD_SAMPLES) return;
  accel_data_service_unsubscribe();
  Position p = {s_sum_x / s_sample_count, s_sum_y / s_sample_count, s_sum_z / s_sample_count};
  save_position(p);
  vibes_double_pulse();
  APP_LOG(APP_LOG_LEVEL_INFO, "RTW position %d,%d,%d %s", p.x, p.y, p.z, classify(p));
  snprintf(s_record_buffer, sizeof(s_record_buffer), "Saved #%d\n%d, %d, %d\n%s",
           position_count(), p.x, p.y, p.z, classify(p));
  text_layer_set_text(s_record_text, s_record_buffer);
}

static void countdown_tick(void *context) {
  s_record_timer = NULL;
  if (s_countdown > 0) {
    snprintf(s_record_buffer, sizeof(s_record_buffer), "Hold your wrist where you want it\n\n%d",
             s_countdown--);
    text_layer_set_text(s_record_text, s_record_buffer);
    s_record_timer = app_timer_register(1000, countdown_tick, NULL);
    return;
  }
  vibes_short_pulse();
  text_layer_set_text(s_record_text, "Recording...\nkeep still");
  s_sum_x = s_sum_y = s_sum_z = 0;
  s_sample_count = 0;
  accel_data_service_subscribe(5, record_accel);
  accel_service_set_sampling_rate(ACCEL_SAMPLING_25HZ);
}

static void record_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_record_text = text_layer_create(GRect(4, bounds.size.h / 2 - 50, bounds.size.w - 8, 100));
  text_layer_set_font(s_record_text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_record_text, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_record_text));
  s_countdown = COUNTDOWN_SECONDS;
  countdown_tick(NULL);
}

static void record_unload(Window *window) {
  if (s_record_timer) app_timer_cancel(s_record_timer);
  s_record_timer = NULL;
  accel_data_service_unsubscribe();
  text_layer_destroy(s_record_text);
  window_destroy(s_record_window);
  s_record_window = NULL;
}

static void open_record(void) {
  s_record_window = window_create();
  window_set_window_handlers(s_record_window, (WindowHandlers) {
    .load = record_load,
    .unload = record_unload,
  });
  window_stack_push(s_record_window, true);
}

// Saved positions: a scrolling list; hold Select to clear.
static Window *s_positions_window;
static ScrollLayer *s_positions_scroll;
static TextLayer *s_positions_text;
static char s_positions_buffer[MAX_POSITIONS * 32 + 40];

static void fill_positions(void) {
  int count = position_count();
  int len = snprintf(s_positions_buffer, sizeof(s_positions_buffer), "%s",
                     count ? "x, y, z (hold Select to clear)\n" : "None yet.\nUse Record position.");
  for (int i = 0; i < count && len < (int)sizeof(s_positions_buffer); ++i) {
    Position p;
    persist_read_data(POSITION_FIRST + i, &p, sizeof(p));
    len += snprintf(s_positions_buffer + len, sizeof(s_positions_buffer) - len,
                    "%d: %d, %d, %d %s\n", i + 1, p.x, p.y, p.z, classify(p));
    // Also to the app log, so the list can be copied from a log session.
    APP_LOG(APP_LOG_LEVEL_INFO, "RTW saved position %d: %d,%d,%d %s", i + 1, p.x, p.y, p.z,
            classify(p));
  }
  text_layer_set_text(s_positions_text, s_positions_buffer);
  GSize size = text_layer_get_content_size(s_positions_text);
  scroll_layer_set_content_size(s_positions_scroll, GSize(size.w, size.h + 8));
}

static void clear_positions(ClickRecognizerRef recognizer, void *context) {
  persist_write_int(POSITION_COUNT, 0);
  vibes_short_pulse();
  fill_positions();
}

static void positions_click_config(void *context) {
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, clear_positions, NULL);
}

static void positions_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_positions_scroll = scroll_layer_create(bounds);
  scroll_layer_set_callbacks(s_positions_scroll, (ScrollLayerCallbacks) {
    .click_config_provider = positions_click_config,
  });
  scroll_layer_set_click_config_onto_window(s_positions_scroll, window);
  s_positions_text = text_layer_create(GRect(4, 0, bounds.size.w - 8, 2000));
  text_layer_set_font(s_positions_text, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  scroll_layer_add_child(s_positions_scroll, text_layer_get_layer(s_positions_text));
  layer_add_child(root, scroll_layer_get_layer(s_positions_scroll));
  fill_positions();
}

static void positions_unload(Window *window) {
  text_layer_destroy(s_positions_text);
  scroll_layer_destroy(s_positions_scroll);
  window_destroy(s_positions_window);
  s_positions_window = NULL;
}

static void open_positions(void) {
  s_positions_window = window_create();
  window_set_window_handlers(s_positions_window, (WindowHandlers) {
    .load = positions_load,
    .unload = positions_unload,
  });
  window_stack_push(s_positions_window, true);
}


/****************************************************************************
 * Settings menu
 ****************************************************************************/

typedef enum {
  ROW_ENABLED, ROW_SENSITIVITY,
  ROW_DURATION, ROW_SENSOR, ROW_CHARGER,
  ROW_SCHEDULE, ROW_START, ROW_STOP, ROW_SLEEP_PAUSE,
  ROW_LOGGING, ROW_RECORD, ROW_POSITIONS,
} Row;

enum { SECTION_WAKE, SECTION_LIGHT, SECTION_SCHEDULE, SECTION_ADVANCED, NUM_SECTIONS };
static const char *s_section_titles[] = {"Raise to wake", "Light", "Schedule", "Advanced"};

static Row row_at(MenuIndex *index) {
  switch (index->section) {
    case SECTION_WAKE: return ROW_ENABLED + index->row;
    case SECTION_LIGHT: return ROW_DURATION + index->row;
    case SECTION_SCHEDULE: {
      // Schedule, its two times when in use, then sleep pause.
      const int time_rows = s_schedule ? 3 : 1;
      return index->row < time_rows ? ROW_SCHEDULE + index->row : ROW_SLEEP_PAUSE;
    }
    default: return ROW_LOGGING + index->row;
  }
}

static uint16_t get_num_sections(MenuLayer *menu, void *context) { return NUM_SECTIONS; }

static uint16_t get_num_rows(MenuLayer *menu, uint16_t section, void *context) {
  switch (section) {
    case SECTION_WAKE: return 2;
    case SECTION_LIGHT: return 3;
    case SECTION_SCHEDULE: return (s_schedule ? 3 : 1) + HAS_SLEEP_PAUSE; // Times only when in use.
    default: return 3; // Logging, record position, saved positions.
  }
}

static int16_t get_header_height(MenuLayer *menu, uint16_t section, void *context) {
  return PBL_IF_ROUND_ELSE(0, MENU_CELL_BASIC_HEADER_HEIGHT);
}

static void draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *context) {
  menu_cell_basic_header_draw(ctx, cell, s_section_titles[section]);
}

static int16_t get_cell_height(MenuLayer *menu, MenuIndex *index, void *context) {
  return PBL_IF_ROUND_ELSE(menu_layer_is_index_selected(menu, index) ?
                           MENU_CELL_ROUND_FOCUSED_TALL_CELL_HEIGHT :
                           MENU_CELL_ROUND_UNFOCUSED_SHORT_CELL_HEIGHT, 44);
}

static void format_time(char *buffer, size_t size, int hour, int min, bool compact) {
  if (clock_is_24h_style()) {
    snprintf(buffer, size, "%02d:%02d", hour, min);
  } else {
    snprintf(buffer, size, compact ? "%d:%02d%s" : "%d:%02d %s", (hour + 11) % 12 + 1, min,
             hour < 12 ? (compact ? "a" : "AM") : (compact ? "p" : "PM"));
  }
}

#if !defined(PBL_ROUND) // Round screens show "On" / "Off" instead.
// Check mark for on, cross for off, drawn so it follows the highlight colours.
static void draw_toggle(GContext *ctx, GRect cell, bool on, bool highlighted) {
  GPoint o = GPoint(cell.size.w - 24, cell.size.h / 2 - 7);
#ifdef PBL_COLOR
  GColor color = highlighted ? ACCENT_TEXT : (on ? GColorIslamicGreen : GColorRed);
  graphics_context_set_antialiased(ctx, true);
#else
  GColor color = highlighted ? ACCENT_TEXT : GColorBlack;
#endif
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 3);
  if (on) {
    graphics_draw_line(ctx, GPoint(o.x + 1, o.y + 7), GPoint(o.x + 5, o.y + 12));
    graphics_draw_line(ctx, GPoint(o.x + 5, o.y + 12), GPoint(o.x + 14, o.y + 1));
  } else {
    graphics_draw_line(ctx, GPoint(o.x + 2, o.y + 2), GPoint(o.x + 12, o.y + 12));
    graphics_draw_line(ctx, GPoint(o.x + 12, o.y + 2), GPoint(o.x + 2, o.y + 12));
  }
  graphics_context_set_stroke_width(ctx, 1);
}
#endif

static void draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *context) {
  static char value[24];
  const char *title = "", *subtitle = NULL;
  int toggle = -1; // -1 = value row, 0 = off, 1 = on

  switch (row_at(index)) {
    case ROW_ENABLED:
      title = "Enabled"; subtitle = "Light on raise"; toggle = s_worker_on;
      break;
    case ROW_SENSITIVITY:
      title = "Sensitivity"; subtitle = s_sensitivity_names[s_sensitivity];
      break;
    case ROW_DURATION:
      title = "Light duration";
      if (s_ambient) {
        subtitle = "Watch default";
      } else if (s_duration) {
        snprintf(value, sizeof(value), "%d seconds", s_duration);
        subtitle = value;
      } else {
        subtitle = "Until lowered";
      }
      break;
    case ROW_SENSOR:
      title = "Light sensor"; subtitle = "Only when dark"; toggle = s_ambient;
      break;
    case ROW_CHARGER:
      title = "Charger light";
      subtitle = s_charger_names[s_plugged ? 2 : (s_charging ? 1 : 0)];
      break;
    case ROW_SCHEDULE:
      title = "Schedule"; toggle = s_schedule;
      if (s_schedule) {
        char start[8], stop[8];
        format_time(start, sizeof(start), s_start_hour, s_start_min, true);
        format_time(stop, sizeof(stop), s_stop_hour, s_stop_min, true);
        snprintf(value, sizeof(value), "%s - %s", start, stop);
        subtitle = value;
      } else {
        subtitle = "Runs all day";
      }
      break;
    case ROW_SLEEP_PAUSE:
      title = "Sleep pause"; subtitle = s_sleep_pause_names[s_sleep_pause];
      break;
    case ROW_START:
    case ROW_STOP: {
      bool start = row_at(index) == ROW_START;
      title = start ? "Turn on at" : "Turn off at";
      format_time(value, sizeof(value), start ? s_start_hour : s_stop_hour,
                  start ? s_start_min : s_stop_min, false);
      subtitle = value;
      break;
    }
    case ROW_LOGGING:
      title = "Logging"; subtitle = s_logging ? "Uses battery" : "For tuning"; toggle = s_logging;
      break;
    case ROW_RECORD:
      title = "Record position"; subtitle = "Where it should wake";
      break;
    case ROW_POSITIONS:
      title = "Saved positions";
      snprintf(value, sizeof(value), "%d saved", position_count());
      subtitle = value;
      break;
  }

#if defined(PBL_ROUND)
  if (toggle >= 0) subtitle = toggle ? "On" : "Off";
  menu_cell_basic_draw(ctx, cell, title, subtitle, NULL);
#else
  GRect bounds = layer_get_bounds(cell);
  bool highlighted = menu_cell_layer_is_highlighted(cell);
  int width = bounds.size.w - 10 - (toggle >= 0 ? 26 : 0);
  graphics_context_set_text_color(ctx, highlighted ? ACCENT_TEXT : GColorBlack);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(5, -3, width, 28), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, subtitle, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     GRect(5, 21, width, 22), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  if (toggle >= 0) draw_toggle(ctx, bounds, toggle, highlighted);
#endif
}

static void select_click(MenuLayer *menu, MenuIndex *index, void *context) {
  switch (row_at(index)) {
    case ROW_ENABLED:
      set_worker(!s_worker_on);
      break;
    case ROW_SENSITIVITY:
      s_sensitivity = (s_sensitivity + 1) % 3;
      persist_write_int(RTW_SENSITIVITY_PERSIST_KEY, s_sensitivity);
      send_settings();
      break;
    case ROW_DURATION: {
      if (s_ambient) return; // The watch's own timeout applies with the sensor on.
      // Next preset after the current value; unknown values jump to a preset.
      unsigned count = ARRAY_LENGTH(s_durations), next = 0;
      for (unsigned i = 0; i < count; ++i) {
        if (s_durations[i] == s_duration) { next = (i + 1) % count; break; }
        if (s_durations[i] > s_duration) { next = i; break; }
      }
      s_duration = s_durations[next];
      persist_write_int(RTW_DURATION_PERSIST_KEY, s_duration);
      send_settings();
      break;
    }
    case ROW_SENSOR:
      s_ambient = !s_ambient;
      persist_write_bool(RTW_AMBIENT_PERSIST_KEY, s_ambient);
      send_settings();
      break;
    case ROW_CHARGER: {
      int next = (s_plugged ? 2 : (s_charging ? 1 : 0)) + 1;
      s_charging = next == 1;
      s_plugged = next == 2;
      persist_write_bool(RTW_CHARGING_PERSIST_KEY, s_charging);
      persist_write_bool(RTW_PLUGGED_PERSIST_KEY, s_plugged);
      send_settings();
      break;
    }
    case ROW_SCHEDULE:
      s_schedule = !s_schedule;
      persist_write_bool(SCHEDULE, s_schedule);
      update_schedule(true);
      break;
    case ROW_START:
      open_picker(TIME_START);
      return;
    case ROW_STOP:
      open_picker(TIME_STOP);
      return;
    case ROW_SLEEP_PAUSE:
      s_sleep_pause = (s_sleep_pause + 1) % ARRAY_LENGTH(s_sleep_pause_names);
      persist_write_int(RTW_SLEEP_PAUSE_PERSIST_KEY, s_sleep_pause);
      send_settings();
      break;
    case ROW_LOGGING:
      s_logging = !s_logging;
      persist_write_bool(RTW_LOGGING_PERSIST_KEY, s_logging);
      send_settings();
      break;
    case ROW_RECORD:
      open_record();
      return;
    case ROW_POSITIONS:
      open_positions();
      return;
  }
  menu_layer_reload_data(menu);
}

static void refresh_worker_state(void *context) {
  s_refresh_timer = NULL;
  s_worker_on = app_worker_is_running();
  menu_layer_reload_data(s_menu);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = get_num_sections,
    .get_num_rows = get_num_rows,
    .get_header_height = get_header_height,
    .draw_header = draw_header,
    .get_cell_height = get_cell_height,
    .draw_row = draw_row,
    .select_click = select_click,
  });
  menu_layer_set_normal_colors(s_menu, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_menu, ACCENT, ACCENT_TEXT);
  menu_layer_set_click_config_onto_window(s_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}

static void window_appear(Window *window) {
  // Back from the time picker or a "switch background app?" prompt. A newly
  // launched worker can take a moment to report running, so check shortly.
  s_worker_on = app_worker_is_running() || s_worker_on;
  menu_layer_reload_data(s_menu);
  if (s_refresh_timer) app_timer_cancel(s_refresh_timer);
  s_refresh_timer = app_timer_register(1000, refresh_worker_state, NULL);
}

static void window_disappear(Window *window) {
  if (s_refresh_timer) app_timer_cancel(s_refresh_timer);
  s_refresh_timer = NULL;
}

static void window_unload(Window *window) {
  window_disappear(window);
  menu_layer_destroy(s_menu);
}


/****************************************************************************
 * Launch
 ****************************************************************************/

static void handle_wakeup(int32_t cookie) {
  if (cookie == TIME_START) {
    app_worker_launch();
    schedule_daily(s_start_hour, s_start_min, TIME_START);
  } else {
    app_worker_kill();
    schedule_daily(s_stop_hour, s_stop_min, TIME_STOP);
  }
}

int main(void) {
  load_settings();

  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    WakeupId id;
    int32_t cookie = TIME_START;
    wakeup_get_launch_event(&id, &cookie);
    handle_wakeup(cookie);
    return 0;
  }

  // Opening the app used to kill and relaunch the worker, which cut any
  // raise-to-wake light and froze the screen for a second.
  s_worker_on = app_worker_is_running();
  if (s_worker_on) {
    hand_off_light();
  } else if (!persist_exists(STARTED_ONCE)) {
    set_worker(true); // First run: start raise to wake. Afterwards, respect "Enabled".
  }
  persist_write_bool(STARTED_ONCE, true);
  update_schedule(false);

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .appear = window_appear,
    .disappear = window_disappear,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
  app_event_loop();
  window_destroy(s_window);
  return 0;
}
