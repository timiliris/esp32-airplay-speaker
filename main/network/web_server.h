#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Web server for control panel
 * Provides:
 * - WiFi configuration
 * - Device name configuration
 * - OTA update
 */

/**
 * Initialize and start the web server
 * @param port HTTP server port (default: 80)
 */
esp_err_t web_server_start(uint16_t port);

/**
 * Stop the web server
 */
void web_server_stop(void);

/**
 * @return true iff a device admin password is set (i.e. auth is required).
 */
bool web_server_auth_required(void);

/**
 * @param tok Candidate session token (32-hex chars)
 * @return true iff tok is a live (non-expired) session token.
 */
bool web_server_auth_token_valid(const char *tok);
