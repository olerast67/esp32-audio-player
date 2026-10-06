// SPDX-License-Identifier: Apache-2.0
// Number and filter text helpers (AutoEQ / Equalizer APO syntax).
#include "dsp/dsp_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static const char *skip_space(const char *p) {
    while (is_space(*p)) p++;
    return p;
}

// Length of the word at p (letters only), so "Hz," or "dB" units are recognised.
static size_t word_len(const char *p) {
    size_t n = 0;
    while ((p[n] >= 'a' && p[n] <= 'z') || (p[n] >= 'A' && p[n] <= 'Z')) n++;
    return n;
}

static bool word_eq(const char *p, size_t n, const char *w) {
    size_t m = strlen(w);
    if (m != n) return false;
    for (size_t i = 0; i < n; i++) {
        if (lower(p[i]) != lower(w[i])) return false;
    }
    return true;
}

bool dsp_text_parse_float(const char **pp, float *out) {
    static const float k_pow10[] = {1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f};
    if (!pp || !*pp || !out) return false;
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    bool neg = false;
    if (*p == '+' || *p == '-') {
        neg = *p == '-';
        p++;
    }
    uint32_t mant = 0;
    int exp10 = 0;
    bool any = false;
    while (is_digit(*p)) {
        any = true;
        if (mant < 100000000u) {
            mant = mant * 10u + (uint32_t)(*p - '0');
        } else if (exp10 < 100) {
            exp10++;  // digits beyond float precision only scale the value
        }
        p++;
    }
    if ((*p == '.' || *p == ',') && is_digit(p[1])) {
        p++;
        while (is_digit(*p)) {
            any = true;
            if (mant < 100000000u) {
                mant = mant * 10u + (uint32_t)(*p - '0');
                exp10--;
            }
            p++;
        }
    } else if (*p == '.' && any) {
        p++;  // "5." is a valid number
    }
    if (!any) return false;
    if ((*p == 'e' || *p == 'E') && (is_digit(p[1]) || ((p[1] == '+' || p[1] == '-') && is_digit(p[2])))) {
        p++;
        bool eneg = false;
        if (*p == '+' || *p == '-') {
            eneg = *p == '-';
            p++;
        }
        int e = 0;
        while (is_digit(*p)) {
            if (e < 1000) e = e * 10 + (*p - '0');
            p++;
        }
        exp10 += eneg ? -e : e;
    }
    float v = (float)mant;
    if (mant != 0) {
        if (exp10 > 38) return false;  // overflow
        if (exp10 < -60) {
            v = 0.0f;
        } else {
            // mant and 10^k are exact floats for k <= 10, so one step rounds correctly.
            while (exp10 > 0) {
                int k = exp10 > 10 ? 10 : exp10;
                v *= k_pow10[k];
                exp10 -= k;
            }
            while (exp10 < 0) {
                int k = -exp10 > 10 ? 10 : -exp10;
                v /= k_pow10[k];
                exp10 += k;
            }
        }
    }
    if (!isfinite(v)) return false;
    *out = neg ? -v : v;
    *pp = p;
    return true;
}

int dsp_text_format_float(char *out, size_t size, float v, int min_dec, int max_dec) {
    static const uint32_t k_pow10u[] = {1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u};
    if (!out || size == 0) return CORE_EINVAL;
    out[0] = 0;
    if (max_dec < 0) max_dec = 0;
    if (max_dec > 6) max_dec = 6;
    if (min_dec < 0) min_dec = 0;
    if (min_dec > max_dec) min_dec = max_dec;
    if (!isfinite(v)) v = 0.0f;
    bool neg = v < 0.0f;
    float a = fabsf(v);
    const float limit = 4.0e9f / (float)k_pow10u[max_dec];
    if (a > limit) a = limit;
    uint32_t scaled = (uint32_t)(a * (float)k_pow10u[max_dec] + 0.5f);
    uint32_t ip = scaled / k_pow10u[max_dec];
    uint32_t fp = scaled % k_pow10u[max_dec];
    int dec = max_dec;
    while (dec > min_dec && fp % 10u == 0u) {
        fp /= 10u;
        dec--;
    }
    if (scaled == 0) neg = false;
    int n;
    if (dec > 0) {
        n = snprintf(out, size, "%s%lu.%0*lu", neg ? "-" : "", (unsigned long)ip, dec, (unsigned long)fp);
    } else {
        n = snprintf(out, size, "%s%lu", neg ? "-" : "", (unsigned long)ip);
    }
    if (n < 0 || (size_t)n >= size) {
        out[0] = 0;
        return CORE_EINVAL;
    }
    return n;
}

// ------------------------------------------------------------------ bands ----
typedef struct {
    const char *name;
    peq_type_t type;
} type_name_t;

// Accepted filter type names. LS/HS are treated as LSC/HSC (AutoEQ semantics);
// LPQ/HPQ are Equalizer APO names of the pass filters with an explicit Q.
static const type_name_t k_types[] = {
    {"PK", PEQ_PEAK},        {"PEQ", PEQ_PEAK},       {"PEAK", PEQ_PEAK},      {"LSC", PEQ_LOW_SHELF},
    {"LS", PEQ_LOW_SHELF},   {"LSQ", PEQ_LOW_SHELF},  {"HSC", PEQ_HIGH_SHELF}, {"HS", PEQ_HIGH_SHELF},
    {"HSQ", PEQ_HIGH_SHELF}, {"LP", PEQ_LOW_PASS},    {"LPQ", PEQ_LOW_PASS},   {"HP", PEQ_HIGH_PASS},
    {"HPQ", PEQ_HIGH_PASS},
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

int peq_band_parse(const char *s, peq_band_t *out) {
    if (!s || !out) return CORE_EINVAL;
    const char *p = skip_space(s);
    size_t n = word_len(p);
    if (word_eq(p, n, "filter")) {
        const char *colon = strchr(p, ':');
        if (!colon) return CORE_ECORRUPT;
        p = skip_space(colon + 1);
        n = word_len(p);
    }
    peq_band_t b = {PEQ_PEAK, 1000.0f, 0.0f, 0.7071f, true};
    if (word_eq(p, n, "on")) {
        b.enabled = true;
    } else if (word_eq(p, n, "off")) {
        b.enabled = false;
    } else {
        return CORE_ECORRUPT;
    }
    p = skip_space(p + n);
    n = word_len(p);
    bool type_ok = false;
    for (size_t i = 0; i < CORE_ARRAY_SIZE(k_types); i++) {
        if (word_eq(p, n, k_types[i].name)) {
            b.type = k_types[i].type;
            type_ok = true;
            break;
        }
    }
    if (!type_ok) return CORE_ECORRUPT;
    p += n;
    // Equalizer APO "LS 12dB" style slope token after the type: ignored.
    {
        const char *t = skip_space(p);
        const char *u = t;
        float slope;
        if (is_digit(*t) && dsp_text_parse_float(&u, &slope) && word_eq(u, word_len(u), "db")) p = u + 2;
    }

    bool have_fc = false;
    for (;;) {
        p = skip_space(p);
        if (!*p) break;
        n = word_len(p);
        if (n == 0) {
            p++;  // stray punctuation or a number without a key: skip
            continue;
        }
        const char *key = p;
        p += n;
        float v;
        if (word_eq(key, n, "fc")) {
            if (!dsp_text_parse_float(&p, &v)) return CORE_ECORRUPT;
            const char *u = skip_space(p);
            size_t un = word_len(u);
            if (word_eq(u, un, "khz")) {
                v *= 1000.0f;
                p = u + un;
            } else if (word_eq(u, un, "hz")) {
                p = u + un;
            }
            b.freq_hz = v;
            have_fc = true;
        } else if (word_eq(key, n, "gain")) {
            if (!dsp_text_parse_float(&p, &v)) return CORE_ECORRUPT;
            const char *u = skip_space(p);
            size_t un = word_len(u);
            if (word_eq(u, un, "db")) p = u + un;
            b.gain_db = v;
        } else if (word_eq(key, n, "q")) {
            if (!dsp_text_parse_float(&p, &v)) return CORE_ECORRUPT;
            if (!(v > 0.0f)) return CORE_ECORRUPT;
            b.q = v;
        } else if (word_eq(key, n, "bw")) {
            const char *u = skip_space(p);
            size_t un = word_len(u);
            if (word_eq(u, un, "oct")) p = u + un;
            if (!dsp_text_parse_float(&p, &v)) return CORE_ECORRUPT;
            if (!(v > 0.0f) || v > 10.0f) return CORE_ECORRUPT;
            // Bandwidth in octaves -> Q (RBJ): Q = sqrt(2^N) / (2^N - 1).
            const float p2 = exp2f(v);
            b.q = sqrtf(p2) / (p2 - 1.0f);
        }
        // Unknown words (units, "Oct", future keys) are skipped.
    }
    if (!have_fc || !isfinite(b.freq_hz) || !(b.freq_hz > 0.0f)) return CORE_ECORRUPT;
    if (!isfinite(b.gain_db) || !isfinite(b.q)) return CORE_ECORRUPT;
    b.freq_hz = clampf(b.freq_hz, PEQ_FREQ_MIN, PEQ_FREQ_MAX);
    b.gain_db = clampf(b.gain_db, PEQ_GAIN_MIN, PEQ_GAIN_MAX);
    b.q = clampf(b.q, PEQ_Q_MIN, PEQ_Q_MAX);
    if (b.type == PEQ_LOW_PASS || b.type == PEQ_HIGH_PASS) b.gain_db = 0.0f;
    *out = b;
    return CORE_OK;
}

static const char *type_code(peq_type_t t) {
    switch (t) {
    case PEQ_LOW_SHELF: return "LSC";
    case PEQ_HIGH_SHELF: return "HSC";
    case PEQ_LOW_PASS: return "LP";
    case PEQ_HIGH_PASS: return "HP";
    case PEQ_PEAK:
    default: return "PK";
    }
}

int peq_band_format(const peq_band_t *b, char *out, size_t size) {
    if (!b || !out || size == 0) return CORE_EINVAL;
    char fc[24], gain[24], q[24];
    if (dsp_text_format_float(fc, sizeof fc, b->freq_hz, 0, 2) < 0) return CORE_EINVAL;
    if (dsp_text_format_float(gain, sizeof gain, b->gain_db, 1, 2) < 0) return CORE_EINVAL;
    if (dsp_text_format_float(q, sizeof q, b->q, 2, 3) < 0) return CORE_EINVAL;
    const char *state = b->enabled ? "ON" : "OFF";
    int n;
    if (b->type == PEQ_LOW_PASS || b->type == PEQ_HIGH_PASS) {
        n = snprintf(out, size, "%s %s Fc %s Hz Q %s", state, type_code(b->type), fc, q);
    } else {
        n = snprintf(out, size, "%s %s Fc %s Hz Gain %s dB Q %s", state, type_code(b->type), fc, gain, q);
    }
    if (n < 0 || (size_t)n >= size) {
        out[0] = 0;
        return CORE_EINVAL;
    }
    return n;
}
