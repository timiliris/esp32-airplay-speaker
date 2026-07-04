#include "led_argb.h"

#include "settings.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "led_argb";

// WS2812 over RMT. 10 MHz resolution => 0.1 us per tick.
//   bit0 = 0.3 us high, 0.9 us low      bit1 = 0.9 us high, 0.3 us low
// The latch/reset (>50 us low) is provided implicitly by the gap between
// frames (we render at ~50 fps => 20 ms idle between transmits).
#define ARGB_RES_HZ     (10 * 1000 * 1000)
#define ARGB_FPS        50
#define ARGB_FRAME_US   (1000000 / ARGB_FPS)
#define ARGB_TASK_STACK 4096
#define ARGB_TASK_PRIO  2
#define ARGB_TASK_CORE  0
#define ARGB_MAX_LEDS   300

// ============================================================================
// Shared audio-analysis state (single producer = audio task, single consumer =
// render task). Identical analysis to the LED matrix.
// ============================================================================

typedef struct {
  float level;  // overall RMS level, ~0..1 (smoothed + AGC)
  float bass;   // low-frequency energy, ~0..1
  float treble; // high-frequency energy, ~0..1
} argb_audio_t;

static portMUX_TYPE s_audio_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile argb_audio_t s_audio = {0};
static float s_peak = 1000.0f; // AGC peak tracker (audio task only)

// ============================================================================
// Live config + runtime state
// ============================================================================

static volatile bool s_enabled = false; // fast-path gate for led_argb_feed()
static bool s_running = false;
static int s_fx = 0;
static int s_brightness = 128; // 0..255 master scale
static int s_gpio = -1;
static int s_count = 0;
static uint8_t s_cr = 0x20, s_cg = 0x80, s_cb = 0xFF; // base colour (R,G,B)
static int s_speed = 5;                               // 1..10 animation speed

static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_encoder = NULL;
static uint8_t *s_fb = NULL;  // working RGB framebuffer (count*3: R,G,B)
static uint8_t *s_out = NULL; // transmit buffer (count*3: G,R,B for WS2812)
static uint8_t *s_aux = NULL; // per-LED scratch (twinkle decay), count bytes

static TaskHandle_t s_task = NULL;
static volatile bool s_task_stop = false;

// ============================================================================
// Colour helpers
// ============================================================================

// h,s,v in 0..255 -> r,g,b in 0..255 (cheap integer-ish HSV).
static void hsv2rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g,
                    uint8_t *b) {
  uint8_t region = h / 43;
  uint8_t rem = (h - region * 43) * 6;
  uint8_t p = (uint8_t)((v * (255 - s)) / 255);
  uint8_t q = (uint8_t)((v * (255 - ((s * rem) / 255))) / 255);
  uint8_t t = (uint8_t)((v * (255 - ((s * (255 - rem)) / 255))) / 255);
  switch (region) {
  case 0:
    *r = v;
    *g = t;
    *b = p;
    break;
  case 1:
    *r = q;
    *g = v;
    *b = p;
    break;
  case 2:
    *r = p;
    *g = v;
    *b = t;
    break;
  case 3:
    *r = p;
    *g = q;
    *b = v;
    break;
  case 4:
    *r = t;
    *g = p;
    *b = v;
    break;
  default:
    *r = v;
    *g = p;
    *b = q;
    break;
  }
}

static inline void fb_set(int i, uint8_t r, uint8_t g, uint8_t b) {
  if (i < 0 || i >= s_count) {
    return;
  }
  s_fb[i * 3 + 0] = r;
  s_fb[i * 3 + 1] = g;
  s_fb[i * 3 + 2] = b;
}

// Set LED i to the user base colour scaled by intensity v (0..255).
static inline void fb_set_color(int i, uint8_t v) {
  fb_set(i, (uint8_t)(s_cr * v / 255), (uint8_t)(s_cg * v / 255),
         (uint8_t)(s_cb * v / 255));
}

// ============================================================================
// Effects: render into s_fb (R,G,B per LED), full 0..255 — the master
// brightness is applied later in strip_show().
// ============================================================================

// --- 0: VU-metre (bar grows from the start, green->red, white peak dot) -----
static void fx_vu(const argb_audio_t *a) {
  static float bar = 0.0f, peak = 0.0f;
  int n = s_count;
  float target = a->level * (float)n;
  if (target > bar) {
    bar = target;
  } else {
    bar -= (float)n * 0.04f;
    if (bar < target)
      bar = target;
    if (bar < 0.0f)
      bar = 0.0f;
  }
  if (bar > peak) {
    peak = bar;
  } else {
    peak -= (float)n * 0.012f;
    if (peak < bar)
      peak = bar;
  }
  int lit = (int)(bar + 0.5f);
  if (lit > n)
    lit = n;
  int pk = (int)(peak + 0.5f);
  if (pk > n)
    pk = n;
  for (int i = 0; i < n; i++) {
    if (i < lit) {
      float frac = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
      uint8_t hue = (uint8_t)(96.0f * (1.0f - frac)); // green -> red
      uint8_t r, g, b;
      hsv2rgb(hue, 255, 255, &r, &g, &b);
      fb_set(i, r, g, b);
    } else {
      fb_set(i, 0, 0, 0);
    }
  }
  if (pk > 0 && pk <= n) {
    fb_set(pk - 1, 255, 255, 255);
  }
}

// --- 1: Spectre (bass at start -> treble at end) ----------------------------
static void fx_spectrum(const argb_audio_t *a) {
  int n = s_count;
  for (int i = 0; i < n; i++) {
    float w = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
    float band = a->bass * (1.0f - w) + a->treble * w;
    if (band < 0.0f)
      band = 0.0f;
    if (band > 1.0f)
      band = 1.0f;
    uint8_t hue = (uint8_t)(170.0f * w);
    uint8_t r, g, b;
    hsv2rgb(hue, 255, (uint8_t)(band * 255.0f), &r, &g, &b);
    fb_set(i, r, g, b);
  }
}

// --- 2: Pulsation basses (whole strip pulses in the base colour) ------------
static void fx_bass_pulse(const argb_audio_t *a) {
  static float pulse = 0.0f;
  float target = a->bass;
  if (target > pulse) {
    pulse = target;
  } else {
    pulse -= 0.05f;
    if (pulse < target)
      pulse = target;
    if (pulse < 0.0f)
      pulse = 0.0f;
  }
  uint8_t v = (uint8_t)(pulse * 255.0f);
  for (int i = 0; i < s_count; i++) {
    fb_set_color(i, v);
  }
}

// --- 3: Arc-en-ciel (flowing rainbow, speed-controlled) ---------------------
static void fx_rainbow(void) {
  static uint32_t t = 0;
  t += (uint32_t)s_speed;
  int n = s_count;
  for (int i = 0; i < n; i++) {
    uint8_t hue =
        (uint8_t)(((uint32_t)i * 256u / (uint32_t)(n > 0 ? n : 1) + t) & 0xFF);
    uint8_t r, g, b;
    hsv2rgb(hue, 255, 255, &r, &g, &b);
    fb_set(i, r, g, b);
  }
}

// --- 4: Veilleuse (warm white breathing, speed-controlled) ------------------
static void fx_nightlight(void) {
  static float t = 0.0f;
  t += 0.01f * (float)s_speed;
  float breathe = 0.35f + 0.35f * sinf(t * 0.5f);
  uint8_t v = (uint8_t)(breathe * 255.0f);
  for (int i = 0; i < s_count; i++) {
    fb_set(i, v, (uint8_t)(v * 0.6f), (uint8_t)(v * 0.18f));
  }
}

// --- 5: VU centre (bar grows from the middle outwards) ----------------------
static void fx_vu_center(const argb_audio_t *a) {
  static float bar = 0.0f;
  int n = s_count;
  int half = n / 2;
  float target = a->level * (float)(half > 0 ? half : 1);
  if (target > bar) {
    bar = target;
  } else {
    bar -= (float)n * 0.025f;
    if (bar < target)
      bar = target;
    if (bar < 0.0f)
      bar = 0.0f;
  }
  int lit = (int)(bar + 0.5f);
  for (int i = 0; i < n; i++) {
    int dist = (i < half) ? (half - 1 - i) : (i - half);
    if (dist < lit) {
      float frac = (half > 0) ? (float)dist / (float)half : 0.0f;
      uint8_t hue =
          (uint8_t)(96.0f * (1.0f - frac)); // green centre -> red edge
      uint8_t r, g, b;
      hsv2rgb(hue, 255, 255, &r, &g, &b);
      fb_set(i, r, g, b);
    } else {
      fb_set(i, 0, 0, 0);
    }
  }
}

// --- 6: Strobe basses (the strip flashes the base colour on each beat) ------
static void fx_beat_strobe(const argb_audio_t *a) {
  static float flash = 0.0f, prev = 0.0f;
  if (a->bass > prev + 0.13f && a->bass > 0.35f) {
    flash = 1.0f; // rising bass transient => trigger
  }
  prev = a->bass;
  flash -= 0.03f * (float)s_speed;
  if (flash < 0.0f)
    flash = 0.0f;
  uint8_t v = (uint8_t)(flash * 255.0f);
  for (int i = 0; i < s_count; i++) {
    fb_set_color(i, v);
  }
}

// --- 7: Couleur fixe (solid base colour) ------------------------------------
static void fx_solid(void) {
  for (int i = 0; i < s_count; i++) {
    fb_set(i, s_cr, s_cg, s_cb);
  }
}

// --- 8: Respiration (base colour breathing, speed-controlled) ---------------
static void fx_breathe(void) {
  static float t = 0.0f;
  t += 0.012f * (float)s_speed;
  float b = 0.12f + 0.88f * (0.5f + 0.5f * sinf(t));
  uint8_t v = (uint8_t)(b * 255.0f);
  for (int i = 0; i < s_count; i++) {
    fb_set_color(i, v);
  }
}

// --- 9: Comete / scanner (base colour dot sweeping with a fading tail) ------
static void fx_comet(void) {
  static float pos = 0.0f;
  static int dir = 1;
  int n = s_count;
  pos += (float)dir * (0.12f * (float)s_speed);
  if (pos >= (float)(n - 1)) {
    pos = (float)(n - 1);
    dir = -1;
  } else if (pos <= 0.0f) {
    pos = 0.0f;
    dir = 1;
  }
  int head = (int)(pos + 0.5f);
  for (int i = 0; i < n; i++) {
    int d = head - i;
    if (d < 0)
      d = -d;
    float fade = 1.0f - (float)d / 5.0f; // ~5-pixel tail
    if (fade < 0.0f)
      fade = 0.0f;
    fb_set_color(i, (uint8_t)(fade * 255.0f));
  }
}

// --- 10: Scintillement (random sparkles in the base colour) -----------------
static void fx_twinkle(void) {
  int n = s_count;
  int spawns = 1 + s_speed / 3;
  for (int k = 0; k < spawns; k++) {
    if ((esp_random() & 0xFF) < 100) {
      s_aux[esp_random() % (uint32_t)n] = 255;
    }
  }
  int dec = 5 + s_speed * 2;
  for (int i = 0; i < n; i++) {
    uint8_t v = s_aux[i];
    fb_set_color(i, v);
    s_aux[i] = (v > dec) ? (uint8_t)(v - dec) : 0;
  }
}

// --- 11: Niveau couleur (whole strip hue mapped from the loudness) ----------
static void fx_level_color(const argb_audio_t *a) {
  static float lv = 0.0f;
  float t = a->level;
  lv = (t > lv) ? t : (lv * 0.85f + t * 0.15f);
  uint8_t hue = (uint8_t)(160.0f * (1.0f - lv)); // blue (quiet) -> red (loud)
  uint8_t v = (uint8_t)(50.0f + 205.0f * lv);
  uint8_t r, g, b;
  hsv2rgb(hue, 255, v, &r, &g, &b);
  for (int i = 0; i < s_count; i++) {
    fb_set(i, r, g, b);
  }
}

// ============================================================================
// Strip output: apply master brightness, pack to GRB, transmit over RMT.
// ============================================================================

static void strip_show(void) {
  int br = s_brightness;
  if (br < 0)
    br = 0;
  if (br > 255)
    br = 255;
  for (int i = 0; i < s_count; i++) {
    uint8_t r = s_fb[i * 3 + 0];
    uint8_t g = s_fb[i * 3 + 1];
    uint8_t b = s_fb[i * 3 + 2];
    s_out[i * 3 + 0] = (uint8_t)((g * br) / 255); // WS2812 byte order is G,R,B
    s_out[i * 3 + 1] = (uint8_t)((r * br) / 255);
    s_out[i * 3 + 2] = (uint8_t)((b * br) / 255);
  }
  rmt_transmit_config_t tx_cfg = {.loop_count = 0};
  if (rmt_transmit(s_chan, s_encoder, s_out, (size_t)s_count * 3, &tx_cfg) ==
      ESP_OK) {
    rmt_tx_wait_all_done(s_chan, pdMS_TO_TICKS(50));
  }
}

// ============================================================================
// Render task
// ============================================================================

static void render_task(void *arg) {
  (void)arg;
  int64_t next_us = esp_timer_get_time();

  while (!s_task_stop) {
    argb_audio_t a;
    portENTER_CRITICAL(&s_audio_mux);
    a.level = s_audio.level;
    a.bass = s_audio.bass;
    a.treble = s_audio.treble;
    portEXIT_CRITICAL(&s_audio_mux);

    memset(s_fb, 0, (size_t)s_count * 3);
    switch (s_fx) {
    case 1:
      fx_spectrum(&a);
      break;
    case 2:
      fx_bass_pulse(&a);
      break;
    case 3:
      fx_rainbow();
      break;
    case 4:
      fx_nightlight();
      break;
    case 5:
      fx_vu_center(&a);
      break;
    case 6:
      fx_beat_strobe(&a);
      break;
    case 7:
      fx_solid();
      break;
    case 8:
      fx_breathe();
      break;
    case 9:
      fx_comet();
      break;
    case 10:
      fx_twinkle();
      break;
    case 11:
      fx_level_color(&a);
      break;
    case 0:
    default:
      fx_vu(&a);
      break;
    }
    strip_show();

    next_us += ARGB_FRAME_US;
    int64_t now = esp_timer_get_time();
    int64_t delay_us = next_us - now;
    if (delay_us < 1000) {
      next_us = now;
      vTaskDelay(1);
    } else {
      TickType_t ticks = pdMS_TO_TICKS(delay_us / 1000);
      if (ticks == 0)
        ticks = 1;
      vTaskDelay(ticks);
    }
  }

  memset(s_fb, 0, (size_t)s_count * 3);
  strip_show();

  s_task = NULL;
  vTaskDelete(NULL);
}

// ============================================================================
// Start / stop
// ============================================================================

static void argb_free_rmt(void) {
  if (s_chan) {
    rmt_disable(s_chan);
  }
  if (s_encoder) {
    rmt_del_encoder(s_encoder);
    s_encoder = NULL;
  }
  if (s_chan) {
    rmt_del_channel(s_chan);
    s_chan = NULL;
  }
}

static void argb_free_buffers(void) {
  free(s_fb);
  s_fb = NULL;
  free(s_out);
  s_out = NULL;
  free(s_aux);
  s_aux = NULL;
}

static void argb_stop(void) {
  if (!s_running) {
    return;
  }
  s_enabled = false;

  if (s_task) {
    s_task_stop = true;
    for (int i = 0; i < 200 && s_task != NULL; i++) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  s_task_stop = false;

  argb_free_rmt();
  argb_free_buffers();
  s_running = false;
  ESP_LOGI(TAG, "strip stopped");
}

static void apply_color(uint32_t color) {
  s_cr = (uint8_t)((color >> 16) & 0xFF);
  s_cg = (uint8_t)((color >> 8) & 0xFF);
  s_cb = (uint8_t)(color & 0xFF);
}

static void argb_start(int fx, int brightness, int gpio, int count,
                       uint32_t color, int speed) {
  if (gpio < 0 || count <= 0) {
    ESP_LOGW(TAG, "strip enabled but invalid (gpio=%d count=%d)", gpio, count);
    return;
  }
  if (count > ARGB_MAX_LEDS) {
    count = ARGB_MAX_LEDS;
  }

  s_fb = malloc((size_t)count * 3);
  s_out = malloc((size_t)count * 3);
  s_aux = calloc((size_t)count, 1);
  if (!s_fb || !s_out || !s_aux) {
    ESP_LOGE(TAG, "strip buffer alloc failed");
    argb_free_buffers();
    return;
  }
  memset(s_fb, 0, (size_t)count * 3);
  memset(s_out, 0, (size_t)count * 3);

  rmt_tx_channel_config_t chan_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .gpio_num = gpio,
      .mem_block_symbols = 64,
      .resolution_hz = ARGB_RES_HZ,
      .trans_queue_depth = 4,
  };
  if (rmt_new_tx_channel(&chan_cfg, &s_chan) != ESP_OK) {
    ESP_LOGE(TAG, "rmt_new_tx_channel failed (gpio=%d)", gpio);
    argb_free_buffers();
    return;
  }
  rmt_bytes_encoder_config_t bytes_cfg = {
      .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
      .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
      .flags = {.msb_first = 1},
  };
  if (rmt_new_bytes_encoder(&bytes_cfg, &s_encoder) != ESP_OK ||
      rmt_enable(s_chan) != ESP_OK) {
    ESP_LOGE(TAG, "rmt encoder/enable failed");
    argb_free_rmt();
    argb_free_buffers();
    return;
  }

  s_fx = fx;
  s_brightness = brightness;
  s_gpio = gpio;
  s_count = count;
  apply_color(color);
  s_speed = (speed < 1) ? 1 : (speed > 10 ? 10 : speed);

  portENTER_CRITICAL(&s_audio_mux);
  s_audio.level = 0.0f;
  s_audio.bass = 0.0f;
  s_audio.treble = 0.0f;
  portEXIT_CRITICAL(&s_audio_mux);
  s_peak = 1000.0f;

  s_task_stop = false;
  s_running = true;
  s_enabled = true;

  BaseType_t ok =
      xTaskCreatePinnedToCore(render_task, "led_argb", ARGB_TASK_STACK, NULL,
                              ARGB_TASK_PRIO, &s_task, ARGB_TASK_CORE);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "failed to create render task");
    s_enabled = false;
    s_running = false;
    s_task = NULL;
    argb_free_rmt();
    argb_free_buffers();
    return;
  }

  ESP_LOGI(TAG, "strip started: fx=%d br=%d gpio=%d count=%d speed=%d", fx,
           brightness, gpio, count, s_speed);
}

// ============================================================================
// Public API
// ============================================================================

void led_argb_init(void) {
  bool en = false;
  int fx = 0, br = 128, gpio = -1, count = 0, speed = 5;
  uint32_t color = 0x2080FF;
  settings_get_argb(&en, &gpio, &count, &fx, &br, &color, &speed);
  if (!en) {
    ESP_LOGI(TAG, "strip disabled (no RMT, no task)");
    return;
  }
  argb_start(fx, br, gpio, count, color, speed);
}

void led_argb_reconfigure(void) {
  bool en = false;
  int fx = 0, br = 128, gpio = -1, count = 0, speed = 5;
  uint32_t color = 0x2080FF;
  settings_get_argb(&en, &gpio, &count, &fx, &br, &color, &speed);

  if (!en) {
    argb_stop();
    return;
  }

  // Same GPIO + count already running: apply fx/brightness/colour/speed live.
  if (s_running && gpio == s_gpio && count == s_count) {
    s_fx = fx;
    s_brightness = br;
    apply_color(color);
    s_speed = (speed < 1) ? 1 : (speed > 10 ? 10 : speed);
    ESP_LOGI(TAG, "strip updated live: fx=%d br=%d speed=%d", fx, br, s_speed);
    return;
  }

  argb_stop();
  argb_start(fx, br, gpio, count, color, speed);
}

void led_argb_feed(const int16_t *pcm, size_t stereo_samples) {
  if (!s_enabled || stereo_samples == 0 || pcm == NULL) {
    return;
  }

  size_t total = stereo_samples * 2;
  uint64_t sum_sq = 0, bass_sum = 0, diff_sum = 0;
  int32_t prev = pcm[0];
  for (size_t i = 0; i < total; i++) {
    int32_t s = pcm[i];
    sum_sq += (uint64_t)(s * s);
    int32_t as = s < 0 ? -s : s;
    bass_sum += (uint64_t)as;
    int32_t d = s - prev;
    diff_sum += (uint64_t)(d < 0 ? -d : d);
    prev = s;
  }

  float rms = sqrtf((float)sum_sq / (float)total);
  float bass_abs = (float)bass_sum / (float)total;
  float treble_abs = (float)diff_sum / (float)total;

  if (rms > s_peak) {
    s_peak = rms;
  } else {
    s_peak *= 0.999f;
  }
  if (s_peak < 500.0f) {
    s_peak = 500.0f;
  }

  float level = rms / s_peak;
  float bass = (bass_abs * 1.4f) / s_peak;
  float treble = (treble_abs * 0.9f) / s_peak;
  if (level > 1.0f)
    level = 1.0f;
  if (bass > 1.0f)
    bass = 1.0f;
  if (treble > 1.0f)
    treble = 1.0f;

  portENTER_CRITICAL(&s_audio_mux);
  float pl = s_audio.level, pb = s_audio.bass, pt = s_audio.treble;
  s_audio.level = (level > pl) ? level : (pl * 0.8f + level * 0.2f);
  s_audio.bass = (bass > pb) ? bass : (pb * 0.85f + bass * 0.15f);
  s_audio.treble = (treble > pt) ? treble : (pt * 0.7f + treble * 0.3f);
  portEXIT_CRITICAL(&s_audio_mux);
}
