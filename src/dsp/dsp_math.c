// SPDX-License-Identifier: Apache-2.0
// Conversions, dither and small math helpers of the DSP module.
#include <math.h>

#include "dsp/dsp_priv.h"

float db_to_lin(float db) { return expf(db * DSP_LN10_OVER_20); }

// 20 * log10(lin); zero, negative and tiny values give -200 dB so results stay finite.
float lin_to_db(float lin) {
    if (isnan(lin)) return lin;
    if (!(lin > 1e-10f)) return -200.0f;
    return 20.0f * log10f(lin);
}

void pcm_q31_to_f32(const int32_t *in, float *out, size_t samples) {
    if (!in || !out) return;
    for (size_t i = 0; i < samples; i++) out[i] = (float)in[i] * DSP_Q31_INV;
}

void pcm_f32_to_q31(const float *in, int32_t *out, size_t samples) {
    if (!in || !out) return;
    for (size_t i = 0; i < samples; i++) out[i] = dsp_f32_to_q31(in[i]);
}

void pcm_q31_to_s16_dither(const int32_t *in, int16_t *out, size_t samples, uint32_t *state) {
    if (!in || !out) return;
    uint32_t s = state ? *state : 0x2545F491u;
    for (size_t i = 0; i < samples; i++) {
        // TPDF: difference of two uniform 16-bit values = +-1 LSB of the 16-bit output.
        const int32_t a = (int32_t)(dsp_lcg_next(&s) >> 16);
        const int32_t b = (int32_t)(dsp_lcg_next(&s) >> 16);
        // Add dither and half an LSB, then floor: rounding to nearest with dither.
        int64_t v = ((int64_t)in[i] + (a - b) + 32768) >> 16;
        if (v > INT16_MAX) v = INT16_MAX;
        if (v < INT16_MIN) v = INT16_MIN;
        out[i] = (int16_t)v;
    }
    if (state) *state = s;
}

void pcm_mono_to_stereo(const int32_t *in, int32_t *out, size_t frames) {
    if (!in || !out) return;
    // Backwards, so in == out works when the buffer holds 2 * frames samples.
    for (size_t i = frames; i-- > 0;) {
        const int32_t v = in[i];
        out[2 * i] = v;
        out[2 * i + 1] = v;
    }
}

float dsp_bessel_i0(float x) {
    // Power series of the modified Bessel function I0; converges fast for |x| < 20.
    const float y = 0.25f * x * x;
    float sum = 1.0f, term = 1.0f;
    for (int k = 1; k < 64; k++) {
        term *= y / (float)(k * k);
        sum += term;
        if (term < sum * 1e-9f) break;
    }
    return sum;
}

float dsp_sinc(float x) {
    if (fabsf(x) < 1e-6f) return 1.0f;
    // sin(pi x) = sin(pi r) with r = x - 2 round(x / 2) in [-1, 1]: keeps sinf's argument small.
    const float r = x - 2.0f * roundf(0.5f * x);
    return sinf(DSP_PI * r) / (DSP_PI * x);
}
