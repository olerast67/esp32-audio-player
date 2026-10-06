// SPDX-License-Identifier: Apache-2.0
// AutoEQ "ParametricEQ.txt" import/export and the built-in presets.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dsp/dsp_priv.h"
#include "dsp/dsp_text.h"

#define TAG "eq"
#define EQ_LINE_MAX 256

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool starts_with_ci(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++) {
        if (lower(*s) != *prefix) return false;
    }
    return true;
}

int eq_preset_parse_autoeq(const char *text, eq_preset_t *out) {
    if (!text || !out) return CORE_EINVAL;
    memset(out, 0, sizeof *out);
    bool have_preamp = false;
    float preamp = 0.0f;
    uint32_t dropped = 0, bad = 0;
    const char *p = text;
    // UTF-8 byte order mark (files saved by Windows editors).
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;

    while (*p) {
        // Copy one line (without CR/LF) into a bounded buffer; overlong lines are cut.
        char line[EQ_LINE_MAX];
        size_t n = 0;
        while (*p && *p != '\n' && *p != '\r') {
            if (n + 1 < sizeof line) line[n++] = *p;
            p++;
        }
        while (*p == '\n' || *p == '\r') p++;
        line[n] = 0;

        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (starts_with_ci(s, "preamp")) {
            const char *v = strchr(s, ':');
            float db;
            if (v && (v++, dsp_text_parse_float(&v, &db))) {
                preamp += db;  // Equalizer APO applies every Preamp line, so they add up
                have_preamp = true;
            } else {
                bad++;
                CORE_LOGW(TAG, "bad preamp line: %.40s", s);
            }
        } else if (starts_with_ci(s, "filter")) {
            peq_band_t b;
            if (peq_band_parse(s, &b) != CORE_OK) {
                bad++;
                CORE_LOGW(TAG, "bad filter line: %.60s", s);
            } else if (out->band_count < DSP_MAX_BANDS) {
                out->bands[out->band_count++] = b;
            } else {
                dropped++;
            }
        }
        // Anything else (comments, "GraphicEQ:", "Device:", blank lines) is ignored.
    }
    if (dropped) CORE_LOGW(TAG, "%lu filters beyond %d dropped", (unsigned long)dropped, DSP_MAX_BANDS);
    if (!have_preamp && out->band_count == 0 && dropped == 0) {
        memset(out, 0, sizeof *out);
        return CORE_ECORRUPT;
    }
    if (!isfinite(preamp)) preamp = 0.0f;
    if (preamp < PEQ_PREAMP_MIN) preamp = PEQ_PREAMP_MIN;
    if (preamp > PEQ_PREAMP_MAX) preamp = PEQ_PREAMP_MAX;
    out->preamp_db = preamp;
    (void)bad;
    return CORE_OK;
}

int eq_preset_format_autoeq(const eq_preset_t *pr, char *out, size_t out_size) {
    if (!pr || !out || out_size == 0) return CORE_EINVAL;
    out[0] = 0;
    char num[24];
    if (dsp_text_format_float(num, sizeof num, pr->preamp_db, 1, 2) < 0) return CORE_EINVAL;
    size_t len = 0;
    int n = snprintf(out, out_size, "Preamp: %s dB\n", num);
    if (n < 0 || (size_t)n >= out_size) goto too_small;
    len = (size_t)n;
    const uint32_t count = pr->band_count <= DSP_MAX_BANDS ? pr->band_count : DSP_MAX_BANDS;
    for (uint32_t i = 0; i < count; i++) {
        char band[96];
        if (peq_band_format(&pr->bands[i], band, sizeof band) < 0) goto too_small;
        n = snprintf(out + len, out_size - len, "Filter %lu: %s\n", (unsigned long)(i + 1), band);
        if (n < 0 || (size_t)n >= out_size - len) goto too_small;
        len += (size_t)n;
    }
    return (int)len;
too_small:
    out[0] = 0;
    return CORE_EINVAL;
}

// ----------------------------------------------------------------- presets ----
// Every preamp is at least the largest boost of its curve, so the presets never
// need the limiter (checked by the tests).
static const eq_preset_t k_builtin[] = {
    {"Flat", 0.0f, 0, {{0}}},
    {"Bass",
     -6.0f,
     1,
     {
         {PEQ_LOW_SHELF, 105.0f, 6.0f, 0.70f, true},
     }},
    {"Treble",
     -5.0f,
     1,
     {
         {PEQ_HIGH_SHELF, 6000.0f, 5.0f, 0.70f, true},
     }},
    {"Vocal",
     -3.5f,
     4,
     {
         {PEQ_LOW_SHELF, 120.0f, -2.0f, 0.70f, true},
         {PEQ_PEAK, 300.0f, -1.5f, 1.00f, true},
         {PEQ_PEAK, 2500.0f, 3.0f, 1.00f, true},
         {PEQ_HIGH_SHELF, 10000.0f, -1.0f, 0.70f, true},
     }},
    {"Loudness",
     -6.5f,
     3,
     {
         {PEQ_LOW_SHELF, 90.0f, 6.0f, 0.70f, true},
         {PEQ_PEAK, 2800.0f, -1.5f, 1.20f, true},
         {PEQ_HIGH_SHELF, 9000.0f, 4.0f, 0.70f, true},
     }},
};

uint32_t eq_builtin_count(void) { return (uint32_t)CORE_ARRAY_SIZE(k_builtin); }

const eq_preset_t *eq_builtin_get(uint32_t index) {
    return index < CORE_ARRAY_SIZE(k_builtin) ? &k_builtin[index] : NULL;
}
