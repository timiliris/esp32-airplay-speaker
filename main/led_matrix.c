#include "led_matrix.h"

#include "settings.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <string.h>

static const char *TAG = "led_matrix";

// ============================================================================
// MAX7219 register addresses
// ============================================================================

#define MAX7219_REG_NOOP        0x00
#define MAX7219_REG_DIGIT0      0x01 // rows 0x01..0x08
#define MAX7219_REG_DECODEMODE  0x09
#define MAX7219_REG_INTENSITY   0x0A
#define MAX7219_REG_SCANLIMIT   0x0B
#define MAX7219_REG_SHUTDOWN    0x0C
#define MAX7219_REG_DISPLAYTEST 0x0F

#define MATRIX_FPS        50
#define MATRIX_FRAME_US   (1000000 / MATRIX_FPS)
#define MATRIX_TASK_STACK 3072
#define MATRIX_TASK_PRIO  2
#define MATRIX_TASK_CORE  0

// ============================================================================
// Shared audio-analysis state (single producer = audio task,
// single consumer = render task). Guarded by a tiny portMUX.
// ============================================================================

typedef struct {
  float level;  // overall RMS level, ~0..1 (smoothed + AGC)
  float bass;   // low-frequency energy, ~0..1
  float treble; // high-frequency energy, ~0..1
} matrix_audio_t;

static portMUX_TYPE s_audio_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile matrix_audio_t s_audio = {0};

// AGC peak tracker (owned by the audio task only).
static float s_peak = 1000.0f;

// ============================================================================
// Live config + runtime state
// ============================================================================

static volatile bool s_enabled = false; // fast-path gate for led_matrix_feed()

static bool s_running = false; // task + GPIOs currently active
static int s_fx = 0;
static int s_brightness = 4;
static int s_din = -1;
static int s_clk = -1;
static int s_cs = -1;

static TaskHandle_t s_task = NULL;
static volatile bool s_task_stop = false;

// ============================================================================
// MAX7219 bit-banged transport (16-bit frames, MSB first)
// ============================================================================

static void max7219_write(int din, int clk, int cs, uint8_t addr,
                          uint8_t data) {
  uint16_t frame = ((uint16_t)addr << 8) | data;
  gpio_set_level(cs, 0);
  for (int i = 15; i >= 0; i--) {
    gpio_set_level(clk, 0);
    gpio_set_level(din, (frame >> i) & 0x01);
    gpio_set_level(clk, 1); // data latched on rising edge
  }
  gpio_set_level(cs, 1); // latch the 16-bit frame
}

static void max7219_init_chip(int din, int clk, int cs, int brightness) {
  if (brightness < 0) {
    brightness = 0;
  } else if (brightness > 15) {
    brightness = 15;
  }
  max7219_write(din, clk, cs, MAX7219_REG_DECODEMODE, 0x00);
  max7219_write(din, clk, cs, MAX7219_REG_INTENSITY, (uint8_t)brightness);
  max7219_write(din, clk, cs, MAX7219_REG_SCANLIMIT, 0x07);
  max7219_write(din, clk, cs, MAX7219_REG_SHUTDOWN, 0x01);    // normal op
  max7219_write(din, clk, cs, MAX7219_REG_DISPLAYTEST, 0x00); // test off
}

static void max7219_push_frame(int din, int clk, int cs,
                               const uint8_t rows[8]) {
  for (int r = 0; r < 8; r++) {
    max7219_write(din, clk, cs, (uint8_t)(MAX7219_REG_DIGIT0 + r), rows[r]);
  }
}

static void gpios_config(int din, int clk, int cs) {
  uint64_t mask = (1ULL << din) | (1ULL << clk) | (1ULL << cs);
  gpio_config_t io = {
      .pin_bit_mask = mask,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io);
  gpio_set_level(cs, 1);
  gpio_set_level(clk, 0);
  gpio_set_level(din, 0);
}

static void gpios_release(int din, int clk, int cs) {
  // Return the pins to a benign input state so they are free for other use.
  if (din >= 0) {
    gpio_reset_pin((gpio_num_t)din);
  }
  if (clk >= 0) {
    gpio_reset_pin((gpio_num_t)clk);
  }
  if (cs >= 0) {
    gpio_reset_pin((gpio_num_t)cs);
  }
}

// ============================================================================
// Effects: compute an 8x8 framebuffer (rows[0]=top row).
// Bit b of rows[r]: column index = (7 - b) maps left..right; we use the
// low bits as the left columns for convenience. A "column height h" lights the
// bottom h pixels of that column (row 7 is the bottom).
// ============================================================================

static inline void set_pixel(uint8_t rows[8], int col, int row, bool on) {
  if (col < 0 || col > 7 || row < 0 || row > 7) {
    return;
  }
  uint8_t bit = (uint8_t)(1u << (7 - col));
  if (on) {
    rows[row] |= bit;
  } else {
    rows[row] &= (uint8_t)~bit;
  }
}

// Light the bottom `height` pixels (0..8) of a column.
static void draw_column(uint8_t rows[8], int col, int height) {
  if (height < 0) {
    height = 0;
  } else if (height > 8) {
    height = 8;
  }
  for (int h = 0; h < height; h++) {
    int row = 7 - h; // grow upward from the bottom
    set_pixel(rows, col, row, true);
  }
}

// --- Effect 0: VU-metre ----------------------------------------------------
static void fx_vu(uint8_t rows[8], const matrix_audio_t *a) {
  static float bar = 0.0f;
  static float peak = 0.0f;

  float target = a->level * 8.0f;
  if (target > bar) {
    bar = target; // attack instantly
  } else {
    bar -= 0.35f; // per-frame decay
    if (bar < target) {
      bar = target;
    }
    if (bar < 0.0f) {
      bar = 0.0f;
    }
  }

  if (bar > peak) {
    peak = bar;
  } else {
    peak -= 0.08f; // slow peak-hold fall
    if (peak < bar) {
      peak = bar;
    }
  }

  int height = (int)(bar + 0.5f);
  if (height > 8) {
    height = 8;
  }
  int peak_h = (int)(peak + 0.5f);
  if (peak_h > 8) {
    peak_h = 8;
  }

  for (int c = 0; c < 8; c++) {
    draw_column(rows, c, height);
    if (peak_h > 0) {
      set_pixel(rows, c, 8 - peak_h, true); // peak-hold top pixel
    }
  }
}

// --- Effect 1: Spectre -----------------------------------------------------
static void fx_spectrum(uint8_t rows[8], const matrix_audio_t *a) {
  static float cols[8] = {0};
  static uint32_t phase = 0;
  phase++;

  for (int c = 0; c < 8; c++) {
    // Blend bass (left) -> treble (right) across the 8 columns.
    float w = (float)c / 7.0f;
    float band = a->bass * (1.0f - w) + a->treble * w;
    // A little moving variation so it reads as a spectrum, not a flat bar.
    float wobble = 0.15f * sinf((float)phase * 0.18f + (float)c * 0.8f);
    float target = (band + wobble) * 8.0f;
    if (target < 0.0f) {
      target = 0.0f;
    }
    // Per-column smoothing: fast up, slow down.
    if (target > cols[c]) {
      cols[c] += (target - cols[c]) * 0.6f;
    } else {
      cols[c] += (target - cols[c]) * 0.25f;
    }
    int h = (int)(cols[c] + 0.5f);
    draw_column(rows, c, h);
  }
}

// --- Effect 2: Pulsation basses --------------------------------------------
static void fx_bass_pulse(uint8_t rows[8], const matrix_audio_t *a) {
  static float pulse = 0.0f;

  float target = a->bass;
  if (target > pulse) {
    pulse = target; // beat attack
  } else {
    pulse -= 0.06f; // decay
    if (pulse < target) {
      pulse = target;
    }
    if (pulse < 0.0f) {
      pulse = 0.0f;
    }
  }

  // Fill a centred square that grows with the bass energy.
  // radius 0..4 from the 8x8 centre.
  int radius = (int)(pulse * 4.0f + 0.5f);
  if (radius > 4) {
    radius = 4;
  }
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      int dr = (r < 4) ? (3 - r) : (r - 4);
      int dc = (c < 4) ? (3 - c) : (c - 4);
      int d = (dr > dc) ? dr : dc; // Chebyshev distance from centre
      if (d < radius) {
        set_pixel(rows, c, r, true);
      }
    }
  }
}

// --- Effect 3: Veilleuse (ambient, non-audio) ------------------------------
static void fx_nightlight(uint8_t rows[8]) {
  static float t = 0.0f;
  t += 0.06f;

  // A gentle scrolling sine wave with a soft breathing envelope.
  float breathe = 0.5f + 0.5f * sinf(t * 0.5f);
  for (int c = 0; c < 8; c++) {
    float s = sinf(t + (float)c * 0.7f); // -1..1
    int mid = (int)((s * 0.5f + 0.5f) * 7.0f + 0.5f);
    // Thickness follows the breathing envelope (1..3 pixels).
    int thick = 1 + (int)(breathe * 2.0f + 0.5f);
    for (int k = -thick / 2; k <= thick / 2; k++) {
      set_pixel(rows, c, mid + k, true);
    }
  }
}

// ============================================================================
// Render task
// ============================================================================

static void render_task(void *arg) {
  (void)arg;
  int din = s_din, clk = s_clk, cs = s_cs;
  uint8_t rows[8];

  int64_t next_us = esp_timer_get_time();

  while (!s_task_stop) {
    // Snapshot the shared audio state.
    matrix_audio_t a;
    portENTER_CRITICAL(&s_audio_mux);
    a.level = s_audio.level;
    a.bass = s_audio.bass;
    a.treble = s_audio.treble;
    portEXIT_CRITICAL(&s_audio_mux);

    memset(rows, 0, sizeof(rows));
    switch (s_fx) {
    case 1:
      fx_spectrum(rows, &a);
      break;
    case 2:
      fx_bass_pulse(rows, &a);
      break;
    case 3:
      fx_nightlight(rows);
      break;
    case 0:
    default:
      fx_vu(rows, &a);
      break;
    }

    max7219_push_frame(din, clk, cs, rows);

    // ~50 fps pacing using esp_timer for drift-free timing.
    next_us += MATRIX_FRAME_US;
    int64_t now = esp_timer_get_time();
    int64_t delay_us = next_us - now;
    if (delay_us < 1000) {
      // Fell behind (or first frame): resync and yield 1 tick.
      next_us = now;
      vTaskDelay(1);
    } else {
      TickType_t ticks = pdMS_TO_TICKS(delay_us / 1000);
      if (ticks == 0) {
        ticks = 1;
      }
      vTaskDelay(ticks);
    }
  }

  // Clear the display before exiting so it does not freeze on the last frame.
  memset(rows, 0, sizeof(rows));
  max7219_push_frame(din, clk, cs, rows);

  s_task = NULL;
  vTaskDelete(NULL);
}

// ============================================================================
// Start / stop helpers
// ============================================================================

static void matrix_stop(void) {
  if (!s_running) {
    return;
  }
  s_enabled = false; // gate led_matrix_feed() immediately

  if (s_task) {
    s_task_stop = true;
    // Wait for the task to self-delete (it clears s_task on exit).
    for (int i = 0; i < 200 && s_task != NULL; i++) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  s_task_stop = false;

  gpios_release(s_din, s_clk, s_cs);
  s_running = false;
  ESP_LOGI(TAG, "matrix stopped");
}

static void matrix_start(int fx, int brightness, int din, int clk, int cs) {
  if (din < 0 || clk < 0 || cs < 0) {
    ESP_LOGW(TAG, "matrix enabled but pins invalid (din=%d clk=%d cs=%d)", din,
             clk, cs);
    return;
  }

  s_fx = fx;
  s_brightness = brightness;
  s_din = din;
  s_clk = clk;
  s_cs = cs;

  gpios_config(din, clk, cs);
  max7219_init_chip(din, clk, cs, brightness);

  // Reset shared state so the first frames start clean.
  portENTER_CRITICAL(&s_audio_mux);
  s_audio.level = 0.0f;
  s_audio.bass = 0.0f;
  s_audio.treble = 0.0f;
  portEXIT_CRITICAL(&s_audio_mux);
  s_peak = 1000.0f;

  s_task_stop = false;
  s_running = true;
  s_enabled = true; // open the led_matrix_feed() gate

  BaseType_t ok = xTaskCreatePinnedToCore(
      render_task, "led_matrix", MATRIX_TASK_STACK, NULL, MATRIX_TASK_PRIO,
      &s_task, MATRIX_TASK_CORE);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "failed to create render task");
    s_enabled = false;
    s_running = false;
    s_task = NULL;
    gpios_release(din, clk, cs);
    return;
  }

  ESP_LOGI(TAG, "matrix started: fx=%d br=%d din=%d clk=%d cs=%d", fx,
           brightness, din, clk, cs);
}

// ============================================================================
// Public API
// ============================================================================

void led_matrix_init(void) {
  bool en = false;
  int fx = 0, br = 4, din = -1, clk = -1, cs = -1;
  settings_get_matrix(&en, &fx, &br, &din, &clk, &cs);

  if (!en) {
    ESP_LOGI(TAG, "matrix disabled (no GPIO use, no task)");
    return;
  }

  matrix_start(fx, br, din, clk, cs);
}

void led_matrix_reconfigure(void) {
  bool en = false;
  int fx = 0, br = 4, din = -1, clk = -1, cs = -1;
  settings_get_matrix(&en, &fx, &br, &din, &clk, &cs);

  if (!en) {
    matrix_stop();
    return;
  }

  // If already running with the same pins, apply fx/brightness live without a
  // restart.
  if (s_running && din == s_din && clk == s_clk && cs == s_cs) {
    s_fx = fx;
    if (br != s_brightness) {
      s_brightness = br;
      max7219_init_chip(s_din, s_clk, s_cs, br);
    }
    ESP_LOGI(TAG, "matrix updated live: fx=%d br=%d", fx, br);
    return;
  }

  // Pins changed or not running: restart cleanly.
  matrix_stop();
  matrix_start(fx, br, din, clk, cs);
}

void led_matrix_feed(const int16_t *pcm, size_t stereo_samples) {
  if (!s_enabled || stereo_samples == 0 || pcm == NULL) {
    return; // cheap early return when disabled
  }

  size_t total = stereo_samples * 2;

  // Overall RMS, plus a heavy low-pass (bass) and successive-difference energy
  // (treble), all in one cheap pass.
  uint64_t sum_sq = 0;
  uint64_t bass_sum = 0; // running low-pass of |sample|
  uint64_t diff_sum = 0; // |s[i] - s[i-1]| energy ~ treble
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

  // AGC: track a slowly-decaying peak so normalization adapts to volume.
  if (rms > s_peak) {
    s_peak = rms; // attack
  } else {
    s_peak *= 0.999f; // slow auto-decay
  }
  if (s_peak < 500.0f) {
    s_peak = 500.0f; // floor to avoid blowing up on silence
  }

  float level = rms / s_peak;
  float bass = (bass_abs * 1.4f) / s_peak;
  // Treble tends to be smaller in magnitude; scale up a touch.
  float treble = (treble_abs * 0.9f) / s_peak;

  if (level > 1.0f) {
    level = 1.0f;
  }
  if (bass > 1.0f) {
    bass = 1.0f;
  }
  if (treble > 1.0f) {
    treble = 1.0f;
  }

  // Smooth into the shared state (read by the render task).
  portENTER_CRITICAL(&s_audio_mux);
  float pl = s_audio.level, pb = s_audio.bass, pt = s_audio.treble;
  // Fast attack, slow decay for a lively but stable look.
  s_audio.level = (level > pl) ? level : (pl * 0.8f + level * 0.2f);
  s_audio.bass = (bass > pb) ? bass : (pb * 0.85f + bass * 0.15f);
  s_audio.treble = (treble > pt) ? treble : (pt * 0.7f + treble * 0.3f);
  portEXIT_CRITICAL(&s_audio_mux);
}
