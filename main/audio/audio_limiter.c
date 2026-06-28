#include "audio_limiter.h"

#include "settings.h"
#include "sdkconfig.h"
#include <math.h>

// The limiter runs at the I2S output rate. Attack/release time constants are
// converted to per-sample one-pole smoothing coefficients at this rate.
#define LIM_SAMPLE_RATE ((float)CONFIG_OUTPUT_SAMPLE_RATE_HZ)

// Envelope follower time constants.
//   - Fast attack so transients above the ceiling are caught within a few ms.
//   - Slow release so the gain recovers smoothly without audible pumping.
#define LIM_ATTACK_MS  3.0f
#define LIM_RELEASE_MS 150.0f

// Full-scale reference for int16 audio.
#define LIM_FULL_SCALE 32767.0f

// Limiter state. Published scalars are written from the web handler and read
// by the single audio task; torn reads are harmless (one frame at the old
// setting at worst), so plain volatile is enough.
static volatile bool g_enabled = false;
// Ceiling expressed as a linear peak amplitude in int16 units (0 dBFS = 32767).
static volatile float g_threshold = LIM_FULL_SCALE;
// Smoothed gain currently applied to the signal (1.0 = transparent). Only the
// audio task touches this.
static float g_gain = 1.0f;
// Per-sample one-pole coefficients derived from the attack/release times.
static float g_attack_coef = 0.0f;
static float g_release_coef = 0.0f;

// Convert a time constant (ms) to a one-pole smoothing coefficient.
static float coef_from_ms(float ms) {
  if (ms <= 0.0f) {
    return 0.0f;
  }
  return expf(-1.0f / ((ms / 1000.0f) * LIM_SAMPLE_RATE));
}

static int16_t clamp_i16(float v) {
  if (v > 32767.0f) {
    return 32767;
  }
  if (v < -32768.0f) {
    return -32768;
  }
  return (int16_t)lrintf(v);
}

void audio_limiter_set(bool enabled, int ceiling_db) {
  if (ceiling_db > 0) {
    ceiling_db = 0;
  } else if (ceiling_db < -12) {
    ceiling_db = -12;
  }

  // Convert the dBFS ceiling to a linear peak amplitude (int16 units).
  g_threshold = LIM_FULL_SCALE * powf(10.0f, (float)ceiling_db / 20.0f);
  g_attack_coef = coef_from_ms(LIM_ATTACK_MS);
  g_release_coef = coef_from_ms(LIM_RELEASE_MS);
  // Publish enable last so the audio task never runs with stale coefficients.
  g_enabled = enabled;

  if (!enabled) {
    // Snap back to unity so re-enabling later starts transparent.
    g_gain = 1.0f;
  }
}

void audio_limiter_init(void) {
  bool lim_en = true;
  int lim_ceil = -1;
  settings_get_protection(&lim_en, &lim_ceil, NULL, NULL, NULL);
  audio_limiter_set(lim_en, lim_ceil);
}

void audio_limiter_process(int16_t *buf, size_t nsamples) {
  if (!g_enabled) {
    // Limiter bypassed: the samples are already valid int16, so this is a
    // true zero-cost no-op.
    return;
  }

  const float threshold = g_threshold;
  const float attack = g_attack_coef;
  const float release = g_release_coef;
  float gain = g_gain;

  // Process interleaved L/R pairs so the same gain is applied to both
  // channels (preserves the stereo image).
  for (size_t i = 0; i + 1 < nsamples; i += 2) {
    float l = (float)buf[i];
    float r = (float)buf[i + 1];

    // Linked detector: the louder of the two channels drives the gain.
    float peak = fabsf(l);
    float ar = fabsf(r);
    if (ar > peak) {
      peak = ar;
    }

    // Target gain needed to bring this sample down to the ceiling. Unity when
    // the peak is already below the threshold (transparent).
    float target = 1.0f;
    if (peak > threshold) {
      target = threshold / peak;
    }

    // Fast attack when we need to pull the gain down, slow release when we
    // can let it back up. One-pole smoothing toward the target.
    float coef = (target < gain) ? attack : release;
    gain = target + coef * (gain - target);

    buf[i] = clamp_i16(l * gain);
    buf[i + 1] = clamp_i16(r * gain);
  }

  g_gain = gain;
}
