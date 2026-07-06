#include "web_server.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_random.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_wifi.h"

#include "settings.h"
#include "eq_dsp.h"
#include "audio_limiter.h"
#include "audio_output.h"
#include "amp_ctrl.h"
#include "led_argb.h"
#include "led_matrix.h"
#include "ha_mqtt.h"
#include "wifi.h"
#include "ethernet.h"
#include "ota.h"
#include "esp_ota_ops.h"
#include "log_stream.h"
#include "rtsp_server.h"
#include "rtsp_events.h"
#include "playback_control.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_DAC_TAS58XX
#include "eq_events.h"
#include "dac_tas58xx_eq.h"
#endif

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

#define SPIFFS_CHUNK_SIZE 1024

/* Random identifier minted once per boot. The web UI polls /api/system/info
   and reloads itself when this value changes (i.e. after a reboot/OTA). It is
   filled lazily on first use and stays constant for the life of the boot. */
static char s_boot_id[17]; /* 16 hex chars + NUL */

static const char *web_server_boot_id(void) {
  if (s_boot_id[0] == '\0') {
    uint8_t raw[8];
    esp_fill_random(raw, sizeof(raw));
    static const char *digits = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
      s_boot_id[i * 2] = digits[(raw[i] >> 4) & 0xF];
      s_boot_id[i * 2 + 1] = digits[raw[i] & 0xF];
    }
    s_boot_id[16] = '\0';
  }
  return s_boot_id;
}

static esp_err_t serve_spiffs_file(httpd_req_t *req, const char *path,
                                   const char *content_type) {
  FILE *f = fopen(path, "r");
  if (!f) {
    ESP_LOGE(TAG, "Failed to open %s", path);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, content_type);
  char buf[SPIFFS_CHUNK_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
      fclose(f);
      httpd_resp_send_chunk(req, NULL, 0);
      return ESP_FAIL;
    }
  }
  fclose(f);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

/* ================================================================== */
/*  Now-playing cache                                                  */
/* ================================================================== */

/* Latest AirPlay metadata, kept up to date by an RTSP event listener so the
   open /api/nowplaying endpoint can serve it without touching the RTSP layer.
   Strings are bounded copies of the rtsp_metadata_t fields. */
typedef struct {
  bool playing;
  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
  char album[METADATA_STRING_MAX];
  uint32_t position_secs;
  uint32_t duration_secs;
} nowplaying_t;

static nowplaying_t s_nowplaying;

static void nowplaying_clear_metadata(void) {
  s_nowplaying.title[0] = '\0';
  s_nowplaying.artist[0] = '\0';
  s_nowplaying.album[0] = '\0';
  s_nowplaying.position_secs = 0;
  s_nowplaying.duration_secs = 0;
}

static void on_rtsp_event_nowplaying(rtsp_event_t event,
                                     const rtsp_event_data_t *data,
                                     void *user_data) {
  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
  case RTSP_EVENT_PLAYING:
    s_nowplaying.playing = true;
    break;
  case RTSP_EVENT_PAUSED:
    s_nowplaying.playing = false;
    break;
  case RTSP_EVENT_DISCONNECTED:
    s_nowplaying.playing = false;
    nowplaying_clear_metadata();
    break;
  case RTSP_EVENT_METADATA:
    if (data) {
      const rtsp_metadata_t *m = &data->metadata;
      /* Metadata arrives in separate SET_PARAMETER messages (a title one, a
         progress one, ...). Only overwrite a field when this event actually
         carries it, so a progress-only update doesn't wipe the title. */
      if (m->title[0]) {
        strlcpy(s_nowplaying.title, m->title, sizeof(s_nowplaying.title));
      }
      if (m->artist[0]) {
        strlcpy(s_nowplaying.artist, m->artist, sizeof(s_nowplaying.artist));
      }
      if (m->album[0]) {
        strlcpy(s_nowplaying.album, m->album, sizeof(s_nowplaying.album));
      }
      if (m->duration_secs > 0) {
        s_nowplaying.position_secs = m->position_secs;
        s_nowplaying.duration_secs = m->duration_secs;
      }
      /* Metadata flowing implies an active stream. */
      s_nowplaying.playing = true;
    }
    break;
  }
}

/* ================================================================== */
/*  Session token store + authentication                               */
/* ================================================================== */

#define AUTH_MAX_TOKENS  4
#define AUTH_TOKEN_HEX   32 /* 16 random bytes -> 32 hex chars */
#define AUTH_TOKEN_BYTES 16
#define AUTH_TTL_US      (24LL * 60 * 60 * 1000000LL) /* 24h in microseconds */

typedef struct {
  char token[AUTH_TOKEN_HEX + 1]; /* lowercase hex, NUL-terminated */
  int64_t expires_us;             /* esp_timer time when this slot dies */
  bool used;
} auth_slot_t;

static auth_slot_t s_tokens[AUTH_MAX_TOKENS];

/* Mint a fresh token, store it (evicting the oldest slot when full), and
   copy it into out (must hold at least AUTH_TOKEN_HEX + 1 bytes). */
static void auth_issue_token(char out[AUTH_TOKEN_HEX + 1]) {
  uint8_t raw[AUTH_TOKEN_BYTES];
  esp_fill_random(raw, sizeof(raw));

  char hex[AUTH_TOKEN_HEX + 1];
  static const char *digits = "0123456789abcdef";
  for (int i = 0; i < AUTH_TOKEN_BYTES; i++) {
    hex[i * 2] = digits[(raw[i] >> 4) & 0xF];
    hex[i * 2 + 1] = digits[raw[i] & 0xF];
  }
  hex[AUTH_TOKEN_HEX] = '\0';

  int64_t now = esp_timer_get_time();

  /* Pick a free/expired slot, else evict the oldest (soonest to expire). */
  int slot = -1;
  for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
    if (!s_tokens[i].used || s_tokens[i].expires_us <= now) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    slot = 0;
    for (int i = 1; i < AUTH_MAX_TOKENS; i++) {
      if (s_tokens[i].expires_us < s_tokens[slot].expires_us) {
        slot = i;
      }
    }
  }

  memcpy(s_tokens[slot].token, hex, sizeof(hex));
  s_tokens[slot].expires_us = now + AUTH_TTL_US;
  s_tokens[slot].used = true;

  memcpy(out, hex, sizeof(hex));
}

// Constant-time equality: compare all n bytes regardless of where they differ,
// so an attacker can't time-probe a session token byte by byte.
static bool ct_equal(const char *a, const char *b, size_t n) {
  const volatile unsigned char *pa = (const volatile unsigned char *)a;
  const volatile unsigned char *pb = (const volatile unsigned char *)b;
  unsigned char diff = 0;
  for (size_t i = 0; i < n; i++) {
    diff |= (unsigned char)(pa[i] ^ pb[i]);
  }
  return diff == 0;
}

bool web_server_auth_token_valid(const char *tok) {
  if (!tok || strlen(tok) != AUTH_TOKEN_HEX) {
    return false;
  }
  int64_t now = esp_timer_get_time();
  for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
    if (s_tokens[i].used && s_tokens[i].expires_us > now &&
        ct_equal(s_tokens[i].token, tok, AUTH_TOKEN_HEX)) {
      return true;
    }
  }
  return false;
}

bool web_server_auth_required(void) {
  return settings_has_device_password();
}

/* Gate a protected handler. In setup mode (no password) this is a no-op and
   returns ESP_OK. Otherwise it validates the X-Auth-Token header; on failure
   it sends a 401 and returns ESP_FAIL so the caller aborts the action. */
static esp_err_t check_auth(httpd_req_t *req) {
  if (!settings_has_device_password()) {
    return ESP_OK;
  }

  char token[AUTH_TOKEN_HEX + 1] = {0};
  esp_err_t err =
      httpd_req_get_hdr_value_str(req, "X-Auth-Token", token, sizeof(token));
  if (err == ESP_OK && web_server_auth_token_valid(token)) {
    return ESP_OK;
  }

  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"error\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
  return ESP_FAIL;
}

// API handlers
static esp_err_t root_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/index.html", "text/html");
}

/* ---- Auth endpoints (always open) ---- */

static esp_err_t auth_status_handler(httpd_req_t *req) {
  bool has_pw = settings_has_device_password();
  bool authed = true;
  if (has_pw) {
    char token[AUTH_TOKEN_HEX + 1] = {0};
    authed = (httpd_req_get_hdr_value_str(req, "X-Auth-Token", token,
                                          sizeof(token)) == ESP_OK) &&
             web_server_auth_token_valid(token);
  }

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "hasPassword", has_pw);
  cJSON_AddBoolToObject(json, "authed", authed);

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t auth_setup_handler(httpd_req_t *req) {
  /* Only valid when NO password is set yet. */
  if (settings_has_device_password()) {
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "already_set");
    char *json_str = cJSON_Print(response);
    if (!json_str) {
      cJSON_Delete(response);
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
  }

  char content[256];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *pw_json = cJSON_GetObjectItem(json, "password");
  const char *pw =
      (pw_json && cJSON_IsString(pw_json)) ? cJSON_GetStringValue(pw_json) : "";

  if (strlen(pw) < 4) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password too short");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  esp_err_t err = settings_set_device_password(pw);
  if (err == ESP_OK) {
    char token[AUTH_TOKEN_HEX + 1];
    auth_issue_token(token);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "token", token);
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
  }

  char *json_str = cJSON_Print(response);
  if (!json_str) {
    cJSON_Delete(json);
    cJSON_Delete(response);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

// Brute-force throttle for the login endpoint: after too many consecutive bad
// passwords, refuse further attempts for a lockout window. Combined with a
// fixed per-failure delay this turns the tiny 4-char keyspace from
// seconds-to-exhaust into something impractical to grind online. State is
// global (single admin).
#define LOGIN_MAX_FAILURES 5
#define LOGIN_LOCKOUT_US   (30 * 1000000LL) // 30 s
static int s_login_failures = 0;
static int64_t s_login_lockout_until_us = 0;

static esp_err_t auth_login_handler(httpd_req_t *req) {
  /* No password configured -> 400 no_password. */
  if (!settings_has_device_password()) {
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "no_password");
    char *json_str = cJSON_Print(response);
    if (!json_str) {
      cJSON_Delete(response);
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
  }

  /* Locked out after too many failures: refuse without even checking. */
  if (esp_timer_get_time() < s_login_lockout_until_us) {
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"locked_out\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char content[256];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *pw_json = cJSON_GetObjectItem(json, "password");
  const char *pw =
      (pw_json && cJSON_IsString(pw_json)) ? cJSON_GetStringValue(pw_json) : "";

  cJSON *response = cJSON_CreateObject();
  if (settings_check_device_password(pw)) {
    s_login_failures = 0; // reset throttle on success
    s_login_lockout_until_us = 0;
    char token[AUTH_TOKEN_HEX + 1];
    auth_issue_token(token);
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "token", token);
    char *json_str = cJSON_Print(response);
    if (!json_str) {
      cJSON_Delete(json);
      cJSON_Delete(response);
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
  } else {
    // Fixed delay per bad guess, plus a lockout once failures pile up.
    vTaskDelay(pdMS_TO_TICKS(300));
    if (++s_login_failures >= LOGIN_MAX_FAILURES) {
      s_login_lockout_until_us = esp_timer_get_time() + LOGIN_LOCKOUT_US;
      s_login_failures = 0;
      ESP_LOGW(TAG, "login locked out after repeated failures");
    }
    cJSON_AddBoolToObject(response, "success", false);
    char *json_str = cJSON_Print(response);
    if (!json_str) {
      cJSON_Delete(json);
      cJSON_Delete(response);
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
  }

  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t logs_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/logs.html", "text/html");
}

static esp_err_t speedtest_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/speedtest.html", "text/html");
}

// Tiny endpoint used by JS for RTT timing. Returns minimal body.
static esp_err_t speedtest_ping_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, "ok", 2);
  return ESP_OK;
}

// Streams `bytes` octets of filler data so the browser can measure DL speed.
// Capped to avoid pathological requests starving audio.
#define SPEEDTEST_MAX_BYTES (16 * 1024 * 1024)
#define SPEEDTEST_CHUNK     2048

static esp_err_t speedtest_download_handler(httpd_req_t *req) {
  // Gate this behind auth so an unauthenticated caller can't repeatedly pull
  // 16 MB and saturate the link / CPU (DoS). No-op in setup mode (no password).
  if (check_auth(req) != ESP_OK) {
    return ESP_FAIL;
  }
  size_t bytes = 1024 * 1024;
  char qbuf[64];
  if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
    char val[16];
    if (httpd_query_key_value(qbuf, "bytes", val, sizeof(val)) == ESP_OK) {
      long v = strtol(val, NULL, 10);
      if (v > 0)
        bytes = (size_t)v;
    }
  }
  if (bytes > SPEEDTEST_MAX_BYTES)
    bytes = SPEEDTEST_MAX_BYTES;

  // Reuse a single buffer of filler bytes. Static so we don't repeatedly
  // hammer the heap; content is irrelevant but non-zero to thwart any
  // compression along the way.
  static uint8_t filler[SPEEDTEST_CHUNK];
  static bool filler_init = false;
  if (!filler_init) {
    for (size_t i = 0; i < sizeof(filler); i++)
      filler[i] = (uint8_t)(i * 37);
    filler_init = true;
  }

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  size_t remaining = bytes;
  while (remaining > 0) {
    size_t n = remaining < SPEEDTEST_CHUNK ? remaining : SPEEDTEST_CHUNK;
    if (httpd_resp_send_chunk(req, (const char *)filler, n) != ESP_OK) {
      return ESP_FAIL;
    }
    remaining -= n;
  }
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// Consumes a POST body and reports how many bytes were received.
static esp_err_t speedtest_upload_handler(httpd_req_t *req) {
  // Same DoS gate as the download side (no-op when no password is set).
  if (check_auth(req) != ESP_OK) {
    return ESP_FAIL;
  }
  size_t total = req->content_len;
  // Cap the accepted body so a huge upload can't starve audio (unbounded DoS).
  if (total > SPEEDTEST_MAX_BYTES) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  size_t got = 0;
  uint8_t buf[SPEEDTEST_CHUNK];
  while (got < total) {
    size_t want = total - got;
    if (want > sizeof(buf))
      want = sizeof(buf);
    int r = httpd_req_recv(req, (char *)buf, want);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT)
        continue;
      return ESP_FAIL;
    }
    got += (size_t)r;
  }
  char reply[64];
  int n = snprintf(reply, sizeof(reply), "received=%u", (unsigned)got);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, reply, n);
  return ESP_OK;
}

// Captive portal detection handlers
// These endpoints are requested by various OS to detect captive portals
static esp_err_t captive_portal_redirect(httpd_req_t *req) {
  // Redirect to the configuration page
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

// Apple devices (iOS/macOS) check these
static esp_err_t captive_apple_handler(httpd_req_t *req) {
  // Apple expects specific response, redirect instead
  return captive_portal_redirect(req);
}

// Android checks this
static esp_err_t captive_android_handler(httpd_req_t *req) {
  // Android expects 204 for no captive portal, anything else triggers portal
  return captive_portal_redirect(req);
}

// Windows checks this
static esp_err_t captive_windows_handler(httpd_req_t *req) {
  return captive_portal_redirect(req);
}

/* Asynchronous WiFi scan.
   A blocking scan still briefly hops the radio off the home channel, so we
   run it in a background task and cache the results; the handler returns
   immediately with the current cache and a "scanning" flag (so the HTTP
   response is never lost to the scan). We use wifi_scan_keep_connected()
   which does NOT disconnect the STA, so the link survives the scan. The web
   UI polls until scanning == false. */
#define SCAN_CACHE_MAX   24
#define SCAN_DEBOUNCE_MS 8000 // don't restart a scan more often than this
static wifi_ap_record_t s_scan_cache[SCAN_CACHE_MAX];
static volatile uint16_t s_scan_cache_n = 0;
static volatile bool s_scanning = false;
static int64_t s_last_scan_ms = 0;

static void wifi_scan_task(void *arg) {
  wifi_ap_record_t *ap_list = NULL;
  uint16_t ap_count = 0;
  esp_err_t err = wifi_scan_keep_connected(&ap_list, &ap_count);
  if (err == ESP_OK) {
    if (ap_count > SCAN_CACHE_MAX) {
      ap_count = SCAN_CACHE_MAX;
    }
    if (ap_list) {
      memcpy(s_scan_cache, ap_list, ap_count * sizeof(wifi_ap_record_t));
      free(ap_list);
    }
    s_scan_cache_n = ap_count;
  }
  // On error, keep the previous cache so the UI still has something to show.
  s_scanning = false;
  vTaskDelete(NULL);
}

static esp_err_t wifi_scan_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;
  // Launch a fresh scan only if one isn't already running AND the last one is
  // stale. Without this debounce the UI's polling would re-trigger scans
  // back-to-back, keeping the radio permanently busy.
  int64_t now_ms = esp_timer_get_time() / 1000;
  bool start_scan = !s_scanning && (now_ms - s_last_scan_ms) > SCAN_DEBOUNCE_MS;
  if (start_scan) {
    s_scanning = true;
    s_last_scan_ms = now_ms;
  }

  // Build and SEND the response (from the cache) before the scan starts, so
  // the brief radio hop can't kill this HTTP response.
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddBoolToObject(json, "scanning", s_scanning);

  cJSON *networks = cJSON_CreateArray();
  uint16_t n = s_scan_cache_n;
  if (n > SCAN_CACHE_MAX) {
    n = SCAN_CACHE_MAX;
  }
  for (uint16_t i = 0; i < n; i++) {
    cJSON *net = cJSON_CreateObject();
    // ssid is uint8_t[33] and may NOT be NUL-terminated for a 32-byte SSID;
    // copy into a local buffer and force termination before handing to cJSON.
    char ssid_buf[33];
    memcpy(ssid_buf, s_scan_cache[i].ssid, sizeof(ssid_buf) - 1);
    ssid_buf[sizeof(ssid_buf) - 1] = '\0';
    cJSON_AddStringToObject(net, "ssid", ssid_buf);
    cJSON_AddNumberToObject(net, "rssi", s_scan_cache[i].rssi);
    cJSON_AddNumberToObject(net, "channel", s_scan_cache[i].primary);
    cJSON_AddItemToArray(networks, net);
  }
  cJSON_AddItemToObject(json, "networks", networks);

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);

  // Now launch the scan (the response is already on the wire).
  if (start_scan) {
    if (xTaskCreate(wifi_scan_task, "wifi_scan_web", 4096, NULL, 3, NULL) !=
        pdPASS) {
      s_scanning = false;
    }
  }
  return ESP_OK;
}

static esp_err_t wifi_config_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;
  char content[512];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *ssid_json = cJSON_GetObjectItem(json, "ssid");
  cJSON *password_json = cJSON_GetObjectItem(json, "password");

  cJSON *response = cJSON_CreateObject();
  bool restart_after = false;
  if (ssid_json && cJSON_IsString(ssid_json)) {
    const char *ssid = cJSON_GetStringValue(ssid_json);
    const char *password = password_json && cJSON_IsString(password_json)
                               ? cJSON_GetStringValue(password_json)
                               : "";

    esp_err_t err = settings_set_wifi_credentials(ssid, password);
    if (err == ESP_OK) {
      cJSON_AddBoolToObject(response, "success", true);
      ESP_LOGI(TAG, "WiFi credentials saved. We are restarting...");
      // Reply to the client first, then restart below (see ota handler).
      restart_after = true;
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid SSID");
  }

  char *json_str = cJSON_Print(response);
  if (!json_str) {
    cJSON_Delete(json);
    cJSON_Delete(response);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);

  // Send the response before restarting so the client actually gets a reply.
  if (restart_after) {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
  }

  return ESP_OK;
}

static esp_err_t device_name_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;
  char content[256];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *name_json = cJSON_GetObjectItem(json, "name");
  cJSON *response = cJSON_CreateObject();

  if (name_json && cJSON_IsString(name_json)) {
    const char *name = cJSON_GetStringValue(name_json);
    esp_err_t err = settings_set_device_name(name);
    if (err == ESP_OK) {
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid name");
  }

  char *json_str = cJSON_Print(response);
  if (!json_str) {
    cJSON_Delete(json);
    cJSON_Delete(response);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);

  return ESP_OK;
}

static esp_err_t audio_gain_get_handler(httpd_req_t *req) {
  int percent = 100;
  settings_get_max_gain(&percent);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "gain", percent);

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t audio_gain_post_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;
  char content[128];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *gain_json = cJSON_GetObjectItem(json, "gain");
  cJSON *response = cJSON_CreateObject();

  if (gain_json && cJSON_IsNumber(gain_json)) {
    int percent = (int)cJSON_GetNumberValue(gain_json);
    if (percent < 0) {
      percent = 0;
    } else if (percent > 100) {
      percent = 100;
    }
    esp_err_t err = settings_set_max_gain(percent);
    if (err == ESP_OK) {
      cJSON_AddBoolToObject(response, "success", true);
      cJSON_AddNumberToObject(response, "gain", percent);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid gain");
  }

  char *json_str = cJSON_Print(response);
  if (!json_str) {
    cJSON_Delete(json);
    cJSON_Delete(response);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* ---- Optional 8x8 LED matrix (MAX7219) ---- */

static esp_err_t matrix_get_handler(httpd_req_t *req) {
  bool en = false;
  int fx = 0, br = 4, din = -1, clk = -1, cs = -1;
  settings_get_matrix(&en, &fx, &br, &din, &clk, &cs);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "enabled", en);
  cJSON_AddNumberToObject(json, "effect", fx);
  cJSON_AddNumberToObject(json, "brightness", br);
  cJSON_AddNumberToObject(json, "din", din);
  cJSON_AddNumberToObject(json, "clk", clk);
  cJSON_AddNumberToObject(json, "cs", cs);

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t matrix_post_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;

  char content[256];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  // Start from the current config so partial bodies keep existing values.
  bool en = false;
  int fx = 0, br = 4, din = -1, clk = -1, cs = -1;
  settings_get_matrix(&en, &fx, &br, &din, &clk, &cs);

  cJSON *j_en = cJSON_GetObjectItem(json, "enabled");
  cJSON *j_fx = cJSON_GetObjectItem(json, "effect");
  cJSON *j_br = cJSON_GetObjectItem(json, "brightness");
  cJSON *j_din = cJSON_GetObjectItem(json, "din");
  cJSON *j_clk = cJSON_GetObjectItem(json, "clk");
  cJSON *j_cs = cJSON_GetObjectItem(json, "cs");

  if (j_en && cJSON_IsBool(j_en)) {
    en = cJSON_IsTrue(j_en);
  }
  if (j_fx && cJSON_IsNumber(j_fx)) {
    fx = (int)cJSON_GetNumberValue(j_fx);
  }
  if (j_br && cJSON_IsNumber(j_br)) {
    br = (int)cJSON_GetNumberValue(j_br);
  }
  if (j_din && cJSON_IsNumber(j_din)) {
    din = (int)cJSON_GetNumberValue(j_din);
  }
  if (j_clk && cJSON_IsNumber(j_clk)) {
    clk = (int)cJSON_GetNumberValue(j_clk);
  }
  if (j_cs && cJSON_IsNumber(j_cs)) {
    cs = (int)cJSON_GetNumberValue(j_cs);
  }

  cJSON *response = cJSON_CreateObject();

  // Validate ranges (effect 0-3, brightness 0-15, pins -1..48).
  if (fx < 0 || fx > 3 || br < 0 || br > 15 || din < -1 || din > 48 ||
      clk < -1 || clk > 48 || cs < -1 || cs > 48) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid range");
  } else {
    esp_err_t err = settings_set_matrix(en, fx, br, din, clk, cs);
    if (err == ESP_OK) {
      led_matrix_reconfigure(); // apply live
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  }

  char *json_str = cJSON_Print(response);
  if (!json_str) {
    cJSON_Delete(json);
    cJSON_Delete(response);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* ---- Optional addressable RGB strip (WS2812) ---- */

static esp_err_t argb_get_handler(httpd_req_t *req) {
  bool en = false;
  int gpio = -1, count = 30, fx = 0, br = 128, speed = 5;
  uint32_t color = 0x2080FF;
  settings_get_argb(&en, &gpio, &count, &fx, &br, &color, &speed);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "enabled", en);
  cJSON_AddNumberToObject(json, "gpio", gpio);
  cJSON_AddNumberToObject(json, "count", count);
  cJSON_AddNumberToObject(json, "effect", fx);
  cJSON_AddNumberToObject(json, "brightness", br);
  cJSON_AddNumberToObject(json, "color", (double)color);
  cJSON_AddNumberToObject(json, "speed", speed);

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t argb_post_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;

  char content[256];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  // Start from the current config so partial bodies keep existing values.
  bool en = false;
  int gpio = -1, count = 30, fx = 0, br = 128, speed = 5;
  uint32_t color = 0x2080FF;
  settings_get_argb(&en, &gpio, &count, &fx, &br, &color, &speed);

  cJSON *j_en = cJSON_GetObjectItem(json, "enabled");
  cJSON *j_gpio = cJSON_GetObjectItem(json, "gpio");
  cJSON *j_count = cJSON_GetObjectItem(json, "count");
  cJSON *j_fx = cJSON_GetObjectItem(json, "effect");
  cJSON *j_br = cJSON_GetObjectItem(json, "brightness");
  cJSON *j_color = cJSON_GetObjectItem(json, "color");
  cJSON *j_speed = cJSON_GetObjectItem(json, "speed");

  if (j_en && cJSON_IsBool(j_en)) {
    en = cJSON_IsTrue(j_en);
  }
  if (j_gpio && cJSON_IsNumber(j_gpio)) {
    gpio = (int)cJSON_GetNumberValue(j_gpio);
  }
  if (j_count && cJSON_IsNumber(j_count)) {
    count = (int)cJSON_GetNumberValue(j_count);
  }
  if (j_fx && cJSON_IsNumber(j_fx)) {
    fx = (int)cJSON_GetNumberValue(j_fx);
  }
  if (j_br && cJSON_IsNumber(j_br)) {
    br = (int)cJSON_GetNumberValue(j_br);
  }
  if (j_color && cJSON_IsNumber(j_color)) {
    color = (uint32_t)cJSON_GetNumberValue(j_color) & 0xFFFFFFu;
  }
  if (j_speed && cJSON_IsNumber(j_speed)) {
    speed = (int)cJSON_GetNumberValue(j_speed);
  }

  cJSON *response = cJSON_CreateObject();

  // Validate ranges (effect 0-11, brightness 0-255, gpio -1..48, count 1-300,
  // speed 1-10).
  if (fx < 0 || fx > 11 || br < 0 || br > 255 || gpio < -1 || gpio > 48 ||
      count < 1 || count > 300 || speed < 1 || speed > 10) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid range");
  } else {
    esp_err_t err = settings_set_argb(en, gpio, count, fx, br, color, speed);
    if (err == ESP_OK) {
      led_argb_reconfigure(); // apply live
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  }

  char *json_str = cJSON_Print(response);
  if (!json_str) {
    cJSON_Delete(json);
    cJSON_Delete(response);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* ---- Software 3-band tone EQ (GET open, POST protected) ---- */

static esp_err_t tone_get_handler(httpd_req_t *req) {
  bool en = false;
  int bass = 0, mid = 0, treble = 0, hpf = 0;
  settings_get_tone(&en, &bass, &mid, &treble, &hpf);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "enabled", en);
  cJSON_AddNumberToObject(json, "bass", bass);
  cJSON_AddNumberToObject(json, "mid", mid);
  cJSON_AddNumberToObject(json, "treble", treble);
  cJSON_AddNumberToObject(json, "hpf", hpf);

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t tone_post_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;

  char content[256];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  // Start from the current config so partial bodies keep existing values.
  bool en = false;
  int bass = 0, mid = 0, treble = 0, hpf = 0;
  settings_get_tone(&en, &bass, &mid, &treble, &hpf);

  cJSON *j_en = cJSON_GetObjectItem(json, "enabled");
  cJSON *j_bass = cJSON_GetObjectItem(json, "bass");
  cJSON *j_mid = cJSON_GetObjectItem(json, "mid");
  cJSON *j_treble = cJSON_GetObjectItem(json, "treble");
  cJSON *j_hpf = cJSON_GetObjectItem(json, "hpf");

  if (j_en && cJSON_IsBool(j_en)) {
    en = cJSON_IsTrue(j_en);
  }
  if (j_bass && cJSON_IsNumber(j_bass)) {
    bass = (int)cJSON_GetNumberValue(j_bass);
  }
  if (j_mid && cJSON_IsNumber(j_mid)) {
    mid = (int)cJSON_GetNumberValue(j_mid);
  }
  if (j_treble && cJSON_IsNumber(j_treble)) {
    treble = (int)cJSON_GetNumberValue(j_treble);
  }
  if (j_hpf && cJSON_IsNumber(j_hpf)) {
    hpf = (int)cJSON_GetNumberValue(j_hpf);
  }

  cJSON *response = cJSON_CreateObject();

  // Validate gains (-12..+12 dB) and high-pass cutoff (0 or 40..400 Hz).
  if (bass < -12 || bass > 12 || mid < -12 || mid > 12 || treble < -12 ||
      treble > 12 || (hpf != 0 && (hpf < 40 || hpf > 400))) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid range");
  } else {
    esp_err_t err = settings_set_tone(en, bass, mid, treble, hpf);
    if (err == ESP_OK) {
      eq_dsp_set(en, bass, mid, treble, hpf); // apply live
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  }

  char *json_str = cJSON_Print(response);
  if (!json_str) {
    cJSON_Delete(json);
    cJSON_Delete(response);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* ---- Speaker protection: software limiter + amp mute/standby ----
   (GET open, POST protected) */

static esp_err_t protection_get_handler(httpd_req_t *req) {
  bool lim_en = true, amp_high = true;
  int lim_ceil = -1, amp_gpio = -1, amp_standby = 5;
  settings_get_protection(&lim_en, &lim_ceil, &amp_gpio, &amp_high,
                          &amp_standby);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "limEnabled", lim_en);
  cJSON_AddNumberToObject(json, "limCeiling", lim_ceil);
  cJSON_AddNumberToObject(json, "ampGpio", amp_gpio);
  cJSON_AddBoolToObject(json, "ampActiveHigh", amp_high);
  cJSON_AddNumberToObject(json, "ampStandbyMin", amp_standby);
  cJSON_AddNumberToObject(json, "channel", settings_get_channel_mode());

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t protection_post_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;

  char content[256];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  // Start from the current config so partial bodies keep existing values.
  bool lim_en = true, amp_high = true;
  int lim_ceil = -1, amp_gpio = -1, amp_standby = 5;
  settings_get_protection(&lim_en, &lim_ceil, &amp_gpio, &amp_high,
                          &amp_standby);
  // Channel mode has its own getter/setter (independent of protection).
  int channel = settings_get_channel_mode();

  cJSON *j_lim_en = cJSON_GetObjectItem(json, "limEnabled");
  cJSON *j_lim_ceil = cJSON_GetObjectItem(json, "limCeiling");
  cJSON *j_amp_gpio = cJSON_GetObjectItem(json, "ampGpio");
  cJSON *j_amp_high = cJSON_GetObjectItem(json, "ampActiveHigh");
  cJSON *j_amp_standby = cJSON_GetObjectItem(json, "ampStandbyMin");
  cJSON *j_channel = cJSON_GetObjectItem(json, "channel");

  if (j_lim_en && cJSON_IsBool(j_lim_en)) {
    lim_en = cJSON_IsTrue(j_lim_en);
  }
  if (j_lim_ceil && cJSON_IsNumber(j_lim_ceil)) {
    lim_ceil = (int)cJSON_GetNumberValue(j_lim_ceil);
  }
  if (j_amp_gpio && cJSON_IsNumber(j_amp_gpio)) {
    amp_gpio = (int)cJSON_GetNumberValue(j_amp_gpio);
  }
  if (j_amp_high && cJSON_IsBool(j_amp_high)) {
    amp_high = cJSON_IsTrue(j_amp_high);
  }
  if (j_amp_standby && cJSON_IsNumber(j_amp_standby)) {
    amp_standby = (int)cJSON_GetNumberValue(j_amp_standby);
  }
  if (j_channel && cJSON_IsNumber(j_channel)) {
    channel = (int)cJSON_GetNumberValue(j_channel);
  }

  cJSON *response = cJSON_CreateObject();

  // Validate ranges: ceiling -12..0, amp GPIO -1..48, standby 0..120,
  // channel 0..3.
  if (lim_ceil < -12 || lim_ceil > 0 || amp_gpio < -1 || amp_gpio > 48 ||
      amp_standby < 0 || amp_standby > 120 || channel < 0 || channel > 3) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid range");
  } else {
    esp_err_t err = settings_set_protection(lim_en, lim_ceil, amp_gpio,
                                            amp_high, amp_standby);
    // Persist the channel mode independently and apply it live.
    if (err == ESP_OK) {
      err = settings_set_channel_mode(channel);
    }
    if (err == ESP_OK) {
      // Apply live: the limiter re-reads enable/ceiling, the amp controller
      // re-reads its GPIO/polarity/standby and re-applies.
      audio_limiter_set(lim_en, lim_ceil);
      amp_ctrl_reconfigure();
      audio_output_set_channel_mode(channel);
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  }

  char *json_str = cJSON_Print(response);
  if (!json_str) {
    cJSON_Delete(json);
    cJSON_Delete(response);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* ---- Runtime-configurable physical buttons ---- */

static esp_err_t buttons_get_handler(httpd_req_t *req) {
  int pp = -1, vu = -1, vd = -1, nx = -1, pv = -1;
  settings_get_buttons(&pp, &vu, &vd, &nx, &pv);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "playpause", pp);
  cJSON_AddNumberToObject(json, "volup", vu);
  cJSON_AddNumberToObject(json, "voldown", vd);
  cJSON_AddNumberToObject(json, "next", nx);
  cJSON_AddNumberToObject(json, "prev", pv);

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t buttons_post_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;

  char content[256];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  // Start from the current config so partial bodies keep existing values.
  int pp = -1, vu = -1, vd = -1, nx = -1, pv = -1;
  settings_get_buttons(&pp, &vu, &vd, &nx, &pv);

  cJSON *j_pp = cJSON_GetObjectItem(json, "playpause");
  cJSON *j_vu = cJSON_GetObjectItem(json, "volup");
  cJSON *j_vd = cJSON_GetObjectItem(json, "voldown");
  cJSON *j_nx = cJSON_GetObjectItem(json, "next");
  cJSON *j_pv = cJSON_GetObjectItem(json, "prev");

  if (j_pp && cJSON_IsNumber(j_pp)) {
    pp = (int)cJSON_GetNumberValue(j_pp);
  }
  if (j_vu && cJSON_IsNumber(j_vu)) {
    vu = (int)cJSON_GetNumberValue(j_vu);
  }
  if (j_vd && cJSON_IsNumber(j_vd)) {
    vd = (int)cJSON_GetNumberValue(j_vd);
  }
  if (j_nx && cJSON_IsNumber(j_nx)) {
    nx = (int)cJSON_GetNumberValue(j_nx);
  }
  if (j_pv && cJSON_IsNumber(j_pv)) {
    pv = (int)cJSON_GetNumberValue(j_pv);
  }

  cJSON *response = cJSON_CreateObject();

  // Validate ranges (each GPIO -1..48, -1 = disabled).
  if (pp < -1 || pp > 48 || vu < -1 || vu > 48 || vd < -1 || vd > 48 ||
      nx < -1 || nx > 48 || pv < -1 || pv > 48) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid range");
  } else {
    esp_err_t err = settings_set_buttons(pp, vu, vd, nx, pv);
    if (err == ESP_OK) {
      // Button GPIO/timer setup happens once at boot, so the new mapping
      // only takes effect after a reboot.
      cJSON_AddBoolToObject(response, "success", true);
      cJSON_AddBoolToObject(response, "reboot", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  }

  char *json_str = cJSON_Print(response);
  if (!json_str) {
    cJSON_Delete(json);
    cJSON_Delete(response);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

/* ---- Home Assistant MQTT integration (GET open, POST protected) ---- */

static esp_err_t mqtt_get_handler(httpd_req_t *req) {
  bool en = false;
  char host[129] = {0};
  int port = 1883;
  char user[65] = {0};
  settings_get_mqtt(&en, host, sizeof(host), &port, user, sizeof(user), NULL,
                    0);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "enabled", en);
  cJSON_AddStringToObject(json, "host", host);
  cJSON_AddNumberToObject(json, "port", port);
  cJSON_AddStringToObject(json, "user", user);
  /* NEVER return the password. Expose a live connection indicator instead. */
  cJSON_AddBoolToObject(json, "connected", ha_mqtt_connected());

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t mqtt_post_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;

  char content[512];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  // Start from the current config so partial bodies keep existing values.
  bool en = false;
  char host[129] = {0};
  int port = 1883;
  char user[65] = {0};
  settings_get_mqtt(&en, host, sizeof(host), &port, user, sizeof(user), NULL,
                    0);

  cJSON *j_en = cJSON_GetObjectItem(json, "enabled");
  cJSON *j_host = cJSON_GetObjectItem(json, "host");
  cJSON *j_port = cJSON_GetObjectItem(json, "port");
  cJSON *j_user = cJSON_GetObjectItem(json, "user");
  cJSON *j_pass = cJSON_GetObjectItem(json, "pass");

  if (j_en && cJSON_IsBool(j_en)) {
    en = cJSON_IsTrue(j_en);
  }
  if (j_host && cJSON_IsString(j_host)) {
    strlcpy(host, cJSON_GetStringValue(j_host), sizeof(host));
  }
  if (j_port && cJSON_IsNumber(j_port)) {
    port = (int)cJSON_GetNumberValue(j_port);
  }
  if (j_user && cJSON_IsString(j_user)) {
    strlcpy(user, cJSON_GetStringValue(j_user), sizeof(user));
  }
  /* A missing/empty password keeps the stored one (NULL -> settings_set_mqtt
     leaves the NVS key untouched). */
  const char *pass = NULL;
  if (j_pass && cJSON_IsString(j_pass) &&
      strlen(cJSON_GetStringValue(j_pass)) > 0) {
    pass = cJSON_GetStringValue(j_pass);
  }

  cJSON *response = cJSON_CreateObject();
  esp_err_t err = settings_set_mqtt(en, host, port, user, pass);
  if (err == ESP_OK) {
    ha_mqtt_reconfigure(); // (re)start/stop the client live
    cJSON_AddBoolToObject(response, "success", true);
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
  }

  char *json_str = cJSON_Print(response);
  if (!json_str) {
    cJSON_Delete(json);
    cJSON_Delete(response);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

static esp_err_t ota_update_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;
  if (req->content_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware uploaded");
    return ESP_FAIL;
  }

  // Reject uploads larger than the target OTA partition before we tear down
  // AirPlay or open the flash writer — an oversized image can never fit.
  const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
  if (!p || req->content_len > p->size) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware too large");
    return ESP_FAIL;
  }

  // Stop AirPlay to free resources during OTA
  ESP_LOGI(TAG, "Stopping AirPlay for OTA update");
  rtsp_server_stop();

  esp_err_t err = ota_start_from_http(req);

  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        esp_err_to_name(err));
    return ESP_FAIL;
  }

  // Send response before restarting
  httpd_resp_sendstr(req, "Firmware update complete, rebooting now!\n");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();

  return ESP_OK;
}

static esp_err_t system_info_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON *info = cJSON_CreateObject();

  char ip_str[16] = {0};
  char mac_str[18] = {0};
  char device_name[65] = {0};
  bool wifi_connected = wifi_is_connected();
  bool eth_connected = ethernet_is_connected();

  // Show IP and MAC for the active interface
  if (eth_connected) {
    ethernet_get_ip_str(ip_str, sizeof(ip_str));
    ethernet_get_mac_str(mac_str, sizeof(mac_str));
  } else {
    wifi_get_ip_str(ip_str, sizeof(ip_str));
    wifi_get_mac_str(mac_str, sizeof(mac_str));
  }
  settings_get_device_name(device_name, sizeof(device_name));

  cJSON_AddStringToObject(info, "ip", ip_str);
  cJSON_AddStringToObject(info, "mac", mac_str);
  cJSON_AddStringToObject(info, "device_name", device_name);
  cJSON_AddBoolToObject(info, "wifi_connected", wifi_connected);
  cJSON_AddBoolToObject(info, "eth_connected", eth_connected);
  cJSON_AddNumberToObject(info, "free_heap", esp_get_free_heap_size());

  // WiFi link diagnostics (only meaningful when associated as STA)
  if (wifi_connected) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      char ssid_buf[33];
      size_t slen = strnlen((const char *)ap.ssid, sizeof(ap.ssid));
      if (slen > sizeof(ssid_buf) - 1)
        slen = sizeof(ssid_buf) - 1;
      memcpy(ssid_buf, ap.ssid, slen);
      ssid_buf[slen] = '\0';
      char bssid_buf[18];
      snprintf(bssid_buf, sizeof(bssid_buf), "%02x:%02x:%02x:%02x:%02x:%02x",
               ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4],
               ap.bssid[5]);
      const char *phy = "?";
      if (ap.phy_11n)
        phy = "11n";
      else if (ap.phy_11g)
        phy = "11g";
      else if (ap.phy_11b)
        phy = "11b";
      else if (ap.phy_lr)
        phy = "LR";
      cJSON_AddStringToObject(info, "wifi_ssid", ssid_buf);
      cJSON_AddStringToObject(info, "wifi_bssid", bssid_buf);
      cJSON_AddNumberToObject(info, "wifi_rssi", ap.rssi);
      cJSON_AddNumberToObject(info, "wifi_channel", ap.primary);
      cJSON_AddStringToObject(info, "wifi_phy", phy);
    }
  }
  const esp_app_desc_t *app_desc = esp_app_get_description();
  cJSON_AddStringToObject(info, "firmware_version", app_desc->version);
  cJSON_AddStringToObject(info, "boot_id", web_server_boot_id());
#ifdef CONFIG_DAC_TAS58XX
  cJSON_AddBoolToObject(info, "eq_supported", true);
#else
  cJSON_AddBoolToObject(info, "eq_supported", false);
#endif

  cJSON_AddItemToObject(json, "info", info);
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);

  return ESP_OK;
}

/* Open endpoint mirroring the cached AirPlay metadata. */
static esp_err_t nowplaying_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "playing", s_nowplaying.playing);
  cJSON_AddStringToObject(json, "title", s_nowplaying.title);
  cJSON_AddStringToObject(json, "artist", s_nowplaying.artist);
  cJSON_AddStringToObject(json, "album", s_nowplaying.album);
  cJSON_AddNumberToObject(json, "position", s_nowplaying.position_secs);
  cJSON_AddNumberToObject(json, "duration", s_nowplaying.duration_secs);
  /* Current SOURCE (iPhone) AirPlay volume as a linear Q15 (0..32768), or -1
     when nothing is playing. The UI maps it to a perceptual % to display the
     content volume separately from the board's own output level. */
  cJSON_AddNumberToObject(json, "sourceq15",
                          s_nowplaying.playing ? (int)airplay_get_volume_q15()
                                               : -1);

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

/* Protected nudge of the master output gain by +/-5 (clamped 0..100). Used by
   remote controls; the master gain always applies locally, including AirPlay
   2, where per-stream volume may not reach us. */
static esp_err_t volume_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;

  char content[128];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *dir_json = cJSON_GetObjectItem(json, "dir");
  const char *dir = (dir_json && cJSON_IsString(dir_json))
                        ? cJSON_GetStringValue(dir_json)
                        : NULL;

  cJSON *response = cJSON_CreateObject();
  if (dir && (strcmp(dir, "up") == 0 || strcmp(dir, "down") == 0)) {
    int percent = 100;
    settings_get_max_gain(&percent);
    percent += (strcmp(dir, "up") == 0) ? 5 : -5;
    if (percent < 0) {
      percent = 0;
    } else if (percent > 100) {
      percent = 100;
    }
    esp_err_t err = settings_set_max_gain(percent);
    if (err == ESP_OK) {
      cJSON_AddBoolToObject(response, "success", true);
      cJSON_AddNumberToObject(response, "gain", percent);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid dir");
  }

  char *json_str = cJSON_Print(response);
  if (!json_str) {
    cJSON_Delete(json);
    cJSON_Delete(response);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

static esp_err_t system_restart_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);

  ESP_LOGI(TAG, "Restart requested via web interface");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();

  return ESP_OK;
}

/* ================================================================== */
/*  SPIFFS File Management API                                         */
/* ================================================================== */

// Allowed path prefixes for read-only listing (must stay inside SPIFFS).
static const char *ALLOWED_PREFIXES[] = {"/spiffs/"};

// Writable prefix for upload/delete. Deliberately a dedicated subdir so the
// file API can NEVER overwrite or delete anything in the served web-UI
// directory (/spiffs/www/). The only legitimate writer is the SqueezeAMP /
// TAS575x hybrid-flow DSP firmware at /spiffs/hf/tas57xx_fw.bin.
static const char *WRITABLE_PREFIX = "/spiffs/hf/";

static bool is_path_allowed(const char *path) {
  for (int i = 0; i < sizeof(ALLOWED_PREFIXES) / sizeof(ALLOWED_PREFIXES[0]);
       i++) {
    if (strncmp(path, ALLOWED_PREFIXES[i], strlen(ALLOWED_PREFIXES[i])) == 0) {
      // Reject path traversal
      if (strstr(path, "..") != NULL) {
        return false;
      }
      return true;
    }
  }
  return false;
}

// Stricter check for upload/delete: only the dedicated writable subdir, and
// never a traversal/absolute-escape sequence.
static bool is_write_path_allowed(const char *path) {
  if (strncmp(path, WRITABLE_PREFIX, strlen(WRITABLE_PREFIX)) != 0) {
    return false;
  }
  if (strstr(path, "..") != NULL) {
    return false;
  }
  // Require an actual filename after the prefix (no writing the dir itself).
  if (path[strlen(WRITABLE_PREFIX)] == '\0') {
    return false;
  }
  return true;
}

static esp_err_t fs_upload_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;
  // Get target path from query string
  char query[128] = {0};
  char path[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing 'path' query parameter");
    return ESP_FAIL;
  }

  if (!is_write_path_allowed(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  if (req->content_len == 0 || req->content_len > (size_t)(64 * 1024)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body required (max 64KB)");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "wb");
  if (!f) {
    ESP_LOGE(TAG, "Failed to create %s", path);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to create file");
    return ESP_FAIL;
  }

  char buf[SPIFFS_CHUNK_SIZE];
  size_t remaining = req->content_len;
  while (remaining > 0) {
    size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
    int received = httpd_req_recv(req, buf, to_read);
    if (received <= 0) {
      fclose(f);
      remove(path);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Receive failed");
      return ESP_FAIL;
    }
    // A short write means the FS is full; reporting success would silently
    // leave a truncated/corrupt file. Treat it as a hard failure.
    size_t written = fwrite(buf, 1, (size_t)received, f);
    if (written != (size_t)received) {
      ESP_LOGE(TAG, "Short write to %s (%u/%u)", path, (unsigned)written,
               (unsigned)received);
      fclose(f);
      remove(path);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
      return ESP_FAIL;
    }
    remaining -= (size_t)received;
  }
  fclose(f);

  ESP_LOGI(TAG, "Uploaded %u bytes to %s", (unsigned)req->content_len, path);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "size", (double)req->content_len);
  cJSON_AddStringToObject(json, "path", path);
  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t fs_delete_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;
  char query[128] = {0};
  char path[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing 'path' query parameter");
    return ESP_FAIL;
  }

  if (!is_write_path_allowed(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_CreateObject();
  if (remove(path) == 0) {
    ESP_LOGI(TAG, "Deleted %s", path);
    cJSON_AddBoolToObject(json, "success", true);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", "File not found");
  }
  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t fs_list_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;
  char query[128] = {0};
  char dir_path[64] = "/spiffs";

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "dir", dir_path, sizeof(dir_path));
  }

  if (!is_path_allowed(dir_path) && strcmp(dir_path, "/spiffs") != 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  DIR *d = opendir(dir_path);
  cJSON *json = cJSON_CreateObject();
  cJSON *files = cJSON_CreateArray();

  if (d) {
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", entry->d_name);

      char full_path[320];
      snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
      struct stat st;
      if (stat(full_path, &st) == 0) {
        cJSON_AddNumberToObject(item, "size", (double)st.st_size);
      }
      cJSON_AddItemToArray(files, item);
    }
    closedir(d);
    cJSON_AddBoolToObject(json, "success", true);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", "Cannot open directory");
  }

  cJSON_AddItemToObject(json, "files", files);
  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

/* ================================================================== */
/*  EQ Page + API  (only when TAS58xx DAC is configured)               */
/* ================================================================== */

#ifdef CONFIG_DAC_TAS58XX

static esp_err_t eq_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/eq.html", "text/html");
}

static esp_err_t eq_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON *arr = cJSON_CreateArray();

  float gains[SETTINGS_EQ_BANDS];
  if (settings_get_eq_gains(gains) == ESP_OK) {
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)gains[i]));
    }
  } else {
    /* No saved EQ — return all zeros (flat) */
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      cJSON_AddItemToArray(arr, cJSON_CreateNumber(0.0));
    }
  }

  cJSON_AddItemToObject(json, "gains", arr);
  cJSON_AddNumberToObject(json, "bands", SETTINGS_EQ_BANDS);
  cJSON_AddBoolToObject(json, "success", true);

  char *json_str = cJSON_Print(json);
  if (!json_str) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t eq_post_handler(httpd_req_t *req) {
  if (check_auth(req) != ESP_OK)
    return ESP_FAIL;
  char content[512];
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *gains_arr = cJSON_GetObjectItem(json, "gains");

  if (gains_arr && cJSON_IsArray(gains_arr) &&
      cJSON_GetArraySize(gains_arr) == SETTINGS_EQ_BANDS) {

    float gains[SETTINGS_EQ_BANDS];
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      cJSON *item = cJSON_GetArrayItem(gains_arr, i);
      gains[i] = cJSON_IsNumber(item) ? (float)item->valuedouble : 0.0f;
      /* Clamp */
      if (gains[i] > 15.0f) {
        gains[i] = 15.0f;
      }
      if (gains[i] < -15.0f) {
        gains[i] = -15.0f;
      }
    }

    /* Emit event — listeners (settings + DAC) will handle it */
    eq_event_data_t ev_data;
    memcpy(ev_data.all_bands.gains_db, gains, sizeof(gains));
    eq_events_emit(EQ_EVENT_ALL_BANDS_SET, &ev_data);

    cJSON_AddBoolToObject(response, "success", true);
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error",
                            "Expected 'gains' array with 15 values");
  }

  char *json_str = cJSON_Print(response);
  if (!json_str) {
    cJSON_Delete(json);
    cJSON_Delete(response);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

#endif /* CONFIG_DAC_TAS58XX */

esp_err_t web_server_start(uint16_t port) {
  if (s_server) {
    ESP_LOGW(TAG, "Web server already running");
    return ESP_OK;
  }

  // Mint the per-boot id up front so it is stable from the first request.
  (void)web_server_boot_id();

  // Keep the now-playing cache fresh from RTSP/AirPlay events.
  rtsp_events_register(on_rtsp_event_nowplaying, NULL);

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
#ifdef CONFIG_BT_ENABLED
  config.max_open_sockets = 2;   // BT: tighter socket budget (LWIP 12)
  config.send_wait_timeout = 10; // BT/WiFi coexistence slows TCP drain
#else
  config.max_open_sockets = 3; // Limit to save lwIP socket slots for AirPlay
#endif
  config.lru_purge_enable = true; // Reclaim stale sockets when all are in use
  config.max_uri_handlers =
      46; // captive portal + EQ + speedtest + gain + auth + matrix + argb +
          // tone + nowplaying + volume + buttons + mqtt + protection
  config.max_resp_headers = 8;
  config.stack_size = 8192;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
    return err;
  }

  // Register handlers
  httpd_uri_t root_uri = {
      .uri = "/", .method = HTTP_GET, .handler = root_handler};
  httpd_register_uri_handler(s_server, &root_uri);

  httpd_uri_t favicon_uri = {
      .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler};
  httpd_register_uri_handler(s_server, &favicon_uri);

  // Auth API (always open)
  httpd_uri_t auth_status_uri = {.uri = "/api/auth/status",
                                 .method = HTTP_GET,
                                 .handler = auth_status_handler};
  httpd_register_uri_handler(s_server, &auth_status_uri);

  httpd_uri_t auth_setup_uri = {.uri = "/api/auth/setup",
                                .method = HTTP_POST,
                                .handler = auth_setup_handler};
  httpd_register_uri_handler(s_server, &auth_setup_uri);

  httpd_uri_t auth_login_uri = {.uri = "/api/auth/login",
                                .method = HTTP_POST,
                                .handler = auth_login_handler};
  httpd_register_uri_handler(s_server, &auth_login_uri);

  httpd_uri_t logs_uri = {
      .uri = "/logs", .method = HTTP_GET, .handler = logs_page_handler};
  httpd_register_uri_handler(s_server, &logs_uri);

  httpd_uri_t speedtest_page_uri = {.uri = "/speedtest",
                                    .method = HTTP_GET,
                                    .handler = speedtest_page_handler};
  httpd_register_uri_handler(s_server, &speedtest_page_uri);

  httpd_uri_t speedtest_ping_uri = {.uri = "/api/speedtest/ping",
                                    .method = HTTP_GET,
                                    .handler = speedtest_ping_handler};
  httpd_register_uri_handler(s_server, &speedtest_ping_uri);

  httpd_uri_t speedtest_dl_uri = {.uri = "/api/speedtest/download",
                                  .method = HTTP_GET,
                                  .handler = speedtest_download_handler};
  httpd_register_uri_handler(s_server, &speedtest_dl_uri);

  httpd_uri_t speedtest_ul_uri = {.uri = "/api/speedtest/upload",
                                  .method = HTTP_POST,
                                  .handler = speedtest_upload_handler};
  httpd_register_uri_handler(s_server, &speedtest_ul_uri);

  httpd_uri_t wifi_scan_uri = {.uri = "/api/wifi/scan",
                               .method = HTTP_GET,
                               .handler = wifi_scan_handler};
  httpd_register_uri_handler(s_server, &wifi_scan_uri);

  httpd_uri_t wifi_config_uri = {.uri = "/api/wifi/config",
                                 .method = HTTP_POST,
                                 .handler = wifi_config_handler};
  httpd_register_uri_handler(s_server, &wifi_config_uri);

  httpd_uri_t device_name_uri = {.uri = "/api/device/name",
                                 .method = HTTP_POST,
                                 .handler = device_name_handler};
  httpd_register_uri_handler(s_server, &device_name_uri);

  httpd_uri_t audio_gain_get_uri = {.uri = "/api/audio/gain",
                                    .method = HTTP_GET,
                                    .handler = audio_gain_get_handler};
  httpd_register_uri_handler(s_server, &audio_gain_get_uri);

  httpd_uri_t audio_gain_post_uri = {.uri = "/api/audio/gain",
                                     .method = HTTP_POST,
                                     .handler = audio_gain_post_handler};
  httpd_register_uri_handler(s_server, &audio_gain_post_uri);

  // Now-playing metadata (open) + volume nudge (protected)
  httpd_uri_t nowplaying_uri = {.uri = "/api/nowplaying",
                                .method = HTTP_GET,
                                .handler = nowplaying_handler};
  httpd_register_uri_handler(s_server, &nowplaying_uri);

  httpd_uri_t volume_uri = {
      .uri = "/api/volume", .method = HTTP_POST, .handler = volume_handler};
  httpd_register_uri_handler(s_server, &volume_uri);

  // Optional 8x8 LED matrix (MAX7219)
  httpd_uri_t matrix_get_uri = {
      .uri = "/api/matrix", .method = HTTP_GET, .handler = matrix_get_handler};
  httpd_register_uri_handler(s_server, &matrix_get_uri);

  httpd_uri_t matrix_post_uri = {.uri = "/api/matrix",
                                 .method = HTTP_POST,
                                 .handler = matrix_post_handler};
  httpd_register_uri_handler(s_server, &matrix_post_uri);

  httpd_uri_t argb_get_uri = {
      .uri = "/api/argb", .method = HTTP_GET, .handler = argb_get_handler};
  httpd_register_uri_handler(s_server, &argb_get_uri);

  httpd_uri_t argb_post_uri = {
      .uri = "/api/argb", .method = HTTP_POST, .handler = argb_post_handler};
  httpd_register_uri_handler(s_server, &argb_post_uri);

  // Software 3-band tone EQ (GET open, POST protected)
  httpd_uri_t tone_get_uri = {
      .uri = "/api/tone", .method = HTTP_GET, .handler = tone_get_handler};
  httpd_register_uri_handler(s_server, &tone_get_uri);

  httpd_uri_t tone_post_uri = {
      .uri = "/api/tone", .method = HTTP_POST, .handler = tone_post_handler};
  httpd_register_uri_handler(s_server, &tone_post_uri);

  // Speaker protection: limiter + amp mute/standby (GET open, POST protected)
  httpd_uri_t protection_get_uri = {.uri = "/api/protection",
                                    .method = HTTP_GET,
                                    .handler = protection_get_handler};
  httpd_register_uri_handler(s_server, &protection_get_uri);

  httpd_uri_t protection_post_uri = {.uri = "/api/protection",
                                     .method = HTTP_POST,
                                     .handler = protection_post_handler};
  httpd_register_uri_handler(s_server, &protection_post_uri);

  // Runtime-configurable physical buttons (GET open, POST protected)
  httpd_uri_t buttons_get_uri = {.uri = "/api/buttons",
                                 .method = HTTP_GET,
                                 .handler = buttons_get_handler};
  httpd_register_uri_handler(s_server, &buttons_get_uri);

  httpd_uri_t buttons_post_uri = {.uri = "/api/buttons",
                                  .method = HTTP_POST,
                                  .handler = buttons_post_handler};
  httpd_register_uri_handler(s_server, &buttons_post_uri);

  // Home Assistant MQTT integration (GET open, POST protected)
  httpd_uri_t mqtt_get_uri = {
      .uri = "/api/mqtt", .method = HTTP_GET, .handler = mqtt_get_handler};
  httpd_register_uri_handler(s_server, &mqtt_get_uri);

  httpd_uri_t mqtt_post_uri = {
      .uri = "/api/mqtt", .method = HTTP_POST, .handler = mqtt_post_handler};
  httpd_register_uri_handler(s_server, &mqtt_post_uri);

  httpd_uri_t ota_uri = {.uri = "/api/ota/update",
                         .method = HTTP_POST,
                         .handler = ota_update_handler};
  httpd_register_uri_handler(s_server, &ota_uri);

  httpd_uri_t system_info_uri = {.uri = "/api/system/info",
                                 .method = HTTP_GET,
                                 .handler = system_info_handler};
  httpd_register_uri_handler(s_server, &system_info_uri);

  httpd_uri_t system_restart_uri = {.uri = "/api/system/restart",
                                    .method = HTTP_POST,
                                    .handler = system_restart_handler};
  httpd_register_uri_handler(s_server, &system_restart_uri);

  // File management API
  httpd_uri_t fs_upload_uri = {.uri = "/api/fs/upload",
                               .method = HTTP_POST,
                               .handler = fs_upload_handler};
  httpd_register_uri_handler(s_server, &fs_upload_uri);

  httpd_uri_t fs_delete_uri = {.uri = "/api/fs/delete",
                               .method = HTTP_POST,
                               .handler = fs_delete_handler};
  httpd_register_uri_handler(s_server, &fs_delete_uri);

  httpd_uri_t fs_list_uri = {
      .uri = "/api/fs/list", .method = HTTP_GET, .handler = fs_list_handler};
  httpd_register_uri_handler(s_server, &fs_list_uri);

  // Captive portal detection endpoints
  // Apple iOS/macOS
  httpd_uri_t apple_captive1 = {.uri = "/hotspot-detect.html",
                                .method = HTTP_GET,
                                .handler = captive_apple_handler};
  httpd_register_uri_handler(s_server, &apple_captive1);

  httpd_uri_t apple_captive2 = {.uri = "/library/test/success.html",
                                .method = HTTP_GET,
                                .handler = captive_apple_handler};
  httpd_register_uri_handler(s_server, &apple_captive2);

  // Android
  httpd_uri_t android_captive = {.uri = "/generate_204",
                                 .method = HTTP_GET,
                                 .handler = captive_android_handler};
  httpd_register_uri_handler(s_server, &android_captive);

  // Windows
  httpd_uri_t windows_captive = {.uri = "/connecttest.txt",
                                 .method = HTTP_GET,
                                 .handler = captive_windows_handler};
  httpd_register_uri_handler(s_server, &windows_captive);

#ifdef CONFIG_DAC_TAS58XX
  httpd_uri_t eq_page_uri = {
      .uri = "/eq", .method = HTTP_GET, .handler = eq_page_handler};
  httpd_register_uri_handler(s_server, &eq_page_uri);

  httpd_uri_t eq_get_uri = {
      .uri = "/api/eq", .method = HTTP_GET, .handler = eq_get_handler};
  httpd_register_uri_handler(s_server, &eq_get_uri);

  httpd_uri_t eq_post_uri = {
      .uri = "/api/eq", .method = HTTP_POST, .handler = eq_post_handler};
  httpd_register_uri_handler(s_server, &eq_post_uri);
#endif

  log_stream_register(s_server);

  ESP_LOGI(TAG, "Web server started on port %d with captive portal support",
           port);
  return ESP_OK;
}

void web_server_stop(void) {
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "Web server stopped");
  }
}
