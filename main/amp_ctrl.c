#include "amp_ctrl.h"

#include "settings.h"
#include "rtsp_events.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "amp_ctrl";

// Cached configuration (mirrors the NVS protection settings).
static int g_gpio = -1;          // -1 = disabled
static bool g_active_high = true; // amp enabled when GPIO high
static int g_standby_min = 5;     // 0 = never auto-standby

// One-shot standby timer (fires once after the idle timeout to mute the amp).
static esp_timer_handle_t g_standby_timer = NULL;
// Tracks the level we last drove so we avoid redundant GPIO writes / logs.
static bool g_is_active = false;

// Drive the GPIO to the amp-ACTIVE or amp-STANDBY level per the polarity.
static void drive_amp(bool active) {
  if (g_gpio < 0) {
    return;
  }
  int level = active ? (g_active_high ? 1 : 0) : (g_active_high ? 0 : 1);
  gpio_set_level((gpio_num_t)g_gpio, level);
  if (active != g_is_active) {
    ESP_LOGI(TAG, "Amp %s (GPIO%d=%d)", active ? "ACTIVE" : "STANDBY", g_gpio,
             level);
  }
  g_is_active = active;
}

static void standby_timer_cb(void *arg) {
  (void)arg;
  // Idle timeout elapsed: mute the amp to kill hiss and save power.
  drive_amp(false);
}

static void cancel_standby_timer(void) {
  if (g_standby_timer) {
    esp_timer_stop(g_standby_timer); // harmless if not running
  }
}

static void start_standby_timer(void) {
  if (g_gpio < 0 || g_standby_min <= 0 || !g_standby_timer) {
    return; // disabled, or "never auto-standby"
  }
  cancel_standby_timer();
  uint64_t us = (uint64_t)g_standby_min * 60ULL * 1000000ULL;
  esp_timer_start_once(g_standby_timer, us);
}

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)data;
  (void)user_data;
  if (g_gpio < 0) {
    return;
  }

  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
  case RTSP_EVENT_PLAYING:
  case RTSP_EVENT_METADATA:
    // Playback is (or is becoming) active: power the amp and cancel any
    // pending standby.
    cancel_standby_timer();
    drive_amp(true);
    break;
  case RTSP_EVENT_PAUSED:
  case RTSP_EVENT_DISCONNECTED:
    // Idle: arm the one-shot standby timer (no-op if standby_min == 0).
    start_standby_timer();
    break;
  default:
    break;
  }
}

// (Re)configure the GPIO and cached settings from NVS, and put the amp in a
// sane state (standby at boot / after a config change).
static void apply_config(void) {
  bool lim_en;
  int lim_ceil;
  int amp_gpio = -1;
  bool active_high = true;
  int standby_min = 5;
  settings_get_protection(&lim_en, &lim_ceil, &amp_gpio, &active_high,
                          &standby_min);

  cancel_standby_timer();

  // If the GPIO changed away from a previously configured pin, release it.
  if (g_gpio >= 0 && g_gpio != amp_gpio) {
    gpio_reset_pin((gpio_num_t)g_gpio);
  }

  g_gpio = amp_gpio;
  g_active_high = active_high;
  g_standby_min = standby_min;
  g_is_active = false; // force the next drive_amp() to log the level

  if (g_gpio < 0) {
    ESP_LOGI(TAG, "Amp control disabled");
    return;
  }

  gpio_config_t io = {
      .pin_bit_mask = 1ULL << (uint32_t)g_gpio,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io);

  // Start in standby (amp muted) so we never pop on boot or before playback.
  drive_amp(false);
  ESP_LOGI(TAG, "Amp control on GPIO%d (active_%s, standby=%d min)", g_gpio,
           g_active_high ? "high" : "low", g_standby_min);
}

void amp_ctrl_init(void) {
  if (!g_standby_timer) {
    const esp_timer_create_args_t args = {
        .callback = standby_timer_cb,
        .arg = NULL,
        .name = "amp_standby",
    };
    esp_timer_create(&args, &g_standby_timer);
  }

  apply_config();

  // Register a single RTSP listener (stays within the listener limit).
  static bool s_listener_registered = false;
  if (!s_listener_registered) {
    rtsp_events_register(on_rtsp_event, NULL);
    s_listener_registered = true;
  }
}

void amp_ctrl_reconfigure(void) { apply_config(); }
