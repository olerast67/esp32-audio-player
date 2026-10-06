// SPDX-License-Identifier: Apache-2.0
// The DSP chain: ReplayGain -> EQ preamp + bands -> crossfeed -> software volume
// -> lookahead limiter -> Q31 output (TPDF dither for 16/24-bit sinks).
//
// ReplayGain, the EQ preamp and the volume are all linear gains, and so are the EQ
// and the crossfeed, so the three gains are merged into one smoothed multiplier
// applied after the filters. The result equals the documented order up to float
// rounding and costs one multiply per sample. Any change of that gain ramps over
// 10 ms, so volume steps and track-to-track ReplayGain changes never click.
//
// Latency: while the chain is active the limiter delay line always runs (about
// 1 ms), whether the limiter currently computes gain or not. Switching the
// limiter on or off therefore never shifts the audio in time. When the whole chain
// goes to bypass, dsp_process_out() plays that delayed millisecond out first, so no
// audio is lost (a gapless ReplayGain change, a volume step to 0 dB); going back to
// active holds the last bypassed frame for the same millisecond.
#include <math.h>
#include <string.h>

#include "dsp/dsp_priv.h"

#define TAG "dsp"

#define DSP_CHUNK 128                // frames per internal float block
#define DSP_RATE_MIN 1000u
#define DSP_RATE_MAX 768000u
#define LIM_MAX 128                  // lookahead window, frames (1 ms up to 128 kHz)
#define LIM_MASK (LIM_MAX - 1)
#define LIM_CEILING 0.994f           // about -0.05 dBFS: room for dither and rounding
#define LIM_RELEASE_S 0.080f
#define LIM_Q24 16777216.0f
#define RAMP_S 0.010f                // gain ramp length
#define GAIN_EPS_DB 0.1f             // potential gain above this enables the limiter
#define MUTE_DB (-120.0f)

typedef struct {
    float val;
    uint32_t idx;
} lim_dq_t;

struct dsp {
    dsp_config_t cfg;                // sanitized configuration in use
    uint32_t rate;
    uint8_t ch;
    bool configured;
    bool active;                     // false = bypass, samples untouched
    bool primed;                     // limiter delay line holds this stream's history
    bool held;                       // the delay line holds processed audio not played yet
    bool fresh;                      // no audio since reset: gain changes jump instead of ramping

    // ReplayGain
    float rg_track_db, rg_track_peak, rg_album_db, rg_album_peak;
    bool rg_album_ctx;
    bool rg_active;
    float rg_db;                     // applied gain
    float rg_out_peak_db;            // peak level after the RG stage (0 dBFS when the peak is unknown)

    // parametric EQ
    bool eq_active;
    uint8_t eq_nb;                   // realised biquads
    uint16_t eq_mask;                // preset bands realised as biquads
    uint8_t eq_idx[DSP_MAX_BANDS];   // preset band of each biquad (its state slot)
    biquad_coef_t eq_coef[DSP_MAX_BANDS];
    dsp_bq_state_t eq_state[DSP_MAX_BANDS][AUDIO_MAX_CHANNELS];
    float eq_curve_max_db;           // highest point of the band curve, without preamp

    // crossfeed: one first-order low-pass on (L - R)
    bool xf_active;
    float xf_b0, xf_a1, xf_k, xf_s;
    float xf_max_db;                 // worst-case gain for any stereo input

    // merged gain with ramp
    bool vol_active;
    float gain_target, gain_cur, gain_step;
    uint32_t ramp_left;

    // lookahead limiter
    bool lim_want;                   // gain reduction wanted by the configuration
    bool lim_run;                    // gain computation running (wanted, or releasing)
    uint32_t lim_len;                // window L; the audio is delayed by L - 1 frames
    uint32_t lim_pos;
    uint32_t lim_n;                  // frame counter for the sliding minimum
    float lim_rel_keep;
    float lim_gs;                    // release-smoothed gain
    float lim_inv;                   // 1 / (L * 2^24)
    uint32_t lim_sum;                // boxcar sum of lim_h (Q24, exact)
    uint32_t dq_head, dq_count;
    uint32_t lim_h[LIM_MAX];
    lim_dq_t lim_dq[LIM_MAX];
    float lim_delay[LIM_MAX * AUDIO_MAX_CHANNELS];

    // output
    uint8_t dither_bits;             // 16 or 24, 0 = none
    uint32_t dither_seed;
    int32_t last_frame[AUDIO_MAX_CHANNELS];

    float buf[DSP_CHUNK * AUDIO_MAX_CHANNELS];
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ----------------------------------------------------------------- config ----
void dsp_config_defaults(dsp_config_t *c) {
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->eq_enabled = false;
    c->eq = *eq_builtin_get(0);
    c->rg_mode = RG_OFF;
    c->rg_preamp_db = 0.0f;
    c->rg_fallback_db = 0.0f;
    c->rg_prevent_clip = true;
    c->crossfeed_enabled = false;
    c->crossfeed_fcut_hz = 700;
    c->crossfeed_level_db = 4.5f;
    c->sw_volume = false;
    c->volume_db = 0.0f;
    c->limiter_enabled = false;
    c->out_bits = 32;
}

static float finite_or(float v, float fallback) { return isfinite(v) ? v : fallback; }

static void sanitize_config(dsp_config_t *c) {
    if (c->eq.band_count > DSP_MAX_BANDS) c->eq.band_count = DSP_MAX_BANDS;
    c->eq.name[DSP_PRESET_NAME_MAX - 1] = 0;
    c->eq.preamp_db = clampf(finite_or(c->eq.preamp_db, 0.0f), -30.0f, 30.0f);
    if ((unsigned)c->rg_mode > (unsigned)RG_AUTO) c->rg_mode = RG_OFF;
    c->rg_preamp_db = clampf(finite_or(c->rg_preamp_db, 0.0f), -30.0f, 30.0f);
    c->rg_fallback_db = clampf(finite_or(c->rg_fallback_db, 0.0f), -30.0f, 30.0f);
    c->crossfeed_fcut_hz = (uint16_t)CORE_CLAMP(c->crossfeed_fcut_hz, 300, 2000);
    c->crossfeed_level_db = clampf(finite_or(c->crossfeed_level_db, 4.5f), 1.0f, 15.0f);
    // A broken volume must never become loud: NaN mutes.
    c->volume_db = isnan(c->volume_db) ? -INFINITY : c->volume_db;
    if (c->volume_db > 0.0f) c->volume_db = 0.0f;
}

static uint8_t dither_bits_for(uint8_t out_bits) {
    if (out_bits >= 1 && out_bits <= 16) return 16;
    if (out_bits > 16 && out_bits <= 24) return 24;
    return 0;
}

static bool eq_bands_differ(const dsp_config_t *a, const dsp_config_t *b) {
    if (a->eq_enabled != b->eq_enabled || a->eq.band_count != b->eq.band_count) return true;
    for (uint32_t i = 0; i < b->eq.band_count; i++) {
        const peq_band_t *x = &a->eq.bands[i], *y = &b->eq.bands[i];
        if (x->type != y->type || x->enabled != y->enabled || x->freq_hz != y->freq_hz || x->gain_db != y->gain_db ||
            x->q != y->q) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------- EQ ----
static float eq_curve_db_at(const dsp_t *d, float f) {
    const float s = sinf(DSP_PI * f / (float)d->rate);
    const float phi = s * s;
    float m = 1.0f;
    for (uint32_t i = 0; i < d->eq_nb; i++) m *= dsp_biquad_mag2(&d->eq_coef[i], phi);
    return 10.0f * log10f(m > 1e-30f ? m : 1e-30f);
}

// Highest point of the combined band curve: 1/12-octave grid plus DC, Nyquist and
// every band centre (peaks sit exactly at f0 with RBJ prewarping).
static float eq_curve_max(const dsp_t *d) {
    const float nyq = 0.5f * (float)d->rate;
    float mx = eq_curve_db_at(d, 0.0f);
    float v = eq_curve_db_at(d, nyq);
    if (v > mx) mx = v;
    for (float f = 10.0f; f < nyq; f *= 1.0594631f) {
        v = eq_curve_db_at(d, f);
        if (v > mx) mx = v;
    }
    for (uint32_t i = 0; i < d->eq_nb; i++) {
        const float f0 = d->cfg.eq.bands[d->eq_idx[i]].freq_hz;
        if (f0 > 0.0f && f0 < nyq) {
            v = eq_curve_db_at(d, f0);
            if (v > mx) mx = v;
        }
    }
    return mx;
}

static void eq_setup(dsp_t *d, bool clear_all) {
    const eq_preset_t *pr = &d->cfg.eq;
    uint16_t mask = 0;
    uint8_t nb = 0;
    if (d->cfg.eq_enabled) {
        for (uint32_t i = 0; i < pr->band_count; i++) {
            const peq_band_t *b = &pr->bands[i];
            if (!b->enabled) continue;
            biquad_coef_t c;
            biquad_design(&c, b->type, (float)d->rate, b->freq_hz, b->gain_db, b->q);
            if (dsp_biquad_is_identity(&c)) continue;
            // A band that was already running keeps its state (no click while editing).
            if (clear_all || !(d->eq_mask & (1u << i))) memset(d->eq_state[i], 0, sizeof d->eq_state[i]);
            d->eq_coef[nb] = c;
            d->eq_idx[nb] = (uint8_t)i;
            nb++;
            mask |= (uint16_t)(1u << i);
        }
    }
    d->eq_nb = nb;
    d->eq_mask = mask;
    d->eq_curve_max_db = nb ? eq_curve_max(d) : 0.0f;
}

// ---------------------------------------------------------------- crossfeed ----
// Bauer's stereophonic-to-binaural idea: at low frequencies each ear also hears the
// other speaker, a little later and weaker; high frequencies are shadowed by the head.
// Here the cross path is C(z) = k * LP(z), a first-order low-pass at fcut, and the
// direct path is its complement 1 - C(z) (a high shelf):
//   L' = L + C (R - L),  R' = R + C (L - R)
// For mono (L = R) the output equals the input exactly, so mono level and tone are
// preserved. At low frequencies the cross signal is level_db below the direct one,
// the low-pass adds an interaural-like delay (~1 / (2 pi fcut)), and high
// frequencies keep their full separation.
static void xf_complex_h(const dsp_t *d, float f, float *hr, float *hi) {
    const float w = DSP_TWO_PI * f / (float)d->rate;
    const float c = cosf(w), s = sinf(w);
    const float nr = d->xf_b0 * (1.0f + c), ni = -d->xf_b0 * s;
    const float dr = 1.0f + d->xf_a1 * c, di = -d->xf_a1 * s;
    const float den = dr * dr + di * di;
    *hr = (nr * dr + ni * di) / den;
    *hi = (ni * dr - nr * di) / den;
}

static void xf_setup(dsp_t *d, bool clear_state) {
    d->xf_active = d->cfg.crossfeed_enabled && d->ch == 2;
    if (!d->xf_active) {
        d->xf_s = 0.0f;
        d->xf_max_db = 0.0f;
        return;
    }
    const float fs = (float)d->rate;
    const float c = db_to_lin(-d->cfg.crossfeed_level_db);  // cross / direct at DC
    d->xf_k = c / (1.0f + c);
    const float fc = clampf((float)d->cfg.crossfeed_fcut_hz, 1.0f, 0.45f * fs);
    const float K = tanf(DSP_PI * fc / fs);
    d->xf_b0 = K / (1.0f + K);
    d->xf_a1 = (K - 1.0f) / (K + 1.0f);
    if (clear_state) d->xf_s = 0.0f;
    // Worst case over all stereo inputs: |1 - C| + |C| per frequency.
    float mx = 1.0f;
    const float nyq = 0.5f * fs;
    for (float f = 20.0f; f < nyq; f *= 1.122462f) {
        float hr, hi;
        xf_complex_h(d, f, &hr, &hi);
        const float cr = d->xf_k * hr, ci = d->xf_k * hi;
        const float g = sqrtf((1.0f - cr) * (1.0f - cr) + ci * ci) + sqrtf(cr * cr + ci * ci);
        if (g > mx) mx = g;
    }
    d->xf_max_db = lin_to_db(mx);
}

static void xf_run(dsp_t *d, float *x, uint32_t frames) {
    const float b0 = d->xf_b0, a1 = d->xf_a1, k = d->xf_k;
    float s = d->xf_s;
    for (uint32_t i = 0; i < frames; i++) {
        const float l = x[2 * i], r = x[2 * i + 1];
        const float diff = l - r;
        float y = b0 * diff + s;
        s = b0 * diff - a1 * y;
        y *= k;
        x[2 * i] = l - y;
        x[2 * i + 1] = r + y;
    }
    if (fabsf(s) < 1e-20f) s = 0.0f;
    d->xf_s = s;
}

// ---------------------------------------------------------------- limiter ----
// Lookahead peak limiter that cannot overshoot:
//   1. required gain per frame g_req = min(1, ceiling / peak);
//   2. instant attack, exponential release: g_s = min(g_req, release(g_s));
//   3. sliding minimum of g_s over L frames, then a moving average over L frames.
// Every average that reaches a frame only contains minima whose windows include that
// frame, so the gain applied to each frame (delayed by L - 1) is <= its g_req. The
// average turns gain steps into 1 ms linear ramps: no clicks, no distortion spikes.
static void limiter_idle_state(dsp_t *d) {
    d->lim_run = false;
    d->lim_gs = 1.0f;
    d->dq_head = 0;
    d->dq_count = 0;
    for (uint32_t i = 0; i < LIM_MAX; i++) d->lim_h[i] = (uint32_t)LIM_Q24;
    d->lim_sum = d->lim_len * (uint32_t)LIM_Q24;
}

static void limiter_run(dsp_t *d, float *x, uint32_t frames) {
    const uint32_t ch = d->ch, L = d->lim_len;
    uint32_t pos = d->lim_pos;
    float *dl = d->lim_delay;
    if (!d->lim_run) {
        for (uint32_t i = 0; i < frames; i++) {
            float *f = x + i * ch;
            float *slot = dl + pos * ch;
            const uint32_t nxt = pos + 1 == L ? 0 : pos + 1;
            const float *old = dl + nxt * ch;
            for (uint32_t c = 0; c < ch; c++) slot[c] = f[c];
            for (uint32_t c = 0; c < ch; c++) f[c] = old[c];
            pos = nxt;
        }
        d->lim_pos = pos;
        return;
    }
    const bool want = d->lim_want;
    const float keep = d->lim_rel_keep, inv = d->lim_inv;
    float gs = d->lim_gs;
    uint32_t sum = d->lim_sum, head = d->dq_head, count = d->dq_count, n = d->lim_n;
    lim_dq_t *dq = d->lim_dq;
    for (uint32_t i = 0; i < frames; i++, n++) {
        float *f = x + i * ch;
        float pk = fabsf(f[0]);
        if (ch == 2 && fabsf(f[1]) > pk) pk = fabsf(f[1]);
        gs = 1.0f - (1.0f - gs) * keep;
        // min(gs, ceiling / peak), dividing only when the peak actually needs less gain.
        if (want && pk * gs > LIM_CEILING) gs = LIM_CEILING / pk;
        // Sliding minimum over the last L values (monotonic deque).
        if (count && n - dq[head].idx >= L) {
            head = (head + 1) & LIM_MASK;
            count--;
        }
        while (count && dq[(head + count - 1) & LIM_MASK].val >= gs) count--;
        dq[(head + count) & LIM_MASK] = (lim_dq_t){gs, n};
        count++;
        // Moving average of the minima, exact in Q24 (floor keeps it on the safe side).
        const uint32_t hq = (uint32_t)(dq[head].val * LIM_Q24);
        sum = sum - d->lim_h[pos] + hq;
        d->lim_h[pos] = hq;
        const float g = (float)sum * inv;
        float *slot = dl + pos * ch;
        const uint32_t nxt = pos + 1 == L ? 0 : pos + 1;
        const float *old = dl + nxt * ch;
        for (uint32_t c = 0; c < ch; c++) slot[c] = f[c];
        for (uint32_t c = 0; c < ch; c++) f[c] = old[c] * g;
        pos = nxt;
    }
    d->lim_pos = pos;
    d->lim_gs = gs;
    d->lim_sum = sum;
    d->dq_head = head;
    d->dq_count = count;
    d->lim_n = n;
    // Not wanted any more and fully released: stop computing (the delay keeps running).
    if (!want && gs == 1.0f && sum == L * (uint32_t)LIM_Q24) limiter_idle_state(d);
}

// Fill the delay line with the last bypassed frame: switching from bypass to active
// then holds one value for 1 ms instead of dropping to silence (no click).
static void limiter_prime(dsp_t *d) {
    const uint32_t ch = d->ch;
    for (uint32_t i = 0; i < d->lim_len; i++) {
        for (uint32_t c = 0; c < ch; c++) d->lim_delay[i * ch + c] = (float)d->last_frame[c] * DSP_Q31_INV;
    }
    d->primed = true;
}

// ------------------------------------------------------------------- gains ----
static bool rg_gain_ok(float g) { return isfinite(g) && g > -60.0f && g < 60.0f; }
static bool rg_peak_ok(float p) { return isfinite(p) && p > 0.0f && p < 100.0f; }

static void rg_resolve(dsp_t *d) {
    d->rg_active = false;
    d->rg_db = 0.0f;
    d->rg_out_peak_db = 0.0f;
    const dsp_config_t *c = &d->cfg;
    if (c->rg_mode == RG_OFF) return;
    const bool album = c->rg_mode == RG_ALBUM || (c->rg_mode == RG_AUTO && d->rg_album_ctx);
    const float g1 = album ? d->rg_album_db : d->rg_track_db, p1 = album ? d->rg_album_peak : d->rg_track_peak;
    const float g2 = album ? d->rg_track_db : d->rg_album_db, p2 = album ? d->rg_track_peak : d->rg_album_peak;
    float g, pk;
    if (rg_gain_ok(g1)) {
        g = g1 + c->rg_preamp_db;
        pk = rg_peak_ok(p1) ? p1 : (rg_peak_ok(p2) ? p2 : NAN);
    } else if (rg_gain_ok(g2)) {
        g = g2 + c->rg_preamp_db;
        pk = rg_peak_ok(p2) ? p2 : (rg_peak_ok(p1) ? p1 : NAN);
    } else {
        g = c->rg_fallback_db;
        pk = NAN;
    }
    if (c->rg_prevent_clip) {
        // Unknown peak: assume a full-scale file, so no positive gain at all.
        const float limit = -lin_to_db(isnan(pk) ? 1.0f : pk);
        if (g > limit) g = limit;
    }
    g = clampf(g, -60.0f, 30.0f);
    if (fabsf(g) < 0.001f) return;
    d->rg_active = true;
    d->rg_db = g;
    d->rg_out_peak_db = g + (isnan(pk) ? 0.0f : lin_to_db(pk));
}

static void set_gain_target(dsp_t *d, float t) {
    if (d->fresh) {
        d->gain_target = t;
        d->gain_cur = t;
        d->ramp_left = 0;
        return;
    }
    if (t == d->gain_target) return;
    d->gain_target = t;
    uint32_t frames = (uint32_t)(RAMP_S * (float)d->rate + 0.5f);
    if (frames < 1) frames = 1;
    d->gain_step = (t - d->gain_cur) / (float)frames;
    d->ramp_left = frames;
}

static void update_active(dsp_t *d) {
    d->active = d->configured && (d->rg_active || d->eq_active || d->xf_active || d->vol_active || d->ramp_left > 0 ||
                                  d->gain_cur != 1.0f);
}

static void update_limiter(dsp_t *d) {
    // Highest level the chain can produce for a full-scale (or known-peak) input.
    float pot = d->rg_active ? d->rg_out_peak_db : 0.0f;
    if (d->eq_active) pot += d->eq_curve_max_db + d->cfg.eq.preamp_db;
    if (d->xf_active) pot += d->xf_max_db;
    d->lim_want = d->cfg.limiter_enabled || pot > GAIN_EPS_DB;
    if (d->lim_want) d->lim_run = true;  // starts from the idle state: seamless
}

static void update_gains(dsp_t *d) {
    rg_resolve(d);
    d->eq_active = d->cfg.eq_enabled && (d->eq_nb > 0 || d->cfg.eq.preamp_db != 0.0f);
    float t = 1.0f;
    if (d->rg_active) t *= db_to_lin(d->rg_db);
    if (d->eq_active && d->cfg.eq.preamp_db != 0.0f) t *= db_to_lin(d->cfg.eq.preamp_db);
    d->vol_active = d->cfg.sw_volume && d->cfg.volume_db != 0.0f;
    if (d->vol_active) t *= d->cfg.volume_db <= MUTE_DB ? 0.0f : db_to_lin(d->cfg.volume_db);
    set_gain_target(d, t);
    update_limiter(d);
    update_active(d);
}

// ----------------------------------------------------------------- state ----
static void reset_state(dsp_t *d) {
    memset(d->eq_state, 0, sizeof d->eq_state);
    d->xf_s = 0.0f;
    limiter_idle_state(d);
    d->lim_pos = 0;
    d->lim_n = 0;
    memset(d->lim_delay, 0, sizeof d->lim_delay);
    memset(d->last_frame, 0, sizeof d->last_frame);
    d->primed = true;  // zeros are the right history for a new stream
    d->held = false;
    d->fresh = true;
    d->gain_cur = d->gain_target;
    d->ramp_left = 0;
}

dsp_t *dsp_create(void) {
    // Hot state (~4.5 KB): internal RAM if available.
    dsp_t *d = core_malloc_fast(sizeof *d);
    if (!d) d = core_malloc(sizeof *d);
    if (!d) return NULL;
    memset(d, 0, sizeof *d);
    dsp_config_defaults(&d->cfg);
    d->rg_track_db = d->rg_track_peak = d->rg_album_db = d->rg_album_peak = NAN;
    d->gain_target = d->gain_cur = 1.0f;
    d->lim_len = 2;
    d->dither_seed = 0x2545F491u;
    limiter_idle_state(d);
    return d;
}

void dsp_destroy(dsp_t *d) { core_free(d); }

void dsp_configure(dsp_t *d, const dsp_config_t *cfg_in, uint32_t sample_rate, uint8_t channels) {
    if (!d || !cfg_in) return;
    if (sample_rate < DSP_RATE_MIN || sample_rate > DSP_RATE_MAX || channels < 1 || channels > AUDIO_MAX_CHANNELS) {
        CORE_LOGE(TAG, "unsupported format %lu Hz x %u, bypassing", (unsigned long)sample_rate, (unsigned)channels);
        d->configured = false;
        d->active = false;
        return;
    }
    dsp_config_t cfg = *cfg_in;
    sanitize_config(&cfg);
    const bool fmt_changed = !d->configured || sample_rate != d->rate || channels != d->ch;
    const dsp_config_t old = d->cfg;
    const bool xf_changed = old.crossfeed_enabled != cfg.crossfeed_enabled ||
                            old.crossfeed_fcut_hz != cfg.crossfeed_fcut_hz ||
                            old.crossfeed_level_db != cfg.crossfeed_level_db;
    d->cfg = cfg;
    d->rate = sample_rate;
    d->ch = channels;
    if (fmt_changed) {
        uint32_t len = (sample_rate + 500u) / 1000u;  // 1 ms lookahead
        d->lim_len = CORE_CLAMP(len, 2u, (uint32_t)LIM_MAX);
        d->lim_inv = 1.0f / ((float)d->lim_len * LIM_Q24);
        d->lim_rel_keep = expf(-1.0f / (LIM_RELEASE_S * (float)sample_rate));
        reset_state(d);
    }
    if (fmt_changed || eq_bands_differ(&old, &cfg)) eq_setup(d, fmt_changed);
    if (fmt_changed || xf_changed) xf_setup(d, fmt_changed || !old.crossfeed_enabled);
    d->dither_bits = dither_bits_for(cfg.out_bits);
    d->configured = true;
    update_gains(d);
}

void dsp_set_replaygain(dsp_t *d, float track_gain_db, float track_peak, float album_gain_db, float album_peak,
                        bool album_context) {
    if (!d) return;
    d->rg_track_db = track_gain_db;
    d->rg_track_peak = track_peak;
    d->rg_album_db = album_gain_db;
    d->rg_album_peak = album_peak;
    d->rg_album_ctx = album_context;
    if (d->configured) update_gains(d);
}

void dsp_reset(dsp_t *d) {
    if (!d) return;
    reset_state(d);
    if (d->configured) update_limiter(d);
    update_active(d);
}

bool dsp_is_bypass(const dsp_t *d) { return !d || !d->active; }

uint32_t dsp_active_flags(const dsp_t *d) {
    if (!d || !d->active) return 0;
    uint32_t f = 0;
    if (d->rg_active) f |= DSP_FLAG_RG;
    if (d->eq_active) f |= DSP_FLAG_EQ;
    if (d->xf_active) f |= DSP_FLAG_CROSSFEED;
    if (d->vol_active) f |= DSP_FLAG_SW_VOLUME;
    if (d->lim_want) f |= DSP_FLAG_LIMITER;
    if (d->dither_bits) f |= DSP_FLAG_DITHER;
    return f;
}

// ---------------------------------------------------------------- process ----
static void gain_run(dsp_t *d, float *x, uint32_t frames) {
    const uint32_t ch = d->ch;
    float g = d->gain_cur;
    uint32_t left = d->ramp_left;
    if (left == 0) {
        if (g != 1.0f) {
            for (uint32_t i = 0; i < frames * ch; i++) x[i] *= g;
        }
        return;
    }
    const float step = d->gain_step;
    for (uint32_t i = 0; i < frames; i++) {
        if (left) {
            if (--left == 0) {
                g = d->gain_target;
            } else {
                g += step;
            }
        }
        for (uint32_t c = 0; c < ch; c++) x[i * ch + c] *= g;
    }
    d->gain_cur = g;
    d->ramp_left = left;
}

// Round to an integer in [lo, hi]; NaN gives 0.
static inline int32_t quantize(float v, float lo, float hi) {
    if (!(v > lo)) return isnan(v) ? 0 : (int32_t)lo;
    if (v >= hi) return (int32_t)hi;
    return dsp_round_i32(v);
}

static void output_run(dsp_t *d, const float *x, int32_t *pcm, uint32_t samples) {
    uint32_t s = d->dither_seed;
    switch (d->dither_bits) {
    case 16:
        for (uint32_t i = 0; i < samples; i++) {
            const int32_t q = quantize(x[i] * 32768.0f + dsp_tpdf(&s), -32768.0f, 32767.0f);
            pcm[i] = (int32_t)((uint32_t)q << 16);
        }
        break;
    case 24:
        for (uint32_t i = 0; i < samples; i++) {
            const int32_t q = quantize(x[i] * 8388608.0f + dsp_tpdf(&s), -8388608.0f, 8388607.0f);
            pcm[i] = (int32_t)((uint32_t)q << 8);
        }
        break;
    default:
        for (uint32_t i = 0; i < samples; i++) pcm[i] = dsp_f32_to_q31(x[i]);
        break;
    }
    d->dither_seed = s;
}

static void run_chunk(dsp_t *d, int32_t *pcm, uint32_t frames) {
    const uint32_t ch = d->ch, samples = frames * ch;
    float *x = d->buf;
    pcm_q31_to_f32(pcm, x, samples);
    for (uint32_t b = 0; b < d->eq_nb; b++) dsp_biquad_run(&d->eq_coef[b], d->eq_state[d->eq_idx[b]], x, frames, ch);
    if (d->xf_active) xf_run(d, x, frames);
    gain_run(d, x, frames);
    limiter_run(d, x, frames);
    output_run(d, x, pcm, samples);
}

void dsp_process(dsp_t *d, int32_t *pcm, uint32_t frames) {
    if (!d || !pcm || frames == 0 || !d->configured) return;
    const uint32_t ch = d->ch;
    if (!d->active) {
        // Bit-perfect path: only remember the last frame (for a click-free switch-on). Audio
        // still delayed in the limiter cannot be played here (no room): dsp_process_out() can.
        for (uint32_t c = 0; c < ch; c++) d->last_frame[c] = pcm[(size_t)(frames - 1) * ch + c];
        d->primed = false;
        d->held = false;
        d->fresh = false;
        return;
    }
    if (!d->primed) limiter_prime(d);
    while (frames) {
        const uint32_t n = frames < DSP_CHUNK ? frames : DSP_CHUNK;
        run_chunk(d, pcm, n);
        pcm += (size_t)n * ch;
        frames -= n;
    }
    d->held = true;
    d->fresh = false;
    if (d->ramp_left == 0) update_active(d);  // a finished ramp to unity may end in bypass
}

// Plays the delay line out after the chain went to bypass: silence pushes the pending
// frames through the limiter (its gain keeps releasing) and the output stage (dither).
// Writes lim_len - 1 frames to out.
static uint32_t drain_delay(dsp_t *d, int32_t *out) {
    const uint32_t n = d->lim_len - 1, ch = d->ch;
    float *x = d->buf;  // DSP_CHUNK >= LIM_MAX frames
    memset(x, 0, (size_t)n * ch * sizeof *x);
    limiter_run(d, x, n);
    output_run(d, x, out, n * ch);
    limiter_idle_state(d);  // bypassed samples never pass the limiter
    d->held = false;
    return n;
}

uint32_t dsp_process_out(dsp_t *d, int32_t *pcm, uint32_t frames, uint32_t cap_frames) {
    if (!d || !pcm || !d->configured) return frames;
    const uint32_t ch = d->ch;
    uint32_t total = 0;
    if (!d->active && d->held) {
        // Bypass since the last call: the delayed audio goes before this chunk.
        const uint32_t n = d->lim_len - 1;
        if (cap_frames >= frames + n) {
            if (frames) memmove(pcm + (size_t)n * ch, pcm, (size_t)frames * ch * sizeof *pcm);
            total = drain_delay(d, pcm);
        }
    }
    dsp_process(d, pcm + (size_t)total * ch, frames);
    total += frames;
    if (!d->active && d->held) {
        // The chain reached bypass inside this chunk: its tail follows right after it. Without
        // room it stays held and goes before the next chunk.
        const uint32_t n = d->lim_len - 1;
        if (cap_frames >= total + n) total += drain_delay(d, pcm + (size_t)total * ch);
    }
    return total;
}
