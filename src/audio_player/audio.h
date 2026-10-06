// SPDX-License-Identifier: Apache-2.0
/// \file
/// Audio formats and PCM conventions.
///
/// PCM inside the pipeline is ALWAYS interleaved int32_t, left-justified (Q31):
/// a 16-bit sample 0x1234 becomes 0x12340000, a 24-bit sample 0x123456 becomes
/// 0x12345600. Full scale is INT32_MIN..INT32_MAX. This keeps 16/24/32-bit sources
/// bit-exact when DSP is bypassed: the sink takes the top bits it needs.
///
/// DSD is carried as DoP (DSD over PCM, 176.4 kHz for DSD64, 24-bit payload in the
/// top 24 bits with 0x05/0xFA markers). DoP frames must never pass through DSP or
/// volume; audio_format_t.dop marks them.
#pragma once

#include "audio_player/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_MAX_CHANNELS 2

typedef struct {
    uint32_t sample_rate;   ///< Hz, e.g. 44100
    uint8_t channels;       ///< 1 or 2 (mono is upmixed by the player before the sink)
    uint8_t bits;           ///< significant bits of the source: 16, 24 or 32
    bool dop;               ///< true: frames are DoP-encoded DSD, bypass all processing
} audio_format_t;

static inline bool audio_format_equal(const audio_format_t *a, const audio_format_t *b) {
    return a->sample_rate == b->sample_rate && a->channels == b->channels && a->bits == b->bits && a->dop == b->dop;
}

/// 44.1 kHz family (44100, 88200, 176400, 352800) or 48 kHz family (32000, 48000, 96000, 192000...).
typedef enum { RATE_FAMILY_44K1 = 0, RATE_FAMILY_48K = 1, RATE_FAMILY_OTHER = 2 } rate_family_t;
rate_family_t audio_rate_family(uint32_t sample_rate);

static inline int32_t audio_s16_to_q31(int16_t v) { return (int32_t)((uint32_t)(uint16_t)v << 16); }
static inline int32_t audio_s24_to_q31(int32_t v24) { return (int32_t)((uint32_t)v24 << 8); }
static inline int16_t audio_q31_to_s16_trunc(int32_t v) { return (int16_t)(v >> 16); }

uint64_t audio_ms_to_frames(uint32_t ms, uint32_t sample_rate);
uint32_t audio_frames_to_ms(uint64_t frames, uint32_t sample_rate);

#ifdef __cplusplus
}
#endif
