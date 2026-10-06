// SPDX-License-Identifier: Apache-2.0
// I2S output planning (see i2s_plan.h).
#include "port/i2s_plan.h"

#include <stdio.h>
#include <string.h>

const uint32_t ap_i2s_rates[] = {8000,  11025, 16000,  22050,  32000, 44100,
                                 48000, 88200, 96000, 176400, 192000};
const uint32_t ap_i2s_rate_count = (uint32_t)(sizeof(ap_i2s_rates) / sizeof(ap_i2s_rates[0]));

static bool is_known_rate(uint32_t rate) {
    for (uint32_t i = 0; i < ap_i2s_rate_count; i++) {
        if (ap_i2s_rates[i] == rate) return true;
    }
    return false;
}

int ap_i2s_plan_clock(uint32_t rate, uint32_t mclk_multiple, ap_i2s_clock_plan_t *out) {
    if (!out) return CORE_EINVAL;
    memset(out, 0, sizeof(*out));
    if (!is_known_rate(rate)) return CORE_EUNSUPPORTED;
    uint32_t multiple = mclk_multiple;
    if (!multiple) multiple = rate > 96000u ? 128u : 256u;
    if (multiple % AP_I2S_BCK_PER_FRAME != 0 || multiple / AP_I2S_BCK_PER_FRAME < AP_I2S_MIN_BCLK_DIV)
        return CORE_EUNSUPPORTED;
    out->sample_rate = rate;
    out->mclk_multiple = multiple;
    out->mclk_hz = rate * multiple;
    out->bclk_div = multiple / AP_I2S_BCK_PER_FRAME;
    return CORE_OK;
}

bool ap_i2s_rate_supported(uint32_t max_rate, uint32_t mclk_multiple, uint32_t rate) {
    if (max_rate && rate > max_rate) return false;
    ap_i2s_clock_plan_t p;
    return ap_i2s_plan_clock(rate, mclk_multiple, &p) == CORE_OK;
}

uint32_t ap_i2s_max_rate(uint32_t max_rate, uint32_t mclk_multiple) {
    uint32_t best = 0;
    for (uint32_t i = 0; i < ap_i2s_rate_count; i++) {
        uint32_t r = ap_i2s_rates[i];
        if (ap_i2s_rate_supported(max_rate, mclk_multiple, r) && r > best) best = r;
    }
    return best;
}

int ap_i2s_negotiate(uint32_t max_rate, uint32_t mclk_multiple, bool accept_dop, const audio_format_t *src,
                     audio_format_t *out) {
    if (!src || !out) return CORE_EINVAL;
    if (src->sample_rate == 0 || src->channels == 0 || src->channels > AUDIO_MAX_CHANNELS) return CORE_EINVAL;
    if (src->dop) {
        // DoP is a bit pattern, not audio: it plays at its own rate or not at all.
        if (!accept_dop || src->channels != 2 || !ap_i2s_rate_supported(max_rate, mclk_multiple, src->sample_rate))
            return CORE_EUNSUPPORTED;
        *out = *src;
        return CORE_OK;
    }

    uint32_t rate = src->sample_rate;
    if (!ap_i2s_rate_supported(max_rate, mclk_multiple, rate)) {
        // Same family first: the smallest usable rate at or above the source, else the
        // highest usable one below it (e.g. 352.8 -> 176.4).
        rate_family_t fam = audio_rate_family(src->sample_rate);
        uint32_t above = 0, below = 0, any_above = 0, any_max = 0;
        for (uint32_t i = 0; i < ap_i2s_rate_count; i++) {
            uint32_t r = ap_i2s_rates[i];
            if (!ap_i2s_rate_supported(max_rate, mclk_multiple, r)) continue;
            if (r > any_max) any_max = r;
            if (r >= src->sample_rate && !any_above) any_above = r;
            if (fam != RATE_FAMILY_OTHER && audio_rate_family(r) != fam) continue;
            if (r >= src->sample_rate) {
                if (!above) above = r;
            } else {
                below = r;
            }
        }
        if (fam == RATE_FAMILY_OTHER) {
            rate = any_above ? any_above : any_max;
        } else {
            rate = above ? above : (below ? below : (any_above ? any_above : any_max));
        }
        if (!rate) return CORE_EUNSUPPORTED;
    }

    out->sample_rate = rate;
    out->channels = 2;
    uint8_t bits = src->bits;
    if (bits < 16) bits = 16;
    if (bits > 32) bits = 32;
    out->bits = bits;
    out->dop = false;
    return CORE_OK;
}

void ap_i2s_format_khz(uint32_t rate, char *out, size_t out_size) {
    if (!out || !out_size) return;
    uint32_t whole = rate / 1000u, frac = rate % 1000u;
    if (frac == 0) {
        snprintf(out, out_size, "%u", (unsigned)whole);
    } else if (frac % 100u == 0) {
        snprintf(out, out_size, "%u.%u", (unsigned)whole, (unsigned)(frac / 100u));
    } else if (frac % 10u == 0) {
        snprintf(out, out_size, "%u.%02u", (unsigned)whole, (unsigned)(frac / 10u));
    } else {
        snprintf(out, out_size, "%u.%03u", (unsigned)whole, (unsigned)frac);
    }
}

void ap_i2s_describe(const char *dac_name, const audio_format_t *fmt, char *out, size_t out_size) {
    if (!out || !out_size) return;
    const char *name = dac_name && dac_name[0] ? dac_name : "I2S";
    if (!fmt || !fmt->sample_rate) {
        snprintf(out, out_size, "%s", name);
        return;
    }
    char khz[16];
    ap_i2s_format_khz(fmt->sample_rate, khz, sizeof khz);
    if (fmt->dop) {
        snprintf(out, out_size, "%s DoP %s", name, khz);
    } else {
        snprintf(out, out_size, "%s %u/%s", name, (unsigned)fmt->bits, khz);
    }
}

void ap_dop_silence(int32_t *frames, uint32_t n, uint32_t *phase) {
    uint32_t ph = phase ? (*phase & 1u) : 0u;
    for (uint32_t i = 0; frames && i < n; i++, ph ^= 1u) {
        uint32_t marker = ph ? AP_DOP_MARKER_B : AP_DOP_MARKER_A;
        int32_t word = (int32_t)(marker << 24 | AP_DSD_IDLE << 16 | AP_DSD_IDLE << 8);
        frames[2 * i] = word;
        frames[2 * i + 1] = word;
    }
    if (phase) *phase = ph;
}

uint32_t ap_dop_phase_after(int32_t word) { return ((uint32_t)word >> 24) == AP_DOP_MARKER_A ? 1u : 0u; }
