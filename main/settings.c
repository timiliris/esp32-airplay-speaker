#include "settings.h"

#include "dac.h"
#include "esp_log.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include <string.h>

static const char *TAG = "settings";

#define NVS_NAMESPACE  "airplay"
#define NVS_KEY_VOLUME "volume_db"
#ifdef CONFIG_BT_A2DP_ENABLE
#define NVS_KEY_BT_VOLUME "bt_vol"
#endif
#define NVS_KEY_WIFI_SSID     "wifi_ssid"
#define NVS_KEY_WIFI_PASSWORD "wifi_pass"
#define NVS_KEY_DEVICE_NAME   "device_name"
#define NVS_KEY_EQ_GAINS      "eq_gains"
#define NVS_KEY_MAX_GAIN      "max_gain"
#define NVS_KEY_DEVICE_PW     "dev_pw"
#define NVS_KEY_MATRIX_EN     "matrix_en"
#define NVS_KEY_MATRIX_FX     "matrix_fx"
#define NVS_KEY_MATRIX_BR     "matrix_br"
#define NVS_KEY_MATRIX_DIN    "matrix_din"
#define NVS_KEY_MATRIX_CLK    "matrix_clk"
#define NVS_KEY_MATRIX_CS     "matrix_cs"
#define NVS_KEY_ARGB_EN       "argb_en"
#define NVS_KEY_ARGB_GPIO     "argb_gpio"
#define NVS_KEY_ARGB_COUNT    "argb_count"
#define NVS_KEY_ARGB_FX       "argb_fx"
#define NVS_KEY_ARGB_BR       "argb_br"
#define NVS_KEY_ARGB_COLOR    "argb_color"
#define NVS_KEY_ARGB_SPEED    "argb_speed"
#define NVS_KEY_BTN_PP        "btn_pp"
#define NVS_KEY_BTN_VU        "btn_vu"
#define NVS_KEY_BTN_VD        "btn_vd"
#define NVS_KEY_BTN_NX        "btn_nx"
#define NVS_KEY_BTN_PV        "btn_pv"
#define NVS_KEY_EQ_ON         "eq_on"
#define NVS_KEY_EQ_BASS       "eq_bass"
#define NVS_KEY_EQ_MID        "eq_mid"
#define NVS_KEY_EQ_TREBLE     "eq_treble"
#define NVS_KEY_EQ_HPF        "eq_hpf"
#define NVS_KEY_MQTT_EN       "mqtt_en"
#define NVS_KEY_MQTT_HOST     "mqtt_host"
#define NVS_KEY_MQTT_PORT     "mqtt_port"
#define NVS_KEY_MQTT_USER     "mqtt_user"
#define NVS_KEY_MQTT_PASS     "mqtt_pass"
#define NVS_KEY_LIM_EN        "lim_en"
#define NVS_KEY_LIM_CEILING   "lim_ceiling"
#define NVS_KEY_AMP_GPIO      "amp_gpio"
#define NVS_KEY_AMP_ACT_HIGH  "amp_active_high"
#define NVS_KEY_AMP_STBY_MIN  "amp_standby_min"
#define NVS_KEY_CHAN_MODE     "chan_mode"

// MQTT defaults (per shared contract): off, no broker, standard port.
#define MQTT_DEFAULT_EN   0
#define MQTT_DEFAULT_PORT 1883
#define MAX_MQTT_HOST_LEN 128
#define MAX_MQTT_USER_LEN 64
#define MAX_MQTT_PASS_LEN 64

// LED matrix defaults (per shared contract)
#define MATRIX_DEFAULT_EN  0
#define MATRIX_DEFAULT_FX  0
#define MATRIX_DEFAULT_BR  4
#define MATRIX_DEFAULT_DIN 4
#define MATRIX_DEFAULT_CLK 5
#define MATRIX_DEFAULT_CS  6

// Addressable RGB strip (WS2812) defaults: off, data GPIO8 (XIAO D9), 30 LEDs.
#define ARGB_DEFAULT_EN    0
#define ARGB_DEFAULT_GPIO  8
#define ARGB_DEFAULT_COUNT 30
#define ARGB_DEFAULT_FX    0
#define ARGB_DEFAULT_BR    128
#define ARGB_DEFAULT_COLOR 0x2080FFu  // pleasant blue (0xRRGGBB)
#define ARGB_DEFAULT_SPEED 5          // 1..10 animation speed

// Button GPIO defaults = Kconfig values (so behaviour is unchanged
// until the user overrides them via the API).
#define BTN_DEFAULT_PP CONFIG_BTN_PLAY_PAUSE_GPIO
#define BTN_DEFAULT_VU CONFIG_BTN_VOLUME_UP_GPIO
#define BTN_DEFAULT_VD CONFIG_BTN_VOLUME_DOWN_GPIO
#define BTN_DEFAULT_NX CONFIG_BTN_NEXT_GPIO
#define BTN_DEFAULT_PV CONFIG_BTN_PREV_GPIO

// Speaker-protection defaults (per shared contract). The limiter is ON by
// default (transparent until peaks, protects the speakers); the amp GPIO is
// OFF until the user wires their amp.
#define PROT_DEFAULT_LIM_EN     1
#define PROT_DEFAULT_LIM_CEIL   (-1)
#define PROT_LIM_CEIL_MIN       (-12)
#define PROT_LIM_CEIL_MAX       0
#define PROT_DEFAULT_AMP_GPIO   (-1)
#define PROT_DEFAULT_AMP_HIGH   1
#define PROT_DEFAULT_STBY_MIN   5
#define PROT_STBY_MIN_MIN       0
#define PROT_STBY_MIN_MAX       120

// Tone EQ defaults (per shared contract): off, flat, high-pass off.
#define TONE_DEFAULT_ON     0
#define TONE_DEFAULT_GAIN   0
#define TONE_GAIN_MIN       (-12)
#define TONE_GAIN_MAX       (12)
// High-pass cutoff (Hz): 0 = OFF, otherwise 40..400.
#define TONE_DEFAULT_HPF    0
#define TONE_HPF_MIN        40
#define TONE_HPF_MAX        400

#define MAX_WIFI_SSID_LEN     32
#define MAX_WIFI_PASSWORD_LEN 64
#define MAX_DEVICE_NAME_LEN   64

#define DEVICE_PW_DIGEST_LEN  32 /* SHA-256 */
#define MIN_DEVICE_PW_LEN     4

// Cached values  (defaults = 50 %)
static float g_volume_db = -15.0f;
static bool g_volume_loaded = false;

// Master output gain as a percentage (0-100). 100 = full (default).
static int g_max_gain = 100;

#ifdef CONFIG_BT_A2DP_ENABLE
static uint8_t g_bt_volume = 64; /* default: 50 % */
static bool g_bt_volume_loaded = false;
#endif

static float g_eq_gains[SETTINGS_EQ_BANDS];
static bool g_eq_loaded = false;

// Cached has-password flag (loaded in settings_init).
static bool g_has_device_pw = false;

esp_err_t settings_init(void) {
  // Load volume on init
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    int32_t vol_fixed;
    err = nvs_get_i32(nvs, NVS_KEY_VOLUME, &vol_fixed);
    if (err == ESP_OK) {
      g_volume_db = (float)vol_fixed / 100.0f;
      g_volume_loaded = true;
      ESP_LOGI(TAG, "Loaded volume: %.2f dB", g_volume_db);
    }

    /* Load EQ gains blob */
    size_t eq_size = sizeof(g_eq_gains);
    err = nvs_get_blob(nvs, NVS_KEY_EQ_GAINS, g_eq_gains, &eq_size);
    if (err == ESP_OK && eq_size == sizeof(g_eq_gains)) {
      g_eq_loaded = true;
      ESP_LOGI(TAG, "Loaded EQ gains (%d bands)", SETTINGS_EQ_BANDS);
    }

    /* Load master output gain */
    int32_t max_gain;
    if (nvs_get_i32(nvs, NVS_KEY_MAX_GAIN, &max_gain) == ESP_OK) {
      if (max_gain < 0) {
        max_gain = 0;
      } else if (max_gain > 100) {
        max_gain = 100;
      }
      g_max_gain = (int)max_gain;
      ESP_LOGI(TAG, "Loaded max gain: %d%%", g_max_gain);
    }

    /* Detect whether a device admin password is configured by probing for
       the digest blob's existence (without reading its bytes). */
    size_t pw_size = 0;
    if (nvs_get_blob(nvs, NVS_KEY_DEVICE_PW, NULL, &pw_size) == ESP_OK &&
        pw_size == DEVICE_PW_DIGEST_LEN) {
      g_has_device_pw = true;
      ESP_LOGI(TAG, "Device admin password is set");
    }

    nvs_close(nvs);
  }

  return ESP_OK;
}

esp_err_t settings_get_volume(float *volume_db) {
  if (!volume_db) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!g_volume_loaded) {
    return ESP_ERR_NOT_FOUND;
  }

  *volume_db = g_volume_db;
  return ESP_OK;
}

esp_err_t settings_set_volume(float volume_db) {
  // Skip if unchanged
  if (g_volume_loaded && volume_db == g_volume_db) {
    return ESP_OK;
  }

  dac_set_volume(volume_db);

  g_volume_db = volume_db;
  g_volume_loaded = true;
  return ESP_OK;
}

esp_err_t settings_persist_volume(void) {
  if (!g_volume_loaded) {
    return ESP_OK;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  int32_t vol_fixed = (int32_t)(g_volume_db * 100.0f);
  err = nvs_set_i32(nvs, NVS_KEY_VOLUME, vol_fixed);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Persisted volume: %.2f dB", g_volume_db);
  } else {
    ESP_LOGE(TAG, "Failed to persist volume: %s", esp_err_to_name(err));
  }

  return err;
}

#ifdef CONFIG_BT_A2DP_ENABLE
esp_err_t settings_get_bt_volume(uint8_t *volume) {
  if (!volume) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!g_bt_volume_loaded) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
      return err;
    }
    err = nvs_get_u8(nvs, NVS_KEY_BT_VOLUME, &g_bt_volume);
    nvs_close(nvs);
    if (err != ESP_OK) {
      return err;
    }
    g_bt_volume_loaded = true;
  }
  *volume = g_bt_volume;
  return ESP_OK;
}

esp_err_t settings_set_bt_volume(uint8_t volume) {
  if (g_bt_volume_loaded && volume == g_bt_volume) {
    return ESP_OK;
  }

  g_bt_volume = volume;
  g_bt_volume_loaded = true;
  return ESP_OK;
}

esp_err_t settings_persist_bt_volume(void) {
  if (!g_bt_volume_loaded) {
    return ESP_OK;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_u8(nvs, NVS_KEY_BT_VOLUME, g_bt_volume);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Persisted BT volume: %d/127", g_bt_volume);
  } else {
    ESP_LOGE(TAG, "Failed to persist BT volume: %s", esp_err_to_name(err));
  }
  return err;
}
#endif

esp_err_t settings_get_wifi_ssid(char *ssid, size_t len) {
  if (!ssid || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return ESP_ERR_NOT_FOUND;
  }

  size_t required_size = len;
  err = nvs_get_str(nvs, NVS_KEY_WIFI_SSID, ssid, &required_size);
  nvs_close(nvs);

  if (err == ESP_OK && required_size > len) {
    return ESP_ERR_NVS_INVALID_LENGTH;
  }

  return err;
}

esp_err_t settings_get_wifi_password(char *password, size_t len) {
  if (!password || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return ESP_ERR_NOT_FOUND;
  }

  size_t required_size = len;
  err = nvs_get_str(nvs, NVS_KEY_WIFI_PASSWORD, password, &required_size);
  nvs_close(nvs);

  if (err == ESP_OK && required_size > len) {
    return ESP_ERR_NVS_INVALID_LENGTH;
  }

  return err;
}

esp_err_t settings_set_wifi_credentials(const char *ssid,
                                        const char *password) {
  if (!ssid || strlen(ssid) == 0 || strlen(ssid) > MAX_WIFI_SSID_LEN) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!password || strlen(password) > MAX_WIFI_PASSWORD_LEN) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_str(nvs, NVS_KEY_WIFI_SSID, ssid);
  if (err == ESP_OK) {
    err = nvs_set_str(nvs, NVS_KEY_WIFI_PASSWORD, password);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved WiFi credentials: SSID=%s", ssid);
  } else {
    ESP_LOGE(TAG, "Failed to save WiFi credentials: %s", esp_err_to_name(err));
  }

  return err;
}

bool settings_has_wifi_credentials(void) {
  char ssid[MAX_WIFI_SSID_LEN + 1];
  return settings_get_wifi_ssid(ssid, sizeof(ssid)) == ESP_OK;
}

esp_err_t settings_get_device_name(char *name, size_t len) {
  if (!name || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    size_t required_size = len;
    err = nvs_get_str(nvs, NVS_KEY_DEVICE_NAME, name, &required_size);
    nvs_close(nvs);

    if (err == ESP_OK && required_size <= len) {
      return ESP_OK;
    }
  }

  // Return default if not found or error
  strncpy(name, SETTINGS_DEFAULT_DEVICE_NAME, len - 1);
  name[len - 1] = '\0';
  return ESP_OK;
}

esp_err_t settings_set_device_name(const char *name) {
  if (!name || strlen(name) == 0 || strlen(name) > MAX_DEVICE_NAME_LEN) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_str(nvs, NVS_KEY_DEVICE_NAME, name);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved device name: %s", name);
  } else {
    ESP_LOGE(TAG, "Failed to save device name: %s", esp_err_to_name(err));
  }

  return err;
}

/* ================================================================== */
/*  Master output gain                                                 */
/* ================================================================== */

esp_err_t settings_get_max_gain(int *percent) {
  if (!percent) {
    return ESP_ERR_INVALID_ARG;
  }
  *percent = g_max_gain;
  return ESP_OK;
}

int32_t settings_get_max_gain_q15(void) {
  return (int32_t)g_max_gain * 32768 / 100;
}

esp_err_t settings_set_max_gain(int percent) {
  if (percent < 0) {
    percent = 0;
  } else if (percent > 100) {
    percent = 100;
  }

  // Skip write if unchanged (reduces flash wear)
  if (percent == g_max_gain) {
    return ESP_OK;
  }

  // Apply immediately (the audio path reads g_max_gain every frame)
  g_max_gain = percent;

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_i32(nvs, NVS_KEY_MAX_GAIN, (int32_t)percent);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved max gain: %d%%", percent);
  } else {
    ESP_LOGE(TAG, "Failed to save max gain: %s", esp_err_to_name(err));
  }
  return err;
}

/* ================================================================== */
/*  EQ Gains                                                           */
/* ================================================================== */

esp_err_t settings_get_eq_gains(float gains_db[SETTINGS_EQ_BANDS]) {
  if (!gains_db) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!g_eq_loaded) {
    return ESP_ERR_NOT_FOUND;
  }

  memcpy(gains_db, g_eq_gains, sizeof(g_eq_gains));
  return ESP_OK;
}

esp_err_t settings_set_eq_gains(const float gains_db[SETTINGS_EQ_BANDS]) {
  if (!gains_db) {
    return ESP_ERR_INVALID_ARG;
  }

  /* Skip write if unchanged (compare element-by-element to avoid
     memcmp on floats, which is flagged by
     bugprone-suspicious-memory-comparison) */
  if (g_eq_loaded) {
    bool unchanged = true;
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      if (gains_db[i] != g_eq_gains[i]) {
        unchanged = false;
        break;
      }
    }
    if (unchanged) {
      return ESP_OK;
    }
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_blob(nvs, NVS_KEY_EQ_GAINS, gains_db,
                     sizeof(float) * SETTINGS_EQ_BANDS);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);

  if (err == ESP_OK) {
    memcpy(g_eq_gains, gains_db, sizeof(g_eq_gains));
    g_eq_loaded = true;
    ESP_LOGI(TAG, "Saved EQ gains (%d bands)", SETTINGS_EQ_BANDS);
  } else {
    ESP_LOGE(TAG, "Failed to save EQ gains: %s", esp_err_to_name(err));
  }

  return err;
}

esp_err_t settings_clear_eq(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_erase_key(nvs, NVS_KEY_EQ_GAINS);
  if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
    err = nvs_commit(nvs);
    if (err == ESP_OK) {
      memset(g_eq_gains, 0, sizeof(g_eq_gains));
      g_eq_loaded = false;
    } else {
      ESP_LOGE(TAG, "Failed to commit EQ clear: %s", esp_err_to_name(err));
    }
  }

  nvs_close(nvs);
  return err;
}

bool settings_has_eq(void) {
  return g_eq_loaded;
}

/* ================================================================== */
/*  Device admin password                                              */
/* ================================================================== */

bool settings_has_device_password(void) {
  return g_has_device_pw;
}

esp_err_t settings_set_device_password(const char *pw) {
  if (!pw || strlen(pw) < MIN_DEVICE_PW_LEN) {
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t digest[DEVICE_PW_DIGEST_LEN];
  if (mbedtls_sha256((const unsigned char *)pw, strlen(pw), digest, 0) != 0) {
    return ESP_FAIL;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_blob(nvs, NVS_KEY_DEVICE_PW, digest, sizeof(digest));
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    g_has_device_pw = true;
    ESP_LOGI(TAG, "Saved device admin password");
  } else {
    ESP_LOGE(TAG, "Failed to save device password: %s", esp_err_to_name(err));
  }
  return err;
}

bool settings_check_device_password(const char *pw) {
  if (!pw || !g_has_device_pw) {
    return false;
  }

  uint8_t candidate[DEVICE_PW_DIGEST_LEN];
  if (mbedtls_sha256((const unsigned char *)pw, strlen(pw), candidate, 0) != 0) {
    return false;
  }

  uint8_t stored[DEVICE_PW_DIGEST_LEN];
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return false;
  }
  size_t len = sizeof(stored);
  err = nvs_get_blob(nvs, NVS_KEY_DEVICE_PW, stored, &len);
  nvs_close(nvs);
  if (err != ESP_OK || len != DEVICE_PW_DIGEST_LEN) {
    return false;
  }

  /* Constant-time comparison: never short-circuit. */
  uint8_t diff = 0;
  for (size_t i = 0; i < DEVICE_PW_DIGEST_LEN; i++) {
    diff |= (uint8_t)(candidate[i] ^ stored[i]);
  }
  return diff == 0;
}

/* ================================================================== */
/*  Optional 8x8 LED matrix (MAX7219)                                  */
/* ================================================================== */

esp_err_t settings_get_matrix(bool *en, int *fx, int *br, int *din, int *clk,
                              int *cs) {
  // Start from defaults so unset keys yield the contract defaults.
  uint8_t v_en = MATRIX_DEFAULT_EN;
  uint8_t v_fx = MATRIX_DEFAULT_FX;
  uint8_t v_br = MATRIX_DEFAULT_BR;
  int8_t v_din = MATRIX_DEFAULT_DIN;
  int8_t v_clk = MATRIX_DEFAULT_CLK;
  int8_t v_cs = MATRIX_DEFAULT_CS;

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    uint8_t u8;
    int8_t i8;
    if (nvs_get_u8(nvs, NVS_KEY_MATRIX_EN, &u8) == ESP_OK) {
      v_en = u8;
    }
    if (nvs_get_u8(nvs, NVS_KEY_MATRIX_FX, &u8) == ESP_OK) {
      v_fx = u8;
    }
    if (nvs_get_u8(nvs, NVS_KEY_MATRIX_BR, &u8) == ESP_OK) {
      v_br = u8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_MATRIX_DIN, &i8) == ESP_OK) {
      v_din = i8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_MATRIX_CLK, &i8) == ESP_OK) {
      v_clk = i8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_MATRIX_CS, &i8) == ESP_OK) {
      v_cs = i8;
    }
    nvs_close(nvs);
  }

  if (en) {
    *en = (v_en != 0);
  }
  if (fx) {
    *fx = (int)v_fx;
  }
  if (br) {
    *br = (int)v_br;
  }
  if (din) {
    *din = (int)v_din;
  }
  if (clk) {
    *clk = (int)v_clk;
  }
  if (cs) {
    *cs = (int)v_cs;
  }
  return ESP_OK;
}

esp_err_t settings_set_matrix(bool en, int fx, int br, int din, int clk,
                              int cs) {
  // Validate ranges (effect 0-3, brightness 0-15, pins -1..48).
  if (fx < 0 || fx > 3) {
    return ESP_ERR_INVALID_ARG;
  }
  if (br < 0 || br > 15) {
    return ESP_ERR_INVALID_ARG;
  }
  if (din < -1 || din > 48 || clk < -1 || clk > 48 || cs < -1 || cs > 48) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_u8(nvs, NVS_KEY_MATRIX_EN, en ? 1 : 0);
  if (err == ESP_OK) {
    err = nvs_set_u8(nvs, NVS_KEY_MATRIX_FX, (uint8_t)fx);
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(nvs, NVS_KEY_MATRIX_BR, (uint8_t)br);
  }
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_MATRIX_DIN, (int8_t)din);
  }
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_MATRIX_CLK, (int8_t)clk);
  }
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_MATRIX_CS, (int8_t)cs);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved matrix: en=%d fx=%d br=%d din=%d clk=%d cs=%d", en, fx,
             br, din, clk, cs);
  } else {
    ESP_LOGE(TAG, "Failed to save matrix config: %s", esp_err_to_name(err));
  }
  return err;
}

esp_err_t settings_get_argb(bool *en, int *gpio, int *count, int *fx, int *br,
                            uint32_t *color, int *speed) {
  uint8_t v_en = ARGB_DEFAULT_EN;
  int8_t v_gpio = ARGB_DEFAULT_GPIO;
  uint16_t v_count = ARGB_DEFAULT_COUNT;
  uint8_t v_fx = ARGB_DEFAULT_FX;
  uint8_t v_br = ARGB_DEFAULT_BR;
  uint32_t v_color = ARGB_DEFAULT_COLOR;
  uint8_t v_speed = ARGB_DEFAULT_SPEED;

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    uint8_t u8;
    int8_t i8;
    uint16_t u16;
    uint32_t u32;
    if (nvs_get_u8(nvs, NVS_KEY_ARGB_EN, &u8) == ESP_OK) {
      v_en = u8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_ARGB_GPIO, &i8) == ESP_OK) {
      v_gpio = i8;
    }
    if (nvs_get_u16(nvs, NVS_KEY_ARGB_COUNT, &u16) == ESP_OK) {
      v_count = u16;
    }
    if (nvs_get_u8(nvs, NVS_KEY_ARGB_FX, &u8) == ESP_OK) {
      v_fx = u8;
    }
    if (nvs_get_u8(nvs, NVS_KEY_ARGB_BR, &u8) == ESP_OK) {
      v_br = u8;
    }
    if (nvs_get_u32(nvs, NVS_KEY_ARGB_COLOR, &u32) == ESP_OK) {
      v_color = u32;
    }
    if (nvs_get_u8(nvs, NVS_KEY_ARGB_SPEED, &u8) == ESP_OK) {
      v_speed = u8;
    }
    nvs_close(nvs);
  }

  if (en) {
    *en = (v_en != 0);
  }
  if (gpio) {
    *gpio = (int)v_gpio;
  }
  if (count) {
    *count = (int)v_count;
  }
  if (fx) {
    *fx = (int)v_fx;
  }
  if (br) {
    *br = (int)v_br;
  }
  if (color) {
    *color = v_color;
  }
  if (speed) {
    *speed = (int)v_speed;
  }
  return ESP_OK;
}

esp_err_t settings_set_argb(bool en, int gpio, int count, int fx, int br,
                            uint32_t color, int speed) {
  // Validate ranges (effect 0-11, brightness 0-255, gpio -1..48, count 1..300,
  // speed 1..10; colour is any 0xRRGGBB).
  if (fx < 0 || fx > 11) {
    return ESP_ERR_INVALID_ARG;
  }
  if (br < 0 || br > 255) {
    return ESP_ERR_INVALID_ARG;
  }
  if (gpio < -1 || gpio > 48) {
    return ESP_ERR_INVALID_ARG;
  }
  if (count < 1 || count > 300) {
    return ESP_ERR_INVALID_ARG;
  }
  if (speed < 1 || speed > 10) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_u8(nvs, NVS_KEY_ARGB_EN, en ? 1 : 0);
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_ARGB_GPIO, (int8_t)gpio);
  }
  if (err == ESP_OK) {
    err = nvs_set_u16(nvs, NVS_KEY_ARGB_COUNT, (uint16_t)count);
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(nvs, NVS_KEY_ARGB_FX, (uint8_t)fx);
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(nvs, NVS_KEY_ARGB_BR, (uint8_t)br);
  }
  if (err == ESP_OK) {
    err = nvs_set_u32(nvs, NVS_KEY_ARGB_COLOR, color & 0xFFFFFFu);
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(nvs, NVS_KEY_ARGB_SPEED, (uint8_t)speed);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved argb: en=%d gpio=%d count=%d fx=%d br=%d color=%06lX speed=%d",
             en, gpio, count, fx, br, (unsigned long)(color & 0xFFFFFFu), speed);
  } else {
    ESP_LOGE(TAG, "Failed to save argb config: %s", esp_err_to_name(err));
  }
  return err;
}

/* ================================================================== */
/*  Software 3-band tone EQ                                            */
/* ================================================================== */

esp_err_t settings_get_tone(bool *en, int *bass, int *mid, int *treble,
                            int *hpf) {
  // Start from defaults so unset keys yield the contract defaults.
  uint8_t v_en = TONE_DEFAULT_ON;
  int8_t v_bass = TONE_DEFAULT_GAIN;
  int8_t v_mid = TONE_DEFAULT_GAIN;
  int8_t v_treble = TONE_DEFAULT_GAIN;
  int32_t v_hpf = TONE_DEFAULT_HPF;

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    uint8_t u8;
    int8_t i8;
    int32_t i32;
    if (nvs_get_u8(nvs, NVS_KEY_EQ_ON, &u8) == ESP_OK) {
      v_en = u8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_EQ_BASS, &i8) == ESP_OK) {
      v_bass = i8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_EQ_MID, &i8) == ESP_OK) {
      v_mid = i8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_EQ_TREBLE, &i8) == ESP_OK) {
      v_treble = i8;
    }
    if (nvs_get_i32(nvs, NVS_KEY_EQ_HPF, &i32) == ESP_OK) {
      v_hpf = i32;
    }
    nvs_close(nvs);
  }

  if (en) {
    *en = (v_en != 0);
  }
  if (bass) {
    *bass = (int)v_bass;
  }
  if (mid) {
    *mid = (int)v_mid;
  }
  if (treble) {
    *treble = (int)v_treble;
  }
  if (hpf) {
    *hpf = (int)v_hpf;
  }
  return ESP_OK;
}

esp_err_t settings_set_tone(bool en, int bass, int mid, int treble, int hpf) {
  // Validate each gain in -12..+12 dB.
  if (bass < TONE_GAIN_MIN || bass > TONE_GAIN_MAX || mid < TONE_GAIN_MIN ||
      mid > TONE_GAIN_MAX || treble < TONE_GAIN_MIN || treble > TONE_GAIN_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  // Validate the high-pass cutoff: 0 (OFF) or 40..400 Hz.
  if (hpf != 0 && (hpf < TONE_HPF_MIN || hpf > TONE_HPF_MAX)) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_u8(nvs, NVS_KEY_EQ_ON, en ? 1 : 0);
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_EQ_BASS, (int8_t)bass);
  }
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_EQ_MID, (int8_t)mid);
  }
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_EQ_TREBLE, (int8_t)treble);
  }
  if (err == ESP_OK) {
    err = nvs_set_i32(nvs, NVS_KEY_EQ_HPF, (int32_t)hpf);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved tone EQ: en=%d bass=%d mid=%d treble=%d hpf=%d", en,
             bass, mid, treble, hpf);
  } else {
    ESP_LOGE(TAG, "Failed to save tone EQ: %s", esp_err_to_name(err));
  }
  return err;
}

/* ================================================================== */
/*  Home Assistant MQTT integration                                    */
/* ================================================================== */

esp_err_t settings_get_mqtt(bool *en, char *host, size_t hlen, int *port,
                            char *user, size_t ulen, char *pass, size_t plen) {
  // Start from defaults so unset keys yield the contract defaults.
  uint8_t v_en = MQTT_DEFAULT_EN;
  int32_t v_port = MQTT_DEFAULT_PORT;

  if (host && hlen) {
    host[0] = '\0';
  }
  if (user && ulen) {
    user[0] = '\0';
  }
  if (pass && plen) {
    pass[0] = '\0';
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    uint8_t u8;
    int32_t i32;
    if (nvs_get_u8(nvs, NVS_KEY_MQTT_EN, &u8) == ESP_OK) {
      v_en = u8;
    }
    if (nvs_get_i32(nvs, NVS_KEY_MQTT_PORT, &i32) == ESP_OK) {
      v_port = i32;
    }
    if (host && hlen) {
      size_t sz = hlen;
      nvs_get_str(nvs, NVS_KEY_MQTT_HOST, host, &sz);
    }
    if (user && ulen) {
      size_t sz = ulen;
      nvs_get_str(nvs, NVS_KEY_MQTT_USER, user, &sz);
    }
    if (pass && plen) {
      size_t sz = plen;
      nvs_get_str(nvs, NVS_KEY_MQTT_PASS, pass, &sz);
    }
    nvs_close(nvs);
  }

  if (en) {
    *en = (v_en != 0);
  }
  if (port) {
    *port = (int)v_port;
  }
  return ESP_OK;
}

esp_err_t settings_set_mqtt(bool en, const char *host, int port,
                            const char *user, const char *pass) {
  if (host && strlen(host) > MAX_MQTT_HOST_LEN) {
    return ESP_ERR_INVALID_ARG;
  }
  if (user && strlen(user) > MAX_MQTT_USER_LEN) {
    return ESP_ERR_INVALID_ARG;
  }
  if (pass && strlen(pass) > MAX_MQTT_PASS_LEN) {
    return ESP_ERR_INVALID_ARG;
  }
  if (port < 1 || port > 65535) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_u8(nvs, NVS_KEY_MQTT_EN, en ? 1 : 0);
  if (err == ESP_OK) {
    err = nvs_set_str(nvs, NVS_KEY_MQTT_HOST, host ? host : "");
  }
  if (err == ESP_OK) {
    err = nvs_set_i32(nvs, NVS_KEY_MQTT_PORT, (int32_t)port);
  }
  if (err == ESP_OK) {
    err = nvs_set_str(nvs, NVS_KEY_MQTT_USER, user ? user : "");
  }
  // A NULL password keeps whatever is already stored; "" clears it.
  if (err == ESP_OK && pass) {
    err = nvs_set_str(nvs, NVS_KEY_MQTT_PASS, pass);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved MQTT: en=%d host=%s port=%d user=%s", en,
             host ? host : "", port, user ? user : "");
  } else {
    ESP_LOGE(TAG, "Failed to save MQTT config: %s", esp_err_to_name(err));
  }
  return err;
}

/* ================================================================== */
/*  Physical buttons (runtime-configurable GPIOs)                      */
/* ================================================================== */

esp_err_t settings_get_buttons(int *pp, int *vu, int *vd, int *nx, int *pv) {
  // Start from the Kconfig defaults so unset keys yield unchanged behaviour.
  int8_t v_pp = BTN_DEFAULT_PP;
  int8_t v_vu = BTN_DEFAULT_VU;
  int8_t v_vd = BTN_DEFAULT_VD;
  int8_t v_nx = BTN_DEFAULT_NX;
  int8_t v_pv = BTN_DEFAULT_PV;

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    int8_t i8;
    if (nvs_get_i8(nvs, NVS_KEY_BTN_PP, &i8) == ESP_OK) {
      v_pp = i8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_BTN_VU, &i8) == ESP_OK) {
      v_vu = i8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_BTN_VD, &i8) == ESP_OK) {
      v_vd = i8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_BTN_NX, &i8) == ESP_OK) {
      v_nx = i8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_BTN_PV, &i8) == ESP_OK) {
      v_pv = i8;
    }
    nvs_close(nvs);
  }

  if (pp) {
    *pp = (int)v_pp;
  }
  if (vu) {
    *vu = (int)v_vu;
  }
  if (vd) {
    *vd = (int)v_vd;
  }
  if (nx) {
    *nx = (int)v_nx;
  }
  if (pv) {
    *pv = (int)v_pv;
  }
  return ESP_OK;
}

esp_err_t settings_set_buttons(int pp, int vu, int vd, int nx, int pv) {
  // Validate each GPIO (-1 = disabled, otherwise 0..48).
  if (pp < -1 || pp > 48 || vu < -1 || vu > 48 || vd < -1 || vd > 48 ||
      nx < -1 || nx > 48 || pv < -1 || pv > 48) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_i8(nvs, NVS_KEY_BTN_PP, (int8_t)pp);
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_BTN_VU, (int8_t)vu);
  }
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_BTN_VD, (int8_t)vd);
  }
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_BTN_NX, (int8_t)nx);
  }
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_BTN_PV, (int8_t)pv);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved buttons: pp=%d vu=%d vd=%d nx=%d pv=%d", pp, vu, vd, nx,
             pv);
  } else {
    ESP_LOGE(TAG, "Failed to save button config: %s", esp_err_to_name(err));
  }
  return err;
}

/* ================================================================== */
/*  Speaker protection (software limiter + amp mute/standby)           */
/* ================================================================== */

esp_err_t settings_get_protection(bool *limEn, int *limCeil, int *ampGpio,
                                  bool *ampActiveHigh, int *ampStandbyMin) {
  // Start from defaults so unset keys yield the contract defaults.
  uint8_t v_lim_en = PROT_DEFAULT_LIM_EN;
  int8_t v_lim_ceil = PROT_DEFAULT_LIM_CEIL;
  int8_t v_amp_gpio = PROT_DEFAULT_AMP_GPIO;
  uint8_t v_amp_high = PROT_DEFAULT_AMP_HIGH;
  int8_t v_stby_min = PROT_DEFAULT_STBY_MIN;

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    uint8_t u8;
    int8_t i8;
    if (nvs_get_u8(nvs, NVS_KEY_LIM_EN, &u8) == ESP_OK) {
      v_lim_en = u8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_LIM_CEILING, &i8) == ESP_OK) {
      v_lim_ceil = i8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_AMP_GPIO, &i8) == ESP_OK) {
      v_amp_gpio = i8;
    }
    if (nvs_get_u8(nvs, NVS_KEY_AMP_ACT_HIGH, &u8) == ESP_OK) {
      v_amp_high = u8;
    }
    if (nvs_get_i8(nvs, NVS_KEY_AMP_STBY_MIN, &i8) == ESP_OK) {
      v_stby_min = i8;
    }
    nvs_close(nvs);
  }

  if (limEn) {
    *limEn = (v_lim_en != 0);
  }
  if (limCeil) {
    *limCeil = (int)v_lim_ceil;
  }
  if (ampGpio) {
    *ampGpio = (int)v_amp_gpio;
  }
  if (ampActiveHigh) {
    *ampActiveHigh = (v_amp_high != 0);
  }
  if (ampStandbyMin) {
    *ampStandbyMin = (int)v_stby_min;
  }
  return ESP_OK;
}

esp_err_t settings_set_protection(bool limEn, int limCeil, int ampGpio,
                                  bool ampActiveHigh, int ampStandbyMin) {
  // Validate the limiter ceiling, amp GPIO and standby minutes.
  if (limCeil < PROT_LIM_CEIL_MIN || limCeil > PROT_LIM_CEIL_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  if (ampGpio < -1 || ampGpio > 48) {
    return ESP_ERR_INVALID_ARG;
  }
  if (ampStandbyMin < PROT_STBY_MIN_MIN || ampStandbyMin > PROT_STBY_MIN_MAX) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_u8(nvs, NVS_KEY_LIM_EN, limEn ? 1 : 0);
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_LIM_CEILING, (int8_t)limCeil);
  }
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_AMP_GPIO, (int8_t)ampGpio);
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(nvs, NVS_KEY_AMP_ACT_HIGH, ampActiveHigh ? 1 : 0);
  }
  if (err == ESP_OK) {
    err = nvs_set_i8(nvs, NVS_KEY_AMP_STBY_MIN, (int8_t)ampStandbyMin);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG,
             "Saved protection: lim_en=%d ceil=%d amp_gpio=%d active_high=%d "
             "standby=%d",
             limEn, limCeil, ampGpio, ampActiveHigh, ampStandbyMin);
  } else {
    ESP_LOGE(TAG, "Failed to save protection config: %s", esp_err_to_name(err));
  }
  return err;
}

/* ================================================================== */
/*  Software output channel mode                                       */
/* ================================================================== */

int settings_get_channel_mode(void) {
  uint8_t mode = 0; // default: stereo
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    uint8_t u8;
    if (nvs_get_u8(nvs, NVS_KEY_CHAN_MODE, &u8) == ESP_OK) {
      mode = u8;
    }
    nvs_close(nvs);
  }
  if (mode > 3) {
    mode = 0;
  }
  return (int)mode;
}

esp_err_t settings_set_channel_mode(int mode) {
  if (mode < 0 || mode > 3) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_u8(nvs, NVS_KEY_CHAN_MODE, (uint8_t)mode);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved channel mode: %d", mode);
  } else {
    ESP_LOGE(TAG, "Failed to save channel mode: %s", esp_err_to_name(err));
  }
  return err;
}
