// SPDX-License-Identifier: Apache-2.0
// Raw PCM sample conversion to the pipeline format (interleaved Q31, left-justified).
// Shared by the WAV and AIFF backends.
#pragma once

#include "audio_player/stream.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PCM_INT = 0,  // two's complement, sample left-justified in its container
    PCM_UINT,     // offset binary (8-bit WAV, AIFC "raw ")
    PCM_FLOAT,    // IEEE 754 binary32 (4 bytes) or binary64 (8 bytes)
} pcm_kind_t;

typedef struct {
    pcm_kind_t kind;
    uint8_t bytes;    // container bytes per sample: 1..4 for integers, 4 or 8 for float
    bool big_endian;
} pcm_layout_t;

// True if the layout is one pcm_to_q31() can convert.
bool pcm_layout_valid(const pcm_layout_t *l);

// Converts n samples. Integer samples keep all their bits (a 24-bit sample 0x123456 becomes
// 0x12345600); floats are scaled by 2^31 and saturated to the Q31 range, NaN becomes 0.
void pcm_to_q31(const pcm_layout_t *l, const uint8_t *src, int32_t *dst, size_t n);

// IEEE 754 binary64 bit pattern -> saturated Q31, integer arithmetic only.
int32_t pcm_f64_bits_to_q31(uint64_t bits);

// ------------------------------------------------------------------------------------------------
// Reader for a contiguous block of interleaved PCM frames. WAV and AIFF use it as their backend
// state: open parses the header, then pcm_reader_create(); read/seek/close are shared.
// ------------------------------------------------------------------------------------------------
typedef struct pcm_reader pcm_reader_t;

// Creates a reader positioned at the first frame. On failure returns NULL and sets *err.
pcm_reader_t *pcm_reader_create(core_stream_t *s, const pcm_layout_t *l, uint8_t channels, int64_t data_start,
                                uint64_t frames, int *err);
int32_t pcm_reader_read(void *reader, int32_t *out, uint32_t max_frames);
int pcm_reader_seek(void *reader, uint64_t frame);
void pcm_reader_close(void *reader);

#ifdef __cplusplus
}
#endif
