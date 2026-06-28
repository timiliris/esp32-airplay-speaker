#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Persistent settings storage (NVS)
 */

// Default device name (used if none configured)
#define SETTINGS_DEFAULT_DEVICE_NAME "ESP32 AirPlay"

/**
 * Initialize settings module (call once at startup)
 */
esp_err_t settings_init(void);

/**
 * Get saved volume in dB
 * @param volume_db Output: volume in dB (0 = max, -30 = mute)
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved value
 */
esp_err_t settings_get_volume(float *volume_db);

/**
 * Apply volume (updates cached value and DAC, does NOT write to NVS).
 * @param volume_db Volume in dB (0 = max, -30 = mute)
 */
esp_err_t settings_set_volume(float volume_db);

/**
 * Persist the current cached volume to NVS.
 * Call once at session disconnect rather than on every change.
 */
esp_err_t settings_persist_volume(void);

#ifdef CONFIG_BT_A2DP_ENABLE
/**
 * Get saved Bluetooth volume (AVRC 0-127 scale).
 * @param volume Output: 0 (mute) to 127 (max)
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved value
 */
esp_err_t settings_get_bt_volume(uint8_t *volume);

/**
 * Update cached Bluetooth volume (does NOT write to NVS).
 * Caller is responsible for calling dac_set_volume().
 * @param volume 0 (mute) to 127 (max)
 */
esp_err_t settings_set_bt_volume(uint8_t volume);

/**
 * Persist the current cached BT volume to NVS.
 * Call once at session disconnect rather than on every change.
 */
esp_err_t settings_persist_bt_volume(void);
#endif

/**
 * Get saved WiFi SSID
 * @param ssid Output buffer for SSID
 * @param len Size of SSID buffer
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved value
 */
esp_err_t settings_get_wifi_ssid(char *ssid, size_t len);

/**
 * Get saved WiFi password
 * @param password Output buffer for password
 * @param len Size of password buffer
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved value
 */
esp_err_t settings_get_wifi_password(char *password, size_t len);

/**
 * Save WiFi credentials to persistent storage
 * @param ssid WiFi SSID
 * @param password WiFi password
 */
esp_err_t settings_set_wifi_credentials(const char *ssid, const char *password);

/**
 * Check if WiFi credentials are stored
 * @return true if credentials exist, false otherwise
 */
bool settings_has_wifi_credentials(void);

/**
 * Get device name (returns default if none saved)
 * @param name Output buffer for device name
 * @param len Size of name buffer
 * @return ESP_OK (always returns a valid name)
 */
esp_err_t settings_get_device_name(char *name, size_t len);

/**
 * Save device name to persistent storage
 * @param name Device name
 */
esp_err_t settings_set_device_name(const char *name);

// ---- Master output gain (software volume cap) ----

/**
 * Get the master output gain as a percentage (0-100).
 * This caps the maximum output level on top of the AirPlay volume,
 * useful when the amplifier gain is too high for the speakers.
 * @param percent Output: 0 (silent) to 100 (full). Defaults to 100.
 * @return ESP_OK (always returns a valid value)
 */
esp_err_t settings_get_max_gain(int *percent);

/**
 * Set and persist the master output gain percentage (0-100).
 * Takes effect on the next audio frame (no restart needed).
 */
esp_err_t settings_set_max_gain(int percent);

/**
 * Get the master output gain as a Q15 multiplier (0..32768).
 * Cheap accessor meant to be called from the audio output path.
 */
int32_t settings_get_max_gain_q15(void);

// ---- EQ settings ----

/** Number of EQ bands stored in NVS */
#define SETTINGS_EQ_BANDS 15

/**
 * Get saved EQ gains.
 * @param gains_db Output array of SETTINGS_EQ_BANDS floats
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no saved EQ
 */
esp_err_t settings_get_eq_gains(float gains_db[SETTINGS_EQ_BANDS]);

/**
 * Save EQ gains to persistent storage.
 * @param gains_db Array of SETTINGS_EQ_BANDS floats (dB)
 */
esp_err_t settings_set_eq_gains(const float gains_db[SETTINGS_EQ_BANDS]);

/**
 * Clear saved EQ (revert to flat on next boot).
 */
esp_err_t settings_clear_eq(void);

/**
 * Check if EQ gains are saved.
 */
bool settings_has_eq(void);

// ---- Software 3-band tone EQ ----

/**
 * Get the saved tone EQ settings. Fills the contract defaults for any key
 * not stored (enabled=false, bass=mid=treble=0, hpf=0). Any output pointer
 * may be NULL.
 * @param en     Output: tone EQ enabled
 * @param bass   Output: low-shelf gain in dB (-12..+12)
 * @param mid    Output: peaking gain in dB (-12..+12)
 * @param treble Output: high-shelf gain in dB (-12..+12)
 * @param hpf    Output: high-pass cutoff in Hz (0 = OFF, otherwise 40..400)
 * @return ESP_OK (always returns valid values)
 */
esp_err_t settings_get_tone(bool *en, int *bass, int *mid, int *treble,
                            int *hpf);

/**
 * Set and persist the tone EQ settings. Validates each gain in -12..+12 dB
 * and the high-pass cutoff as 0 (OFF) or 40..400 Hz.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on a bad range
 */
esp_err_t settings_set_tone(bool en, int bass, int mid, int treble, int hpf);

// ---- Device admin password ----

/**
 * Check if a device admin password is configured.
 * @return true if a password digest is stored, false otherwise
 */
bool settings_has_device_password(void);

/**
 * Set (and persist) the device admin password.
 * Stores SHA-256(pw) as a 32-byte blob. Rejects passwords shorter than 4.
 * @param pw New password (plaintext)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if too short
 */
esp_err_t settings_set_device_password(const char *pw);

/**
 * Check a candidate password against the stored digest (constant time).
 * @param pw Candidate password (plaintext)
 * @return true if it matches the stored password, false otherwise
 */
bool settings_check_device_password(const char *pw);

// ---- Optional 8x8 LED matrix (MAX7219) ----

/**
 * Get the LED matrix configuration. Fills defaults for any key not stored
 * (enabled=false, effect=0, brightness=4, din=4, clk=5, cs=6).
 * Any output pointer may be NULL.
 * @param en Output: feature enabled
 * @param fx Output: effect (0=VU, 1=Spectrum, 2=Bass pulse, 3=Nightlight)
 * @param br Output: brightness (0-15)
 * @param din Output: data-in GPIO (-1 = unset)
 * @param clk Output: clock GPIO (-1 = unset)
 * @param cs Output: chip-select GPIO (-1 = unset)
 * @return ESP_OK (always returns valid values)
 */
esp_err_t settings_get_matrix(bool *en, int *fx, int *br, int *din, int *clk,
                              int *cs);

/**
 * Set and persist the LED matrix configuration. Validates ranges
 * (effect 0-3, brightness 0-15, pins -1..48).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on a bad range
 */
esp_err_t settings_set_matrix(bool en, int fx, int br, int din, int clk,
                              int cs);

// ---- Optional addressable RGB strip (WS2812) ----

/**
 * Get the RGB strip configuration. Fills defaults for any key not stored
 * (enabled=false, gpio=8, count=30, effect=0, brightness=128). Any output
 * pointer may be NULL.
 * @param en    Output: feature enabled
 * @param gpio  Output: data GPIO (-1 = unset)
 * @param count Output: number of LEDs (1-300)
 * @param fx    Output: effect (0-11)
 * @param br    Output: master brightness (0-255)
 * @param color Output: base colour 0xRRGGBB (for solid/breathe/comet/etc.)
 * @param speed Output: animation speed (1-10)
 * @return ESP_OK (always returns valid values)
 */
esp_err_t settings_get_argb(bool *en, int *gpio, int *count, int *fx, int *br,
                            uint32_t *color, int *speed);

/**
 * Set and persist the RGB strip configuration. Validates ranges
 * (effect 0-11, brightness 0-255, gpio -1..48, count 1-300, speed 1-10;
 * colour is any 0xRRGGBB).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on a bad range
 */
esp_err_t settings_set_argb(bool en, int gpio, int count, int fx, int br,
                            uint32_t color, int speed);

// ---- Home Assistant MQTT integration ----

/**
 * Get the MQTT integration settings. Fills the contract defaults for any key
 * not stored (enabled=false, host="", port=1883, user="", pass=""). Any
 * output pointer may be NULL.
 * @param en   Output: integration enabled
 * @param host Output buffer for the broker host (may be NULL)
 * @param hlen Size of host buffer
 * @param port Output: broker TCP port (defaults to 1883)
 * @param user Output buffer for the username (may be NULL)
 * @param ulen Size of user buffer
 * @param pass Output buffer for the password (may be NULL)
 * @param plen Size of pass buffer
 * @return ESP_OK (always returns valid values)
 */
esp_err_t settings_get_mqtt(bool *en, char *host, size_t hlen, int *port,
                            char *user, size_t ulen, char *pass, size_t plen);

/**
 * Set and persist the MQTT integration settings.
 * @param en   Integration enabled
 * @param host Broker host (NULL = clear)
 * @param port Broker TCP port
 * @param user Username (NULL = clear)
 * @param pass Password (NULL keeps the stored one; "" clears it)
 * @return ESP_OK on success
 */
esp_err_t settings_set_mqtt(bool en, const char *host, int port,
                            const char *user, const char *pass);

// ---- Physical buttons (runtime-configurable GPIOs) ----

/**
 * Get the physical button GPIO assignments. Fills the corresponding
 * CONFIG_BTN_*_GPIO default for any key not stored, so behaviour is
 * unchanged until the user overrides it. -1 means the button is disabled.
 * Any output pointer may be NULL.
 * @param pp Output: play/pause GPIO (-1 = disabled)
 * @param vu Output: volume-up GPIO (-1 = disabled)
 * @param vd Output: volume-down GPIO (-1 = disabled)
 * @param nx Output: next-track GPIO (-1 = disabled)
 * @param pv Output: previous-track GPIO (-1 = disabled)
 * @return ESP_OK (always returns valid values)
 */
esp_err_t settings_get_buttons(int *pp, int *vu, int *vd, int *nx, int *pv);

/**
 * Set and persist the physical button GPIO assignments. Validates each
 * value in -1..48 (-1 = disabled). Changes apply on the next reboot
 * (button GPIO/timer setup happens once at boot).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on a bad range
 */
esp_err_t settings_set_buttons(int pp, int vu, int vd, int nx, int pv);

// ---- Speaker protection (software limiter + amp mute/standby) ----

/**
 * Get the speaker-protection settings. Fills the contract defaults for any
 * key not stored (limEn=true, limCeil=-1, ampGpio=-1, ampActiveHigh=true,
 * ampStandbyMin=5). Any output pointer may be NULL.
 * @param limEn         Output: software peak limiter enabled (default true)
 * @param limCeil       Output: limiter ceiling in dBFS, -12..0 (default -1)
 * @param ampGpio       Output: amp mute/standby GPIO, -1 = disabled (default)
 * @param ampActiveHigh Output: amp enabled when GPIO high (default true)
 * @param ampStandbyMin Output: idle minutes before standby, 0 = never (def 5)
 * @return ESP_OK (always returns valid values)
 */
esp_err_t settings_get_protection(bool *limEn, int *limCeil, int *ampGpio,
                                  bool *ampActiveHigh, int *ampStandbyMin);

/**
 * Set and persist the speaker-protection settings. Validates the limiter
 * ceiling (-12..0), the amp GPIO (-1..48) and the standby minutes (0..120).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on a bad range
 */
esp_err_t settings_set_protection(bool limEn, int limCeil, int ampGpio,
                                  bool ampActiveHigh, int ampStandbyMin);

/**
 * Get the software output channel mode.
 * @return 0=stereo (default), 1=mono (L+R)/2, 2=left, 3=right
 */
int settings_get_channel_mode(void); // 0=stereo(def),1=mono,2=left,3=right

/**
 * Set and persist the software output channel mode (validates 0..3).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on a bad value
 */
esp_err_t settings_set_channel_mode(int mode); // validates 0..3
