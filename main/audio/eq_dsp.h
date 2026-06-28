#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Software 3-band tone EQ (cascaded RBJ biquads) for the generic
 * PCM5102A / I2S path. The board has no hardware DSP, so the tone
 * controls are applied in software on the output stream.
 *
 * Bands (computed at CONFIG_OUTPUT_SAMPLE_RATE_HZ):
 *   - hpf    = 4th-order Butterworth high-pass (two cascaded 2nd-order
 *              sections, 24 dB/oct), fc = hpf Hz, prepended as the FIRST
 *              stage of the cascade. 0 = OFF.
 *   - bass   = low-shelf,  fc ~120 Hz
 *   - mid    = peaking,    fc ~1000 Hz, Q ~1.0
 *   - treble = high-shelf, fc ~8000 Hz
 *
 * Gains are integers in dB, range -12..+12. 0/0/0 == flat.
 * hpf is an integer cutoff in Hz: 0 = OFF, otherwise clamped to 40..400.
 */

/**
 * Initialize the tone EQ. Reads the saved tone settings from NVS via
 * settings_get_tone() and applies them (computes coefficients). Call once
 * at startup after settings_init().
 */
void eq_dsp_init(void);

/**
 * Update the tone EQ at runtime. Gains are clamped to -12..+12 dB. The
 * high-pass + 3 RBJ biquad coefficient sets are recomputed at
 * CONFIG_OUTPUT_SAMPLE_RATE_HZ and swapped in atomically so the audio task
 * never reads half-updated coefficients. The EQ becomes a no-op unless
 * enabled and either the high-pass is on or at least one gain is non-zero.
 *
 * Safe to call from the web handler while audio is playing.
 *
 * @param enabled Master enable flag.
 * @param bass    Low-shelf gain in dB (-12..+12).
 * @param mid     Peaking gain in dB (-12..+12).
 * @param treble  High-shelf gain in dB (-12..+12).
 * @param hpf     High-pass cutoff in Hz. 0 = OFF; otherwise clamped to
 *                40..400 and run as a 4th-order Butterworth (24 dB/oct,
 *                two cascaded 2nd-order sections) first stage to protect
 *                small full-range satellites.
 */
void eq_dsp_set(bool enabled, int bass, int mid, int treble, int hpf);

/**
 * Process a block of interleaved 16-bit stereo samples in place. If the EQ
 * is not active (disabled or flat) this returns immediately at zero cost.
 *
 * @param stereo Interleaved L,R 16-bit samples.
 * @param frames Number of stereo sample pairs.
 */
void eq_dsp_process(int16_t *stereo, size_t frames);
