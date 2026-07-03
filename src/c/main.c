#include <pebble.h>
#include <string.h>
#include "generated_assets.h"

#define FRAME_MS 50
#define SLIDE_MS 7000
#define HOLD_MS 2000
#define TRANSITION_MS 900
#define BACKLIGHT_TIMEOUT_MS (5 * 60 * 1000)
#define AUDIO_ENCODED_BUFFER 128
#define AUDIO_PCM_BUFFER (AUDIO_ENCODED_BUFFER * 4 * GABAGOOL_AUDIO_UPSAMPLE)
#define GABAGOOL_MIN(a, b) ((a) < (b) ? (a) : (b))

typedef enum {
  TransitionSlide = 0,
  TransitionMosaic,
  TransitionChecker,
  TransitionBlinds,
  TransitionWipe,
  TransitionCount
} TransitionKind;

static Window *s_window;
static Layer *s_canvas;
static AppTimer *s_frame_timer;
static AppTimer *s_audio_timer;
static AppTimer *s_backlight_timer;
static GBitmap *s_current_bitmap;

static int s_current_index;
static int s_next_index;
static int s_phase_ms;
static int s_transition_ms;
static bool s_transitioning;
static TransitionKind s_transition_kind;

static GBitmap *load_image(int index);

#if defined(PBL_SPEAKER)
static void start_audio(void);
static void stop_audio(void);
static void play_random_track(void);

static bool s_audio_active;
static ResHandle s_audio_handle;
static uint32_t s_audio_chunk_index;
static uint32_t s_audio_chunk_offset;
static uint32_t s_audio_samples_remaining;
static int16_t s_audio_predictor;
static int s_audio_step_index;
static uint8_t s_encoded_buffer[AUDIO_ENCODED_BUFFER];
static int8_t s_pcm_buffer[AUDIO_PCM_BUFFER];
static uint32_t s_pcm_count;
static uint32_t s_pcm_offset;

static const int16_t s_audio_step_table[] = {
  2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 19, 22, 26, 31, 36,
  42, 49, 57, 66, 77, 89, 103, 119, 127,
};
static int s_audio_track_index;
#endif

static int modulo_index(int value) {
  while (value < 0) {
    value += GABAGOOL_IMAGE_COUNT;
  }
  return value % GABAGOOL_IMAGE_COUNT;
}

static int pan_offset_for(int image_index, int elapsed_ms) {
  int max_offset = GABAGOOL_PAN_W - GABAGOOL_SCREEN_W;
  int clamped = elapsed_ms;
  if (clamped < 0) {
    clamped = 0;
  } else if (clamped > SLIDE_MS) {
    clamped = SLIDE_MS;
  }
  
  // Cubic ease-in-ease-out interpolation using integer math
  int32_t t = (clamped * 1000) / SLIDE_MS;
  int32_t f;
  if (t < 500) {
    f = (4 * t * t * t) / 1000000;
  } else {
    int32_t dt = 1000 - t;
    f = 1000 - (4 * dt * dt * dt) / 1000000;
  }
  
  int offset = (max_offset * f) / 1000;
  return (image_index % 2) ? (max_offset - offset) : offset;
}

static void draw_panned_bitmap(GContext *ctx, GBitmap *bitmap, int image_index, int elapsed_ms) {
  if (!bitmap) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, GRect(0, 0, GABAGOOL_SCREEN_W, GABAGOOL_SCREEN_H), 0, GCornerNone);
    return;
  }
  int offset = pan_offset_for(image_index, elapsed_ms);
  graphics_draw_bitmap_in_rect(ctx, bitmap, GRect(-offset, 0, GABAGOOL_PAN_W, GABAGOOL_SCREEN_H));
}

static void fill_black_rect(GContext *ctx, GRect rect) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, rect, 0, GCornerNone);
}

static uint16_t tile_hash(int x, int y) {
  uint32_t n = (uint32_t)(x * 37 + y * 91 + s_current_index * 53);
  n = (n ^ (n << 7)) * 1103515245u;
  return (uint16_t)((n >> 16) & 0x3ff);
}

static void apply_tile_effect(GContext *ctx, int progress, bool checker, bool incoming) {
  const int tile = checker ? 20 : 16;

  for (int y = 0; y < GABAGOOL_SCREEN_H; y += tile) {
    for (int x = 0; x < GABAGOOL_SCREEN_W; x += tile) {
      int threshold;
      if (checker) {
        threshold = (((x / tile) + (y / tile)) & 1) ? 350 : 50;
      } else {
        threshold = tile_hash(x / tile, y / tile);
      }
      
      bool show_black = incoming ? (progress < threshold) : (progress >= threshold);
      if (show_black) {
        GRect rect = GRect(x, y, GABAGOOL_MIN(tile, GABAGOOL_SCREEN_W - x),
                           GABAGOOL_MIN(tile, GABAGOOL_SCREEN_H - y));
        fill_black_rect(ctx, rect);
      }
    }
  }
}

static void apply_blinds_effect(GContext *ctx, int progress, bool incoming) {
  const int stripe_w = 20;
  int reveal_h = (GABAGOOL_SCREEN_H * progress) / 1024;

  for (int x = 0; x < GABAGOOL_SCREEN_W; x += stripe_w) {
    int local_h = reveal_h - (((x / stripe_w) & 1) ? 24 : 0);
    if (local_h < 0) {
      local_h = 0;
    } else if (local_h > GABAGOOL_SCREEN_H) {
      local_h = GABAGOOL_SCREEN_H;
    }
    
    if (incoming) {
      if (local_h < GABAGOOL_SCREEN_H) {
        GRect rect = GRect(x, local_h, GABAGOOL_MIN(stripe_w, GABAGOOL_SCREEN_W - x), GABAGOOL_SCREEN_H - local_h);
        fill_black_rect(ctx, rect);
      }
    } else {
      if (local_h > 0) {
        GRect rect = GRect(x, 0, GABAGOOL_MIN(stripe_w, GABAGOOL_SCREEN_W - x), local_h);
        fill_black_rect(ctx, rect);
      }
    }
  }
}

static void apply_wipe_effect(GContext *ctx, int progress, bool incoming) {
  int w = (GABAGOOL_SCREEN_W * progress) / 1024;
  if (incoming) {
    if (w < GABAGOOL_SCREEN_W) {
      fill_black_rect(ctx, GRect(w, 0, GABAGOOL_SCREEN_W - w, GABAGOOL_SCREEN_H));
    }
  } else {
    if (w > 0) {
      fill_black_rect(ctx, GRect(0, 0, w, GABAGOOL_SCREEN_H));
    }
  }
}

static void swap_to_transition_target(void) {
  if (s_current_index == s_next_index && s_current_bitmap) {
    return;
  }
  if (s_current_bitmap) {
    gbitmap_destroy(s_current_bitmap);
    s_current_bitmap = NULL;
  }
  s_current_bitmap = load_image(s_next_index);
  if (s_current_bitmap) {
    s_current_index = s_next_index;
  }
}

static void draw_transition(GContext *ctx) {
  bool incoming = s_transition_ms >= (TRANSITION_MS / 2);
  if (incoming) {
    swap_to_transition_target();
  }

  int local_ms = incoming ? (s_transition_ms - (TRANSITION_MS / 2)) : s_transition_ms;
  int progress = (1024 * local_ms) / (TRANSITION_MS / 2);
  if (progress < 0) {
    progress = 0;
  } else if (progress > 1024) {
    progress = 1024;
  }

  if (s_transition_kind == TransitionSlide) {
    int x = (GABAGOOL_SCREEN_W * progress) / 1024;
    int current_offset = pan_offset_for(s_current_index, incoming ? 0 : SLIDE_MS);
    if (!incoming && s_current_bitmap) {
      graphics_draw_bitmap_in_rect(ctx, s_current_bitmap,
                                   GRect(-current_offset - x, 0, GABAGOOL_PAN_W, GABAGOOL_SCREEN_H));
    } else if (incoming && s_current_bitmap) {
      graphics_draw_bitmap_in_rect(ctx, s_current_bitmap,
                                   GRect(GABAGOOL_SCREEN_W - x - current_offset, 0,
                                         GABAGOOL_PAN_W, GABAGOOL_SCREEN_H));
    }
    return;
  }

  // Draw the full background first
  if (incoming) {
    draw_panned_bitmap(ctx, s_current_bitmap, s_current_index, 0);
  } else {
    draw_panned_bitmap(ctx, s_current_bitmap, s_current_index, SLIDE_MS);
  }

  switch (s_transition_kind) {
    case TransitionMosaic:
      apply_tile_effect(ctx, progress, false, incoming);
      break;
    case TransitionChecker:
      apply_tile_effect(ctx, progress, true, incoming);
      break;
    case TransitionBlinds:
      apply_blinds_effect(ctx, progress, incoming);
      break;
    case TransitionWipe:
      apply_wipe_effect(ctx, progress, incoming);
      break;
    default:
      apply_wipe_effect(ctx, progress, incoming);
      break;
  }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  (void)layer;
  if (s_transitioning) {
    draw_transition(ctx);
  } else {
    draw_panned_bitmap(ctx, s_current_bitmap, s_current_index, s_phase_ms);
  }
}

static GBitmap *load_image(int index) {
  return gbitmap_create_with_resource(GABAGOOL_IMAGE_RESOURCE_IDS[modulo_index(index)]);
}

static void begin_transition(int delta) {
  (void)delta;
  if (s_transitioning || GABAGOOL_IMAGE_COUNT < 2) {
    return;
  }
  int next_idx = s_current_index;
  while (next_idx == s_current_index) {
    next_idx = rand() % GABAGOOL_IMAGE_COUNT;
  }
  s_next_index = next_idx;
  s_transitioning = true;
  s_transition_ms = 0;
  s_transition_kind = (TransitionKind)((s_transition_kind + 1) % TransitionCount);
}

static void finish_transition(void) {
  if (s_current_index != s_next_index) {
    swap_to_transition_target();
  }
  s_phase_ms = 0;
  s_transitioning = false;
}

static void frame_timer_callback(void *context) {
  (void)context;
  if (s_transitioning) {
    s_transition_ms += FRAME_MS;
    if (s_transition_ms >= TRANSITION_MS) {
      finish_transition();
    }
  } else {
    s_phase_ms += FRAME_MS;
    if (s_phase_ms >= SLIDE_MS + HOLD_MS) {
      begin_transition(1);
    }
  }

  layer_mark_dirty(s_canvas);
  s_frame_timer = app_timer_register(FRAME_MS, frame_timer_callback, NULL);
}

#if defined(PBL_SPEAKER)
static void audio_schedule(int delay_ms);

static void audio_reset_decoder(void) {
  const GabagoolAudioTrack *track = &GABAGOOL_AUDIO_TRACKS[s_audio_track_index];
  s_audio_chunk_index = 0;
  s_audio_chunk_offset = 0;
  s_audio_samples_remaining = track->total_samples;
  s_audio_predictor = 0;
  s_audio_step_index = 10;
  s_audio_handle = resource_get_handle(track->chunk_resource_ids[0]);
  s_pcm_count = 0;
  s_pcm_offset = 0;
}

static int16_t clamp_audio(int16_t value) {
  if (value < -128) {
    return -128;
  }
  if (value > 127) {
    return 127;
  }
  return value;
}

static int decode_audio_code(uint8_t code) {
  int16_t step = s_audio_step_table[s_audio_step_index];
  int16_t diff = step / 2;
  if (code & 1) {
    diff += step;
  }
  if (code & 2) {
    s_audio_predictor -= diff;
  } else {
    s_audio_predictor += diff;
  }
  s_audio_predictor = clamp_audio(s_audio_predictor);
  s_audio_step_index += (code & 1) ? 2 : -1;
  if (s_audio_step_index < 0) {
    s_audio_step_index = 0;
  } else if (s_audio_step_index >= (int)ARRAY_LENGTH(s_audio_step_table)) {
    s_audio_step_index = ARRAY_LENGTH(s_audio_step_table) - 1;
  }
  return s_audio_predictor;
}

static bool decode_next_audio_block(void) {
  const GabagoolAudioTrack *track = &GABAGOOL_AUDIO_TRACKS[s_audio_track_index];
  if (s_audio_samples_remaining == 0 || s_audio_chunk_index >= track->chunk_count) {
    return false;
  }

  size_t chunk_size = resource_size(s_audio_handle);
  if (s_audio_chunk_offset >= chunk_size) {
    s_audio_chunk_index++;
    s_audio_chunk_offset = 0;
    if (s_audio_chunk_index >= track->chunk_count) {
      return false;
    }
    s_audio_handle = resource_get_handle(track->chunk_resource_ids[s_audio_chunk_index]);
    chunk_size = resource_size(s_audio_handle);
  }

  size_t to_read = GABAGOOL_MIN((size_t)AUDIO_ENCODED_BUFFER, chunk_size - s_audio_chunk_offset);
  size_t read = resource_load_byte_range(s_audio_handle, s_audio_chunk_offset, s_encoded_buffer, to_read);
  if (read == 0) {
    return false;
  }
  s_audio_chunk_offset += read;

  s_pcm_count = 0;
  s_pcm_offset = 0;
  for (size_t i = 0; i < read && s_audio_samples_remaining > 0; i++) {
    uint8_t packed = s_encoded_buffer[i];
    for (int shift = 0; shift < 8 && s_audio_samples_remaining > 0; shift += 2) {
      uint8_t code = (packed >> shift) & 0x3;
      int8_t sample = (int8_t)decode_audio_code(code);
      for (int repeat = 0; repeat < GABAGOOL_AUDIO_UPSAMPLE; repeat++) {
        s_pcm_buffer[s_pcm_count++] = sample;
      }
      s_audio_samples_remaining--;
    }
  }
  return s_pcm_count > 0;
}

static void play_random_track(void) {
  if (GABAGOOL_AUDIO_TRACK_COUNT > 1) {
    int next_track = s_audio_track_index;
    while (next_track == s_audio_track_index) {
      next_track = rand() % GABAGOOL_AUDIO_TRACK_COUNT;
    }
    s_audio_track_index = next_track;
  } else {
    s_audio_track_index = 0;
  }
  start_audio();
}

static void audio_pump_callback(void *context) {
  (void)context;
  s_audio_timer = NULL;
  if (!s_audio_active) {
    return;
  }

  for (int loops = 0; loops < 8; loops++) {
    if (s_pcm_offset >= s_pcm_count) {
      if (!decode_next_audio_block()) {
        speaker_stream_close();
        s_audio_active = false;
        play_random_track();
        return;
      }
    }

    uint32_t remaining = s_pcm_count - s_pcm_offset;
    uint32_t written = speaker_stream_write(&s_pcm_buffer[s_pcm_offset], remaining);
    if (written == 0) {
      audio_schedule(10);
      return;
    }
    s_pcm_offset += written;
  }

  audio_schedule(1);
}

static void audio_schedule(int delay_ms) {
  if (s_audio_active) {
    s_audio_timer = app_timer_register(delay_ms, audio_pump_callback, NULL);
  }
}

static void speaker_finished_callback(SpeakerFinishReason reason, void *context) {
  (void)context;
  if (!s_audio_active) {
    return;
  }
  if (reason == SpeakerFinishReasonPreempted || reason == SpeakerFinishReasonError) {
    s_audio_active = false;
  }
}

static void start_audio(void) {
  if (s_audio_active) {
    return;
  }
  if (speaker_is_muted()) {
    return;
  }
  if (!speaker_stream_open(SpeakerPcmFormat_8kHz_8bit, 80)) {
    return;
  }
  audio_reset_decoder();
  s_audio_active = true;
  audio_schedule(1);
}

static void stop_audio(void) {
  if (!s_audio_active) {
    return;
  }
  s_audio_active = false;
  speaker_stop();
}
#else
static void start_audio(void) {
}

static void stop_audio(void) {
}
#endif

static void backlight_off_callback(void *context) {
  (void)context;
  s_backlight_timer = NULL;
  light_enable(false);
}

static void reset_backlight(void) {
  light_enable(true);
  if (s_backlight_timer) {
    app_timer_cancel(s_backlight_timer);
  }
  s_backlight_timer = app_timer_register(BACKLIGHT_TIMEOUT_MS, backlight_off_callback, NULL);
}

static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
  (void)axis;
  (void)direction;
  reset_backlight();
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  reset_backlight();
#if defined(PBL_SPEAKER)
  if (s_audio_active) {
    stop_audio();
  } else {
    start_audio();
  }
#else
  start_audio();
#endif
}

static void select_double_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  reset_backlight();
#if defined(PBL_SPEAKER)
  if (s_audio_active) {
    stop_audio();
  }
  play_random_track();
#endif
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  reset_backlight();
  begin_transition(-1);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  reset_backlight();
  begin_transition(1);
}

static void click_config_provider(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_multi_click_subscribe(BUTTON_ID_SELECT, 2, 2, 0, true, select_double_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(GRect(0, 0, GABAGOOL_SCREEN_W, GABAGOOL_SCREEN_H));
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);

  s_current_bitmap = load_image(0);
  s_frame_timer = app_timer_register(FRAME_MS, frame_timer_callback, NULL);
  reset_backlight();
#if defined(PBL_SPEAKER)
  s_audio_track_index = rand() % GABAGOOL_AUDIO_TRACK_COUNT;
#endif
  start_audio();
}

static void window_unload(Window *window) {
  (void)window;
  light_enable(false);
#if defined(PBL_SPEAKER)
  stop_audio();
#endif
  if (s_backlight_timer) {
    app_timer_cancel(s_backlight_timer);
    s_backlight_timer = NULL;
  }
  if (s_frame_timer) {
    app_timer_cancel(s_frame_timer);
    s_frame_timer = NULL;
  }
  if (s_audio_timer) {
    app_timer_cancel(s_audio_timer);
    s_audio_timer = NULL;
  }
  if (s_current_bitmap) {
    gbitmap_destroy(s_current_bitmap);
    s_current_bitmap = NULL;
  }
  if (s_canvas) {
    layer_destroy(s_canvas);
    s_canvas = NULL;
  }
}

static void init(void) {
  srand(time(NULL));
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
#if defined(PBL_SPEAKER)
  speaker_set_finish_callback(speaker_finished_callback, NULL);
#endif
  accel_tap_service_subscribe(accel_tap_handler);
  window_stack_push(s_window, true);
}

static void deinit(void) {
  accel_tap_service_unsubscribe();
#if defined(PBL_SPEAKER)
  speaker_set_finish_callback(NULL, NULL);
#endif
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
