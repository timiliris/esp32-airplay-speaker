/**
 * @file board.c
 * @brief Waveshare ESP32P4 DEV KIT board implementation
 *
 * Minimal implementation for Waveshare ESP32P4 DEV KIT boards with external I2S
 * DAC.
 */

#include "iot_board.h"

#include "esp_log.h"

static const char TAG[] = "Waveshare-ESP32P4-DEV-KIT";

static bool s_board_initialized = false;

const char *iot_board_get_info(void) {
  return BOARD_NAME;
}

bool iot_board_is_init(void) {
  return s_board_initialized;
}

board_res_handle_t iot_board_get_handle(int id) {
  (void)id;
  return NULL;
}

esp_err_t iot_board_init(void) {
  if (s_board_initialized) {
    ESP_LOGW(TAG, "Board already initialized");
    return ESP_OK;
  }

  s_board_initialized = true;
  ESP_LOGI(TAG, "Waveshare ESP32-P4 DEV KIT initialized");
  return ESP_OK;
}

esp_err_t iot_board_deinit(void) {
  s_board_initialized = false;
  return ESP_OK;
}
