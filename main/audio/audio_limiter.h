#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Feed-forward peak limiter for the generic int16 stereo output path.
 *
 * Protects connected speakers from clipping distortion by smoothly pulling
 * the gain back when the post-volume signal exceeds a configurable ceiling
 * (in dBFS), instead of letting samples hard-clip at full scale. A single
 * linked L/R gain factor is applied to both channels so the stereo image is
 * preserved. A per-sample envelope follower with a fast attack (~3 ms) and a
 * slow release (~150 ms) keeps the gain reduction transparent: the gain
 * stays at unity whenever the signal sits below the ceiling.
 *
 * Runs from the single audio playback task only; all state is static.
 * When disabled it is zero-cost (early return), but the int16 clamp is
 * always applied as a final safety net.
 */

/**
 * Initialize the limiter. Reads the saved protection settings from NVS via
 * settings_get_protection() and applies them (enable + ceiling). Call once
 * at startup after settings_init().
 */
void audio_limiter_init(void);

/**
 * Update the limiter at runtime. Safe to call from the web handler while
 * audio is playing.
 *
 * @param enabled    Master enable flag. When false the limiter only clamps.
 * @param ceiling_db Output ceiling in dBFS, clamped to -12..0.
 */
void audio_limiter_set(bool enabled, int ceiling_db);

/**
 * Process a block of interleaved 16-bit stereo samples in place. Applies the
 * gain reduction (when enabled) and always clamps the result to int16 range.
 *
 * @param buf      Interleaved L,R 16-bit samples.
 * @param nsamples Total sample count (frames * 2).
 */
void audio_limiter_process(int16_t *buf, size_t nsamples);
