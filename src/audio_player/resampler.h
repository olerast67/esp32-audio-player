// SPDX-License-Identifier: Apache-2.0
/// \file
/// Sample-rate conversion for the Bluetooth path (A2DP runs at 44.1 or 48 kHz).
/// Integer ratios 2:1 and 4:1 use cascaded half-band decimators (cheap, the common
/// hi-res case). Other ratios (48k -> 44.1k = 147/160) use a polyphase windowed-sinc
/// filter. The wired path never resamples.
#pragma once

#include "audio_player/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RESAMPLER_FAST = 0,    ///< ~16 taps/phase, for weak CPUs
    RESAMPLER_GOOD = 1,    ///< ~32 taps/phase, default
} resampler_quality_t;

typedef struct resampler resampler_t;

/// Returns NULL if the conversion is not supported. in_rate == out_rate is allowed (copy).
resampler_t *resampler_create(uint32_t in_rate, uint32_t out_rate, uint8_t channels, resampler_quality_t q);
void resampler_destroy(resampler_t *r);
void resampler_reset(resampler_t *r);

/// Convert interleaved Q31. Consumes up to in_frames, writes up to out_cap frames.
/// *consumed receives input frames used. Returns output frames written.
uint32_t resampler_process(resampler_t *r, const int32_t *in, uint32_t in_frames, uint32_t *consumed, int32_t *out,
                           uint32_t out_cap);

/// Upper bound of output frames for a given input count (for buffer sizing).
uint32_t resampler_max_output(const resampler_t *r, uint32_t in_frames);

#ifdef __cplusplus
}
#endif
