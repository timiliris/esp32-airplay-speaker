#pragma once

#include <stdbool.h>

/**
 * Home Assistant integration over MQTT (auto-discovery).
 *
 * DISABLED by default. When no broker is configured (or the feature is off in
 * NVS) nothing runs: no client is created, no task is started, and the device
 * behaves exactly as if this module did not exist. The esp-mqtt client owns
 * its own background task and reconnects on its own, so a missing/unreachable
 * broker never blocks or breaks AirPlay.
 *
 * Topics (base = airplay2/<deviceid>):
 *   - availability (LWT): airplay2/<id>/status      ("online"/"offline")
 *   - sensors:            airplay2/<id>/state,/title,/artist
 *   - number volume:      airplay2/<id>/volume/{set,state}
 *   - switch eq:          airplay2/<id>/eq/{set,state}
 *   - select eq preset:   airplay2/<id>/eqpreset/{set,state}
 *   - light matrix (json):airplay2/<id>/matrix/{set,state}
 *   - button restart:     airplay2/<id>/restart/set
 * Discovery configs are published (retained) to
 *   homeassistant/<component>/<deviceid>_<obj>/config
 */

/**
 * Initialize the MQTT integration (call once, after the network is up).
 * Reads the saved config via settings_get_mqtt(). If enabled and a host is
 * set, starts the esp-mqtt client with an LWT and registers an RTSP event
 * listener for now-playing state. Otherwise it does nothing.
 */
void ha_mqtt_init(void);

/**
 * Apply a config change at runtime. Stops any running client and (re)starts it
 * from the latest NVS config. Safe to call from the web handler.
 */
void ha_mqtt_reconfigure(void);

/**
 * @return true iff the MQTT client is currently connected to the broker.
 */
bool ha_mqtt_connected(void);
