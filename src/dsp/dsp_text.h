// SPDX-License-Identifier: Apache-2.0
// Text helpers of the DSP module, shared with the settings module (private to the core).
// Locale-independent and free of double precision, so results are identical on the
// ESP32 and on the host.
#pragma once

#include "audio_player/dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

// Parse a decimal number ("-2.9", "0.70", "1e3", "105,5") at *p after optional
// spaces/tabs. On success stores the value, advances *p past the number and returns true.
bool dsp_text_parse_float(const char **p, float *out);

// Format v rounded to max_dec decimals, trimming trailing zeros down to min_dec
// (0 <= min_dec <= max_dec <= 6). Never prints "-0". Returns the length, or
// CORE_EINVAL if the buffer is too small (out is then an empty string).
int dsp_text_format_float(char *out, size_t size, float v, int min_dec, int max_dec);

// One filter in AutoEQ / Equalizer APO syntax, with or without the "Filter N:" prefix:
//   "ON PK Fc 105 Hz Gain -2.9 dB Q 0.70", "OFF LSC Fc 105 Hz Gain 5.5 dB Q 0.71",
//   "ON HP Fc 20 Hz", "ON PK Fc 1 kHz Gain 3 dB BW Oct 1.0"
// Returns CORE_OK or CORE_ECORRUPT. Values are clamped into the ranges below.
int peq_band_parse(const char *s, peq_band_t *out);
// Inverse of peq_band_parse (without the prefix). Returns the length or CORE_EINVAL.
int peq_band_format(const peq_band_t *b, char *out, size_t size);

// Band parameter limits applied by the parser and the settings module.
#define PEQ_FREQ_MIN 10.0f
#define PEQ_FREQ_MAX 40000.0f
#define PEQ_GAIN_MIN (-30.0f)
#define PEQ_GAIN_MAX 30.0f
#define PEQ_Q_MIN 0.05f
#define PEQ_Q_MAX 30.0f
#define PEQ_PREAMP_MIN (-30.0f)
#define PEQ_PREAMP_MAX 30.0f

#ifdef __cplusplus
}
#endif
