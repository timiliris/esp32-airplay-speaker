#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Optional MAX7219-driven 8x8 LED matrix, audio-reactive.
 *
 * DISABLED by default. When disabled it uses no GPIOs, runs no task, and
 * led_matrix_feed() returns immediately, so it has zero effect on devices
 * that do not opt in.
 *
 * Effects (matrix_fx):
 *   0 = VU-metre, 1 = Spectre, 2 = Pulsation basses, 3 = Veilleuse.
 */

/**
 * Initialize the LED matrix subsystem (call once at startup, after
 * settings_init()). Reads NVS config; if disabled, does nothing.
 */
void led_matrix_init(void);

/**
 * Apply a config change at runtime. Safe to call repeatedly from the web
 * handler. Stops/frees the task and GPIOs if now disabled or if pins changed,
 * then (re)starts the renderer if enabled.
 */
void led_matrix_reconfigure(void);

/**
 * Lightweight audio analysis tap. Call from the audio playback task next to
 * led_audio_feed(). Returns immediately (cheap) if the feature is disabled.
 *
 * @param pcm            Interleaved stereo int16 samples (L,R,L,R,...)
 * @param stereo_samples Number of stereo frames (so pcm has 2*stereo_samples
 *                       int16 values).
 */
void led_matrix_feed(const int16_t *pcm, size_t stereo_samples);
