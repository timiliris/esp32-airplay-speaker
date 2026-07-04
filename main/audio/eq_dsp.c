#include "eq_dsp.h"

#include "settings.h"
#include "sdkconfig.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* Sample rate the biquads are designed for (matches the I2S output rate). */
#define EQ_SAMPLE_RATE ((float)CONFIG_OUTPUT_SAMPLE_RATE_HZ)

/* Band centre frequencies / Q (per the shared contract). */
#define EQ_BASS_FC   120.0f  /* low-shelf  */
#define EQ_MID_FC    1000.0f /* peaking    */
#define EQ_MID_Q     1.0f
#define EQ_TREBLE_FC 8000.0f /* high-shelf */
/* Shelving "Q" / slope: 0.707 gives a clean Butterworth-ish shelf. */
#define EQ_SHELF_Q 0.707f

#define EQ_GAIN_MIN  (-12)
#define EQ_GAIN_MAX  (12)
#define EQ_NUM_BANDS 3

/* High-pass cutoff bounds (Hz). 0 = OFF; otherwise clamped to this range.
   Pole Q values for a 4th-order Butterworth high-pass, realised as two
   cascaded 2nd-order Butterworth sections at the same cutoff. */
#define EQ_HPF_FC_MIN 40
#define EQ_HPF_FC_MAX 400
#define EQ_HPF_Q1     0.54119610f
#define EQ_HPF_Q2     1.30656296f

/* Normalised RBJ biquad: y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2
   (a0 is divided out at compute time). */
typedef struct {
  float b0, b1, b2, a1, a2;
} biquad_coeffs_t;

/* One full coefficient set = the optional high-pass + the 3 cascaded bands.
   The high-pass is a 4th-order Butterworth realised as two cascaded 2nd-order
   sections (hpf1 then hpf2). hpf_on selects whether the stage is run. */
typedef struct {
  biquad_coeffs_t hpf1;
  biquad_coeffs_t hpf2;
  bool hpf_on;
  biquad_coeffs_t band[EQ_NUM_BANDS];
} eq_coeff_set_t;

/* Per-channel, per-band filter state (transposed Direct Form II). */
typedef struct {
  float z1, z2;
} biquad_state_t;

/* Double-buffered coefficient sets. The audio task reads the set pointed to
   by g_active_set; the web handler fills the other slot then atomically
   publishes its index. This gives a lock-free, tear-free swap. */
static eq_coeff_set_t g_sets[2];
static volatile int g_active_set = 0;

/* Master "do work" flag. When false, eq_dsp_process() returns immediately. */
static volatile bool g_active = false;

/* 2 channels (L,R) x 3 bands of state. Only the audio task touches this. */
static biquad_state_t g_state[2][EQ_NUM_BANDS];

/* 2 channels (L,R) of high-pass state, one array per cascaded 2nd-order
   section. Only the audio task touches this. */
static biquad_state_t g_hpf1_state[2];
static biquad_state_t g_hpf2_state[2];

static int clamp_gain(int g) {
  if (g < EQ_GAIN_MIN) {
    return EQ_GAIN_MIN;
  }
  if (g > EQ_GAIN_MAX) {
    return EQ_GAIN_MAX;
  }
  return g;
}

/* ---- RBJ Audio EQ Cookbook biquad designs (normalised by a0) ---- */

static void design_lowshelf(biquad_coeffs_t *c, float fc, float q,
                            float gain_db) {
  float A = powf(10.0f, gain_db / 40.0f);
  float w0 = 2.0f * (float)M_PI * fc / EQ_SAMPLE_RATE;
  float cw = cosf(w0);
  float sw = sinf(w0);
  float alpha = sw / (2.0f * q);
  float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;

  float b0 = A * ((A + 1.0f) - (A - 1.0f) * cw + two_sqrtA_alpha);
  float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
  float b2 = A * ((A + 1.0f) - (A - 1.0f) * cw - two_sqrtA_alpha);
  float a0 = (A + 1.0f) + (A - 1.0f) * cw + two_sqrtA_alpha;
  float a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cw);
  float a2 = (A + 1.0f) + (A - 1.0f) * cw - two_sqrtA_alpha;

  c->b0 = b0 / a0;
  c->b1 = b1 / a0;
  c->b2 = b2 / a0;
  c->a1 = a1 / a0;
  c->a2 = a2 / a0;
}

static void design_highshelf(biquad_coeffs_t *c, float fc, float q,
                             float gain_db) {
  float A = powf(10.0f, gain_db / 40.0f);
  float w0 = 2.0f * (float)M_PI * fc / EQ_SAMPLE_RATE;
  float cw = cosf(w0);
  float sw = sinf(w0);
  float alpha = sw / (2.0f * q);
  float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;

  float b0 = A * ((A + 1.0f) + (A - 1.0f) * cw + two_sqrtA_alpha);
  float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
  float b2 = A * ((A + 1.0f) + (A - 1.0f) * cw - two_sqrtA_alpha);
  float a0 = (A + 1.0f) - (A - 1.0f) * cw + two_sqrtA_alpha;
  float a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cw);
  float a2 = (A + 1.0f) - (A - 1.0f) * cw - two_sqrtA_alpha;

  c->b0 = b0 / a0;
  c->b1 = b1 / a0;
  c->b2 = b2 / a0;
  c->a1 = a1 / a0;
  c->a2 = a2 / a0;
}

static void design_peaking(biquad_coeffs_t *c, float fc, float q,
                           float gain_db) {
  float A = powf(10.0f, gain_db / 40.0f);
  float w0 = 2.0f * (float)M_PI * fc / EQ_SAMPLE_RATE;
  float cw = cosf(w0);
  float sw = sinf(w0);
  float alpha = sw / (2.0f * q);

  float b0 = 1.0f + alpha * A;
  float b1 = -2.0f * cw;
  float b2 = 1.0f - alpha * A;
  float a0 = 1.0f + alpha / A;
  float a1 = -2.0f * cw;
  float a2 = 1.0f - alpha / A;

  c->b0 = b0 / a0;
  c->b1 = b1 / a0;
  c->b2 = b2 / a0;
  c->a1 = a1 / a0;
  c->a2 = a2 / a0;
}

/* 2nd-order Butterworth high-pass section (RBJ Audio EQ Cookbook, normalised
   by a0). Two of these cascaded at distinct pole Qs form the 4th-order HPF. */
static void design_highpass(biquad_coeffs_t *c, float fc, float q) {
  float w0 = 2.0f * (float)M_PI * fc / EQ_SAMPLE_RATE;
  float cw = cosf(w0);
  float sw = sinf(w0);
  float alpha = sw / (2.0f * q);

  float b0 = (1.0f + cw) / 2.0f;
  float b1 = -(1.0f + cw);
  float b2 = (1.0f + cw) / 2.0f;
  float a0 = 1.0f + alpha;
  float a1 = -2.0f * cw;
  float a2 = 1.0f - alpha;

  c->b0 = b0 / a0;
  c->b1 = b1 / a0;
  c->b2 = b2 / a0;
  c->a1 = a1 / a0;
  c->a2 = a2 / a0;
}

void eq_dsp_set(bool enabled, int bass, int mid, int treble, int hpf) {
  bass = clamp_gain(bass);
  mid = clamp_gain(mid);
  treble = clamp_gain(treble);

  /* High-pass: 0 = OFF, otherwise clamp the cutoff to the supported range. */
  bool hpf_on = (hpf > 0);
  if (hpf_on) {
    if (hpf < EQ_HPF_FC_MIN) {
      hpf = EQ_HPF_FC_MIN;
    } else if (hpf > EQ_HPF_FC_MAX) {
      hpf = EQ_HPF_FC_MAX;
    }
  }

  /* Fill the inactive slot, then publish it atomically. */
  int next = g_active_set ^ 1;
  eq_coeff_set_t *set = &g_sets[next];

  set->hpf_on = hpf_on;
  if (hpf_on) {
    design_highpass(&set->hpf1, (float)hpf, EQ_HPF_Q1);
    design_highpass(&set->hpf2, (float)hpf, EQ_HPF_Q2);
  }
  design_lowshelf(&set->band[0], EQ_BASS_FC, EQ_SHELF_Q, (float)bass);
  design_peaking(&set->band[1], EQ_MID_FC, EQ_MID_Q, (float)mid);
  design_highshelf(&set->band[2], EQ_TREBLE_FC, EQ_SHELF_Q, (float)treble);

  /* Publish the new coefficient set before flipping the active flag so the
     audio task always sees a complete, consistent set. */
  g_active_set = next;

  bool tone_flat = (bass == 0 && mid == 0 && treble == 0);
  g_active = (enabled && (hpf_on || !tone_flat));
}

void eq_dsp_init(void) {
  bool enabled = false;
  int bass = 0, mid = 0, treble = 0, hpf = 0;
  settings_get_tone(&enabled, &bass, &mid, &treble, &hpf);

  /* Reset per-channel filter state. */
  for (int ch = 0; ch < 2; ch++) {
    g_hpf1_state[ch].z1 = 0.0f;
    g_hpf1_state[ch].z2 = 0.0f;
    g_hpf2_state[ch].z1 = 0.0f;
    g_hpf2_state[ch].z2 = 0.0f;
    for (int b = 0; b < EQ_NUM_BANDS; b++) {
      g_state[ch][b].z1 = 0.0f;
      g_state[ch][b].z2 = 0.0f;
    }
  }

  eq_dsp_set(enabled, bass, mid, treble, hpf);
}

/* Process one sample through a single biquad (transposed Direct Form II). */
static inline float biquad_step(const biquad_coeffs_t *c, biquad_state_t *s,
                                float x) {
  float y = c->b0 * x + s->z1;
  s->z1 = c->b1 * x - c->a1 * y + s->z2;
  s->z2 = c->b2 * x - c->a2 * y;
  return y;
}

void eq_dsp_process(int16_t *stereo, size_t frames) {
  if (!g_active) {
    return; /* true no-op when disabled/flat */
  }

  const eq_coeff_set_t *set = &g_sets[g_active_set];

  for (size_t i = 0; i < frames; i++) {
    for (int ch = 0; ch < 2; ch++) {
      float x = (float)stereo[2 * i + ch];

      /* Optional 4th-order high-pass first (two cascaded 2nd-order sections),
         then the 3 tone bands, with independent per-channel state. */
      if (set->hpf_on) {
        x = biquad_step(&set->hpf1, &g_hpf1_state[ch], x);
        x = biquad_step(&set->hpf2, &g_hpf2_state[ch], x);
      }
      x = biquad_step(&set->band[0], &g_state[ch][0], x);
      x = biquad_step(&set->band[1], &g_state[ch][1], x);
      x = biquad_step(&set->band[2], &g_state[ch][2], x);

      /* EQ boost can exceed full-scale — saturate back to int16. */
      if (x > 32767.0f) {
        x = 32767.0f;
      } else if (x < -32768.0f) {
        x = -32768.0f;
      }
      stereo[2 * i + ch] = (int16_t)lrintf(x);
    }
  }
}
