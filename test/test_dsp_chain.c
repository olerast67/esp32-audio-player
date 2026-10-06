// SPDX-License-Identifier: Apache-2.0
// DSP chain: bypass, EQ processing, gains and ramps, limiter, crossfeed, dither.
#include <stdlib.h>

#include "audio_player/dsp.h"
#include "test.h"

#define FS 48000u
#define FULL_SCALE 2147483648.0

static uint32_t g_rng = 12345;
static int32_t rnd32(void) {
    g_rng = g_rng * 1103515245u + 12345u;
    uint32_t hi = g_rng >> 16;
    g_rng = g_rng * 1103515245u + 12345u;
    return (int32_t)((hi << 16) | (g_rng >> 16));
}

// Interleaved sine: amplitude relative to full scale, channel mask 1 = L, 2 = R, 3 = both.
static void gen_sine(int32_t *x, uint32_t frames, uint32_t ch, double f, double amp, uint32_t mask, uint64_t t0) {
    for (uint32_t i = 0; i < frames; i++) {
        const double v = amp * sin(2.0 * M_PI * f * (double)(t0 + i) / FS);
        double q = v * FULL_SCALE;
        if (q > 2147483647.0) q = 2147483647.0;
        if (q < -2147483648.0) q = -2147483648.0;
        for (uint32_t c = 0; c < ch; c++) x[i * ch + c] = (mask & (1u << c)) ? (int32_t)llround(q) : 0;
    }
}

// Amplitude of the component at frequency f (window must hold an integer number of cycles).
static double tone_amp(const int32_t *x, uint32_t frames, uint32_t ch, uint32_t c, double f) {
    double s = 0, co = 0;
    for (uint32_t i = 0; i < frames; i++) {
        const double v = x[i * ch + c] / FULL_SCALE;
        s += v * sin(2.0 * M_PI * f * i / FS);
        co += v * cos(2.0 * M_PI * f * i / FS);
    }
    return 2.0 * sqrt(s * s + co * co) / frames;
}

static double db(double lin) { return 20.0 * log10(lin); }

static dsp_config_t base_cfg(void) {
    dsp_config_t c;
    dsp_config_defaults(&c);
    return c;
}

// ------------------------------------------------------------------ bypass ----
TEST(defaults_and_bypass) {
    dsp_config_t c = base_cfg();
    CHECK(!c.eq_enabled && c.rg_mode == RG_OFF && !c.crossfeed_enabled && !c.sw_volume && !c.limiter_enabled);
    CHECK_EQ_INT(c.crossfeed_fcut_hz, 700);
    CHECK_NEAR(c.crossfeed_level_db, 4.5, 1e-6);
    CHECK(c.rg_prevent_clip);
    CHECK_STR(c.eq.name, "Flat");

    dsp_t *d = dsp_create();
    CHECK(d != NULL);
    static int32_t buf[4096 * 2], ref[4096 * 2];
    for (int i = 0; i < 4096 * 2; i++) buf[i] = ref[i] = rnd32();
    // Not configured yet: untouched.
    dsp_process(d, buf, 4096);
    CHECK(memcmp(buf, ref, sizeof buf) == 0);
    CHECK(dsp_is_bypass(d));

    // Every "neutral" configuration is bit-perfect.
    dsp_config_t variants[5];
    for (int i = 0; i < 5; i++) variants[i] = base_cfg();
    variants[1].eq_enabled = true;  // Flat preset
    variants[2].sw_volume = true;   // 0 dB
    variants[3].rg_mode = RG_TRACK;  // no tags, fallback 0 dB
    variants[4].crossfeed_enabled = true;  // mono stream: crossfeed does not apply
    variants[4].limiter_enabled = true;    // nothing can exceed full scale
    for (int v = 0; v < 5; v++) {
        dsp_configure(d, &variants[v], FS, v == 4 ? 1 : 2);
        CHECK(dsp_is_bypass(d));
        CHECK_EQ_INT(dsp_active_flags(d), 0);
        for (uint32_t k = 0; k < 8; k++) dsp_process(d, buf + k * 512, 512 / (v == 4 ? 1 : 2));
        CHECK(memcmp(buf, ref, sizeof buf) == 0);
    }
    // ReplayGain tag of exactly 0 dB is neutral too.
    dsp_config_t rg = base_cfg();
    rg.rg_mode = RG_TRACK;
    dsp_configure(d, &rg, FS, 2);
    dsp_set_replaygain(d, 0.0f, 0.5f, NAN, NAN, false);
    CHECK(dsp_is_bypass(d));
    dsp_process(d, buf, 4096);
    CHECK(memcmp(buf, ref, sizeof buf) == 0);

    // Unsupported formats bypass instead of corrupting memory.
    dsp_config_t eq = base_cfg();
    eq.eq_enabled = true;
    eq.eq.band_count = 1;
    eq.eq.bands[0] = (peq_band_t){PEQ_PEAK, 1000.0f, 6.0f, 1.0f, true};
    dsp_configure(d, &eq, FS, 3);
    CHECK(dsp_is_bypass(d));
    dsp_process(d, buf, 4096 / 2);
    dsp_configure(d, &eq, 0, 2);
    CHECK(dsp_is_bypass(d));
    dsp_process(d, buf, 4096);
    CHECK(memcmp(buf, ref, sizeof buf) == 0);
    // NULL safety.
    dsp_process(NULL, buf, 10);
    dsp_process(d, NULL, 10);
    dsp_configure(NULL, &eq, FS, 2);
    dsp_configure(d, NULL, FS, 2);
    dsp_reset(NULL);
    CHECK(dsp_is_bypass(NULL));
    CHECK_EQ_INT(dsp_active_flags(NULL), 0);
    dsp_destroy(d);
    dsp_destroy(NULL);
}

// ---------------------------------------------------------------------- EQ ----
TEST(eq_processing_gain) {
    dsp_t *d = dsp_create();
    dsp_config_t c = base_cfg();
    c.eq_enabled = true;
    c.eq.band_count = 2;
    c.eq.bands[0] = (peq_band_t){PEQ_PEAK, 1000.0f, 6.0f, 1.0f, true};
    c.eq.bands[1] = (peq_band_t){PEQ_PEAK, 8000.0f, -12.0f, 2.0f, false};  // disabled
    dsp_configure(d, &c, FS, 2);
    CHECK(!dsp_is_bypass(d));
    const uint32_t flags = dsp_active_flags(d);
    CHECK(flags & DSP_FLAG_EQ);
    CHECK(flags & DSP_FLAG_LIMITER);  // +6 dB with 0 dB preamp can exceed full scale
    CHECK(!(flags & DSP_FLAG_DITHER));

    static int32_t x[FS * 2];
    const double freqs[] = {1000.0, 100.0, 8000.0};
    double expect[3];
    biquad_coef_t bq;
    biquad_design(&bq, PEQ_PEAK, (float)FS, 1000.0f, 6.0f, 1.0f);
    for (int k = 0; k < 3; k++) expect[k] = biquad_response_db(&bq, (float)FS, (float)freqs[k]);
    CHECK_NEAR(expect[0], 6.0, 0.01);
    for (int k = 0; k < 3; k++) {
        dsp_reset(d);
        gen_sine(x, FS, 2, freqs[k], 0.1, 3, 0);
        dsp_process(d, x, FS);
        // Skip the start (filter settling, 1 ms limiter delay): analyse the last 0.5 s.
        const uint32_t half = FS / 2;
        gen_sine(x, half, 2, freqs[k], 0.1, 3, 0);  // reference in the first half of the buffer
        const double in_amp = tone_amp(x, half, 2, 0, freqs[k]);
        const double out_amp = tone_amp(x + half * 2, half, 2, 0, freqs[k]);
        CHECK_NEAR(db(out_amp / in_amp), expect[k], 0.1);
        CHECK_NEAR(db(tone_amp(x + half * 2, half, 2, 1, freqs[k]) / in_amp), expect[k], 0.1);
    }
    dsp_destroy(d);
}

TEST(preamp_and_state_kept) {
    // A preamp equal to the boost keeps the limiter off.
    dsp_t *d = dsp_create();
    dsp_config_t c = base_cfg();
    c.eq_enabled = true;
    c.eq.preamp_db = -6.0f;
    c.eq.band_count = 1;
    c.eq.bands[0] = (peq_band_t){PEQ_LOW_SHELF, 100.0f, 6.0f, 0.7f, true};
    c.sw_volume = true;
    c.volume_db = -10.0f;
    dsp_configure(d, &c, FS, 2);
    CHECK(!(dsp_active_flags(d) & DSP_FLAG_LIMITER));
    CHECK(dsp_active_flags(d) & DSP_FLAG_SW_VOLUME);

    // Run A: continuous. Run B: same, but the volume changes by -2 dB half way.
    static int32_t a[FS * 2], b[FS * 2];
    gen_sine(a, FS, 2, 60.0, 0.3, 3, 0);
    memcpy(b, a, sizeof a);
    dsp_process(d, a, FS / 2);
    dsp_process(d, a + FS, FS / 2);

    dsp_t *e = dsp_create();
    dsp_configure(e, &c, FS, 2);
    dsp_process(e, b, FS / 2);
    // Reconfiguring with identical settings changes nothing.
    dsp_configure(e, &c, FS, 2);
    dsp_config_t c2 = c;
    c2.volume_db = -12.0f;
    dsp_configure(e, &c2, FS, 2);
    dsp_process(e, b + FS, FS / 2);
    CHECK(memcmp(a, b, FS * sizeof(int32_t)) == 0);
    // After the 10 ms ramp (+1 ms delay) B is exactly A scaled by -2 dB: the filter
    // state survived the gain change (a reset would leave a transient here).
    double worst = 0;
    const double g = pow(10.0, -2.0 / 20.0);
    for (uint32_t i = FS / 2 + 600; i < FS; i++) {
        const double err = fabs(b[i * 2] - a[i * 2] * g) / FULL_SCALE;
        if (err > worst) worst = err;
    }
    CHECK(worst < 1e-6);
    dsp_destroy(d);
    dsp_destroy(e);
}

// ------------------------------------------------------------------ volume ----
TEST(volume_ramp) {
    dsp_t *d = dsp_create();
    dsp_config_t c = base_cfg();
    c.sw_volume = true;
    c.volume_db = -6.0f;
    dsp_configure(d, &c, FS, 1);
    static int32_t x[4800];
    const int32_t level = 1 << 30;  // 0.5 full scale
    for (int i = 0; i < 4800; i++) x[i] = level;
    dsp_process(d, x, 4800);
    // First configure: no ramp from unity, only the 1 ms delay line (zeros).
    CHECK_EQ_INT(x[0], 0);
    CHECK_NEAR(x[100] / FULL_SCALE, 0.5 * pow(10, -6.0 / 20), 1e-6);
    CHECK_NEAR(x[4799] / FULL_SCALE, 0.5 * pow(10, -6.0 / 20), 1e-6);

    // -6 dB -> -26 dB: smooth ramp over ~10 ms (480 frames).
    c.volume_db = -26.0f;
    dsp_configure(d, &c, FS, 1);
    for (int i = 0; i < 4800; i++) x[i] = level;
    dsp_process(d, x, 4800);
    const double g0 = 0.5 * pow(10, -6.0 / 20), g1 = 0.5 * pow(10, -26.0 / 20);
    const double max_step = (g0 - g1) / 480.0 * 1.01 + 1e-9;
    bool monotonic = true, smooth = true;
    for (int i = 1; i < 4800; i++) {
        const double diff = (x[i] - (double)x[i - 1]) / FULL_SCALE;
        if (diff > 1e-9) monotonic = false;
        if (-diff > max_step) smooth = false;
    }
    CHECK(monotonic);
    CHECK(smooth);
    CHECK_NEAR(x[20] / FULL_SCALE, g0, 1e-6);  // still the old gain (delay line)
    CHECK_NEAR(x[47 + 240] / FULL_SCALE, (g0 + g1) / 2, 0.01 * g0);  // half way through the ramp
    CHECK_NEAR(x[47 + 480 + 1] / FULL_SCALE, g1, 1e-6);
    CHECK_NEAR(x[4799] / FULL_SCALE, g1, 1e-6);

    // Back to 0 dB: the chain returns to bypass once the ramp is done.
    c.volume_db = 0.0f;
    dsp_configure(d, &c, FS, 1);
    CHECK(!dsp_is_bypass(d));  // ramp pending
    for (int i = 0; i < 4800; i++) x[i] = level;
    dsp_process(d, x, 4800);
    CHECK(dsp_is_bypass(d));
    for (int i = 0; i < 4800; i++) x[i] = level;
    dsp_process(d, x, 4800);
    CHECK_EQ_INT(x[0], level);

    // Mute and NaN volume are silent.
    c.volume_db = -200.0f;
    dsp_reset(d);
    dsp_configure(d, &c, FS, 1);
    for (int i = 0; i < 4800; i++) x[i] = level;
    dsp_process(d, x, 4800);
    CHECK_EQ_INT(x[4799], 0);
    c.volume_db = NAN;
    dsp_configure(d, &c, FS, 1);
    for (int i = 0; i < 4800; i++) x[i] = level;
    dsp_process(d, x, 4800);
    CHECK_EQ_INT(x[4799], 0);
    dsp_destroy(d);
}

TEST(bypass_to_active_no_click) {
    // Switching the chain on mid-stream holds the last sample for 1 ms instead of
    // dropping to zero: the output never jumps more than the signal itself does.
    dsp_t *d = dsp_create();
    dsp_config_t c = base_cfg();
    c.sw_volume = true;
    dsp_configure(d, &c, FS, 2);
    static int32_t x[9600 * 2];
    gen_sine(x, 9600, 2, 440.0, 0.9, 3, 0);
    dsp_process(d, x, 4800);  // bypass
    c.volume_db = -0.5f;
    dsp_configure(d, &c, FS, 2);
    dsp_process(d, x + 4800 * 2, 4800);
    const double max_slope = 0.9 * 2 * M_PI * 440.0 / FS * 1.05;
    double worst = 0;
    for (int i = 1; i < 9600; i++) {
        const double diff = fabs((double)x[i * 2] - x[(i - 1) * 2]) / FULL_SCALE;
        if (diff > worst) worst = diff;
    }
    CHECK(worst <= max_slope);
    dsp_destroy(d);
}

// Runs x (frames, stereo) through dsp_process_out in chunks of `chunk` frames with room for
// the drain; the output goes to out (room for frames + DSP_DRAIN_MAX). switch_at: after that
// many input frames `cfg2` is applied. Returns the output frames; *bypass_from receives the
// first input frame processed after the chain reported bypass.
static uint32_t run_switch(dsp_t *d, const int32_t *x, uint32_t frames, uint32_t chunk, uint32_t switch_at,
                           const dsp_config_t *cfg2, int32_t *out, uint32_t *bypass_from) {
    static int32_t work[(1024 + DSP_DRAIN_MAX) * 2];
    uint32_t n_out = 0;
    *bypass_from = UINT32_MAX;
    for (uint32_t done = 0; done < frames;) {
        if (done == switch_at) dsp_configure(d, cfg2, FS, 2);
        const uint32_t n = CORE_MIN(chunk, frames - done);
        if (*bypass_from == UINT32_MAX && dsp_is_bypass(d) && done >= switch_at) *bypass_from = done;
        memcpy(work, x + (size_t)done * 2, (size_t)n * 8);
        uint32_t got = dsp_process_out(d, work, n, n + DSP_DRAIN_MAX);
        memcpy(out + (size_t)n_out * 2, work, (size_t)got * 8);
        n_out += got;
        done += n;
    }
    n_out += dsp_process_out(d, work, 0, DSP_DRAIN_MAX);  // end of the stream
    return n_out;
}

TEST(active_to_bypass_drains_delay) {
    // The chain delays the audio by the limiter lookahead (lim_len - 1 = 47 frames at 48 kHz).
    // Going back to bypass plays that delayed audio out: nothing is lost or repeated, and
    // everything after the switch is bit-exact.
    const uint32_t N = 24000, D = 47;
    static int32_t x[24000 * 2], out[(24000 + DSP_DRAIN_MAX) * 2];
    gen_sine(x, N, 2, 997.0, 0.5, 3, 0);
    for (int variant = 0; variant < 2; variant++) {
        dsp_t *d = dsp_create();
        dsp_config_t c1 = base_cfg(), c2 = base_cfg();
        if (variant == 0) {
            // Volume -6 dB -> 0 dB: a 10 ms ramp, then bypass inside a chunk (drain after it).
            c1.sw_volume = c2.sw_volume = true;
            c1.volume_db = -6.0f;
        } else {
            // Crossfeed on a mono signal (exact identity) switched off: bypass at once, the
            // drain goes before the next chunk.
            c1.crossfeed_enabled = true;
        }
        dsp_configure(d, &c1, FS, 2);
        CHECK(!dsp_is_bypass(d));
        uint32_t bypass_from = 0;
        const uint32_t n_out = run_switch(d, x, N, 512, 5120, &c2, out, &bypass_from);
        CHECK_EQ_INT(n_out, N + D);  // the delay once, no frame dropped
        CHECK(bypass_from < N);
        for (uint32_t i = 0; i < D * 2; i++) CHECK_EQ_INT(out[i], 0);  // the delay line of a fresh stream
        const double g0 = variant == 0 ? pow(10, -6.0 / 20) : 1.0;
        uint64_t bad = 0, inexact = 0;
        for (uint32_t i = 0; i < N; i++) {
            for (uint32_t c = 0; c < 2; c++) {
                const double v = out[(D + i) * 2 + c], r = x[i * 2 + c];
                if (i >= bypass_from) {
                    inexact += v != r;
                } else {
                    // Scaled by a gain between g0 and 1: never another sample.
                    const double lo = CORE_MIN(g0 * r, r), hi = CORE_MAX(g0 * r, r);
                    bad += v < lo - 512 || v > hi + 512;
                }
            }
        }
        CHECK_EQ_INT(bad, 0);
        CHECK_EQ_INT(inexact, 0);
        // Without room the delayed audio is dropped as before (dsp_process compatibility).
        dsp_configure(d, &c1, FS, 2);
        static int32_t y[512 * 2];
        memcpy(y, x, sizeof y);
        CHECK_EQ_INT(dsp_process_out(d, y, 512, 512), 512);
        dsp_configure(d, &c2, FS, 2);
        memcpy(y, x, sizeof y);
        if (variant == 1) {
            CHECK(dsp_is_bypass(d));
            CHECK_EQ_INT(dsp_process_out(d, y, 512, 512), 512);
            CHECK(memcmp(y, x, sizeof y) == 0);
        }
        dsp_destroy(d);
    }
}

// ------------------------------------------------------------- ReplayGain ----
static double rg_gain_of(dsp_t *d) {
    static int32_t x[2048];
    for (int i = 0; i < 2048; i++) x[i] = 1 << 28;  // 0.125 full scale, mono
    dsp_reset(d);
    dsp_process(d, x, 2048);
    return db(x[2047] / (0.125 * FULL_SCALE));
}

TEST(replaygain) {
    dsp_t *d = dsp_create();
    dsp_config_t c = base_cfg();
    c.rg_mode = RG_TRACK;
    c.rg_prevent_clip = false;
    dsp_configure(d, &c, FS, 1);
    dsp_set_replaygain(d, -6.0f, 0.9f, -8.0f, 1.0f, false);
    CHECK(dsp_active_flags(d) & DSP_FLAG_RG);
    CHECK_NEAR(rg_gain_of(d), -6.0, 0.01);

    c.rg_mode = RG_ALBUM;
    dsp_configure(d, &c, FS, 1);
    CHECK_NEAR(rg_gain_of(d), -8.0, 0.01);
    c.rg_mode = RG_AUTO;  // album gain only when playing an album in order
    dsp_configure(d, &c, FS, 1);
    CHECK_NEAR(rg_gain_of(d), -6.0, 0.01);
    dsp_set_replaygain(d, -6.0f, 0.9f, -8.0f, 1.0f, true);
    CHECK_NEAR(rg_gain_of(d), -8.0, 0.01);
    // Missing album gain falls back to the track gain, missing both to the fallback.
    dsp_set_replaygain(d, -6.0f, 0.9f, NAN, NAN, true);
    CHECK_NEAR(rg_gain_of(d), -6.0, 0.01);
    c.rg_fallback_db = -3.0f;
    dsp_configure(d, &c, FS, 1);
    dsp_set_replaygain(d, NAN, NAN, NAN, NAN, false);
    CHECK_NEAR(rg_gain_of(d), -3.0, 0.01);
    // Preamp applies to tagged files only.
    c.rg_preamp_db = 2.0f;
    dsp_configure(d, &c, FS, 1);
    dsp_set_replaygain(d, -6.0f, 0.5f, NAN, NAN, false);
    CHECK_NEAR(rg_gain_of(d), -4.0, 0.01);

    // Clip prevention: +10 dB on a file peaking at 0.9 is limited to +0.915 dB.
    c.rg_mode = RG_TRACK;
    c.rg_preamp_db = 0.0f;
    c.rg_prevent_clip = true;
    dsp_configure(d, &c, FS, 1);
    dsp_set_replaygain(d, 10.0f, 0.9f, NAN, NAN, false);
    CHECK_NEAR(rg_gain_of(d), -20.0 * log10(0.9), 0.01);
    CHECK(!(dsp_active_flags(d) & DSP_FLAG_LIMITER));  // cannot exceed full scale
    // Unknown peak: no positive gain (bypass once the 10 ms ramp to unity is done).
    dsp_set_replaygain(d, 10.0f, NAN, NAN, NAN, false);
    CHECK(!(dsp_active_flags(d) & DSP_FLAG_RG));
    CHECK_NEAR(rg_gain_of(d), 0.0, 0.001);
    CHECK(dsp_is_bypass(d));
    // Without clip prevention a positive gain turns the limiter on.
    c.rg_prevent_clip = false;
    dsp_configure(d, &c, FS, 1);
    dsp_set_replaygain(d, 5.0f, 0.9f, NAN, NAN, false);
    CHECK(dsp_active_flags(d) & DSP_FLAG_LIMITER);
    // Garbage tags are ignored (and the fallback is 0 dB here).
    c.rg_fallback_db = 0.0f;
    dsp_configure(d, &c, FS, 1);
    dsp_set_replaygain(d, INFINITY, -1.0f, 1e9f, NAN, false);
    CHECK(!(dsp_active_flags(d) & DSP_FLAG_RG));
    dsp_reset(d);
    CHECK(dsp_is_bypass(d));
    dsp_destroy(d);
}

// ----------------------------------------------------------------- limiter ----
TEST(limiter_never_exceeds_full_scale) {
    dsp_t *d = dsp_create();
    dsp_config_t c = base_cfg();
    c.eq_enabled = true;
    c.eq.preamp_db = 12.0f;  // +12 dB on a full-scale sine
    c.eq.band_count = 0;
    dsp_configure(d, &c, FS, 2);
    CHECK(dsp_active_flags(d) & DSP_FLAG_LIMITER);
    static int32_t x[FS * 2];
    // 100 ms of silence, then a full-scale burst: the lookahead must catch the onset.
    memset(x, 0, sizeof x);
    gen_sine(x + 4800 * 2, FS - 4800, 2, 1000.0, 1.0, 3, 0);
    for (uint32_t i = 0; i < FS * 2; i++) {
        if (x[i] == INT32_MIN) x[i] = INT32_MIN + 1;
    }
    dsp_process(d, x, FS);
    int64_t peak = 0;
    uint32_t sat = 0;
    for (uint32_t i = 0; i < FS * 2; i++) {
        const int64_t v = llabs((int64_t)x[i]);
        if (v > peak) peak = v;
        if (x[i] == INT32_MAX || x[i] == INT32_MIN) sat++;
    }
    CHECK_EQ_INT(sat, 0);                             // never reached the saturation stage
    CHECK(peak / FULL_SCALE <= 0.9945);               // at or below the -0.05 dBFS ceiling
    // Steady state: the sine comes out close to the ceiling, not squashed.
    const double amp = tone_amp(x + (FS / 2) * 2, FS / 2, 2, 0, 1000.0);
    CHECK(db(amp) > -0.6 && db(amp) <= 0.0);

    // Same with random full-scale noise and a +12 dB bass boost.
    c.eq.preamp_db = 0.0f;
    c.eq.band_count = 1;
    c.eq.bands[0] = (peq_band_t){PEQ_LOW_SHELF, 200.0f, 12.0f, 0.7f, true};
    dsp_configure(d, &c, FS, 2);
    for (uint32_t i = 0; i < FS * 2; i++) x[i] = rnd32() | 1;
    dsp_process(d, x, FS);
    peak = 0;
    sat = 0;
    for (uint32_t i = 0; i < FS * 2; i++) {
        const int64_t v = llabs((int64_t)x[i]);
        if (v > peak) peak = v;
        if (x[i] == INT32_MAX || x[i] == INT32_MIN) sat++;
    }
    CHECK_EQ_INT(sat, 0);
    CHECK(peak / FULL_SCALE <= 0.9945);

    // limiter_enabled forces it on whenever the chain runs.
    dsp_config_t v = base_cfg();
    v.sw_volume = true;
    v.volume_db = -3.0f;
    v.limiter_enabled = true;
    dsp_configure(d, &v, FS, 2);
    CHECK(dsp_active_flags(d) & DSP_FLAG_LIMITER);
    v.limiter_enabled = false;
    dsp_configure(d, &v, FS, 2);
    CHECK(!(dsp_active_flags(d) & DSP_FLAG_LIMITER));
    dsp_destroy(d);
}

// --------------------------------------------------------------- crossfeed ----
TEST(crossfeed) {
    dsp_t *d = dsp_create();
    dsp_config_t c = base_cfg();
    c.crossfeed_enabled = true;
    dsp_configure(d, &c, FS, 2);
    CHECK(dsp_active_flags(d) & DSP_FLAG_CROSSFEED);
    static int32_t x[FS * 2], ref[FS * 2];
    const uint32_t half = FS / 2;

    // Mono stays mono at the same level.
    const double mono_f[] = {100.0, 700.0, 5000.0};
    for (int k = 0; k < 3; k++) {
        dsp_reset(d);
        gen_sine(x, FS, 2, mono_f[k], 0.5, 3, 0);
        memcpy(ref, x, sizeof x);
        dsp_process(d, x, FS);
        const double in = tone_amp(ref, half, 2, 0, mono_f[k]);
        CHECK_NEAR(db(tone_amp(x + half * 2, half, 2, 0, mono_f[k]) / in), 0.0, 0.5);
        CHECK_NEAR(db(tone_amp(x + half * 2, half, 2, 1, mono_f[k]) / in), 0.0, 0.5);
    }
    // Hard-panned low frequency: the other channel now gets it, ~4.5 dB lower.
    dsp_reset(d);
    gen_sine(x, FS, 2, 100.0, 0.5, 1, 0);
    memcpy(ref, x, sizeof x);
    dsp_process(d, x, FS);
    const double in = tone_amp(ref, half, 2, 0, 100.0);
    const double l = tone_amp(x + half * 2, half, 2, 0, 100.0), r = tone_amp(x + half * 2, half, 2, 1, 100.0);
    CHECK(r > 0.0);
    CHECK(db(l / r) >= 4.5 && db(l / r) < 5.2);
    CHECK(db(l / in) < -3.0);  // direct level drops at low frequencies
    // The sum L + R still equals the input (mono compatibility).
    static int32_t sum[FS];
    for (uint32_t i = 0; i < FS; i++) sum[i] = (int32_t)(((int64_t)x[2 * i] + x[2 * i + 1]));
    CHECK_NEAR(db(tone_amp(sum + half, half, 1, 0, 100.0) / in), 0.0, 0.05);
    // High frequencies keep their separation.
    dsp_reset(d);
    gen_sine(x, FS, 2, 10000.0, 0.5, 1, 0);
    dsp_process(d, x, FS);
    const double lh = tone_amp(x + half * 2, half, 2, 0, 10000.0), rh = tone_amp(x + half * 2, half, 2, 1, 10000.0);
    CHECK(db(rh / lh) < -20.0);
    CHECK_NEAR(db(lh / in), 0.0, 0.5);

    // A stronger setting feeds more.
    c.crossfeed_level_db = 2.0f;
    c.crossfeed_fcut_hz = 1000;
    dsp_configure(d, &c, FS, 2);
    dsp_reset(d);
    gen_sine(x, FS, 2, 200.0, 0.5, 1, 0);
    dsp_process(d, x, FS);
    CHECK(db(tone_amp(x + half * 2, half, 2, 0, 200.0) / tone_amp(x + half * 2, half, 2, 1, 200.0)) < 3.0);
    dsp_destroy(d);
}

// ------------------------------------------------------------------ dither ----
TEST(dither_statistics) {
    enum { N = 400000 };
    static int32_t in[N];
    static int16_t out[N];
    uint32_t seed = 1;
    // Silence: output is -1/0/+1 LSB, mean 0, variance 1/4 LSB^2 (TPDF 1/6 + rounding 1/12).
    memset(in, 0, sizeof in);
    pcm_q31_to_s16_dither(in, out, N, &seed);
    double mean = 0, var = 0;
    int outside = 0;
    for (int i = 0; i < N; i++) {
        mean += out[i];
        var += (double)out[i] * out[i];
        if (out[i] < -1 || out[i] > 1) outside++;
    }
    mean /= N;
    var = var / N - mean * mean;
    CHECK_EQ_INT(outside, 0);
    CHECK_NEAR(mean, 0.0, 0.005);
    CHECK_NEAR(var, 0.25, 0.01);
    CHECK(seed != 1);

    // 0.3 LSB DC is preserved on average (dither linearises the quantiser).
    for (int i = 0; i < N; i++) in[i] = (int32_t)(0.3 * 65536);
    pcm_q31_to_s16_dither(in, out, N, &seed);
    mean = 0;
    var = 0;
    for (int i = 0; i < N; i++) {
        mean += out[i];
        var += (out[i] - 0.3) * (out[i] - 0.3);
    }
    mean /= N;
    var /= N;
    CHECK_NEAR(mean, 0.3, 0.005);
    CHECK_NEAR(var, 0.25, 0.01);

    // Saturation: never wraps around at the rails.
    int32_t ext[2] = {INT32_MAX, INT32_MIN};
    int16_t o[2];
    for (int k = 0; k < 100; k++) {
        pcm_q31_to_s16_dither(ext, o, 2, &seed);
        CHECK(o[0] >= 32766 && o[1] <= -32767);
    }
    // Deterministic for the same seed.
    int16_t o1[64], o2[64];
    uint32_t s1 = 7, s2 = 7;
    for (int i = 0; i < 64; i++) in[i] = rnd32();
    pcm_q31_to_s16_dither(in, o1, 64, &s1);
    pcm_q31_to_s16_dither(in, o2, 64, &s2);
    CHECK(memcmp(o1, o2, sizeof o1) == 0);
    CHECK(s1 == s2 && s1 != 7);
    pcm_q31_to_s16_dither(in, o2, 64, NULL);  // no state is fine
}

TEST(chain_dither) {
    dsp_t *d = dsp_create();
    dsp_config_t c = base_cfg();
    c.sw_volume = true;
    c.volume_db = -1.0f;
    c.out_bits = 16;
    dsp_configure(d, &c, FS, 2);
    CHECK(dsp_active_flags(d) & DSP_FLAG_DITHER);
    static int32_t x[FS * 2];
    gen_sine(x, FS, 2, 1000.0, 0.01, 3, 0);
    dsp_process(d, x, FS);
    int low_bits = 0;
    for (uint32_t i = 0; i < FS * 2; i++) low_bits |= x[i] & 0xFFFF;
    CHECK_EQ_INT(low_bits, 0);  // quantised to 16 bits: the sink can truncate safely
    // The tone survives with the right level, the error is dither-sized noise.
    const double amp = tone_amp(x + FS, FS / 2, 2, 0, 1000.0);
    CHECK_NEAR(db(amp / 0.01), -1.0, 0.05);

    c.out_bits = 24;
    dsp_configure(d, &c, FS, 2);
    gen_sine(x, FS, 2, 1000.0, 0.01, 3, 0);
    dsp_process(d, x, FS);
    low_bits = 0;
    for (uint32_t i = 0; i < FS * 2; i++) low_bits |= x[i] & 0xFF;
    CHECK_EQ_INT(low_bits, 0);

    c.out_bits = 32;
    dsp_configure(d, &c, FS, 2);
    CHECK(!(dsp_active_flags(d) & DSP_FLAG_DITHER));
    // Bypass never dithers.
    c.volume_db = 0.0f;
    c.out_bits = 16;
    dsp_configure(d, &c, FS, 2);
    dsp_process(d, x, 1);  // finishes nothing: ramp to unity still pending
    static int32_t y[4800 * 2];
    gen_sine(y, 4800, 2, 1000.0, 0.01, 3, 0);
    dsp_process(d, y, 4800);
    CHECK(dsp_is_bypass(d));
    CHECK_EQ_INT(dsp_active_flags(d), 0);
    dsp_destroy(d);
}

// ------------------------------------------------------------ conversions ----
TEST(conversions) {
    const float f[] = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.5f, -2.0f, NAN, INFINITY, -INFINITY, 1e-12f};
    int32_t q[11];
    pcm_f32_to_q31(f, q, 11);
    CHECK_EQ_INT(q[0], 0);
    CHECK_EQ_INT(q[1], 1 << 30);
    CHECK_EQ_INT(q[2], -(1 << 30));
    CHECK_EQ_INT(q[3], INT32_MAX);
    CHECK_EQ_INT(q[4], INT32_MIN);
    CHECK_EQ_INT(q[5], INT32_MAX);
    CHECK_EQ_INT(q[6], INT32_MIN);
    CHECK_EQ_INT(q[7], 0);
    CHECK_EQ_INT(q[8], INT32_MAX);
    CHECK_EQ_INT(q[9], INT32_MIN);
    CHECK_EQ_INT(q[10], 0);
    float back[3];
    const int32_t in[3] = {1 << 30, INT32_MIN, 0};
    pcm_q31_to_f32(in, back, 3);
    CHECK(back[0] == 0.5f && back[1] == -1.0f && back[2] == 0.0f);

    int32_t st[8] = {1, 2, 3, 4};
    pcm_mono_to_stereo(st, st, 4);  // in place
    const int32_t expect[8] = {1, 1, 2, 2, 3, 3, 4, 4};
    CHECK(memcmp(st, expect, sizeof st) == 0);
    int32_t mono[3] = {7, -8, 9}, stereo[6];
    pcm_mono_to_stereo(mono, stereo, 3);
    CHECK(stereo[0] == 7 && stereo[1] == 7 && stereo[4] == 9 && stereo[5] == 9);
}

TEST(reset_matches_fresh) {
    dsp_config_t c = base_cfg();
    c.eq_enabled = true;
    c.eq.preamp_db = -3.0f;
    c.eq.band_count = 2;
    c.eq.bands[0] = (peq_band_t){PEQ_PEAK, 300.0f, 4.0f, 1.0f, true};
    c.eq.bands[1] = (peq_band_t){PEQ_HIGH_PASS, 30.0f, 0.0f, 0.7f, true};
    c.crossfeed_enabled = true;
    c.out_bits = 32;
    dsp_t *a = dsp_create(), *b = dsp_create();
    dsp_configure(a, &c, 44100, 2);
    dsp_configure(b, &c, 44100, 2);
    static int32_t x[8192 * 2], y[8192 * 2];
    for (int i = 0; i < 8192 * 2; i++) x[i] = rnd32() >> 2;
    dsp_process(a, x, 8192);  // dirty the state
    for (int i = 0; i < 8192 * 2; i++) x[i] = y[i] = rnd32() >> 2;
    dsp_reset(a);
    dsp_process(a, x, 8192);
    dsp_process(b, y, 8192);
    CHECK(memcmp(x, y, sizeof x) == 0);
    // Chunking does not change the result.
    dsp_reset(a);
    dsp_reset(b);
    for (int i = 0; i < 8192 * 2; i++) x[i] = y[i] = rnd32() >> 2;
    dsp_process(a, x, 8192);
    for (uint32_t done = 0, n = 1; done < 8192; done += n, n = n * 3 % 1000 + 1) {
        if (n > 8192 - done) n = 8192 - done;
        dsp_process(b, y + done * 2, n);
    }
    CHECK(memcmp(x, y, sizeof x) == 0);
    dsp_destroy(a);
    dsp_destroy(b);
}

TEST_MAIN(RUN(defaults_and_bypass) RUN(eq_processing_gain) RUN(preamp_and_state_kept) RUN(volume_ramp)
              RUN(bypass_to_active_no_click) RUN(active_to_bypass_drains_delay) RUN(replaygain) RUN(limiter_never_exceeds_full_scale) RUN(crossfeed)
              RUN(dither_statistics) RUN(chain_dither) RUN(conversions) RUN(reset_matches_fresh))
