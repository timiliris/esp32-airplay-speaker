#include "ha_mqtt.h"

#include "cJSON.h"
#include "eq_dsp.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "led_matrix.h"
#include "mqtt_client.h"
#include "rtsp_events.h"
#include "settings.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ha_mqtt";

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

#define ID_MAX        24  /* "airplay2_" + 6 hex + NUL */
#define TOPIC_MAX     96
#define PAYLOAD_MAX   512

/* Refresh all state topics every ~30s so HA stays in sync with changes that
   happen outside this module (web API, physical buttons). */
#define REFRESH_PERIOD_MS 30000

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static char s_id[ID_MAX] = {0};   /* device id, e.g. "airplay2_a1b2c3" */
static char s_base[40] = {0};     /* "airplay2/<id>" */
static char s_status_topic[48] = {0};

static TimerHandle_t s_refresh_timer = NULL;

/* Latest now-playing snapshot, kept fresh by the RTSP event listener. */
typedef struct {
  bool playing;
  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
} np_state_t;
static np_state_t s_np;

/* EQ presets: name -> (bass, mid, treble, hpf). Index 0 is the default. */
typedef struct {
  const char *name;
  int bass;
  int mid;
  int treble;
  int hpf;
} eq_preset_t;

static const eq_preset_t EQ_PRESETS[] = {
    {"Plat", 0, 0, 0, 0},        {"Basses+", 6, 0, 1, 0},
    {"Voix", -3, 4, 2, 0},       {"Pop", 4, -1, 3, 0},
    {"Live", 3, 1, 3, 0},        {"Satellites", -3, 0, 2, 120},
};
#define EQ_PRESET_COUNT (sizeof(EQ_PRESETS) / sizeof(EQ_PRESETS[0]))

/* LED matrix effect names, indexed by matrix_fx (0..3). */
static const char *MATRIX_EFFECTS[] = {"VU", "Spectre", "Basses", "Veilleuse"};
#define MATRIX_EFFECT_COUNT (sizeof(MATRIX_EFFECTS) / sizeof(MATRIX_EFFECTS[0]))

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

static void build_identity(void) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_id, sizeof(s_id), "airplay2_%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(s_base, sizeof(s_base), "airplay2/%s", s_id);
  snprintf(s_status_topic, sizeof(s_status_topic), "%s/status", s_base);
}

/* Publish a NUL-terminated string to a topic. No-op if not connected. */
static void mqtt_pub(const char *topic, const char *payload, bool retain) {
  if (!s_client || !s_connected) {
    return;
  }
  esp_mqtt_client_publish(s_client, topic, payload ? payload : "",
                          payload ? (int)strlen(payload) : 0, 1, retain ? 1 : 0);
}

/* Build a "<base>/<suffix>" topic into a caller buffer. */
static void topic_of(char *out, size_t out_len, const char *suffix) {
  snprintf(out, out_len, "%s/%s", s_base, suffix);
}

/* Add the shared HA "dev" block + availability to a discovery payload. */
static void add_common(cJSON *cfg, const char *uniq) {
  char dev_name[65] = {0};
  settings_get_device_name(dev_name, sizeof(dev_name));
  const esp_app_desc_t *app = esp_app_get_description();

  cJSON_AddStringToObject(cfg, "avty_t", s_status_topic);
  cJSON_AddStringToObject(cfg, "pl_avail", "online");
  cJSON_AddStringToObject(cfg, "pl_not_avail", "offline");
  cJSON_AddStringToObject(cfg, "uniq_id", uniq);

  cJSON *dev = cJSON_AddObjectToObject(cfg, "dev");
  cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
  cJSON_AddItemToArray(ids, cJSON_CreateString(s_id));
  cJSON_AddStringToObject(dev, "name", dev_name);
  cJSON_AddStringToObject(dev, "mf", "DIY");
  cJSON_AddStringToObject(dev, "mdl", "ESP32-S3 AirPlay 2");
  cJSON_AddStringToObject(dev, "sw", app ? app->version : "?");
}

/* Publish one discovery config (retained) and free the payload. */
static void publish_discovery(const char *component, const char *obj,
                              cJSON *cfg) {
  char topic[TOPIC_MAX];
  snprintf(topic, sizeof(topic), "homeassistant/%s/%s_%s/config", component,
           s_id, obj);
  char *json = cJSON_PrintUnformatted(cfg);
  if (json) {
    mqtt_pub(topic, json, true);
    free(json);
  }
  cJSON_Delete(cfg);
}

/* ------------------------------------------------------------------ */
/*  Discovery payloads                                                 */
/* ------------------------------------------------------------------ */

static void publish_all_discovery(void) {
  char t[TOPIC_MAX];
  char uniq[ID_MAX + 16];

  /* --- sensor: playback state --- */
  {
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", "State");
    topic_of(t, sizeof(t), "state");
    cJSON_AddStringToObject(cfg, "stat_t", t);
    cJSON_AddStringToObject(cfg, "ic", "mdi:music");
    snprintf(uniq, sizeof(uniq), "%s_state", s_id);
    add_common(cfg, uniq);
    publish_discovery("sensor", "state", cfg);
  }

  /* --- sensor: title --- */
  {
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", "Title");
    topic_of(t, sizeof(t), "title");
    cJSON_AddStringToObject(cfg, "stat_t", t);
    cJSON_AddStringToObject(cfg, "ic", "mdi:music-note");
    snprintf(uniq, sizeof(uniq), "%s_title", s_id);
    add_common(cfg, uniq);
    publish_discovery("sensor", "title", cfg);
  }

  /* --- sensor: artist --- */
  {
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", "Artist");
    topic_of(t, sizeof(t), "artist");
    cJSON_AddStringToObject(cfg, "stat_t", t);
    cJSON_AddStringToObject(cfg, "ic", "mdi:account-music");
    snprintf(uniq, sizeof(uniq), "%s_artist", s_id);
    add_common(cfg, uniq);
    publish_discovery("sensor", "artist", cfg);
  }

  /* --- number: master volume (0..100%) --- */
  {
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", "Volume");
    topic_of(t, sizeof(t), "volume/set");
    cJSON_AddStringToObject(cfg, "cmd_t", t);
    topic_of(t, sizeof(t), "volume/state");
    cJSON_AddStringToObject(cfg, "stat_t", t);
    cJSON_AddNumberToObject(cfg, "min", 0);
    cJSON_AddNumberToObject(cfg, "max", 100);
    cJSON_AddNumberToObject(cfg, "step", 1);
    cJSON_AddStringToObject(cfg, "unit_of_meas", "%");
    cJSON_AddStringToObject(cfg, "ic", "mdi:volume-high");
    snprintf(uniq, sizeof(uniq), "%s_volume", s_id);
    add_common(cfg, uniq);
    publish_discovery("number", "volume", cfg);
  }

  /* --- switch: EQ enable --- */
  {
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", "EQ");
    topic_of(t, sizeof(t), "eq/set");
    cJSON_AddStringToObject(cfg, "cmd_t", t);
    topic_of(t, sizeof(t), "eq/state");
    cJSON_AddStringToObject(cfg, "stat_t", t);
    cJSON_AddStringToObject(cfg, "pl_on", "ON");
    cJSON_AddStringToObject(cfg, "pl_off", "OFF");
    cJSON_AddStringToObject(cfg, "ic", "mdi:equalizer");
    snprintf(uniq, sizeof(uniq), "%s_eq", s_id);
    add_common(cfg, uniq);
    publish_discovery("switch", "eq", cfg);
  }

  /* --- select: EQ preset --- */
  {
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", "EQ Preset");
    topic_of(t, sizeof(t), "eqpreset/set");
    cJSON_AddStringToObject(cfg, "cmd_t", t);
    topic_of(t, sizeof(t), "eqpreset/state");
    cJSON_AddStringToObject(cfg, "stat_t", t);
    cJSON *opts = cJSON_AddArrayToObject(cfg, "options");
    for (size_t i = 0; i < EQ_PRESET_COUNT; i++) {
      cJSON_AddItemToArray(opts, cJSON_CreateString(EQ_PRESETS[i].name));
    }
    cJSON_AddStringToObject(cfg, "ic", "mdi:tune");
    snprintf(uniq, sizeof(uniq), "%s_eqpreset", s_id);
    add_common(cfg, uniq);
    publish_discovery("select", "eqpreset", cfg);
  }

  /* --- light: LED matrix (json schema) --- */
  {
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", "Matrix");
    cJSON_AddStringToObject(cfg, "schema", "json");
    topic_of(t, sizeof(t), "matrix/set");
    cJSON_AddStringToObject(cfg, "cmd_t", t);
    topic_of(t, sizeof(t), "matrix/state");
    cJSON_AddStringToObject(cfg, "stat_t", t);
    cJSON_AddBoolToObject(cfg, "brightness", true);
    cJSON_AddBoolToObject(cfg, "effect", true);
    cJSON *fx = cJSON_AddArrayToObject(cfg, "effect_list");
    for (size_t i = 0; i < MATRIX_EFFECT_COUNT; i++) {
      cJSON_AddItemToArray(fx, cJSON_CreateString(MATRIX_EFFECTS[i]));
    }
    cJSON_AddStringToObject(cfg, "ic", "mdi:dots-grid");
    snprintf(uniq, sizeof(uniq), "%s_matrix", s_id);
    add_common(cfg, uniq);
    publish_discovery("light", "matrix", cfg);
  }

  /* --- button: restart --- */
  {
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", "Restart");
    topic_of(t, sizeof(t), "restart/set");
    cJSON_AddStringToObject(cfg, "cmd_t", t);
    cJSON_AddStringToObject(cfg, "pl_prs", "PRESS");
    cJSON_AddStringToObject(cfg, "dev_cla", "restart");
    cJSON_AddStringToObject(cfg, "ic", "mdi:restart");
    snprintf(uniq, sizeof(uniq), "%s_restart", s_id);
    add_common(cfg, uniq);
    publish_discovery("button", "restart", cfg);
  }
}

/* ------------------------------------------------------------------ */
/*  State publishers                                                   */
/* ------------------------------------------------------------------ */

static void publish_nowplaying(void) {
  char t[TOPIC_MAX];
  topic_of(t, sizeof(t), "state");
  mqtt_pub(t, s_np.playing ? "playing" : "idle", true);
  topic_of(t, sizeof(t), "title");
  mqtt_pub(t, s_np.title, true);
  topic_of(t, sizeof(t), "artist");
  mqtt_pub(t, s_np.artist, true);
}

static void publish_volume(void) {
  int percent = 100;
  settings_get_max_gain(&percent);
  char val[8];
  snprintf(val, sizeof(val), "%d", percent);
  char t[TOPIC_MAX];
  topic_of(t, sizeof(t), "volume/state");
  mqtt_pub(t, val, true);
}

static void publish_eq(void) {
  bool en = false;
  settings_get_tone(&en, NULL, NULL, NULL, NULL);
  char t[TOPIC_MAX];
  topic_of(t, sizeof(t), "eq/state");
  mqtt_pub(t, en ? "ON" : "OFF", true);
}

/* Report the preset whose tone values match the current settings, or "Plat"
   when nothing matches (e.g. a manual EQ set via the web UI). */
static void publish_eq_preset(void) {
  int bass = 0, mid = 0, treble = 0, hpf = 0;
  settings_get_tone(NULL, &bass, &mid, &treble, &hpf);
  const char *name = EQ_PRESETS[0].name;
  for (size_t i = 0; i < EQ_PRESET_COUNT; i++) {
    if (EQ_PRESETS[i].bass == bass && EQ_PRESETS[i].mid == mid &&
        EQ_PRESETS[i].treble == treble && EQ_PRESETS[i].hpf == hpf) {
      name = EQ_PRESETS[i].name;
      break;
    }
  }
  char t[TOPIC_MAX];
  topic_of(t, sizeof(t), "eqpreset/state");
  mqtt_pub(t, name, true);
}

static void publish_matrix(void) {
  bool en = false;
  int fx = 0, br = 4;
  settings_get_matrix(&en, &fx, &br, NULL, NULL, NULL);
  if (fx < 0 || fx >= (int)MATRIX_EFFECT_COUNT) {
    fx = 0;
  }

  cJSON *st = cJSON_CreateObject();
  cJSON_AddStringToObject(st, "state", en ? "ON" : "OFF");
  /* Map matrix brightness 0..15 -> 0..255 for HA. */
  cJSON_AddNumberToObject(st, "brightness", (br * 255) / 15);
  cJSON_AddStringToObject(st, "effect", MATRIX_EFFECTS[fx]);

  char *json = cJSON_PrintUnformatted(st);
  if (json) {
    char t[TOPIC_MAX];
    topic_of(t, sizeof(t), "matrix/state");
    mqtt_pub(t, json, true);
    free(json);
  }
  cJSON_Delete(st);
}

static void publish_all_state(void) {
  publish_nowplaying();
  publish_volume();
  publish_eq();
  publish_eq_preset();
  publish_matrix();
}

/* ------------------------------------------------------------------ */
/*  Command handling                                                   */
/* ------------------------------------------------------------------ */

static int matrix_effect_index(const char *name) {
  for (int i = 0; i < (int)MATRIX_EFFECT_COUNT; i++) {
    if (strcmp(MATRIX_EFFECTS[i], name) == 0) {
      return i;
    }
  }
  return -1;
}

static void handle_volume_set(const char *payload) {
  int percent = atoi(payload);
  if (percent < 0) {
    percent = 0;
  } else if (percent > 100) {
    percent = 100;
  }
  settings_set_max_gain(percent); /* applies live on the next audio frame */
  publish_volume();
}

static void handle_eq_set(const char *payload) {
  bool en = (strcmp(payload, "ON") == 0);
  /* Keep the current bass/mid/treble/hpf; only flip the enable flag. */
  int bass = 0, mid = 0, treble = 0, hpf = 0;
  settings_get_tone(NULL, &bass, &mid, &treble, &hpf);
  settings_set_tone(en, bass, mid, treble, hpf);
  eq_dsp_set(en, bass, mid, treble, hpf);
  publish_eq();
}

static void handle_eq_preset_set(const char *payload) {
  for (size_t i = 0; i < EQ_PRESET_COUNT; i++) {
    if (strcmp(EQ_PRESETS[i].name, payload) == 0) {
      const eq_preset_t *p = &EQ_PRESETS[i];
      settings_set_tone(true, p->bass, p->mid, p->treble, p->hpf);
      eq_dsp_set(true, p->bass, p->mid, p->treble, p->hpf);
      publish_eq();
      publish_eq_preset();
      return;
    }
  }
  ESP_LOGW(TAG, "Unknown EQ preset '%s'", payload);
}

static void handle_matrix_set(const char *payload) {
  cJSON *json = cJSON_Parse(payload);
  if (!json) {
    ESP_LOGW(TAG, "Bad matrix JSON");
    return;
  }

  /* Start from the current config so partial commands keep existing values. */
  bool en = false;
  int fx = 0, br = 4, din = -1, clk = -1, cs = -1;
  settings_get_matrix(&en, &fx, &br, &din, &clk, &cs);

  cJSON *j_state = cJSON_GetObjectItem(json, "state");
  if (j_state && cJSON_IsString(j_state)) {
    en = (strcmp(cJSON_GetStringValue(j_state), "ON") == 0);
  }
  cJSON *j_br = cJSON_GetObjectItem(json, "brightness");
  if (j_br && cJSON_IsNumber(j_br)) {
    int b255 = (int)cJSON_GetNumberValue(j_br);
    if (b255 < 0) {
      b255 = 0;
    } else if (b255 > 255) {
      b255 = 255;
    }
    br = (b255 * 15) / 255; /* 0..255 -> 0..15 */
  }
  cJSON *j_fx = cJSON_GetObjectItem(json, "effect");
  if (j_fx && cJSON_IsString(j_fx)) {
    int idx = matrix_effect_index(cJSON_GetStringValue(j_fx));
    if (idx >= 0) {
      fx = idx;
    }
  }
  cJSON_Delete(json);

  if (settings_set_matrix(en, fx, br, din, clk, cs) == ESP_OK) {
    led_matrix_reconfigure(); /* apply live */
  }
  publish_matrix();
}

/* ------------------------------------------------------------------ */
/*  RTSP now-playing listener                                          */
/* ------------------------------------------------------------------ */

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)user_data;
  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
  case RTSP_EVENT_PLAYING:
    s_np.playing = true;
    break;
  case RTSP_EVENT_PAUSED:
    s_np.playing = false;
    break;
  case RTSP_EVENT_DISCONNECTED:
    s_np.playing = false;
    s_np.title[0] = '\0';
    s_np.artist[0] = '\0';
    break;
  case RTSP_EVENT_METADATA:
    if (data) {
      const rtsp_metadata_t *m = &data->metadata;
      if (m->title[0]) {
        strlcpy(s_np.title, m->title, sizeof(s_np.title));
      }
      if (m->artist[0]) {
        strlcpy(s_np.artist, m->artist, sizeof(s_np.artist));
      }
      s_np.playing = true;
    }
    break;
  }
  /* Reflect to HA (state topic also doubles as a "paused" indicator). */
  if (s_connected) {
    char t[TOPIC_MAX];
    topic_of(t, sizeof(t), "state");
    mqtt_pub(t, s_np.playing ? "playing" : "paused", true);
    topic_of(t, sizeof(t), "title");
    mqtt_pub(t, s_np.title, true);
    topic_of(t, sizeof(t), "artist");
    mqtt_pub(t, s_np.artist, true);
  }
}

/* ------------------------------------------------------------------ */
/*  esp-mqtt event handler                                             */
/* ------------------------------------------------------------------ */

static void subscribe_commands(void) {
  static const char *cmds[] = {"volume/set", "eq/set", "eqpreset/set",
                               "matrix/set", "restart/set"};
  char t[TOPIC_MAX];
  for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
    topic_of(t, sizeof(t), cmds[i]);
    esp_mqtt_client_subscribe(s_client, t, 1);
  }
}

/* Returns true iff topic equals "<base>/<suffix>". */
static bool topic_is(const char *topic, int topic_len, const char *suffix) {
  char want[TOPIC_MAX];
  int n = snprintf(want, sizeof(want), "%s/%s", s_base, suffix);
  return n == topic_len && strncmp(topic, want, (size_t)topic_len) == 0;
}

static void handle_data(const char *topic, int topic_len, const char *payload) {
  if (topic_is(topic, topic_len, "volume/set")) {
    handle_volume_set(payload);
  } else if (topic_is(topic, topic_len, "eq/set")) {
    handle_eq_set(payload);
  } else if (topic_is(topic, topic_len, "eqpreset/set")) {
    handle_eq_preset_set(payload);
  } else if (topic_is(topic, topic_len, "matrix/set")) {
    handle_matrix_set(payload);
  } else if (topic_is(topic, topic_len, "restart/set")) {
    if (strcmp(payload, "PRESS") == 0) {
      ESP_LOGI(TAG, "Restart requested via MQTT");
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
    }
  }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  (void)handler_args;
  (void)base;
  esp_mqtt_event_handle_t event = event_data;

  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "Connected to broker");
    s_connected = true;
    mqtt_pub(s_status_topic, "online", true);
    publish_all_discovery();
    subscribe_commands();
    publish_all_state();
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "Disconnected from broker");
    s_connected = false;
    break;

  case MQTT_EVENT_DATA: {
    /* Copy topic + payload into NUL-terminated stack buffers. esp-mqtt does
       not NUL-terminate, and large payloads may arrive chunked; we only act
       on whole, small payloads (the discovery topics we own are tiny). */
    if (event->topic_len <= 0 || event->topic_len >= TOPIC_MAX ||
        event->data_len < 0 || event->data_len >= PAYLOAD_MAX ||
        event->current_data_offset != 0 ||
        event->data_len != event->total_data_len) {
      break;
    }
    char topic[TOPIC_MAX];
    char payload[PAYLOAD_MAX];
    memcpy(topic, event->topic, (size_t)event->topic_len);
    memcpy(payload, event->data, (size_t)event->data_len);
    payload[event->data_len] = '\0';
    handle_data(topic, event->topic_len, payload);
    break;
  }

  case MQTT_EVENT_ERROR:
    ESP_LOGW(TAG, "MQTT error");
    break;

  default:
    break;
  }
}

/* ------------------------------------------------------------------ */
/*  Periodic refresh                                                   */
/* ------------------------------------------------------------------ */

static void refresh_timer_cb(TimerHandle_t xTimer) {
  (void)xTimer;
  if (s_connected) {
    publish_all_state();
  }
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static void stop_client(void) {
  if (s_client) {
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
  }
  s_connected = false;
  if (s_refresh_timer) {
    xTimerStop(s_refresh_timer, 0);
  }
}

static void start_client(void) {
  bool en = false;
  char host[129] = {0};
  int port = 1883;
  char user[65] = {0};
  char pass[65] = {0};
  settings_get_mqtt(&en, host, sizeof(host), &port, user, sizeof(user), pass,
                    sizeof(pass));

  if (!en || host[0] == '\0') {
    ESP_LOGI(TAG, "MQTT disabled or no host configured — not starting");
    return;
  }

  build_identity();

  esp_mqtt_client_config_t cfg = {0};
  cfg.broker.address.hostname = host;
  cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  cfg.broker.address.port = (uint32_t)port;
  cfg.credentials.client_id = s_id;
  if (user[0]) {
    cfg.credentials.username = user;
  }
  if (pass[0]) {
    cfg.credentials.authentication.password = pass;
  }
  /* LWT: broker marks us offline if we drop without a clean disconnect. */
  cfg.session.last_will.topic = s_status_topic;
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.msg_len = (int)strlen("offline");
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.network.disable_auto_reconnect = false; /* keep retrying the broker */

  s_client = esp_mqtt_client_init(&cfg);
  if (!s_client) {
    ESP_LOGE(TAG, "esp_mqtt_client_init failed");
    return;
  }
  esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                 mqtt_event_handler, NULL);
  esp_err_t err = esp_mqtt_client_start(s_client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    return;
  }

  if (!s_refresh_timer) {
    s_refresh_timer =
        xTimerCreate("ha_mqtt_refresh", pdMS_TO_TICKS(REFRESH_PERIOD_MS),
                     pdTRUE, NULL, refresh_timer_cb);
  }
  if (s_refresh_timer) {
    xTimerStart(s_refresh_timer, 0);
  }

  ESP_LOGI(TAG, "MQTT client started (host=%s port=%d id=%s)", host, port,
           s_id);
}

void ha_mqtt_init(void) {
  static bool s_listener_registered = false;
  if (!s_listener_registered) {
    /* Register the RTSP listener once even if MQTT is off — it just keeps the
       local now-playing snapshot up to date at negligible cost. */
    rtsp_events_register(on_rtsp_event, NULL);
    s_listener_registered = true;
  }
  start_client();
}

void ha_mqtt_reconfigure(void) {
  stop_client();
  start_client();
}

bool ha_mqtt_connected(void) { return s_connected; }
