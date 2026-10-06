// SPDX-License-Identifier: Apache-2.0
// RBJ Audio EQ Cookbook biquads (Robert Bristow-Johnson), transposed direct form II.
// Shelves take Q the way AutoEQ / Equalizer APO "LSC"/"HSC" do: alpha = sin(w0) / (2 Q).
#include <math.h>

#include "dsp/dsp_priv.h"

static const biquad_coef_t k_identity = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

void biquad_design(biquad_coef_t *c, peq_type_t type, float fs, float f0, float gain_db, float q) {
    if (!c) return;
    *c = k_identity;
    if (!(fs > 0.0f) || !isfinite(fs) || !(f0 > 0.0f) || !isfinite(f0)) return;
    if (!(q > 0.0f) || !isfinite(q)) q = 0.70710678f;
    if (q < 0.01f) q = 0.01f;
    if (q > 100.0f) q = 100.0f;
    if (!isfinite(gain_db)) gain_db = 0.0f;
    if (gain_db > 40.0f) gain_db = 40.0f;
    if (gain_db < -40.0f) gain_db = -40.0f;

    // A band at or above Nyquist cannot be realised. Keep what it does below Nyquist:
    // peaks, high shelves and low-passes there leave the audio band alone, a low shelf
    // becomes a plain gain, a high-pass is pulled just below Nyquist.
    const float fmax = 0.49f * fs;
    if (f0 > fmax) {
        switch (type) {
        case PEQ_LOW_SHELF: c->b0 = db_to_lin(gain_db); return;
        case PEQ_HIGH_PASS: f0 = fmax; break;
        default: return;
        }
    }
    // Lowest usable frequency: keeps cos(w0) distinguishable from 1 in float32.
    const float fmin = 2.0e-5f * fs;
    if (f0 < fmin) f0 = fmin;

    const float w0 = DSP_TWO_PI * f0 / fs;
    const float sn = sinf(w0);
    const float half = sinf(0.5f * w0);
    const float omc = 2.0f * half * half;  // 1 - cos(w0), exact at low frequencies
    const float cs = 1.0f - omc;
    const float alpha = sn / (2.0f * q);
    const float A = expf(gain_db * (DSP_LN10_OVER_20 * 0.5f));  // 10^(gain/40)

    float b0, b1, b2, a0, a1, a2;
    switch (type) {
    case PEQ_LOW_SHELF: {
        const float sa = 2.0f * sqrtf(A) * alpha;
        const float ap1 = A + 1.0f, am1 = A - 1.0f;
        b0 = A * (ap1 - am1 * cs + sa);
        b1 = 2.0f * A * (am1 - ap1 * cs);
        b2 = A * (ap1 - am1 * cs - sa);
        a0 = ap1 + am1 * cs + sa;
        a1 = -2.0f * (am1 + ap1 * cs);
        a2 = ap1 + am1 * cs - sa;
        break;
    }
    case PEQ_HIGH_SHELF: {
        const float sa = 2.0f * sqrtf(A) * alpha;
        const float ap1 = A + 1.0f, am1 = A - 1.0f;
        b0 = A * (ap1 + am1 * cs + sa);
        b1 = -2.0f * A * (am1 + ap1 * cs);
        b2 = A * (ap1 + am1 * cs - sa);
        a0 = ap1 - am1 * cs + sa;
        a1 = 2.0f * (am1 - ap1 * cs);
        a2 = ap1 - am1 * cs - sa;
        break;
    }
    case PEQ_LOW_PASS:
        b0 = 0.5f * omc;
        b1 = omc;
        b2 = 0.5f * omc;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cs;
        a2 = 1.0f - alpha;
        break;
    case PEQ_HIGH_PASS: {
        const float opc = 2.0f - omc;  // 1 + cos(w0)
        b0 = 0.5f * opc;
        b1 = -opc;
        b2 = 0.5f * opc;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cs;
        a2 = 1.0f - alpha;
        break;
    }
    case PEQ_PEAK:
    default:
        if (gain_db == 0.0f) return;
        b0 = 1.0f + alpha * A;
        b1 = -2.0f * cs;
        b2 = 1.0f - alpha * A;
        a0 = 1.0f + alpha / A;
        a1 = -2.0f * cs;
        a2 = 1.0f - alpha / A;
        break;
    }
    if (!(a0 != 0.0f) || !isfinite(a0)) return;
    const float inv = 1.0f / a0;
    biquad_coef_t r = {b0 * inv, b1 * inv, b2 * inv, a1 * inv, a2 * inv};
    if (!isfinite(r.b0) || !isfinite(r.b1) || !isfinite(r.b2) || !isfinite(r.a1) || !isfinite(r.a2)) return;
    *c = r;
}

float dsp_biquad_mag2(const biquad_coef_t *c, float phi) {
    // |H(e^jw)|^2 with phi = sin^2(w/2), from the RBJ cookbook.
    const float bs = c->b0 + c->b1 + c->b2;
    const float as = 1.0f + c->a1 + c->a2;
    float num = bs * bs - 4.0f * (c->b0 * c->b1 + 4.0f * c->b0 * c->b2 + c->b1 * c->b2) * phi +
                16.0f * c->b0 * c->b2 * phi * phi;
    float den = as * as - 4.0f * (c->a1 + 4.0f * c->a2 + c->a1 * c->a2) * phi + 16.0f * c->a2 * phi * phi;
    if (!(num > 1e-30f)) num = 1e-30f;
    if (!(den > 1e-30f)) den = 1e-30f;
    return num / den;
}

float biquad_response_db(const biquad_coef_t *c, float fs, float f) {
    if (!c || !(fs > 0.0f)) return 0.0f;
    if (f < 0.0f) f = -f;
    const float s = sinf(DSP_PI * f / fs);  // sin(w/2)
    return 10.0f * log10f(dsp_biquad_mag2(c, s * s));
}

bool dsp_biquad_is_identity(const biquad_coef_t *c) {
    return c->b0 == 1.0f && c->b1 == 0.0f && c->b2 == 0.0f && c->a1 == 0.0f && c->a2 == 0.0f;
}

void dsp_biquad_run(const biquad_coef_t *c, dsp_bq_state_t *st, float *x, uint32_t frames, uint32_t channels) {
    const float b0 = c->b0, b1 = c->b1, b2 = c->b2, a1 = c->a1, a2 = c->a2;
    if (channels == 2) {
        float l1 = st[0].s1, l2 = st[0].s2, r1 = st[1].s1, r2 = st[1].s2;
        for (uint32_t i = 0; i < frames; i++) {
            const float xl = x[2 * i], xr = x[2 * i + 1];
            const float yl = b0 * xl + l1;
            const float yr = b0 * xr + r1;
            l1 = b1 * xl - a1 * yl + l2;
            r1 = b1 * xr - a1 * yr + r2;
            l2 = b2 * xl - a2 * yl;
            r2 = b2 * xr - a2 * yr;
            x[2 * i] = yl;
            x[2 * i + 1] = yr;
        }
        // Flush denormals: far below the Q31 LSB (4.7e-10) and slow on some FPUs.
        if (fabsf(l1) < 1e-20f) l1 = 0.0f;
        if (fabsf(l2) < 1e-20f) l2 = 0.0f;
        if (fabsf(r1) < 1e-20f) r1 = 0.0f;
        if (fabsf(r2) < 1e-20f) r2 = 0.0f;
        st[0].s1 = l1;
        st[0].s2 = l2;
        st[1].s1 = r1;
        st[1].s2 = r2;
        return;
    }
    for (uint32_t ch = 0; ch < channels; ch++) {
        float s1 = st[ch].s1, s2 = st[ch].s2;
        float *p = x + ch;
        for (uint32_t i = 0; i < frames; i++, p += channels) {
            const float xi = *p;
            const float y = b0 * xi + s1;
            s1 = b1 * xi - a1 * y + s2;
            s2 = b2 * xi - a2 * y;
            *p = y;
        }
        if (fabsf(s1) < 1e-20f) s1 = 0.0f;
        if (fabsf(s2) < 1e-20f) s2 = 0.0f;
        st[ch].s1 = s1;
        st[ch].s2 = s2;
    }
}
