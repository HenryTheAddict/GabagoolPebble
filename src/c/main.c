#include <pebble.h>
#include <string.h>
#include "generated_assets.h"

#define FRAME_MS 33
#define SLIDE_MS 7000
#define HOLD_MS 2000
#define TRANSITION_MS 900
#define BACKLIGHT_TIMEOUT_MS (5 * 60 * 1000)
#define AUDIO_ENCODED_BUFFER 16
#define AUDIO_PCM_BUFFER (AUDIO_ENCODED_BUFFER * 4 * GABAGOOL_AUDIO_UPSAMPLE)
#define AUDIO_STREAM_PASSES 8
#define GABAGOOL_MIN(a, b) ((a) < (b) ? (a) : (b))
#define PET_GRAVITY 0.38f
#define PET_WALL_BOUNCE 0.85f
#define PET_CEILING_BOUNCE 0.72f
#define PET_FLOOR_BOUNCE 0.60f
#define PET_FLOOR_FRICTION 0.94f
#define PET_MAX_SPEED 8.0f

#define PERSIST_KEY_PET_STATE 100
#define PERSIST_KEY_LAST_PET_TIME 101

typedef enum {
  TransitionSlide = 0,
  TransitionMosaic,
  TransitionChecker,
  TransitionBlinds,
  TransitionWipe,
  TransitionCount
} TransitionKind;

typedef enum {
  PetStateEgg = 0,
  PetStateEggTap1,
  PetStateEggTap2,
  PetStateEggTap3,
  PetStateHatching,
  PetStateNormal,
  PetStatePetted,
  PetStateDead
} PetState;

static void start_audio(void);
static void stop_audio(void);
static void play_random_track(void);
static void play_gubby_sfx(void);
static void set_pet_animation(uint32_t resource_id);
static GBitmap *load_image(int index);

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

// Pet state variables
static PetState s_pet_state = PetStateEgg;
static int s_egg_taps = 0;
static int s_pet_x = 84;
static int s_pet_y = 98;

static GBitmapSequence *s_pet_seq = NULL;
static GBitmap *s_pet_frame_bitmap = NULL;
static uint32_t s_pet_frame_delay_ms = 100;
static uint32_t s_pet_frame_elapsed_ms = 0;

static uint32_t s_last_pet_time = 0;
static bool s_pet_grabbed = false;
static int s_pet_grab_offset_x = 16;
static int s_pet_grab_offset_y = 16;
static int s_last_touch_x = 0;
static int s_last_touch_y = 0;
static int s_last_touch_dx = 0;
static int s_last_touch_dy = 0;
static int s_rub_count = 0;
static int s_last_rub_dir = 0;
static int s_hatch_progress = 0;
static int s_flash_frames = 0;
static int s_pet_petted_frames = 0;

static float s_pet_fx = 84.0f;
static float s_pet_fy = 98.0f;
static float s_pet_fvx = 3.0f;
static float s_pet_fvy = 2.0f;

static int clamp_int(int value, int min_value, int max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

static float clamp_float(float value, float min_value, float max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

static void redraw_now(void) {
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

static void set_pet_position(int x, int y) {
  s_pet_x = clamp_int(x, 0, GABAGOOL_SCREEN_W - 32);
  s_pet_y = clamp_int(y, 0, GABAGOOL_SCREEN_H - 32);
  s_pet_fx = (float)s_pet_x;
  s_pet_fy = (float)s_pet_y;
  redraw_now();
}

static void drag_pet_to(int tx, int ty) {
  set_pet_position(tx - s_pet_grab_offset_x, ty - s_pet_grab_offset_y);
}

static void release_grab(void) {
  s_pet_grabbed = false;
  s_pet_fx = (float)s_pet_x;
  s_pet_fy = (float)s_pet_y;
  s_pet_fvx = clamp_float((float)s_last_touch_dx * 0.45f, -PET_MAX_SPEED, PET_MAX_SPEED);
  s_pet_fvy = clamp_float((float)s_last_touch_dy * 0.45f, -PET_MAX_SPEED, PET_MAX_SPEED);
  if (s_pet_fvx > -0.3f && s_pet_fvx < 0.3f) {
    s_pet_fvx = (rand() % 2 == 0 ? 1.2f : -1.2f);
  }
  play_gubby_sfx();
}

static GBitmap *s_static_pet_bitmap = NULL;
static uint32_t s_static_pet_resource_id = 0;

#if defined(PBL_SPEAKER)
static bool s_audio_active;
static ResHandle s_audio_handle;
static uint32_t s_audio_chunk_index;
static uint32_t s_audio_chunk_offset;
static uint32_t s_audio_chunk_size;
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
  int image_w = GABAGOOL_IMAGE_WIDTHS[image_index];
  int max_offset = image_w - GABAGOOL_SCREEN_W;
  if (max_offset <= 0) {
    return 0;
  }
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
  int image_w = GABAGOOL_IMAGE_WIDTHS[image_index];
  int offset = pan_offset_for(image_index, elapsed_ms);
  graphics_draw_bitmap_in_rect(ctx, bitmap, GRect(-offset, 0, image_w, GABAGOOL_SCREEN_H));
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

static void apply_tile_effect(GContext *ctx, int progress, bool is_checker, bool incoming) {
  int tile_size = is_checker ? 20 : 16;
  for (int y = 0; y < GABAGOOL_SCREEN_H; y += tile_size) {
    for (int x = 0; x < GABAGOOL_SCREEN_W; x += tile_size) {
      int th = tile_hash(x, y);
      bool reveal = is_checker ? (((x / tile_size + y / tile_size) & 1) ? (th < progress) : (th >= (1024 - progress))) : (th < progress);
      if (incoming) {
        if (!reveal) {
          fill_black_rect(ctx, GRect(x, y, tile_size, tile_size));
        }
      } else {
        if (reveal) {
          fill_black_rect(ctx, GRect(x, y, tile_size, tile_size));
        }
      }
    }
  }
}

static void apply_blinds_effect(GContext *ctx, int progress, bool incoming) {
  int stripe_w = 8;
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
    int curr_w = GABAGOOL_IMAGE_WIDTHS[s_current_index];
    if (!incoming && s_current_bitmap) {
      int target_x = -current_offset - ((curr_w - current_offset) * x) / GABAGOOL_SCREEN_W;
      graphics_draw_bitmap_in_rect(ctx, s_current_bitmap,
                                   GRect(target_x, 0, curr_w, GABAGOOL_SCREEN_H));
    } else if (incoming && s_current_bitmap) {
      int target_x = GABAGOOL_SCREEN_W - ((GABAGOOL_SCREEN_W + current_offset) * x) / GABAGOOL_SCREEN_W;
      graphics_draw_bitmap_in_rect(ctx, s_current_bitmap,
                                   GRect(target_x, 0, curr_w, GABAGOOL_SCREEN_H));
    }
    return;
  }

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

static void draw_static_sprite(GContext *ctx, uint32_t res_id, GRect rect) {
  if (s_static_pet_resource_id != res_id) {
    if (s_static_pet_bitmap) {
      gbitmap_destroy(s_static_pet_bitmap);
    }
    s_static_pet_bitmap = gbitmap_create_with_resource(res_id);
    s_static_pet_resource_id = res_id;
  }
  if (s_static_pet_bitmap) {
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, s_static_pet_bitmap, rect);
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
  }
}

static void draw_pet(GContext *ctx) {
  if (s_flash_frames > 0) {
    s_flash_frames--;
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, GRect(0, 0, GABAGOOL_SCREEN_W, GABAGOOL_SCREEN_H), 0, GCornerNone);
    return;
  }

  switch (s_pet_state) {
    case PetStateEgg:
      draw_static_sprite(ctx, RESOURCE_ID_PET_EGG, GRect(36, 50, 128, 128));
      break;
    case PetStateEggTap1:
      draw_static_sprite(ctx, RESOURCE_ID_PET_EGG_TAP1, GRect(36, 50, 128, 128));
      break;
    case PetStateEggTap2:
      draw_static_sprite(ctx, RESOURCE_ID_PET_EGG_TAP2, GRect(36, 50, 128, 128));
      break;
    case PetStateEggTap3:
      draw_static_sprite(ctx, RESOURCE_ID_PET_EGG_TAP3, GRect(36, 50, 128, 128));
      break;
    case PetStateHatching:
      draw_static_sprite(ctx, RESOURCE_ID_PET_EGG_TAP3, GRect(36, 50, 128, 128));
      if (s_hatch_progress < 60) {
        graphics_context_set_stroke_color(ctx, GColorPastelYellow);
        graphics_context_set_stroke_width(ctx, 4);
        int seed = s_hatch_progress ^ 0x5A;
        for (int i = 0; i < 5; i++) {
          int target_x = 20 + i * 40 + (seed % 15);
          graphics_draw_line(ctx, GPoint(100, 0), GPoint(target_x, 114));
          seed = seed * 31 + 17;
        }
      } else {
        int y = (s_hatch_progress - 60) * 3;
        if (s_pet_frame_bitmap) {
          graphics_context_set_compositing_mode(ctx, GCompOpSet);
          graphics_draw_bitmap_in_rect(ctx, s_pet_frame_bitmap, GRect(84, y, 32, 32));
          graphics_context_set_compositing_mode(ctx, GCompOpAssign);
        }
      }
      break;
    case PetStateNormal:
    case PetStatePetted:
      if (s_pet_frame_bitmap) {
        graphics_context_set_compositing_mode(ctx, GCompOpSet);
        graphics_draw_bitmap_in_rect(ctx, s_pet_frame_bitmap, GRect(s_pet_x, s_pet_y, 32, 32));
        graphics_context_set_compositing_mode(ctx, GCompOpAssign);
      }
      break;
    case PetStateDead:
      draw_static_sprite(ctx, RESOURCE_ID_PET_DEAD, GRect(s_pet_x, s_pet_y, 32, 32));
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
  draw_pet(ctx);
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

static void set_pet_animation(uint32_t resource_id) {
  if (s_pet_seq) {
    gbitmap_sequence_destroy(s_pet_seq);
    s_pet_seq = NULL;
  }
  if (s_pet_frame_bitmap) {
    gbitmap_destroy(s_pet_frame_bitmap);
    s_pet_frame_bitmap = NULL;
  }
  if (resource_id == 0) {
    return;
  }
  s_pet_seq = gbitmap_sequence_create_with_resource(resource_id);
  if (s_pet_seq) {
    s_pet_frame_bitmap = gbitmap_create_blank(gbitmap_sequence_get_bitmap_size(s_pet_seq), GBitmapFormat8Bit);
    gbitmap_sequence_update_bitmap_next_frame(s_pet_seq, s_pet_frame_bitmap, &s_pet_frame_delay_ms);
    s_pet_frame_elapsed_ms = 0;
  }
}

static void trigger_breeding(void) {
  s_pet_state = PetStateEgg;
  s_egg_taps = 0;
  s_pet_grabbed = false;
  s_rub_count = 0;
  s_last_rub_dir = 0;
  s_flash_frames = 4;
  persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
  set_pet_animation(0);
}

static void update_pet_logic(void) {
  if (s_pet_state == PetStateHatching) {
    s_hatch_progress += 2;
    if (s_hatch_progress == 60) {
      set_pet_animation(RESOURCE_ID_PET_GUBBY_ANIM);
    }
    if (s_hatch_progress >= 100) {
      s_pet_state = PetStateNormal;
      s_pet_x = 84;
      s_pet_y = 114;
      s_pet_fx = 84.0f;
      s_pet_fy = 114.0f;
      s_pet_fvx = 1.5f;
      s_pet_fvy = 0.0f;
      s_last_pet_time = time(NULL);
      persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
      persist_write_int(PERSIST_KEY_LAST_PET_TIME, s_last_pet_time);
      set_pet_animation(RESOURCE_ID_PET_GUBBY_ANIM);
    }
    return;
  }

  if (s_pet_state == PetStateNormal || s_pet_state == PetStatePetted) {
    if (time(NULL) - s_last_pet_time > 180) {
      s_pet_state = PetStateDead;
      persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
      set_pet_animation(0);
      return;
    }

    if (s_pet_state == PetStatePetted) {
      s_pet_petted_frames++;
      if (s_pet_petted_frames >= 40) {
        s_pet_state = PetStateNormal;
        s_pet_petted_frames = 0;
        set_pet_animation(RESOURCE_ID_PET_GUBBY_ANIM);
      }
    }

    if (!s_pet_grabbed) {
      s_pet_fvy += PET_GRAVITY;
      s_pet_fvx = clamp_float(s_pet_fvx, -PET_MAX_SPEED, PET_MAX_SPEED);
      s_pet_fvy = clamp_float(s_pet_fvy, -PET_MAX_SPEED, PET_MAX_SPEED);
      s_pet_fx += s_pet_fvx;
      s_pet_fy += s_pet_fvy;

      if (s_pet_fx <= 0) {
        s_pet_fx = 0;
        s_pet_fvx = -s_pet_fvx * PET_WALL_BOUNCE;
      } else if (s_pet_fx >= GABAGOOL_SCREEN_W - 32) {
        s_pet_fx = GABAGOOL_SCREEN_W - 32;
        s_pet_fvx = -s_pet_fvx * PET_WALL_BOUNCE;
      }

      if (s_pet_fy <= 0) {
        s_pet_fy = 0;
        s_pet_fvy = -s_pet_fvy * PET_CEILING_BOUNCE;
      } else if (s_pet_fy >= GABAGOOL_SCREEN_H - 32) {
        s_pet_fy = GABAGOOL_SCREEN_H - 32;
        s_pet_fvy = -s_pet_fvy * PET_FLOOR_BOUNCE;
        s_pet_fvx *= PET_FLOOR_FRICTION;
        
        if (s_pet_fvy < 0 && s_pet_fvy > -1.0f) {
          s_pet_fvy = 0;
        }
        if (s_pet_fvx > -0.2f && s_pet_fvx < 0.2f) {
          s_pet_fvx = (rand() % 2 == 0 ? 1.0f : -1.0f) * (1.2f + (rand() % 100) / 150.0f);
        }
      }

      s_pet_x = (int)s_pet_fx;
      s_pet_y = (int)s_pet_fy;
    }
  }

  if (s_pet_seq && s_pet_frame_bitmap) {
    s_pet_frame_elapsed_ms += FRAME_MS;
    if (s_pet_frame_elapsed_ms >= s_pet_frame_delay_ms) {
      s_pet_frame_elapsed_ms = 0;
      bool next_frame = gbitmap_sequence_update_bitmap_next_frame(s_pet_seq, s_pet_frame_bitmap, &s_pet_frame_delay_ms);
      if (!next_frame) {
        gbitmap_sequence_restart(s_pet_seq);
        gbitmap_sequence_update_bitmap_next_frame(s_pet_seq, s_pet_frame_bitmap, &s_pet_frame_delay_ms);
      }
    }
  }
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

  update_pet_logic();

  layer_mark_dirty(s_canvas);
  s_frame_timer = app_timer_register(FRAME_MS, frame_timer_callback, NULL);
}

#if defined(PBL_SPEAKER)
static void audio_schedule(int delay_ms);

static void audio_cancel_timer(void) {
  if (s_audio_timer) {
    app_timer_cancel(s_audio_timer);
    s_audio_timer = NULL;
  }
}

static void audio_reset_decoder(void) {
  const GabagoolAudioTrack *track = &GABAGOOL_AUDIO_TRACKS[s_audio_track_index];
  s_audio_chunk_index = 0;
  s_audio_chunk_offset = 0;
  s_audio_samples_remaining = track->total_samples;
  s_audio_predictor = 0;
  s_audio_step_index = 10;
  s_audio_handle = resource_get_handle(track->chunk_resource_ids[0]);
  s_audio_chunk_size = resource_size(s_audio_handle);
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

  if (s_audio_chunk_offset >= s_audio_chunk_size) {
    s_audio_chunk_index++;
    s_audio_chunk_offset = 0;
    if (s_audio_chunk_index >= track->chunk_count) {
      return false;
    }
    s_audio_handle = resource_get_handle(track->chunk_resource_ids[s_audio_chunk_index]);
    s_audio_chunk_size = resource_size(s_audio_handle);
  }

  size_t to_read = GABAGOOL_MIN((size_t)AUDIO_ENCODED_BUFFER, s_audio_chunk_size - s_audio_chunk_offset);
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
  if (GABAGOOL_MUSIC_TRACK_COUNT > 1) {
    int next_track = s_audio_track_index;
    while (next_track == s_audio_track_index) {
      next_track = GABAGOOL_MUSIC_TRACK_INDEXES[rand() % GABAGOOL_MUSIC_TRACK_COUNT];
    }
    s_audio_track_index = next_track;
  } else if (GABAGOOL_MUSIC_TRACK_COUNT == 1) {
    s_audio_track_index = GABAGOOL_MUSIC_TRACK_INDEXES[0];
  } else {
    s_audio_track_index = 0;
  }
  start_audio();
}

static void play_track(int track_index) {
  if (track_index < 0 || track_index >= GABAGOOL_AUDIO_TRACK_COUNT) {
    return;
  }
  if (s_audio_active) {
    stop_audio();
  }
  s_audio_track_index = track_index;
  start_audio();
}

static void play_gubby_sfx(void) {
#if GABAGOOL_GUBBY_SFX_TRACK_INDEX >= 0
  play_track(GABAGOOL_GUBBY_SFX_TRACK_INDEX);
#endif
}

static void audio_pump_callback(void *context) {
  (void)context;
  s_audio_timer = NULL;
  if (!s_audio_active) {
    return;
  }

  for (int loops = 0; loops < AUDIO_STREAM_PASSES; loops++) {
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
    audio_cancel_timer();
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
  audio_cancel_timer();
  if (s_audio_active) {
    speaker_stream_close();
    s_audio_active = false;
  }
}
#else
static void start_audio(void) {
}

static void stop_audio(void) {
}

static void play_gubby_sfx(void) {
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

  if (s_pet_state == PetStateEgg) {
    s_egg_taps = 1;
    s_pet_state = PetStateEggTap1;
    persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
  } else if (s_pet_state == PetStateEggTap1) {
    s_egg_taps = 2;
    s_pet_state = PetStateEggTap2;
    persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
  } else if (s_pet_state == PetStateEggTap2) {
    s_egg_taps = 3;
    s_pet_state = PetStateEggTap3;
    persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
  } else if (s_pet_state == PetStateEggTap3) {
    s_egg_taps = 4;
    s_pet_state = PetStateHatching;
    s_hatch_progress = 0;
  } else if (s_pet_state == PetStateNormal || s_pet_state == PetStatePetted) {
    s_pet_state = PetStatePetted;
    s_pet_petted_frames = 0;
    s_last_pet_time = time(NULL);
    persist_write_int(PERSIST_KEY_LAST_PET_TIME, s_last_pet_time);
    set_pet_animation(RESOURCE_ID_PET_PETTED_ANIM);
    play_gubby_sfx();
  } else if (s_pet_state == PetStateDead) {
    s_pet_state = PetStateEgg;
    s_egg_taps = 0;
    persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
  }
  redraw_now();
}

static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  reset_backlight();
  if (s_pet_state == PetStateNormal || s_pet_state == PetStatePetted) {
    s_pet_grabbed = true;
    s_pet_fx = (float)s_pet_x;
    s_pet_fy = (float)s_pet_y;
    s_pet_fvx = 0.0f;
    s_pet_fvy = 0.0f;
    s_last_touch_dx = 0;
    s_last_touch_dy = 0;
    s_rub_count = 0;
    s_last_rub_dir = 0;
  }
  redraw_now();
}

static void select_long_click_release_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  reset_backlight();
  if (s_pet_grabbed) {
    release_grab();
  }
  redraw_now();
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
  if (s_pet_grabbed) {
    set_pet_position(s_pet_x, s_pet_y - 15);
    s_last_touch_dx = 0;
    s_last_touch_dy = -15;
    if (s_last_rub_dir != -1) {
      s_rub_count++;
      s_last_rub_dir = -1;
      if (s_rub_count >= 8) {
        trigger_breeding();
      }
    }
  } else {
    begin_transition(-1);
  }
  redraw_now();
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  reset_backlight();
  if (s_pet_grabbed) {
    set_pet_position(s_pet_x, s_pet_y + 15);
    s_last_touch_dx = 0;
    s_last_touch_dy = 15;
    if (s_last_rub_dir != 1) {
      s_rub_count++;
      s_last_rub_dir = 1;
      if (s_rub_count >= 8) {
        trigger_breeding();
      }
    }
  } else {
    begin_transition(1);
  }
  redraw_now();
}

static void click_config_provider(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_multi_click_subscribe(BUTTON_ID_SELECT, 2, 2, 0, true, select_double_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long_click_handler, select_long_click_release_handler);
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

  // Load persistent pet state
  if (persist_exists(PERSIST_KEY_PET_STATE)) {
    s_pet_state = persist_read_int(PERSIST_KEY_PET_STATE);
  } else {
    s_pet_state = PetStateEgg;
  }
  
  if (persist_exists(PERSIST_KEY_LAST_PET_TIME)) {
    s_last_pet_time = persist_read_int(PERSIST_KEY_LAST_PET_TIME);
  } else {
    s_last_pet_time = time(NULL);
  }
  
  // Offline age check (10 minutes)
  if (s_pet_state == PetStateNormal || s_pet_state == PetStatePetted) {
    if (time(NULL) - s_last_pet_time > 600) {
      s_pet_state = PetStateDead;
      persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
    }
  }

  // Load animation if active
  if (s_pet_state == PetStateNormal) {
    set_pet_animation(RESOURCE_ID_PET_GUBBY_ANIM);
  } else if (s_pet_state == PetStatePetted) {
    set_pet_animation(RESOURCE_ID_PET_PETTED_ANIM);
  }

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
  if (s_static_pet_bitmap) {
    gbitmap_destroy(s_static_pet_bitmap);
    s_static_pet_bitmap = NULL;
  }
  set_pet_animation(0);
  if (s_canvas) {
    layer_destroy(s_canvas);
    s_canvas = NULL;
  }
}

static void touch_handler(const TouchEvent *event, void *context) {
  (void)context;
  reset_backlight();
  if (!event) {
    return;
  }
  
  int tx = event->x;
  int ty = event->y;
  
  if (event->type == TouchEvent_Touchdown) {
    if (s_pet_state == PetStateEgg) {
      if (tx >= 36 && tx <= 36 + 128 && ty >= 50 && ty <= 50 + 128) {
        s_egg_taps = 1;
        s_pet_state = PetStateEggTap1;
        persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
      }
    } else if (s_pet_state == PetStateEggTap1) {
      if (tx >= 36 && tx <= 36 + 128 && ty >= 50 && ty <= 50 + 128) {
        s_egg_taps = 2;
        s_pet_state = PetStateEggTap2;
        persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
      }
    } else if (s_pet_state == PetStateEggTap2) {
      if (tx >= 36 && tx <= 36 + 128 && ty >= 50 && ty <= 50 + 128) {
        s_egg_taps = 3;
        s_pet_state = PetStateEggTap3;
        persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
      }
    } else if (s_pet_state == PetStateEggTap3) {
      if (tx >= 36 && tx <= 36 + 128 && ty >= 50 && ty <= 50 + 128) {
        s_egg_taps = 4;
        s_pet_state = PetStateHatching;
        s_hatch_progress = 0;
      }
    } else if (s_pet_state == PetStateNormal || s_pet_state == PetStatePetted) {
      // Touch down on pet to grab him!
      if (tx >= s_pet_x && tx <= s_pet_x + 32 && ty >= s_pet_y && ty <= s_pet_y + 32) {
        s_pet_grabbed = true;
        s_rub_count = 0;
        s_last_rub_dir = 0;
        s_pet_grab_offset_x = clamp_int(tx - s_pet_x, 0, 31);
        s_pet_grab_offset_y = clamp_int(ty - s_pet_y, 0, 31);
        s_last_touch_x = tx;
        s_last_touch_y = ty;
        s_last_touch_dx = 0;
        s_last_touch_dy = 0;
        s_pet_fvx = 0.0f;
        s_pet_fvy = 0.0f;
        drag_pet_to(tx, ty);
        
        s_pet_state = PetStatePetted;
        s_pet_petted_frames = 0;
        s_last_pet_time = time(NULL);
        persist_write_int(PERSIST_KEY_LAST_PET_TIME, s_last_pet_time);
        set_pet_animation(RESOURCE_ID_PET_PETTED_ANIM);
        play_gubby_sfx();
      }
    } else if (s_pet_state == PetStateDead) {
      if (tx >= s_pet_x && tx <= s_pet_x + 32 && ty >= s_pet_y && ty <= s_pet_y + 32) {
        s_pet_state = PetStateEgg;
        s_egg_taps = 0;
        persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
      }
    }
  } else if (event->type == TouchEvent_PositionUpdate) {
    if (s_pet_grabbed) {
      s_last_touch_dx = tx - s_last_touch_x;
      s_last_touch_dy = ty - s_last_touch_y;
      drag_pet_to(tx, ty);
      
      int new_dir = 0;
      if (ty < s_last_touch_y - 8) {
        new_dir = -1;
      } else if (ty > s_last_touch_y + 8) {
        new_dir = 1;
      }
      
      if (new_dir != 0 && new_dir != s_last_rub_dir) {
        s_rub_count++;
        s_last_rub_dir = new_dir;
        if (s_rub_count >= 8) {
          trigger_breeding();
        }
      }
      s_last_touch_x = tx;
      s_last_touch_y = ty;
    }
  } else if (event->type == TouchEvent_Liftoff) {
    if (s_pet_grabbed) {
      release_grab();
    }
  }
  redraw_now();
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
  touch_service_subscribe(touch_handler, NULL);
  window_stack_push(s_window, true);
}

static void deinit(void) {
  accel_tap_service_unsubscribe();
  touch_service_unsubscribe();
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
