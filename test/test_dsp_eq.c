// SPDX-License-Identifier: Apache-2.0
// Biquad designs, AutoEQ import/export and built-in presets.
#include <stdlib.h>

#include "audio_player/dsp.h"
#include "test.h"

#define FS 48000.0f

// ------------------------------------------------------------------ biquads ----
TEST(peak_filter) {
    biquad_coef_t c;
    biquad_design(&c, PEQ_PEAK, FS, 1000.0f, 6.0f, 1.0f);
    CHECK_NEAR(biquad_response_db(&c, FS, 1000.0f), 6.0, 0.1);
    CHECK_NEAR(biquad_response_db(&c, FS, 20.0f), 0.0, 0.1);
    CHECK_NEAR(biquad_response_db(&c, FS, 20000.0f), 0.0, 0.2);
    // The peak is the maximum of the curve.
    CHECK(biquad_response_db(&c, FS, 900.0f) < 6.0f);
    CHECK(biquad_response_db(&c, FS, 1100.0f) < 6.0f);

    biquad_design(&c, PEQ_PEAK, FS, 3000.0f, -9.0f, 2.5f);
    CHECK_NEAR(biquad_response_db(&c, FS, 3000.0f), -9.0, 0.1);
    CHECK_NEAR(biquad_response_db(&c, FS, 300.0f), 0.0, 0.1);

    // At 44.1 kHz, close to Nyquist, prewarping still puts the peak at f0.
    biquad_design(&c, PEQ_PEAK, 44100.0f, 16000.0f, 4.0f, 2.0f);
    CHECK_NEAR(biquad_response_db(&c, 44100.0f, 16000.0f), 4.0, 0.1);

    // 0 dB peak is an exact pass-through.
    biquad_design(&c, PEQ_PEAK, FS, 1000.0f, 0.0f, 1.0f);
    CHECK(c.b0 == 1.0f && c.b1 == 0.0f && c.b2 == 0.0f && c.a1 == 0.0f && c.a2 == 0.0f);
}

TEST(shelf_filters) {
    biquad_coef_t c;
    // AutoEQ style low shelf: Q 0.7 at 105 Hz.
    biquad_design(&c, PEQ_LOW_SHELF, FS, 105.0f, 6.0f, 0.7f);
    CHECK_NEAR(biquad_response_db(&c, FS, 5.0f), 6.0, 0.1);
    CHECK_NEAR(biquad_response_db(&c, FS, 0.0f), 6.0, 0.02);
    CHECK_NEAR(biquad_response_db(&c, FS, 105.0f), 3.0, 0.1);  // half the gain at f0
    CHECK_NEAR(biquad_response_db(&c, FS, 5000.0f), 0.0, 0.05);

    biquad_design(&c, PEQ_LOW_SHELF, FS, 200.0f, -8.0f, 0.707f);
    CHECK_NEAR(biquad_response_db(&c, FS, 10.0f), -8.0, 0.1);
    CHECK_NEAR(biquad_response_db(&c, FS, 10000.0f), 0.0, 0.05);

    biquad_design(&c, PEQ_HIGH_SHELF, FS, 1000.0f, 6.0f, 0.7f);
    CHECK_NEAR(biquad_response_db(&c, FS, 24000.0f), 6.0, 0.02);
    CHECK_NEAR(biquad_response_db(&c, FS, 20000.0f), 6.0, 0.1);
    CHECK_NEAR(biquad_response_db(&c, FS, 1000.0f), 3.0, 0.1);
    CHECK_NEAR(biquad_response_db(&c, FS, 20.0f), 0.0, 0.05);

    biquad_design(&c, PEQ_HIGH_SHELF, 44100.0f, 10000.0f, -4.0f, 0.7f);
    CHECK_NEAR(biquad_response_db(&c, 44100.0f, 22050.0f), -4.0, 0.05);
    CHECK_NEAR(biquad_response_db(&c, 44100.0f, 100.0f), 0.0, 0.05);
}

TEST(pass_filters) {
    biquad_coef_t c;
    const float q = 0.70710678f;
    biquad_design(&c, PEQ_LOW_PASS, FS, 1000.0f, 0.0f, q);
    CHECK_NEAR(biquad_response_db(&c, FS, 1000.0f), -3.01, 0.05);
    CHECK_NEAR(biquad_response_db(&c, FS, 50.0f), 0.0, 0.05);
    CHECK(biquad_response_db(&c, FS, 10000.0f) < -35.0f);
    CHECK(biquad_response_db(&c, FS, 24000.0f) < -100.0f);  // zero at Nyquist

    biquad_design(&c, PEQ_HIGH_PASS, FS, 80.0f, 0.0f, q);
    CHECK_NEAR(biquad_response_db(&c, FS, 80.0f), -3.01, 0.05);
    CHECK_NEAR(biquad_response_db(&c, FS, 5000.0f), 0.0, 0.05);
    CHECK(biquad_response_db(&c, FS, 10.0f) < -30.0f);

    // Gain is ignored by pass filters.
    biquad_coef_t c2;
    biquad_design(&c2, PEQ_HIGH_PASS, FS, 80.0f, 12.0f, q);
    CHECK_NEAR(biquad_response_db(&c2, FS, 5000.0f), 0.0, 0.05);
}

TEST(design_edge_cases) {
    biquad_coef_t c;
    // Above Nyquist: peak/high shelf/low-pass do nothing, low shelf is a flat gain.
    biquad_design(&c, PEQ_PEAK, 44100.0f, 30000.0f, 6.0f, 1.0f);
    CHECK_NEAR(biquad_response_db(&c, 44100.0f, 15000.0f), 0.0, 1e-4);
    biquad_design(&c, PEQ_HIGH_SHELF, 44100.0f, 30000.0f, 6.0f, 1.0f);
    CHECK_NEAR(biquad_response_db(&c, 44100.0f, 15000.0f), 0.0, 1e-4);
    biquad_design(&c, PEQ_LOW_SHELF, 44100.0f, 30000.0f, -3.0f, 0.7f);
    CHECK_NEAR(biquad_response_db(&c, 44100.0f, 100.0f), -3.0, 1e-3);
    CHECK_NEAR(biquad_response_db(&c, 44100.0f, 20000.0f), -3.0, 1e-3);
    // Garbage in: identity, never NaN.
    biquad_design(&c, PEQ_PEAK, FS, NAN, 6.0f, 1.0f);
    CHECK_NEAR(biquad_response_db(&c, FS, 1000.0f), 0.0, 1e-6);
    biquad_design(&c, PEQ_PEAK, 0.0f, 1000.0f, 6.0f, 1.0f);
    CHECK_NEAR(biquad_response_db(&c, FS, 1000.0f), 0.0, 1e-6);
    biquad_design(&c, PEQ_PEAK, FS, 1000.0f, 6.0f, -1.0f);  // bad Q -> 0.707
    CHECK_NEAR(biquad_response_db(&c, FS, 1000.0f), 6.0, 0.1);
    biquad_design(&c, PEQ_PEAK, FS, 1000.0f, INFINITY, 1.0f);
    CHECK(isfinite(biquad_response_db(&c, FS, 1000.0f)));
    // Stability: poles inside the unit circle for extreme but valid parameters.
    const float qs[] = {0.05f, 0.7f, 30.0f};
    const float fs[] = {20.0f, 1000.0f, 20000.0f};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            for (int t = 0; t < 5; t++) {
                biquad_design(&c, (peq_type_t)t, FS, fs[j], 12.0f, qs[i]);
                CHECK(fabsf(c.a2) < 1.0f && fabsf(c.a1) < 1.0f + c.a2);
            }
        }
    }
    CHECK_NEAR(db_to_lin(6.0f), 1.99526, 1e-4);
    CHECK_NEAR(db_to_lin(-20.0f), 0.1, 1e-6);
    CHECK_NEAR(lin_to_db(0.5f), -6.0206, 1e-3);
    CHECK_NEAR(lin_to_db(0.0f), -200.0, 1e-6);
    CHECK_NEAR(lin_to_db(-1.0f), -200.0, 1e-6);
}

// ------------------------------------------------------------------ AutoEQ ----
// Sennheiser HD 600 style file as AutoEQ writes it (values are illustrative).
static const char k_hd600[] =
    "Preamp: -6.2 dB\n"
    "Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.70\n"
    "Filter 2: ON PK Fc 206 Hz Gain -3.3 dB Q 0.46\n"
    "Filter 3: ON PK Fc 1432 Hz Gain 2.1 dB Q 1.46\n"
    "Filter 4: ON PK Fc 2878 Hz Gain -2.4 dB Q 2.15\n"
    "Filter 5: ON PK Fc 4296 Hz Gain 3.6 dB Q 3.66\n"
    "Filter 6: ON PK Fc 5761 Hz Gain -2.0 dB Q 4.84\n"
    "Filter 7: ON PK Fc 7362 Hz Gain 2.1 dB Q 4.07\n"
    "Filter 8: ON PK Fc 9152 Hz Gain -1.5 dB Q 2.64\n"
    "Filter 9: ON PK Fc 11000 Hz Gain 1.2 dB Q 1.83\n"
    "Filter 10: ON HSC Fc 10000 Hz Gain 1.1 dB Q 0.70\n";

static bool band_eq(const peq_band_t *a, const peq_band_t *b) {
    return a->type == b->type && a->freq_hz == b->freq_hz && a->gain_db == b->gain_db && a->q == b->q &&
           a->enabled == b->enabled;
}

TEST(autoeq_parse_hd600) {
    eq_preset_t p;
    CHECK_EQ_INT(eq_preset_parse_autoeq(k_hd600, &p), CORE_OK);
    CHECK_EQ_INT(p.band_count, 10);
    CHECK(p.preamp_db == -6.2f);
    CHECK_STR(p.name, "");
    CHECK_EQ_INT(p.bands[0].type, PEQ_LOW_SHELF);
    CHECK(p.bands[0].freq_hz == 105.0f && p.bands[0].gain_db == 5.5f && p.bands[0].q == 0.70f);
    CHECK(p.bands[0].enabled);
    CHECK_EQ_INT(p.bands[4].type, PEQ_PEAK);
    CHECK(p.bands[4].freq_hz == 4296.0f && p.bands[4].gain_db == 3.6f && p.bands[4].q == 3.66f);
    CHECK_EQ_INT(p.bands[9].type, PEQ_HIGH_SHELF);
    CHECK(p.bands[9].freq_hz == 10000.0f && p.bands[9].gain_db == 1.1f);
}

TEST(autoeq_round_trip) {
    eq_preset_t p, p2;
    CHECK_EQ_INT(eq_preset_parse_autoeq(k_hd600, &p), CORE_OK);
    char text[1024];
    const int n = eq_preset_format_autoeq(&p, text, sizeof text);
    CHECK(n > 0);
    CHECK_EQ_INT(n, (int)strlen(text));
    // Same text as AutoEQ writes.
    CHECK_STR(text, k_hd600);
    CHECK_EQ_INT(eq_preset_parse_autoeq(text, &p2), CORE_OK);
    CHECK_EQ_INT(p2.band_count, p.band_count);
    CHECK(p2.preamp_db == p.preamp_db);
    for (int i = 0; i < p.band_count; i++) CHECK(band_eq(&p.bands[i], &p2.bands[i]));

    // Non-AutoEQ values (pass filters, OFF bands, fractional frequencies) round-trip too.
    eq_preset_t q = {"x", -1.25f, 3, {{PEQ_HIGH_PASS, 25.5f, 0.0f, 0.707f, true},
                                      {PEQ_PEAK, 1234.0f, -0.5f, 1.41f, false},
                                      {PEQ_LOW_PASS, 18000.0f, 0.0f, 0.5f, true}}};
    CHECK(eq_preset_format_autoeq(&q, text, sizeof text) > 0);
    CHECK_EQ_INT(eq_preset_parse_autoeq(text, &p2), CORE_OK);
    CHECK_EQ_INT(p2.band_count, 3);
    CHECK(p2.preamp_db == -1.25f);
    for (int i = 0; i < 3; i++) CHECK(band_eq(&q.bands[i], &p2.bands[i]));

    // Too small a buffer fails cleanly.
    char small[40];
    CHECK(eq_preset_format_autoeq(&p, small, sizeof small) < 0);
    CHECK_STR(small, "");
}

TEST(autoeq_tolerant_parsing) {
    eq_preset_t p;
    // BOM, CRLF, tabs, extra spaces, comments, lower case, no filter number, other types.
    const char *txt =
        "\xEF\xBB\xBF# HD 600 tuned\r\n"
        "\r\n"
        "Preamp:   -3,5   dB\r\n"
        "  filter 1:  on   pk   fc 1000 hz   gain 3 db   q 1\r\n"
        "Filter:\tON\tLS\tFc\t80 Hz\tGain\t4.0 dB\r\n"
        "Filter 3: OFF HS Fc 8 kHz Gain -2 dB Q 0.7\r\n"
        "Filter 4: ON LP Fc 20000 Hz\r\n"
        "Filter 5: ON HPQ Fc 20 Hz Q 0.5\r\n"
        "Filter 6: ON PK Fc 500 Hz Gain -1 dB BW Oct 1.0\r\n"
        "Filter 7: ON LS 12dB Fc 100 Hz Gain 2 dB\r\n"
        "GraphicEQ: 20 0; 30 1\r\n";
    CHECK_EQ_INT(eq_preset_parse_autoeq(txt, &p), CORE_OK);
    CHECK(p.preamp_db == -3.5f);
    CHECK_EQ_INT(p.band_count, 7);
    CHECK(p.bands[0].type == PEQ_PEAK && p.bands[0].freq_hz == 1000.0f && p.bands[0].gain_db == 3.0f &&
          p.bands[0].q == 1.0f);
    CHECK(p.bands[1].type == PEQ_LOW_SHELF && p.bands[1].gain_db == 4.0f);
    CHECK_NEAR(p.bands[1].q, 0.7071, 1e-3);  // default Q
    CHECK(p.bands[2].type == PEQ_HIGH_SHELF && !p.bands[2].enabled && p.bands[2].freq_hz == 8000.0f);
    CHECK(p.bands[3].type == PEQ_LOW_PASS && p.bands[3].freq_hz == 20000.0f);
    CHECK(p.bands[4].type == PEQ_HIGH_PASS && p.bands[4].q == 0.5f);
    CHECK_NEAR(p.bands[5].q, 1.4142, 1e-3);  // 1 octave
    CHECK(p.bands[6].type == PEQ_LOW_SHELF && p.bands[6].freq_hz == 100.0f && p.bands[6].gain_db == 2.0f);

    // Bad filter lines are skipped, the rest is kept; values are clamped.
    const char *mixed =
        "Filter 1: ON PK Gain 3 dB Q 1\n"          // no Fc
        "Filter 2: ON XX Fc 100 Hz Gain 3 dB\n"    // unknown type
        "Filter 3: MAYBE PK Fc 100 Hz\n"           // bad state
        "Filter 4: ON PK Fc 100 Hz Gain 99 dB Q 0\n"  // Q 0 is invalid
        "Filter 5: ON PK Fc 100 Hz Gain 99 dB Q 100\n";
    CHECK_EQ_INT(eq_preset_parse_autoeq(mixed, &p), CORE_OK);
    CHECK_EQ_INT(p.band_count, 1);
    CHECK(p.bands[0].gain_db == 30.0f && p.bands[0].q == 30.0f);

    // More than 10 filters: the extra ones are dropped.
    char many[2048] = "Preamp: -1 dB\n";
    for (int i = 1; i <= 13; i++) {
        char line[96];
        snprintf(line, sizeof line, "Filter %d: ON PK Fc %d Hz Gain 1.0 dB Q 1.00\n", i, 100 * i);
        strcat(many, line);
    }
    CHECK_EQ_INT(eq_preset_parse_autoeq(many, &p), CORE_OK);
    CHECK_EQ_INT(p.band_count, DSP_MAX_BANDS);
    CHECK(p.bands[9].freq_hz == 1000.0f);

    // Nothing usable.
    CHECK_EQ_INT(eq_preset_parse_autoeq("", &p), CORE_ECORRUPT);
    CHECK_EQ_INT(eq_preset_parse_autoeq("hello\nworld\n", &p), CORE_ECORRUPT);
    CHECK_EQ_INT(eq_preset_parse_autoeq("Filter 1: ON PK\n", &p), CORE_ECORRUPT);
    CHECK_EQ_INT(eq_preset_parse_autoeq(NULL, &p), CORE_EINVAL);
    CHECK_EQ_INT(eq_preset_parse_autoeq("Preamp: 0 dB", NULL), CORE_EINVAL);
    CHECK_EQ_INT(eq_preset_parse_autoeq("Preamp: 0 dB", &p), CORE_OK);  // preamp only is valid

    // Random bytes never crash and never produce out-of-range values.
    srand(1234);
    char junk[512];
    for (int round = 0; round < 2000; round++) {
        const int n = rand() % (int)(sizeof junk - 1);
        for (int i = 0; i < n; i++) {
            const char alphabet[] = "Filter 1:ON OFF PK LSC HSC Fc Hz Gain dB Q 0123456789.-e\n\r\tPreamp";
            junk[i] = (rand() % 4) ? alphabet[rand() % (int)(sizeof alphabet - 1)] : (char)(1 + rand() % 255);
        }
        junk[n] = 0;
        if (eq_preset_parse_autoeq(junk, &p) == CORE_OK) {
            CHECK(p.band_count <= DSP_MAX_BANDS);
            for (int i = 0; i < p.band_count; i++) {
                if (!(isfinite(p.bands[i].freq_hz) && p.bands[i].q > 0.0f && fabsf(p.bands[i].gain_db) <= 30.0f)) {
                    CHECK(!"band out of range");
                    break;
                }
            }
            CHECK(fabsf(p.preamp_db) <= 30.0f);
        }
    }
}

// ----------------------------------------------------------------- presets ----
static float preset_max_db(const eq_preset_t *p, float fs) {
    float mx = -1000.0f;
    for (float f = 10.0f; f < fs / 2; f *= 1.02f) {
        float db = p->preamp_db;
        for (int i = 0; i < p->band_count; i++) {
            biquad_coef_t c;
            biquad_design(&c, p->bands[i].type, fs, p->bands[i].freq_hz, p->bands[i].gain_db, p->bands[i].q);
            db += biquad_response_db(&c, fs, f);
        }
        if (db > mx) mx = db;
    }
    return mx;
}

TEST(builtin_presets) {
    static const char *names[] = {"Flat", "Bass", "Treble", "Vocal", "Loudness"};
    CHECK_EQ_INT(eq_builtin_count(), 5);
    for (uint32_t i = 0; i < eq_builtin_count(); i++) {
        const eq_preset_t *p = eq_builtin_get(i);
        CHECK(p != NULL);
        if (!p) continue;
        CHECK_STR(p->name, names[i]);
        CHECK(p->band_count <= DSP_MAX_BANDS);
        // Preamp covers the boost: the presets never clip and never need the limiter.
        CHECK(preset_max_db(p, 44100.0f) <= 0.05f);
        CHECK(preset_max_db(p, 48000.0f) <= 0.05f);
        char text[1024];
        eq_preset_t back;
        CHECK(eq_preset_format_autoeq(p, text, sizeof text) > 0);
        CHECK_EQ_INT(eq_preset_parse_autoeq(text, &back), CORE_OK);
        CHECK_EQ_INT(back.band_count, p->band_count);
    }
    const eq_preset_t *flat = eq_builtin_get(0);
    CHECK(flat && flat->band_count == 0 && flat->preamp_db == 0.0f);
    CHECK(eq_builtin_get(5) == NULL);
    CHECK(eq_builtin_get(UINT32_MAX) == NULL);
    // Non-flat presets actually change the sound.
    CHECK(preset_max_db(eq_builtin_get(1), 48000.0f) > -0.5f);
}

TEST_MAIN(RUN(peak_filter) RUN(shelf_filters) RUN(pass_filters) RUN(design_edge_cases) RUN(autoeq_parse_hd600)
              RUN(autoeq_round_trip) RUN(autoeq_tolerant_parsing) RUN(builtin_presets))
