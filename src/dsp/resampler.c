// SPDX-License-Identifier: Apache-2.0
// Sample-rate conversion for the Bluetooth path.
//
// The conversion is split into
//   1. up to four half-band decimators, each halving the rate while the result stays
//      >= the output rate (96k -> 48k, 176.4k -> 88.2k -> 44.1k). Half-band filters
//      have every other tap zero and are symmetric: about K multiplies per output;
//   2. one polyphase windowed-sinc (Kaiser) stage for the remaining ratio L/M
//      (48k -> 44.1k = 147/160). Exact phases while L <= 1024, otherwise 256 phases
//      with linear interpolation between neighbouring phases.
// All tables are computed at create; processing never allocates. The math is float32.
//
// Output timing is exact: output k sits at input time k * M / L of its stage, so
// the total output count depends only on the total input count, never on how the
// input was split into calls.
#include "audio_player/resampler.h"

#include <math.h>
#include <string.h>

#include "dsp/dsp_priv.h"

#define TAG "resampler"

#define RS_BLOCK 256u                 // input frames per internal block
#define RS_RATE_MIN 1000u
#define RS_RATE_MAX 768000u
#define RS_MAX_HB 4
#define RS_EXACT_MAX_PHASES 1024u
#define RS_INTERP_PHASES 256u
#define RS_MAX_TAPS 512u
#define RS_MAX_TABLE (512u * 1024u)   // coefficients (2 MB)

typedef struct {
    uint32_t K;                       // non-zero odd taps per side; length 4K - 1
    uint32_t hist;                    // 4K - 2 frames of history
    uint32_t next;                    // buffer index of the next input that yields an output
    float *coef;                      // taps at offsets +-(2j + 1) from the centre
    float *buf[AUDIO_MAX_CHANNELS];
} hb_stage_t;

typedef struct {
    uint32_t L, M;                    // out / in = L / M, reduced
    uint32_t T;                       // taps per phase
    uint32_t P;                       // phases in the table (P == L when exact)
    bool interp;
    float inv_L;                      // 1 / L, for the interpolation weight
    uint32_t hist;                    // T - 1 frames of history
    uint32_t cur;                     // buffer index of the newest input of the next output
    uint32_t phase;                   // fractional position of the next output, 0..L-1
    float *coef;                      // rows of T taps, time-reversed (oldest input first)
    float *buf[AUDIO_MAX_CHANNELS];
} pp_stage_t;

struct resampler {
    uint32_t in_rate, out_rate;
    uint8_t ch;
    bool copy;
    uint32_t nhb;
    hb_stage_t hb[RS_MAX_HB];
    bool has_pp;
    pp_stage_t pp;
    float *out[AUDIO_MAX_CHANNELS];   // planar output of the last stage, one block
};

static uint32_t gcd_u32(uint32_t a, uint32_t b) {
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static float kaiser_beta(float atten_db) {
    if (atten_db > 50.0f) return 0.1102f * (atten_db - 8.7f);
    if (atten_db > 21.0f) return 0.5842f * powf(atten_db - 21.0f, 0.4f) + 0.07886f * (atten_db - 21.0f);
    return 0.0f;
}

// ------------------------------------------------------------ half-band ----
static uint64_t hb_count(const hb_stage_t *s, uint64_t n, bool bound) {
    const uint64_t next = bound ? s->hist : s->next;
    const uint64_t fill = s->hist + n;
    return next >= fill ? 0 : (fill - 1 - next) / 2 + 1;
}

static bool hb_init(hb_stage_t *s, float fs_in, float fpass, float atten_db) {
    // Transition from fpass to fs_in/2 - fpass (half-band symmetry).
    float df = 0.5f - 2.0f * fpass / fs_in;
    if (df < 0.02f) df = 0.02f;
    const float n = (atten_db - 7.95f) / (14.36f * df) + 1.0f;
    uint32_t K = (uint32_t)ceilf((n + 1.0f) * 0.25f);
    K = CORE_CLAMP(K, 2u, 128u);
    s->K = K;
    s->hist = 4 * K - 2;
    s->next = s->hist;
    s->coef = core_malloc(K * sizeof(float));
    if (!s->coef) return false;
    const float centre = (float)(2 * K - 1);
    const float beta = kaiser_beta(atten_db);
    const float inv_i0 = 1.0f / dsp_bessel_i0(beta);
    float sum = 0.0f;
    for (uint32_t j = 0; j < K; j++) {
        const float m = (float)(2 * j + 1);
        const float x = m / centre;
        const float w = dsp_bessel_i0(beta * sqrtf(fmaxf(0.0f, 1.0f - x * x))) * inv_i0;
        const float h = ((j & 1) ? -1.0f : 1.0f) / (DSP_PI * m) * w;
        s->coef[j] = h;
        sum += h;
    }
    // DC gain exactly 1: centre tap 0.5 plus both sides.
    const float scale = 0.25f / sum;
    for (uint32_t j = 0; j < K; j++) s->coef[j] *= scale;
    return true;
}

static uint32_t hb_run(hb_stage_t *s, uint32_t ch, uint32_t n, float *const *dest) {
    const uint32_t hist = s->hist, fill = hist + n, K = s->K, c0 = 2 * K - 1;
    const float *coef = s->coef;
    uint32_t i = s->next, cnt = 0;
    for (; i < fill; i += 2, cnt++) {
        for (uint32_t c = 0; c < ch; c++) {
            const float *x = s->buf[c] + (i - hist);
            const float *lo = x + c0 - 1, *hi = x + c0 + 1;
            float acc = 0.5f * x[c0];
            for (uint32_t j = 0; j < K; j++) acc += coef[j] * (lo[-(int32_t)(2 * j)] + hi[2 * j]);
            dest[c][cnt] = acc;
        }
    }
    s->next = i - n;
    for (uint32_t c = 0; c < ch; c++) memmove(s->buf[c], s->buf[c] + n, hist * sizeof(float));
    return cnt;
}

// ------------------------------------------------------------ polyphase ----
static uint64_t pp_count(const pp_stage_t *s, uint64_t n, bool bound) {
    const uint64_t d = bound ? 0 : s->cur - s->hist;
    const uint64_t phase = bound ? 0 : s->phase;
    if (n <= d) return 0;
    const uint64_t a = (n - d) * s->L - phase;
    return (a + s->M - 1) / s->M;
}

static bool pp_init(pp_stage_t *s, uint32_t fin, uint32_t fout, resampler_quality_t q) {
    const uint32_t g = gcd_u32(fin, fout);
    s->L = fout / g;
    s->M = fin / g;
    const float ratio = (float)s->M / (float)s->L;  // > 1 when downsampling
    const float base = q == RESAMPLER_FAST ? 16.0f : 32.0f;
    const float atten = q == RESAMPLER_FAST ? 72.0f : 90.0f;
    uint32_t T = (uint32_t)ceilf(base * (ratio > 1.0f ? ratio : 1.0f));
    T += T & 1u;
    if (T > RS_MAX_TAPS) {
        CORE_LOGE(TAG, "ratio %lu/%lu needs %lu taps", (unsigned long)fout, (unsigned long)fin, (unsigned long)T);
        return false;
    }
    s->T = T;
    s->interp = s->L > RS_EXACT_MAX_PHASES;
    s->P = s->interp ? RS_INTERP_PHASES : s->L;
    s->inv_L = 1.0f / (float)s->L;
    const uint32_t rows = s->P + (s->interp ? 1u : 0u);
    if ((uint64_t)rows * T > RS_MAX_TABLE) return false;
    s->hist = T - 1;
    s->cur = s->hist;
    s->phase = 0;
    s->coef = core_malloc((size_t)rows * T * sizeof(float));
    if (!s->coef) return false;

    // Cut-off at half the lower rate: whatever the transition band lets through
    // aliases only into the transition band itself, never into the passband.
    const float fb = (float)(fin < fout ? fin : fout);
    const float two_fc = fb / (float)fin;  // 2 * cut-off in cycles per input sample
    const float beta = kaiser_beta(atten);
    const float inv_i0 = 1.0f / dsp_bessel_i0(beta);
    const float half = 0.5f * (float)T;
    const float tc = half - 0.5f / (float)s->P;  // centre of the sampled delay range
    for (uint32_t r = 0; r < rows; r++) {
        float *row = s->coef + (size_t)r * T;
        const float frac = (float)r / (float)s->P;
        float sum = 0.0f;
        for (uint32_t j = 0; j < T; j++) {
            const float t = (float)j + frac - tc;
            const float x = t / half;
            const float w = (x > -1.0f && x < 1.0f) ? dsp_bessel_i0(beta * sqrtf(1.0f - x * x)) * inv_i0 : 0.0f;
            const float h = two_fc * dsp_sinc(two_fc * t) * w;
            row[T - 1 - j] = h;
            sum += h;
        }
        // Every phase gets DC gain 1: no DC ripple between output samples.
        if (fabsf(sum) > 1e-6f) {
            const float inv = 1.0f / sum;
            for (uint32_t j = 0; j < T; j++) row[j] *= inv;
        }
    }
    return true;
}

static inline float dot(const float *a, const float *b, uint32_t n) {
    float acc0 = 0.0f, acc1 = 0.0f;
    uint32_t i = 0;
    for (; i + 1 < n; i += 2) {
        acc0 += a[i] * b[i];
        acc1 += a[i + 1] * b[i + 1];
    }
    if (i < n) acc0 += a[i] * b[i];
    return acc0 + acc1;
}

static uint32_t pp_run(pp_stage_t *s, uint32_t ch, uint32_t n, float *const *dest) {
    const uint32_t hist = s->hist, fill = hist + n, T = s->T, L = s->L, M = s->M;
    uint32_t cur = s->cur, phase = s->phase, cnt = 0;
    while (cur < fill) {
        if (!s->interp) {
            const float *row = s->coef + (size_t)phase * T;
            for (uint32_t c = 0; c < ch; c++) dest[c][cnt] = dot(row, s->buf[c] + (cur - hist), T);
        } else {
            // phase < L <= 768000 and P = 256: the product fits in 32 bits.
            const uint32_t pos = phase * s->P;
            const uint32_t r = pos / L;
            const float w = (float)(pos - r * L) * s->inv_L;
            const float *row0 = s->coef + (size_t)r * T;
            const float *row1 = row0 + T;
            for (uint32_t c = 0; c < ch; c++) {
                const float *x = s->buf[c] + (cur - hist);
                const float a = dot(row0, x, T), b = dot(row1, x, T);
                dest[c][cnt] = a + w * (b - a);
            }
        }
        cnt++;
        phase += M;
        cur += phase / L;
        phase %= L;
    }
    s->cur = cur - n;
    s->phase = phase;
    for (uint32_t c = 0; c < ch; c++) memmove(s->buf[c], s->buf[c] + n, hist * sizeof(float));
    return cnt;
}

// ---------------------------------------------------------------- public ----
static uint64_t chain_count(const resampler_t *r, uint64_t n, bool bound) {
    for (uint32_t s = 0; s < r->nhb; s++) n = hb_count(&r->hb[s], n, bound);
    if (r->has_pp) n = pp_count(&r->pp, n, bound);
    return n;
}

static bool alloc_planar(float **dst, uint32_t ch, uint64_t frames) {
    for (uint32_t c = 0; c < ch; c++) {
        dst[c] = core_calloc((size_t)frames, sizeof(float));
        if (!dst[c]) return false;
    }
    return true;
}

void resampler_destroy(resampler_t *r) {
    if (!r) return;
    for (uint32_t s = 0; s < RS_MAX_HB; s++) {
        core_free(r->hb[s].coef);
        for (uint32_t c = 0; c < AUDIO_MAX_CHANNELS; c++) core_free(r->hb[s].buf[c]);
    }
    core_free(r->pp.coef);
    for (uint32_t c = 0; c < AUDIO_MAX_CHANNELS; c++) {
        core_free(r->pp.buf[c]);
        core_free(r->out[c]);
    }
    core_free(r);
}

resampler_t *resampler_create(uint32_t in_rate, uint32_t out_rate, uint8_t channels, resampler_quality_t q) {
    if (in_rate < RS_RATE_MIN || in_rate > RS_RATE_MAX || out_rate < RS_RATE_MIN || out_rate > RS_RATE_MAX ||
        channels < 1 || channels > AUDIO_MAX_CHANNELS) {
        CORE_LOGE(TAG, "unsupported %lu -> %lu Hz x %u", (unsigned long)in_rate, (unsigned long)out_rate,
                  (unsigned)channels);
        return NULL;
    }
    if ((uint64_t)out_rate > (uint64_t)in_rate * 16u || (uint64_t)in_rate > (uint64_t)out_rate * 64u) {
        CORE_LOGE(TAG, "ratio %lu -> %lu Hz too large", (unsigned long)in_rate, (unsigned long)out_rate);
        return NULL;
    }
    if (q != RESAMPLER_FAST) q = RESAMPLER_GOOD;
    resampler_t *r = core_calloc(1, sizeof *r);
    if (!r) return NULL;
    r->in_rate = in_rate;
    r->out_rate = out_rate;
    r->ch = channels;
    if (in_rate == out_rate) {
        r->copy = true;
        return r;
    }
    // Passband edge of the whole conversion (20 kHz at 44.1 kHz for GOOD).
    const float fpass = q == RESAMPLER_FAST ? fminf(18000.0f, 0.40f * (float)out_rate)
                                            : fminf(20000.0f, 0.4535f * (float)out_rate);
    const float hb_atten = q == RESAMPLER_FAST ? 90.0f : 100.0f;
    uint32_t rate = in_rate;
    while (r->nhb < RS_MAX_HB && rate % 2u == 0 && rate / 2u >= out_rate) {
        if (!hb_init(&r->hb[r->nhb], (float)rate, fpass, hb_atten)) goto fail;
        r->nhb++;
        rate /= 2u;
    }
    if (rate != out_rate) {
        if (!pp_init(&r->pp, rate, out_rate, q)) goto fail;
        r->has_pp = true;
    }
    // Buffers: each stage holds its history plus the largest block it can receive.
    uint64_t max_in = RS_BLOCK;
    for (uint32_t s = 0; s < r->nhb; s++) {
        if (!alloc_planar(r->hb[s].buf, channels, r->hb[s].hist + max_in)) goto fail;
        max_in = hb_count(&r->hb[s], max_in, true);
    }
    if (r->has_pp) {
        if (!alloc_planar(r->pp.buf, channels, r->pp.hist + max_in)) goto fail;
        max_in = pp_count(&r->pp, max_in, true);
    }
    if (!alloc_planar(r->out, channels, max_in + 1)) goto fail;
    CORE_LOGD(TAG, "%lu -> %lu Hz: %lu half-band stage(s), polyphase %lu/%lu x %lu taps%s", (unsigned long)in_rate,
              (unsigned long)out_rate, (unsigned long)r->nhb, (unsigned long)r->pp.L, (unsigned long)r->pp.M,
              (unsigned long)r->pp.T, r->pp.interp ? " (interpolated)" : "");
    return r;
fail:
    CORE_LOGE(TAG, "cannot create %lu -> %lu Hz", (unsigned long)in_rate, (unsigned long)out_rate);
    resampler_destroy(r);
    return NULL;
}

void resampler_reset(resampler_t *r) {
    if (!r || r->copy) return;
    for (uint32_t s = 0; s < r->nhb; s++) {
        hb_stage_t *st = &r->hb[s];
        for (uint32_t c = 0; c < r->ch; c++) memset(st->buf[c], 0, st->hist * sizeof(float));
        st->next = st->hist;
    }
    if (r->has_pp) {
        pp_stage_t *st = &r->pp;
        for (uint32_t c = 0; c < r->ch; c++) memset(st->buf[c], 0, st->hist * sizeof(float));
        st->cur = st->hist;
        st->phase = 0;
    }
}

uint32_t resampler_max_output(const resampler_t *r, uint32_t in_frames) {
    if (!r) return 0;
    if (r->copy) return in_frames;
    // State-independent bound: valid for any call, so buffers can be sized once.
    const uint64_t n = chain_count(r, in_frames, true);
    return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}

uint32_t resampler_process(resampler_t *r, const int32_t *in, uint32_t in_frames, uint32_t *consumed, int32_t *out,
                           uint32_t out_cap) {
    if (consumed) *consumed = 0;
    if (!r || !in || in_frames == 0) return 0;
    if (!out) out_cap = 0;
    const uint32_t ch = r->ch;
    if (r->copy) {
        const uint32_t n = in_frames < out_cap ? in_frames : out_cap;
        if (n) memcpy(out, in, (size_t)n * ch * sizeof(int32_t));
        if (consumed) *consumed = n;
        return n;
    }
    // Take as much input as fits the output space (exact count for the current state).
    uint32_t n = in_frames;
    if (chain_count(r, n, false) > out_cap) {
        uint32_t lo = 0, hi = n;  // count(lo) <= cap < count(hi)
        while (hi - lo > 1) {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (chain_count(r, mid, false) <= out_cap) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        n = lo;
    }
    uint32_t done = 0, written = 0;
    while (done < n) {
        const uint32_t b = n - done < RS_BLOCK ? n - done : RS_BLOCK;
        float *const *first = r->nhb ? r->hb[0].buf : r->pp.buf;
        const uint32_t off = r->nhb ? r->hb[0].hist : r->pp.hist;
        const int32_t *src = in + (size_t)done * ch;
        for (uint32_t c = 0; c < ch; c++) {
            float *dst = first[c] + off;
            for (uint32_t i = 0; i < b; i++) dst[i] = (float)src[(size_t)i * ch + c] * DSP_Q31_INV;
        }
        uint32_t m = b;
        for (uint32_t s = 0; s < r->nhb; s++) {
            float *dest[AUDIO_MAX_CHANNELS];
            for (uint32_t c = 0; c < ch; c++) {
                if (s + 1 < r->nhb) {
                    dest[c] = r->hb[s + 1].buf[c] + r->hb[s + 1].hist;
                } else if (r->has_pp) {
                    dest[c] = r->pp.buf[c] + r->pp.hist;
                } else {
                    dest[c] = r->out[c];
                }
            }
            m = hb_run(&r->hb[s], ch, m, dest);
        }
        if (r->has_pp) m = pp_run(&r->pp, ch, m, r->out);
        if (m > out_cap - written) {  // cannot happen: the counts above are exact
            CORE_LOGE(TAG, "output overflow (%lu > %lu)", (unsigned long)m, (unsigned long)(out_cap - written));
            m = out_cap - written;
        }
        int32_t *dst = out + (size_t)written * ch;
        for (uint32_t c = 0; c < ch; c++) {
            const float *s = r->out[c];
            for (uint32_t i = 0; i < m; i++) dst[(size_t)i * ch + c] = dsp_f32_to_q31(s[i]);
        }
        written += m;
        done += b;
    }
    if (consumed) *consumed = n;
    return written;
}
