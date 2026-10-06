// SPDX-License-Identifier: Apache-2.0
// Resampler: frequency accuracy, THD+N, aliasing rejection, exact frame counts.
#include <stdlib.h>

#include "audio_player/resampler.h"
#include "test.h"

#define FULL_SCALE 2147483648.0

static uint32_t g_rng = 99;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

static int32_t *gen_sine(uint32_t frames, uint32_t ch, double fs, double f, double amp) {
    int32_t *x = malloc((size_t)frames * ch * sizeof(int32_t));
    for (uint32_t i = 0; i < frames; i++) {
        const int32_t v = (int32_t)llround(amp * sin(2.0 * M_PI * f * i / fs) * FULL_SCALE);
        for (uint32_t c = 0; c < ch; c++) x[(size_t)i * ch + c] = c ? -v : v;  // R inverted: channels stay apart
    }
    return x;
}

// Convert all input in one go with a generous output buffer; returns output frames.
static uint32_t run_all(resampler_t *r, const int32_t *in, uint32_t in_frames, int32_t **out, uint32_t ch) {
    const uint32_t cap = resampler_max_output(r, in_frames) + 16;
    *out = malloc((size_t)cap * ch * sizeof(int32_t));
    uint32_t consumed = 0;
    const uint32_t n = resampler_process(r, in, in_frames, &consumed, *out, cap);
    CHECK_EQ_INT(consumed, in_frames);
    CHECK(n <= resampler_max_output(r, in_frames) + 1);
    return n;
}

typedef struct {
    double amp, thdn_db;
    double freq;      // from the phase drift across the window (precise)
    double freq_zc;   // from rising zero crossings (coarse cross-check)
} tone_fit_t;

// Phase of the component at f over [start, start + len), referenced to index 0.
static double phase_at(const int32_t *x, uint32_t start, uint32_t len, uint32_t ch, uint32_t c, double fs, double f) {
    double re = 0, im = 0;
    for (uint32_t i = start; i < start + len; i++) {
        const double v = x[(size_t)i * ch + c];
        re += v * cos(2 * M_PI * f * i / fs);
        im -= v * sin(2 * M_PI * f * i / fs);
    }
    return atan2(im, re);
}

// Least-squares fit of a sine at frequency f over a window with an integer number of
// cycles; THD+N = residual RMS relative to the tone RMS. The frequency comes from the
// phase drift between the first and the last quarter of the window (Goertzel style),
// cross-checked by counting zero crossings.
static tone_fit_t analyse(const int32_t *x, uint32_t frames, uint32_t ch, uint32_t c, double fs, double f) {
    double s = 0, co = 0, dc = 0;
    for (uint32_t i = 0; i < frames; i++) {
        const double v = x[(size_t)i * ch + c] / FULL_SCALE;
        s += v * sin(2 * M_PI * f * i / fs);
        co += v * cos(2 * M_PI * f * i / fs);
        dc += v;
    }
    const double a = 2 * s / frames, b = 2 * co / frames;
    dc /= frames;
    double res = 0;
    for (uint32_t i = 0; i < frames; i++) {
        const double v = x[(size_t)i * ch + c] / FULL_SCALE;
        const double e = v - (a * sin(2 * M_PI * f * i / fs) + b * cos(2 * M_PI * f * i / fs) + dc);
        res += e * e;
    }
    tone_fit_t t;
    t.amp = sqrt(a * a + b * b);
    t.thdn_db = 10 * log10((res / frames) / (t.amp * t.amp / 2) + 1e-30);
    // Rising zero crossings with linear interpolation.
    double first = -1, last = -1;
    int count = 0;
    for (uint32_t i = 1; i < frames; i++) {
        const double p = x[(size_t)(i - 1) * ch + c], q = x[(size_t)i * ch + c];
        if (p < 0 && q >= 0) {
            const double t0 = (i - 1) + (-p) / (q - p);
            if (first < 0) first = t0;
            last = t0;
            count++;
        }
    }
    t.freq_zc = count > 1 ? (count - 1) * fs / (last - first) : 0;
    const uint32_t q = frames / 4;
    double dphi = phase_at(x, 3 * q, q, ch, c, fs, f) - phase_at(x, 0, q, ch, c, fs, f);
    while (dphi > M_PI) dphi -= 2 * M_PI;
    while (dphi < -M_PI) dphi += 2 * M_PI;
    t.freq = f + dphi / (2 * M_PI * (3.0 * q / fs));
    return t;
}

// Tone level at the output (any frequency) relative to amplitude `ref`: RMS based.
static double rms_db(const int32_t *x, uint32_t frames, uint32_t ch, double ref) {
    double acc = 0;
    for (uint32_t i = 0; i < frames * ch; i++) {
        const double v = x[i] / FULL_SCALE;
        acc += v * v;
    }
    return 10 * log10(acc / (frames * ch) / (ref * ref / 2) + 1e-30);
}

static void check_tone(uint32_t in_rate, uint32_t out_rate, resampler_quality_t q, double f, double max_thdn_db) {
    const uint32_t ch = 2;
    resampler_t *r = resampler_create(in_rate, out_rate, ch, q);
    CHECK(r != NULL);
    if (!r) return;
    const uint32_t in_frames = in_rate * 2;  // 2 s
    int32_t *in = gen_sine(in_frames, ch, in_rate, f, 0.5), *out;
    const uint32_t n = run_all(r, in, in_frames, &out, ch);
    CHECK(n + 1 >= out_rate * 2 && n <= out_rate * 2 + 1);
    // Analyse exactly 1 s after the first half second (filter start-up).
    const uint32_t start = out_rate / 2;
    for (uint32_t c = 0; c < ch; c++) {
        const tone_fit_t t = analyse(out + (size_t)start * ch, out_rate, ch, c, out_rate, f);
        CHECK_NEAR(20 * log10(t.amp / 0.5), 0.0, 0.1);
        CHECK_NEAR(t.freq, f, 0.001);
        CHECK_NEAR(t.freq_zc, f, f * 1e-5);
        if (c == 0) {
            printf("  %6lu -> %6lu %s %5.0f Hz: THD+N %6.1f dB (limit %.0f), f error %.1e Hz\n",
                   (unsigned long)in_rate, (unsigned long)out_rate, q == RESAMPLER_FAST ? "FAST" : "GOOD", f,
                   t.thdn_db, max_thdn_db, t.freq - f);
        }
        CHECK(t.thdn_db < max_thdn_db);
    }
    free(in);
    free(out);
    resampler_destroy(r);
}

TEST(tone_48k_to_44k1) {
    check_tone(48000, 44100, RESAMPLER_GOOD, 1000.0, -80.0);
    check_tone(48000, 44100, RESAMPLER_GOOD, 10000.0, -80.0);
    check_tone(48000, 44100, RESAMPLER_FAST, 1000.0, -65.0);
}

TEST(tone_other_ratios) {
    check_tone(44100, 48000, RESAMPLER_GOOD, 1000.0, -80.0);
    check_tone(96000, 44100, RESAMPLER_GOOD, 1000.0, -80.0);  // half-band + polyphase
    check_tone(96000, 48000, RESAMPLER_GOOD, 1000.0, -90.0);  // one half-band stage
    check_tone(176400, 44100, RESAMPLER_GOOD, 1000.0, -90.0); // two half-band stages
    check_tone(192000, 48000, RESAMPLER_FAST, 1000.0, -85.0);
    check_tone(88200, 48000, RESAMPLER_GOOD, 1000.0, -80.0);
    check_tone(32000, 44100, RESAMPLER_GOOD, 1000.0, -80.0);
    check_tone(8000, 48000, RESAMPLER_GOOD, 1000.0, -80.0);   // 6x upsampling
    check_tone(11025, 44100, RESAMPLER_GOOD, 1000.0, -80.0);  // 4x upsampling
    check_tone(44100, 47999, RESAMPLER_GOOD, 1000.0, -75.0);  // odd ratio: interpolated phases
}

// A tone above the output Nyquist must disappear (aliasing rejection).
static void check_reject(uint32_t in_rate, uint32_t out_rate, double f, double min_atten_db) {
    const uint32_t ch = 2;
    resampler_t *r = resampler_create(in_rate, out_rate, ch, RESAMPLER_GOOD);
    CHECK(r != NULL);
    if (!r) return;
    const uint32_t in_frames = in_rate;  // 1 s
    int32_t *in = gen_sine(in_frames, ch, in_rate, f, 0.5), *out;
    const uint32_t n = run_all(r, in, in_frames, &out, ch);
    const uint32_t start = out_rate / 10;
    const double level = rms_db(out + (size_t)start * ch, n - start, ch, 0.5);
    printf("  %6lu -> %6lu: %5.0f Hz tone at %6.1f dB (limit %.0f)\n", (unsigned long)in_rate,
           (unsigned long)out_rate, f, level, -min_atten_db);
    CHECK(level < -min_atten_db);
    free(in);
    free(out);
    resampler_destroy(r);
}

TEST(aliasing_rejection) {
    check_reject(96000, 48000, 30000.0, 80.0);
    check_reject(96000, 48000, 40000.0, 80.0);
    check_reject(96000, 48000, 47000.0, 80.0);
    check_reject(176400, 44100, 30000.0, 80.0);
    check_reject(176400, 44100, 60000.0, 80.0);
    check_reject(176400, 44100, 80000.0, 80.0);
    check_reject(96000, 44100, 30000.0, 80.0);
    check_reject(192000, 44100, 50000.0, 80.0);
    // 48k -> 44.1k: the transition band is centred on 22.05 kHz, so 22.05..24 kHz is only
    // partly attenuated (it folds into 20.1..22.05 kHz, above the passband).
    check_reject(48000, 44100, 23900.0, 20.0);
}

TEST(passband_flatness) {
    // 20 Hz .. 16 kHz within 0.2 dB for the common cases.
    const uint32_t pairs[][2] = {{48000, 44100}, {96000, 48000}, {176400, 44100}, {44100, 48000}};
    const double freqs[] = {20.0, 1000.0, 10000.0, 16000.0};
    for (size_t p = 0; p < 4; p++) {
        for (size_t k = 0; k < 4; k++) {
            resampler_t *r = resampler_create(pairs[p][0], pairs[p][1], 1, RESAMPLER_GOOD);
            const uint32_t in_frames = pairs[p][0];
            int32_t *in = gen_sine(in_frames, 1, pairs[p][0], freqs[k], 0.5), *out;
            const uint32_t n = run_all(r, in, in_frames, &out, 1);
            const uint32_t start = pairs[p][1] / 10;
            const double level = rms_db(out + start, n - start, 1, 0.5);
            CHECK_NEAR(level, 0.0, freqs[k] < 100 ? 0.3 : 0.2);
            free(in);
            free(out);
            resampler_destroy(r);
        }
    }
}

// ------------------------------------------------------------ frame counts ----
static void check_long_run(uint32_t in_rate, uint32_t out_rate, uint32_t ch) {
    resampler_t *r = resampler_create(in_rate, out_rate, ch, RESAMPLER_GOOD);
    resampler_t *ref = resampler_create(in_rate, out_rate, ch, RESAMPLER_GOOD);
    CHECK(r && ref);
    if (!r || !ref) return;
    enum { BLOCK = 4096 };
    static int32_t in[BLOCK * 2], out_a[BLOCK * 8 * 2], out_b[BLOCK * 8 * 2];
    uint64_t total_in = 0, total_out = 0, ref_out = 0;
    bool same = true, within_cap = true, within_bound = true;
    for (int round = 0; round < 800; round++) {
        for (uint32_t i = 0; i < BLOCK * ch; i++) in[i] = (int32_t)(rnd() << 8) >> 2;
        // Reference: whole block at once.
        uint32_t consumed = 0;
        const uint32_t na = resampler_process(ref, in, BLOCK, &consumed, out_a, BLOCK * 8);
        if (consumed != BLOCK) same = false;
        ref_out += na;
        // Under test: random input chunks and random (sometimes tiny) output space.
        uint32_t pos = 0, nb = 0;
        while (pos < BLOCK) {
            uint32_t want = 1 + rnd() % 700;
            if (want > BLOCK - pos) want = BLOCK - pos;
            uint32_t cap = rnd() % 5 == 0 ? rnd() % 3 : 1 + rnd() % 900;
            if (cap > BLOCK * 8 - nb) cap = BLOCK * 8 - nb;
            const uint32_t bound = resampler_max_output(r, want);
            const uint32_t got = resampler_process(r, in + pos * ch, want, &consumed, out_b + (size_t)nb * ch, cap);
            if (got > cap) within_cap = false;
            if (got > bound) within_bound = false;
            if (consumed > want) within_cap = false;
            pos += consumed;
            nb += got;
        }
        if (nb != na || memcmp(out_a, out_b, (size_t)na * ch * sizeof(int32_t)) != 0) same = false;
        total_in += BLOCK;
        total_out += nb;
    }
    CHECK(same);  // chunking never changes a single sample
    CHECK(within_cap);
    CHECK(within_bound);
    CHECK_EQ_INT(total_out, ref_out);
    // No drift: the output count follows in * out / in exactly (rounding only).
    const double ideal = (double)total_in * out_rate / in_rate;
    CHECK(fabs((double)total_out - ideal) <= 1.0);
    resampler_destroy(r);
    resampler_destroy(ref);
}

TEST(long_run_counts) {
    check_long_run(48000, 44100, 2);
    check_long_run(44100, 48000, 2);
    check_long_run(96000, 44100, 2);
    check_long_run(176400, 44100, 1);
    check_long_run(96000, 48000, 2);
    check_long_run(44100, 47999, 1);
    // Exact formula for a single polyphase stage: ceil(n * L / M).
    resampler_t *r = resampler_create(48000, 44100, 1, RESAMPLER_FAST);
    static int32_t in[48000], out[48000];
    uint32_t consumed;
    CHECK_EQ_INT(resampler_process(r, in, 48000, &consumed, out, 48000), 44100);
    CHECK_EQ_INT(resampler_process(r, in, 1, &consumed, out, 48000), 1);
    CHECK_EQ_INT(resampler_max_output(r, 160), 147);
    resampler_destroy(r);
}

// ------------------------------------------------------------ edge cases ----
TEST(create_and_edges) {
    CHECK(resampler_create(48000, 44100, 0, RESAMPLER_GOOD) == NULL);
    CHECK(resampler_create(48000, 44100, 3, RESAMPLER_GOOD) == NULL);
    CHECK(resampler_create(0, 44100, 2, RESAMPLER_GOOD) == NULL);
    CHECK(resampler_create(48000, 0, 2, RESAMPLER_GOOD) == NULL);
    CHECK(resampler_create(2000000, 44100, 2, RESAMPLER_GOOD) == NULL);
    CHECK(resampler_create(1000, 48000, 2, RESAMPLER_GOOD) == NULL);  // 48x upsampling
    resampler_destroy(NULL);
    resampler_reset(NULL);
    CHECK_EQ_INT(resampler_max_output(NULL, 100), 0);
    uint32_t consumed = 5;
    CHECK_EQ_INT(resampler_process(NULL, NULL, 10, &consumed, NULL, 10), 0);
    CHECK_EQ_INT(consumed, 0);

    // Same rate: bit-exact copy limited by out_cap.
    resampler_t *r = resampler_create(44100, 44100, 2, RESAMPLER_GOOD);
    CHECK(r != NULL);
    int32_t in[64], out[64];
    for (int i = 0; i < 64; i++) in[i] = (int32_t)(rnd() << 8);
    CHECK_EQ_INT(resampler_process(r, in, 32, &consumed, out, 20), 20);
    CHECK_EQ_INT(consumed, 20);
    CHECK(memcmp(in, out, 40 * sizeof(int32_t)) == 0);
    CHECK_EQ_INT(resampler_max_output(r, 32), 32);
    resampler_destroy(r);

    // Zero output space consumes only what produces no output.
    r = resampler_create(96000, 48000, 1, RESAMPLER_GOOD);
    CHECK_EQ_INT(resampler_process(r, in, 64, &consumed, out, 0), 0);
    CHECK(consumed <= 1);
    CHECK_EQ_INT(resampler_process(r, in, 64, &consumed, NULL, 64), 0);

    // Reset gives the same output as a fresh instance; full-scale input never wraps.
    resampler_t *fresh = resampler_create(96000, 48000, 1, RESAMPLER_GOOD);
    static int32_t big[4096], o1[4096], o2[4096];
    for (int i = 0; i < 4096; i++) big[i] = (i / 8) % 2 ? INT32_MAX : INT32_MIN;  // square wave overshoots
    resampler_process(r, big, 4096, &consumed, o1, 4096);
    resampler_reset(r);
    const uint32_t n1 = resampler_process(r, big, 4096, &consumed, o1, 4096);
    const uint32_t n2 = resampler_process(fresh, big, 4096, &consumed, o2, 4096);
    CHECK_EQ_INT(n1, n2);
    CHECK(memcmp(o1, o2, n1 * sizeof(int32_t)) == 0);
    // The filtered square wave overshoots and saturates; a wrap-around would show as a
    // sample of the opposite sign between two large neighbours.
    uint32_t wrapped = 0, saturated = 0;
    for (uint32_t i = 1; i + 1 < n1; i++) {
        if (o1[i] == INT32_MAX || o1[i] == INT32_MIN) saturated++;
        if (o1[i - 1] > (1 << 30) && o1[i + 1] > (1 << 30) && o1[i] < 0) wrapped++;
        if (o1[i - 1] < -(1 << 30) && o1[i + 1] < -(1 << 30) && o1[i] > 0) wrapped++;
    }
    CHECK(saturated > 0);
    CHECK_EQ_INT(wrapped, 0);
    resampler_destroy(r);
    resampler_destroy(fresh);
}

TEST_MAIN(RUN(tone_48k_to_44k1) RUN(tone_other_ratios) RUN(aliasing_rejection) RUN(passband_flatness)
              RUN(long_run_counts) RUN(create_and_edges))
