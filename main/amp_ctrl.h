#pragma once

/**
 * Amplifier mute / standby control via a single GPIO.
 *
 * Optional speaker-protection feature: the user wires their amplifier's
 * SD / mute / enable pin to a GPIO. The firmware drives that pin ACTIVE
 * while AirPlay is playing and INACTIVE (standby) after an idle timeout,
 * which suppresses idle hiss and the power-on/off pop and saves power.
 *
 * Driven entirely by RTSP playback events (one registered listener). All
 * settings come from NVS via settings_get_protection():
 *   - amp_gpio        : GPIO number, -1 = feature disabled (default)
 *   - amp_active_high : true  => amp ENABLED when the GPIO is HIGH (default)
 *                       false => amp ENABLED when the GPIO is LOW
 *   - amp_standby_min : minutes of idle before going to standby
 *                       (default 5; 0 = never auto-standby once playing)
 */

/**
 * Initialize amp control. If amp_gpio >= 0, configures it as an output and
 * drives it to the INACTIVE (standby) level so the amp starts muted at boot.
 * Registers a single RTSP-event listener. Call once at startup after
 * settings_init() and after the network is up. No-op when amp_gpio < 0.
 */
void amp_ctrl_init(void);

/**
 * Re-read the protection settings and re-apply them. Called by the web
 * handler after the amp GPIO / polarity / standby timeout changes. Safe to
 * call at runtime.
 */
void amp_ctrl_reconfigure(void);
