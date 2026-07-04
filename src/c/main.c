#include <pebble.h>
#include <string.h>
#include "generated_assets.h"

#define FRAME_MS 50
#define SLIDE_MS 7000
#define HOLD_MS 2000
#define TRANSITION_MS 900
#define BACKLIGHT_TIMEOUT_MS (5 * 60 * 1000)
#define AUDIO_PCM_BUFFER 192
#define AUDIO_STREAM_PASSES 4
#define GABAGOOL_MIN(a, b) ((a) < (b) ? (a) : (b))
#define PET_GRAVITY 0.24f
#define PET_WALL_BOUNCE 0.72f
#define PET_CEILING_BOUNCE 0.55f
#define PET_FLOOR_BOUNCE 0.48f
#define PET_FLOOR_FRICTION 0.88f
#define PET_MAX_SPEED 5.5f
#define MAX_GUBBYS 8
#define SFX_COOLDOWN_MS 250
#define FLING_KILL_SPEED 9.0f
#define EGG_HOLD_MS 1000
#define AUDIO_READ_BUFFER 64

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

typedef struct {
  bool active;
  bool grabbed;
  int x;
  int y;
  int grab_offset_x;
  int grab_offset_y;
  int last_touch_x;
  int last_touch_y;
  int last_touch_dx;
  int last_touch_dy;
  float fx;
  float fy;
  float fvx;
  float fvy;
} GubbyPet;

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
static GBitmap *s_next_bitmap;

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
static uint32_t s_pet_animation_resource_id = 0;
static uint32_t s_pet_frame_delay_ms = 100;
static uint32_t s_pet_frame_elapsed_ms = 0;

static uint32_t s_last_pet_time = 0;
static bool s_pet_grabbed = false;
static int s_rub_count = 0;
static int s_last_rub_dir = 0;
static int s_hatch_progress = 0;
static int s_flash_frames = 0;
static int s_pet_petted_frames = 0;
static int s_egg_hold_ms = 0;
static bool s_egg_touching = false;
static GubbyPet s_gubbys[MAX_GUBBYS];
static int s_gubby_count = 0;
static int s_grabbed_gubby = -1;
static GBitmap *s_static_pet_bitmap = NULL;
static uint32_t s_static_pet_resource_id = 0;

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

static void set_static_pet_sprite(uint32_t res_id) {
  if (s_static_pet_resource_id == res_id) {
    return;
  }
  if (s_static_pet_bitmap) {
    gbitmap_destroy(s_static_pet_bitmap);
    s_static_pet_bitmap = NULL;
  }
  s_static_pet_resource_id = res_id;
  if (res_id != 0) {
    s_static_pet_bitmap = gbitmap_create_with_resource(res_id);
  }
}

static void sync_static_pet_sprite(void) {
  switch (s_pet_state) {
    case PetStateEgg:
      set_static_pet_sprite(RESOURCE_ID_PET_EGG);
      break;
    case PetStateEggTap1:
      set_static_pet_sprite(RESOURCE_ID_PET_EGG_TAP1);
      break;
    case PetStateEggTap2:
      set_static_pet_sprite(RESOURCE_ID_PET_EGG_TAP2);
      break;
    case PetStateEggTap3:
    case PetStateHatching:
      set_static_pet_sprite(RESOURCE_ID_PET_EGG_TAP3);
      break;
    case PetStateDead:
      set_static_pet_sprite(RESOURCE_ID_PET_DEAD);
      break;
    default:
      set_static_pet_sprite(0);
      break;
  }
}

static void redraw_now(void) {
  sync_static_pet_sprite();
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

static GubbyPet *gubby_at(int index) {
  if (index < 0 || index >= MAX_GUBBYS || !s_gubbys[index].active) {
    return NULL;
  }
  return &s_gubbys[index];
}

static void sync_primary_pet_position(void) {
  GubbyPet *gubby = gubby_at(0);
  if (gubby) {
    s_pet_x = gubby->x;
    s_pet_y = gubby->y;
    s_pet_fx = gubby->fx;
    s_pet_fy = gubby->fy;
    s_pet_fvx = gubby->fvx;
    s_pet_fvy = gubby->fvy;
  }
}

static void set_gubby_position(GubbyPet *gubby, int x, int y) {
  if (!gubby) {
    return;
  }
  gubby->x = clamp_int(x, 0, GABAGOOL_SCREEN_W - 32);
  gubby->y = clamp_int(y, 0, GABAGOOL_SCREEN_H - 32);
  gubby->fx = (float)gubby->x;
  gubby->fy = (float)gubby->y;
  sync_primary_pet_position();
  redraw_now();
}

static void drag_pet_to(int tx, int ty) {
  GubbyPet *gubby = gubby_at(s_grabbed_gubby);
  if (!gubby) {
    return;
  }
  set_gubby_position(gubby, tx - gubby->grab_offset_x, ty - gubby->grab_offset_y);
}

static void kill_gubby(int index) {
  if (index < 0 || index >= MAX_GUBBYS || !s_gubbys[index].active) {
    return;
  }
  s_gubbys[index].active = false;
  s_gubby_count--;
  if (s_gubby_count < 0) {
    s_gubby_count = 0;
  }
  if (s_gubby_count == 0) {
    s_pet_state = PetStateDead;
    persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
    set_pet_animation(RESOURCE_ID_PET_DEATH_ANIM);
  }
  sync_primary_pet_position();
}

static void release_grab(void) {
  GubbyPet *gubby = gubby_at(s_grabbed_gubby);
  int released_index = s_grabbed_gubby;
  if (!gubby) {
    s_pet_grabbed = false;
    s_grabbed_gubby = -1;
    return;
  }
  gubby->grabbed = false;
  gubby->fx = (float)gubby->x;
  gubby->fy = (float)gubby->y;
  float fvx = (float)gubby->last_touch_dx * 0.55f;
  float fvy = (float)gubby->last_touch_dy * 0.55f;
  float speed = fvx * fvx + fvy * fvy;
  s_pet_grabbed = false;
  s_grabbed_gubby = -1;
  if (speed > FLING_KILL_SPEED * FLING_KILL_SPEED) {
    // Flung too hard - kill the gubby
    s_flash_frames = 3;
    kill_gubby(released_index);
    play_gubby_sfx();
    return;
  }
  gubby->fvx = clamp_float(fvx, -PET_MAX_SPEED, PET_MAX_SPEED);
  gubby->fvy = clamp_float(fvy, -PET_MAX_SPEED, PET_MAX_SPEED);
  if (gubby->fvx > -0.3f && gubby->fvx < 0.3f) {
    gubby->fvx = (rand() % 2 == 0 ? 1.2f : -1.2f);
  }
  sync_primary_pet_position();
  play_gubby_sfx();
}

static int spawn_gubby_at(int x, int y) {
  if (s_gubby_count >= MAX_GUBBYS) {
    return -1;
  }
  for (int i = 0; i < MAX_GUBBYS; i++) {
    if (!s_gubbys[i].active) {
      GubbyPet *gubby = &s_gubbys[i];
      memset(gubby, 0, sizeof(*gubby));
      gubby->active = true;
      gubby->grab_offset_x = 16;
      gubby->grab_offset_y = 16;
      set_gubby_position(gubby, x, y);
      gubby->fvx = (rand() % 2 == 0 ? 1.2f : -1.2f) * (1.0f + (rand() % 100) / 100.0f);
      gubby->fvy = -1.0f - (rand() % 100) / 120.0f;
      s_gubby_count++;
      sync_primary_pet_position();
      return i;
    }
  }
  return -1;
}

static int hit_test_gubby(int tx, int ty) {
  for (int i = MAX_GUBBYS - 1; i >= 0; i--) {
    GubbyPet *gubby = gubby_at(i);
    if (gubby && tx >= gubby->x && tx <= gubby->x + 32 && ty >= gubby->y && ty <= gubby->y + 32) {
      return i;
    }
  }
  return -1;
}

#if defined(PBL_SPEAKER)
typedef struct {
  bool active;
  int track_index;
  ResHandle handle;
  uint32_t chunk_index;
  uint32_t chunk_offset;
  uint32_t chunk_size;
  uint32_t samples_remaining;
  int16_t predictor;
  int step_index;
  uint8_t packed_byte;
  uint8_t packed_shift;
  uint8_t read_buf[AUDIO_READ_BUFFER];
  uint32_t read_buf_pos;
  uint32_t read_buf_len;
} AudioDecoderState;

static bool s_audio_active;
static int8_t s_pcm_buffer[AUDIO_PCM_BUFFER];
static uint32_t s_pcm_count;
static uint32_t s_pcm_offset;
static AudioDecoderState s_music_decoder;
static AudioDecoderState s_sfx_decoder;
static int s_sfx_cooldown_ms = 0;

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
  if (s_current_bitmap) {
    gbitmap_destroy(s_current_bitmap);
  }
  s_current_bitmap = s_next_bitmap;
  s_next_bitmap = NULL;
  s_current_index = s_next_index;
}

static void draw_transition(GContext *ctx) {
  bool incoming = s_transition_ms >= (TRANSITION_MS / 2);

  int local_ms = incoming ? (s_transition_ms - (TRANSITION_MS / 2)) : s_transition_ms;
  int progress = (1024 * local_ms) / (TRANSITION_MS / 2);
  if (progress < 0) {
    progress = 0;
  } else if (progress > 1024) {
    progress = 1024;
  }

  if (s_transition_kind == TransitionSlide) {
    int x = (GABAGOOL_SCREEN_W * progress) / 1024;
    if (!incoming && s_current_bitmap) {
      int current_offset = pan_offset_for(s_current_index, SLIDE_MS);
      int curr_w = GABAGOOL_IMAGE_WIDTHS[s_current_index];
      int target_x = -current_offset - ((curr_w - current_offset) * x) / GABAGOOL_SCREEN_W;
      graphics_draw_bitmap_in_rect(ctx, s_current_bitmap,
                                   GRect(target_x, 0, curr_w, GABAGOOL_SCREEN_H));
    } else if (incoming && s_next_bitmap) {
      int current_offset = pan_offset_for(s_next_index, 0);
      int curr_w = GABAGOOL_IMAGE_WIDTHS[s_next_index];
      int target_x = GABAGOOL_SCREEN_W - ((GABAGOOL_SCREEN_W + current_offset) * x) / GABAGOOL_SCREEN_W;
      graphics_draw_bitmap_in_rect(ctx, s_next_bitmap,
                                   GRect(target_x, 0, curr_w, GABAGOOL_SCREEN_H));
    }
    return;
  }

  if (incoming) {
    draw_panned_bitmap(ctx, s_next_bitmap, s_next_index, 0);
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
  if (s_static_pet_bitmap && s_static_pet_resource_id == res_id) {
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, s_static_pet_bitmap, rect);
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
  }
}

static void draw_center_light(GContext *ctx, int progress) {
  int cx = GABAGOOL_SCREEN_W / 2;
  int cy = GABAGOOL_SCREEN_H / 2;
  int radius = 14 + progress * 2;
  if (radius > 96) {
    radius = 96;
  }

  graphics_context_set_stroke_width(ctx, 1);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  for (int y = cy - radius; y <= cy + radius; y += 2) {
    if (y < 0 || y >= GABAGOOL_SCREEN_H) {
      continue;
    }
    for (int x = cx - radius; x <= cx + radius; x += 2) {
      if (x < 0 || x >= GABAGOOL_SCREEN_W) {
        continue;
      }
      int dx = x - cx;
      int dy = y - cy;
      int dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
      if (dist < radius && (((x * 3 + y * 5 + progress) & 3) == 0)) {
        graphics_draw_pixel(ctx, GPoint(x, y));
      }
    }
  }

  graphics_context_set_stroke_color(ctx, GColorPastelYellow);
  for (int i = 0; i < 12; i++) {
    int seed = i * 37 + progress * 5;
    int target_x = (seed * 23) % GABAGOOL_SCREEN_W;
    int target_y = (seed * 17) % GABAGOOL_SCREEN_H;
    if (((i + progress) & 1) == 0) {
      graphics_draw_line(ctx, GPoint(cx, cy), GPoint(target_x, target_y));
    }
  }
}

static void draw_egg_progress_bar(GContext *ctx) {
  if (s_egg_hold_ms <= 0) {
    return;
  }
  int progress = (s_egg_hold_ms * 1024) / EGG_HOLD_MS;
  if (progress > 1024) {
    progress = 1024;
  }
  int bar_w = 3;
  int total_perimeter = 2 * (GABAGOOL_SCREEN_W + GABAGOOL_SCREEN_H - 2 * bar_w);
  int fill_len = (total_perimeter * progress) / 1024;
  
  GColor bar_color = GColorYellow;
  if (progress > 768) {
    bar_color = GColorGreen;
  } else if (progress > 512) {
    bar_color = GColorChromeYellow;
  }
  graphics_context_set_fill_color(ctx, bar_color);
  
  int remaining = fill_len;
  // Top edge: left to right
  int seg = GABAGOOL_SCREEN_W;
  if (remaining > 0) {
    int len = remaining < seg ? remaining : seg;
    graphics_fill_rect(ctx, GRect(0, 0, len, bar_w), 0, GCornerNone);
    remaining -= len;
  }
  // Right edge: top to bottom
  seg = GABAGOOL_SCREEN_H - bar_w;
  if (remaining > 0) {
    int len = remaining < seg ? remaining : seg;
    graphics_fill_rect(ctx, GRect(GABAGOOL_SCREEN_W - bar_w, bar_w, bar_w, len), 0, GCornerNone);
    remaining -= len;
  }
  // Bottom edge: right to left
  seg = GABAGOOL_SCREEN_W - bar_w;
  if (remaining > 0) {
    int len = remaining < seg ? remaining : seg;
    graphics_fill_rect(ctx, GRect(GABAGOOL_SCREEN_W - bar_w - len, GABAGOOL_SCREEN_H - bar_w, len, bar_w), 0, GCornerNone);
    remaining -= len;
  }
  // Left edge: bottom to top
  seg = GABAGOOL_SCREEN_H - 2 * bar_w;
  if (remaining > 0) {
    int len = remaining < seg ? remaining : seg;
    graphics_fill_rect(ctx, GRect(0, GABAGOOL_SCREEN_H - bar_w - len, bar_w, len), 0, GCornerNone);
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
      draw_egg_progress_bar(ctx);
      break;
    case PetStateEggTap1:
      draw_static_sprite(ctx, RESOURCE_ID_PET_EGG_TAP1, GRect(36, 50, 128, 128));
      draw_egg_progress_bar(ctx);
      break;
    case PetStateEggTap2:
      draw_static_sprite(ctx, RESOURCE_ID_PET_EGG_TAP2, GRect(36, 50, 128, 128));
      draw_egg_progress_bar(ctx);
      break;
    case PetStateEggTap3:
      draw_static_sprite(ctx, RESOURCE_ID_PET_EGG_TAP3, GRect(36, 50, 128, 128));
      draw_egg_progress_bar(ctx);
      break;
    case PetStateHatching:
      draw_static_sprite(ctx, RESOURCE_ID_PET_EGG_TAP3, GRect(36, 50, 128, 128));
      if (s_hatch_progress < 40) {
        draw_center_light(ctx, s_hatch_progress);
      } else {
        // Gubby drops in from above, landing at center
        int drop_t = s_hatch_progress - 40;
        int target_y = GABAGOOL_SCREEN_H / 2 - 16;
        int start_y = -32;
        int y;
        if (drop_t >= 30) {
          y = target_y;
        } else {
          // Ease-out (decelerate) drop
          int32_t t = (drop_t * 1000) / 30;
          int32_t f = 1000 - ((1000 - t) * (1000 - t)) / 1000;
          y = start_y + ((target_y - start_y) * f) / 1000;
        }
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
        for (int i = 0; i < MAX_GUBBYS; i++) {
          GubbyPet *gubby = gubby_at(i);
          if (gubby) {
            graphics_draw_bitmap_in_rect(ctx, s_pet_frame_bitmap, GRect(gubby->x, gubby->y, 32, 32));
          }
        }
        graphics_context_set_compositing_mode(ctx, GCompOpAssign);
      }
      break;
    case PetStateDead:
      if (s_pet_frame_bitmap) {
        graphics_context_set_compositing_mode(ctx, GCompOpSet);
        for (int i = 0; i < MAX_GUBBYS; i++) {
          GubbyPet *gubby = gubby_at(i);
          if (gubby) {
            graphics_draw_bitmap_in_rect(ctx, s_pet_frame_bitmap, GRect(gubby->x, gubby->y, 32, 32));
          }
        }
        graphics_context_set_compositing_mode(ctx, GCompOpAssign);
      } else {
        for (int i = 0; i < MAX_GUBBYS; i++) {
          GubbyPet *gubby = gubby_at(i);
          if (gubby) {
            draw_static_sprite(ctx, RESOURCE_ID_PET_DEAD, GRect(gubby->x, gubby->y, 32, 32));
          }
        }
      }
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
  if (s_next_bitmap) {
    gbitmap_destroy(s_next_bitmap);
    s_next_bitmap = NULL;
  }
  s_next_bitmap = load_image(s_next_index);
  if (!s_next_bitmap) {
    return;
  }
  s_transitioning = true;
  s_transition_ms = 0;
  s_transition_kind = (TransitionKind)((s_transition_kind + 1) % TransitionCount);
}

static void finish_transition(void) {
  swap_to_transition_target();
  s_phase_ms = 0;
  s_transitioning = false;
}

static void set_pet_animation(uint32_t resource_id) {
  if (s_pet_animation_resource_id == resource_id) {
    return;
  }
  if (s_pet_seq) {
    gbitmap_sequence_destroy(s_pet_seq);
    s_pet_seq = NULL;
  }
  if (s_pet_frame_bitmap) {
    gbitmap_destroy(s_pet_frame_bitmap);
    s_pet_frame_bitmap = NULL;
  }
  s_pet_animation_resource_id = resource_id;
  if (resource_id == 0) {
    return;
  }
  s_pet_seq = gbitmap_sequence_create_with_resource(resource_id);
  if (s_pet_seq) {
    s_pet_frame_bitmap = gbitmap_create_blank(gbitmap_sequence_get_bitmap_size(s_pet_seq), GBitmapFormat8Bit);
    gbitmap_sequence_update_bitmap_next_frame(s_pet_seq, s_pet_frame_bitmap, &s_pet_frame_delay_ms);
    if (s_pet_frame_delay_ms == 0) {
      s_pet_frame_delay_ms = FRAME_MS;
    }
    s_pet_frame_elapsed_ms = 0;
  }
}

static void trigger_breeding(void) {
  if (s_pet_state == PetStateNormal || s_pet_state == PetStatePetted) {
    if (s_gubby_count < MAX_GUBBYS) {
      int x = (GABAGOOL_SCREEN_W / 2 - 16) + ((rand() % 41) - 20);
      int y = 22 + (rand() % 28);
      int index = spawn_gubby_at(x, y);
      GubbyPet *gubby = gubby_at(index);
      if (gubby) {
        gubby->fvx = ((rand() % 101) - 50) / 45.0f;
        gubby->fvy = -2.8f - (rand() % 100) / 120.0f;
      }
    } else {
      for (int i = 0; i < MAX_GUBBYS; i++) {
        GubbyPet *gubby = gubby_at(i);
        if (gubby) {
          gubby->fvy = -3.0f - (rand() % 100) / 80.0f;
          gubby->fvx += (rand() % 2 == 0 ? 1.0f : -1.0f);
        }
      }
    }
    s_rub_count = 0;
    s_last_rub_dir = 0;
    s_flash_frames = 2;
    play_gubby_sfx();
    return;
  }
  s_pet_state = PetStateEgg;
  s_egg_taps = 0;
  s_pet_grabbed = false;
  s_grabbed_gubby = -1;
  memset(s_gubbys, 0, sizeof(s_gubbys));
  s_gubby_count = 0;
  s_rub_count = 0;
  s_last_rub_dir = 0;
  s_flash_frames = 4;
  persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
  set_pet_animation(0);
}

static void update_pet_logic(void) {
  // Egg hold timer
  if (s_egg_touching && (s_pet_state <= PetStateEggTap3)) {
    s_egg_hold_ms += FRAME_MS;
    if (s_egg_hold_ms >= EGG_HOLD_MS) {
      s_egg_hold_ms = 0;
      s_egg_touching = false;
      if (s_pet_state == PetStateEgg) {
        s_pet_state = PetStateEggTap1;
        persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
      } else if (s_pet_state == PetStateEggTap1) {
        s_pet_state = PetStateEggTap2;
        persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
      } else if (s_pet_state == PetStateEggTap2) {
        s_pet_state = PetStateEggTap3;
        persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
      } else if (s_pet_state == PetStateEggTap3) {
        s_pet_state = PetStateHatching;
        s_hatch_progress = 0;
      }
      redraw_now();
    }
  }

  if (s_pet_state == PetStateHatching) {
    s_hatch_progress += 2;
    if (s_hatch_progress == 40) {
      set_pet_animation(RESOURCE_ID_PET_GUBBY_ANIM);
    }
    if (s_hatch_progress >= 80) {
      s_pet_state = PetStateNormal;
      memset(s_gubbys, 0, sizeof(s_gubbys));
      s_gubby_count = 0;
      spawn_gubby_at(84, GABAGOOL_SCREEN_H / 2 - 16);
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
      set_pet_animation(RESOURCE_ID_PET_DEATH_ANIM);
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

    for (int i = 0; i < MAX_GUBBYS; i++) {
      GubbyPet *gubby = gubby_at(i);
      if (!gubby || gubby->grabbed) {
        continue;
      }
      gubby->fvy += PET_GRAVITY;
      gubby->fvx = clamp_float(gubby->fvx, -PET_MAX_SPEED, PET_MAX_SPEED);
      gubby->fvy = clamp_float(gubby->fvy, -PET_MAX_SPEED, PET_MAX_SPEED);
      gubby->fx += gubby->fvx;
      gubby->fy += gubby->fvy;

      if (gubby->fx <= 0) {
        gubby->fx = 0;
        gubby->fvx = -gubby->fvx * PET_WALL_BOUNCE;
      } else if (gubby->fx >= GABAGOOL_SCREEN_W - 32) {
        gubby->fx = GABAGOOL_SCREEN_W - 32;
        gubby->fvx = -gubby->fvx * PET_WALL_BOUNCE;
      }

      if (gubby->fy <= 0) {
        gubby->fy = 0;
        gubby->fvy = -gubby->fvy * PET_CEILING_BOUNCE;
      } else if (gubby->fy >= GABAGOOL_SCREEN_H - 32) {
        gubby->fy = GABAGOOL_SCREEN_H - 32;
        gubby->fvy = -gubby->fvy * PET_FLOOR_BOUNCE;
        gubby->fvx *= PET_FLOOR_FRICTION;

        // Settle when nearly stopped
        if (gubby->fvy < 0 && gubby->fvy > -0.5f) {
          gubby->fvy = 0;
        }
        // Gentle wander instead of abrupt direction change
        if (gubby->fvx > -0.15f && gubby->fvx < 0.15f) {
          gubby->fvx = (rand() % 2 == 0 ? 0.6f : -0.6f) + (rand() % 100) / 200.0f;
        }
      }

      // Inter-gubby collision (push apart)
      for (int j = i + 1; j < MAX_GUBBYS; j++) {
        GubbyPet *other = gubby_at(j);
        if (!other || other->grabbed) {
          continue;
        }
        float dx = other->fx - gubby->fx;
        float dy = other->fy - gubby->fy;
        float dist_sq = dx * dx + dy * dy;
        if (dist_sq < 28.0f * 28.0f && dist_sq > 0.1f) {
          float push = 0.4f;
          float nx = dx > 0 ? push : -push;
          float ny = dy > 0 ? push : -push;
          gubby->fvx -= nx;
          gubby->fvy -= ny;
          other->fvx += nx;
          other->fvy += ny;
        }
      }

      gubby->x = (int)gubby->fx;
      gubby->y = (int)gubby->fy;
    }
    sync_primary_pet_position();
  }

  if (s_pet_seq && s_pet_frame_bitmap) {
    s_pet_frame_elapsed_ms += FRAME_MS;
    while (s_pet_frame_elapsed_ms >= s_pet_frame_delay_ms) {
      s_pet_frame_elapsed_ms -= s_pet_frame_delay_ms;
      bool next_frame = gbitmap_sequence_update_bitmap_next_frame(s_pet_seq, s_pet_frame_bitmap, &s_pet_frame_delay_ms);
      if (!next_frame) {
        gbitmap_sequence_restart(s_pet_seq);
        gbitmap_sequence_update_bitmap_next_frame(s_pet_seq, s_pet_frame_bitmap, &s_pet_frame_delay_ms);
      }
      if (s_pet_frame_delay_ms == 0) {
        s_pet_frame_delay_ms = FRAME_MS;
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
#if defined(PBL_SPEAKER)
  if (s_sfx_cooldown_ms > 0) {
    s_sfx_cooldown_ms -= GABAGOOL_MIN(s_sfx_cooldown_ms, FRAME_MS);
  }
#endif

  sync_static_pet_sprite();
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

static int16_t clamp_audio(int16_t value) {
  if (value < -128) {
    return -128;
  }
  if (value > 127) {
    return 127;
  }
  return value;
}

static int decode_audio_code(AudioDecoderState *decoder, uint8_t code) {
  int16_t step = s_audio_step_table[decoder->step_index];
  int16_t diff = step / 2;
  if (code & 1) {
    diff += step;
  }
  if (code & 2) {
    decoder->predictor -= diff;
  } else {
    decoder->predictor += diff;
  }
  decoder->predictor = clamp_audio(decoder->predictor);
  decoder->step_index += (code & 1) ? 2 : -1;
  if (decoder->step_index < 0) {
    decoder->step_index = 0;
  } else if (decoder->step_index >= (int)ARRAY_LENGTH(s_audio_step_table)) {
    decoder->step_index = ARRAY_LENGTH(s_audio_step_table) - 1;
  }
  return decoder->predictor;
}

static void audio_decoder_start(AudioDecoderState *decoder, int track_index) {
  if (track_index < 0 || track_index >= GABAGOOL_AUDIO_TRACK_COUNT) {
    memset(decoder, 0, sizeof(*decoder));
    return;
  }
  const GabagoolAudioTrack *track = &GABAGOOL_AUDIO_TRACKS[track_index];
  memset(decoder, 0, sizeof(*decoder));
  decoder->active = true;
  decoder->track_index = track_index;
  decoder->samples_remaining = track->total_samples;
  decoder->predictor = 0;
  decoder->step_index = 10;
  decoder->packed_shift = 8;
  decoder->handle = resource_get_handle(track->chunk_resource_ids[0]);
  decoder->chunk_size = resource_size(decoder->handle);
}

static bool audio_decoder_read_byte(AudioDecoderState *decoder, uint8_t *byte) {
  if (decoder->read_buf_pos < decoder->read_buf_len) {
    *byte = decoder->read_buf[decoder->read_buf_pos++];
    return true;
  }

  const GabagoolAudioTrack *track = &GABAGOOL_AUDIO_TRACKS[decoder->track_index];
  if (!decoder->active || decoder->chunk_index >= track->chunk_count) {
    return false;
  }

  if (decoder->chunk_offset >= decoder->chunk_size) {
    decoder->chunk_index++;
    decoder->chunk_offset = 0;
    if (decoder->chunk_index >= track->chunk_count) {
      return false;
    }
    decoder->handle = resource_get_handle(track->chunk_resource_ids[decoder->chunk_index]);
    decoder->chunk_size = resource_size(decoder->handle);
  }

  uint32_t to_read = decoder->chunk_size - decoder->chunk_offset;
  if (to_read > AUDIO_READ_BUFFER) {
    to_read = AUDIO_READ_BUFFER;
  }
  size_t read = resource_load_byte_range(decoder->handle, decoder->chunk_offset, decoder->read_buf, to_read);
  if (read == 0) {
    return false;
  }
  decoder->chunk_offset += read;
  decoder->read_buf_pos = 1;
  decoder->read_buf_len = (uint32_t)read;
  *byte = decoder->read_buf[0];
  return true;
}

static bool audio_decoder_next_output_sample(AudioDecoderState *decoder, int8_t *sample) {
  if (!decoder->active || decoder->samples_remaining == 0) {
    decoder->active = false;
    return false;
  }

  if (decoder->packed_shift >= 8) {
    if (!audio_decoder_read_byte(decoder, &decoder->packed_byte)) {
      decoder->active = false;
      return false;
    }
    decoder->packed_shift = 0;
  }

  uint8_t code = (decoder->packed_byte >> decoder->packed_shift) & 0x3;
  decoder->packed_shift += 2;
  *sample = (int8_t)decode_audio_code(decoder, code);
  decoder->samples_remaining--;
  return true;
}

static int random_music_track_except(int current_track) {
  if (GABAGOOL_MUSIC_TRACK_COUNT == 0) {
    return 0;
  }
  if (GABAGOOL_MUSIC_TRACK_COUNT > 1) {
    int next_track = current_track;
    while (next_track == current_track) {
      next_track = GABAGOOL_MUSIC_TRACK_INDEXES[rand() % GABAGOOL_MUSIC_TRACK_COUNT];
    }
    return next_track;
  }
  return GABAGOOL_MUSIC_TRACK_INDEXES[0];
}

static void play_random_track(void) {
  s_audio_track_index = random_music_track_except(s_audio_track_index);
  audio_decoder_start(&s_music_decoder, s_audio_track_index);
  if (!s_audio_active) {
    start_audio();
  }
}

static void play_gubby_sfx(void) {
#if GABAGOOL_GUBBY_SFX_TRACK_INDEX >= 0
  if (s_sfx_cooldown_ms > 0) {
    return;
  }
  s_sfx_cooldown_ms = SFX_COOLDOWN_MS;
  audio_decoder_start(&s_sfx_decoder, GABAGOOL_GUBBY_SFX_TRACK_INDEX);
  if (!s_audio_active) {
    start_audio();
  }
#endif
}

static bool fill_audio_buffer(void) {
  s_pcm_count = 0;
  s_pcm_offset = 0;

  for (uint32_t i = 0; i < AUDIO_PCM_BUFFER; i++) {
    int8_t music_sample = 0;
    if (!audio_decoder_next_output_sample(&s_music_decoder, &music_sample)) {
      s_audio_track_index = random_music_track_except(s_audio_track_index);
      audio_decoder_start(&s_music_decoder, s_audio_track_index);
      if (!audio_decoder_next_output_sample(&s_music_decoder, &music_sample)) {
        return s_pcm_count > 0;
      }
    }

    int16_t mixed = music_sample;
    int8_t sfx_sample = 0;
    if (audio_decoder_next_output_sample(&s_sfx_decoder, &sfx_sample)) {
      mixed += sfx_sample;
    }
    s_pcm_buffer[s_pcm_count++] = (int8_t)clamp_audio(mixed);
  }

  return s_pcm_count > 0;
}

static void audio_pump_callback(void *context) {
  (void)context;
  s_audio_timer = NULL;
  if (!s_audio_active) {
    return;
  }

  for (int loops = 0; loops < AUDIO_STREAM_PASSES; loops++) {
    if (s_pcm_offset >= s_pcm_count) {
      if (!fill_audio_buffer()) {
        s_audio_active = false;
        speaker_stream_close();
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
  if (!s_music_decoder.active) {
    audio_decoder_start(&s_music_decoder, s_audio_track_index);
  }
  s_audio_active = true;
  audio_schedule(1);
}

static void stop_audio(void) {
  audio_cancel_timer();
  if (s_audio_active) {
    speaker_stream_close();
    s_audio_active = false;
  }
  s_music_decoder.active = false;
  s_sfx_decoder.active = false;
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

  if (s_pet_state <= PetStateEggTap3) {
    // Start egg hold timer on button press
    s_egg_touching = true;
    s_egg_hold_ms = 0;
  } else if (s_pet_state == PetStateNormal || s_pet_state == PetStatePetted) {
    if (s_gubby_count == 0) {
      spawn_gubby_at(84, GABAGOOL_SCREEN_H / 2 - 16);
    }
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
    set_pet_animation(0);
    memset(s_gubbys, 0, sizeof(s_gubbys));
    s_gubby_count = 0;
    s_grabbed_gubby = -1;
  }
  redraw_now();
}

static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  reset_backlight();
  if (s_pet_state == PetStateNormal || s_pet_state == PetStatePetted) {
    if (s_gubby_count == 0) {
      spawn_gubby_at(84, GABAGOOL_SCREEN_H / 2 - 16);
    }
    s_grabbed_gubby = 0;
    GubbyPet *gubby = gubby_at(s_grabbed_gubby);
    if (gubby) {
      s_pet_grabbed = true;
      gubby->grabbed = true;
      gubby->fvx = 0.0f;
      gubby->fvy = 0.0f;
      gubby->last_touch_dx = 0;
      gubby->last_touch_dy = 0;
    }
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
    GubbyPet *gubby = gubby_at(s_grabbed_gubby);
    set_gubby_position(gubby, gubby ? gubby->x : s_pet_x, (gubby ? gubby->y : s_pet_y) - 15);
    if (gubby) {
      gubby->last_touch_dx = 0;
      gubby->last_touch_dy = -15;
    }
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
    GubbyPet *gubby = gubby_at(s_grabbed_gubby);
    set_gubby_position(gubby, gubby ? gubby->x : s_pet_x, (gubby ? gubby->y : s_pet_y) + 15);
    if (gubby) {
      gubby->last_touch_dx = 0;
      gubby->last_touch_dy = 15;
    }
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
      if (s_gubby_count == 0) {
        spawn_gubby_at(84, GABAGOOL_SCREEN_H / 2 - 16);
      }
      set_pet_animation(RESOURCE_ID_PET_DEATH_ANIM);
    }
  }

  // Load animation if active
  if (s_pet_state == PetStateNormal) {
    if (s_gubby_count == 0) {
      spawn_gubby_at(84, GABAGOOL_SCREEN_H / 2 - 16);
    }
    set_pet_animation(RESOURCE_ID_PET_GUBBY_ANIM);
  } else if (s_pet_state == PetStatePetted) {
    if (s_gubby_count == 0) {
      spawn_gubby_at(84, GABAGOOL_SCREEN_H / 2 - 16);
    }
    set_pet_animation(RESOURCE_ID_PET_PETTED_ANIM);
  } else if (s_pet_state == PetStateDead) {
    if (s_gubby_count == 0) {
      spawn_gubby_at(84, GABAGOOL_SCREEN_H / 2 - 16);
    }
    set_pet_animation(RESOURCE_ID_PET_DEATH_ANIM);
  }
  sync_static_pet_sprite();

#if defined(PBL_SPEAKER)
  s_audio_track_index = GABAGOOL_MUSIC_TRACK_INDEXES[rand() % GABAGOOL_MUSIC_TRACK_COUNT];
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
    if (s_pet_state <= PetStateEggTap3) {
      if (tx >= 36 && tx <= 36 + 128 && ty >= 50 && ty <= 50 + 128) {
        s_egg_touching = true;
        s_egg_hold_ms = 0;
      }
    } else if (s_pet_state == PetStateNormal || s_pet_state == PetStatePetted) {
      int hit = hit_test_gubby(tx, ty);
      if (hit >= 0) {
        GubbyPet *gubby = gubby_at(hit);
        s_pet_grabbed = true;
        s_grabbed_gubby = hit;
        gubby->grabbed = true;
        s_rub_count = 0;
        s_last_rub_dir = 0;
        gubby->grab_offset_x = clamp_int(tx - gubby->x, 0, 31);
        gubby->grab_offset_y = clamp_int(ty - gubby->y, 0, 31);
        gubby->last_touch_x = tx;
        gubby->last_touch_y = ty;
        gubby->last_touch_dx = 0;
        gubby->last_touch_dy = 0;
        gubby->fvx = 0.0f;
        gubby->fvy = 0.0f;
        drag_pet_to(tx, ty);
        
        s_pet_state = PetStatePetted;
        s_pet_petted_frames = 0;
        s_last_pet_time = time(NULL);
        persist_write_int(PERSIST_KEY_LAST_PET_TIME, s_last_pet_time);
        set_pet_animation(RESOURCE_ID_PET_PETTED_ANIM);
        play_gubby_sfx();
      }
    } else if (s_pet_state == PetStateDead) {
      if (hit_test_gubby(tx, ty) >= 0) {
        s_pet_state = PetStateEgg;
        s_egg_taps = 0;
        persist_write_int(PERSIST_KEY_PET_STATE, s_pet_state);
        set_pet_animation(0);
        memset(s_gubbys, 0, sizeof(s_gubbys));
        s_gubby_count = 0;
      }
    }
  } else if (event->type == TouchEvent_PositionUpdate) {
    if (s_pet_grabbed) {
      GubbyPet *gubby = gubby_at(s_grabbed_gubby);
      if (!gubby) {
        return;
      }
      gubby->last_touch_dx = tx - gubby->last_touch_x;
      gubby->last_touch_dy = ty - gubby->last_touch_y;
      drag_pet_to(tx, ty);
      
      int new_dir = 0;
      if (ty < gubby->last_touch_y - 8) {
        new_dir = -1;
      } else if (ty > gubby->last_touch_y + 8) {
        new_dir = 1;
      }
      
      if (new_dir != 0 && new_dir != s_last_rub_dir) {
        s_rub_count++;
        s_last_rub_dir = new_dir;
        if (s_rub_count >= 8) {
          trigger_breeding();
        }
      }
      gubby->last_touch_x = tx;
      gubby->last_touch_y = ty;
    }
  } else if (event->type == TouchEvent_Liftoff) {
    // Cancel egg hold on liftoff
    if (s_egg_touching) {
      s_egg_touching = false;
      s_egg_hold_ms = 0;
    }
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
