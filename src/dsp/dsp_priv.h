// SPDX-License-Identifier: Apache-2.0
// Private helpers shared by the DSP sources (not a public contract).
// Everything here is float32: the ESP32 FPU has no double precision.
#pragma once

#include <math.h>

#include "audio_player/dsp.h"

#define DSP_PI 3.14159265358979f
#define DSP_TWO_PI 6.28318530717959f
#define DSP_LN10_OVER_20 0.115129255f  // ln(10) / 20
#define DSP_Q31_INV (1.0f / 2147483648.0f)

// Round half away from zero. Valid for |x| < 2^31 (the caller clamps first).
static inline int32_t dsp_round_i32(float x) { return (int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f); }

// float full scale (+-1.0) -> Q31, rounding and saturating; NaN becomes 0.
static inline int32_t dsp_f32_to_q31(float v) {
    const float s = v * 2147483648.0f;
    if (s >= 2147483648.0f) return INT32_MAX;
    if (s > -2147483648.0f) return dsp_round_i32(s);
    if (s <= -2147483648.0f) return INT32_MIN;
    return 0;  // NaN
}

// Numerical Recipes LCG, the state format of pcm_q31_to_s16_dither().
static inline uint32_t dsp_lcg_next(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

// TPDF dither sample in LSB units, range (-1, +1), mean exactly 0: difference of
// two uniform 16-bit values taken from the high (good) bits of the LCG.
static inline float dsp_tpdf(uint32_t *s) {
    const int32_t a = (int32_t)(dsp_lcg_next(s) >> 16);
    const int32_t b = (int32_t)(dsp_lcg_next(s) >> 16);
    return (float)(a - b) * (1.0f / 65536.0f);
}

// Biquad filter state of one channel (transposed direct form II).
typedef struct {
    float s1, s2;
} dsp_bq_state_t;

// Filter interleaved float audio in place with one biquad; st[] has one entry per channel.
void dsp_biquad_run(const biquad_coef_t *c, dsp_bq_state_t *st, float *x, uint32_t frames, uint32_t channels);
// |H|^2 of a biquad at phi = sin^2(w/2) (RBJ cookbook form, numerically stable at low frequencies).
float dsp_biquad_mag2(const biquad_coef_t *c, float phi);
// True when the coefficients are exactly a pass-through.
bool dsp_biquad_is_identity(const biquad_coef_t *c);
// Kaiser window helpers for the resampler.
float dsp_bessel_i0(float x);
// sin(pi x) / (pi x) with argument reduction (accurate for large |x| in float).
float dsp_sinc(float x);
