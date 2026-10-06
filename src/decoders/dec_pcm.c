// SPDX-License-Identifier: Apache-2.0
// Raw PCM -> Q31 conversion for the WAV and AIFF backends.
#include "decoders/dec_pcm.h"

#include <string.h>

#include "decoders/dec_internal.h"

bool pcm_layout_valid(const pcm_layout_t *l) {
    switch (l->kind) {
    case PCM_INT: return l->bytes >= 1 && l->bytes <= 4;
    case PCM_UINT: return l->bytes == 1;
    case PCM_FLOAT: return l->bytes == 4 || l->bytes == 8;
    }
    return false;
}

int32_t pcm_f64_bits_to_q31(uint64_t bits) {
    bool neg = (bits >> 63) != 0;
    int exp = (int)((bits >> 52) & 0x7FF);
    uint64_t mant = bits & ((UINT64_C(1) << 52) - 1);
    if (exp == 0x7FF) return mant ? 0 : (neg ? INT32_MIN : INT32_MAX);  // NaN -> 0, +-Inf saturates
    if (exp == 0) return 0;                                              // zero or subnormal
    mant |= UINT64_C(1) << 52;
    // |x| * 2^31 = mant * 2^(exp - 1023 - 52 + 31) = mant * 2^(exp - 1044)
    int shift = exp - 1044;
    if (shift >= -21) return neg ? INT32_MIN : INT32_MAX;  // |x| >= 1.0 (x == -1.0 is exactly INT32_MIN)
    int rs = -shift;
    if (rs >= 64) return 0;
    uint32_t mag = (uint32_t)(mant >> rs);  // < 2^31, truncated toward zero like the float path
    return neg ? -(int32_t)mag : (int32_t)mag;
}

static void int_to_q31(const pcm_layout_t *l, const uint8_t *s, int32_t *d, size_t n) {
    size_t i;
    switch (l->bytes) {
    case 1:
        for (i = 0; i < n; i++) d[i] = (int32_t)((uint32_t)s[i] << 24);
        break;
    case 2:
        if (l->big_endian) {
            for (i = 0; i < n; i++, s += 2) d[i] = (int32_t)((uint32_t)dec_be16(s) << 16);
        } else {
            for (i = 0; i < n; i++, s += 2) d[i] = (int32_t)((uint32_t)dec_le16(s) << 16);
        }
        break;
    case 3:
        if (l->big_endian) {
            for (i = 0; i < n; i++, s += 3) {
                d[i] = (int32_t)((uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8);
            }
        } else {
            for (i = 0; i < n; i++, s += 3) {
                d[i] = (int32_t)((uint32_t)s[2] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[0] << 8);
            }
        }
        break;
    case 4:
        if (l->big_endian) {
            for (i = 0; i < n; i++, s += 4) d[i] = (int32_t)dec_be32(s);
        } else {
            for (i = 0; i < n; i++, s += 4) d[i] = (int32_t)dec_le32(s);
        }
        break;
    default:
        memset(d, 0, n * sizeof *d);
        break;
    }
}

static void float_to_q31(const pcm_layout_t *l, const uint8_t *s, int32_t *d, size_t n) {
    size_t i;
    if (l->bytes == 4) {
        for (i = 0; i < n; i++, s += 4) {
            uint32_t bits = l->big_endian ? dec_be32(s) : dec_le32(s);
            float f;
            memcpy(&f, &bits, sizeof f);
            d[i] = dec_f32_to_q31(f);
        }
    } else {
        for (i = 0; i < n; i++, s += 8) d[i] = pcm_f64_bits_to_q31(l->big_endian ? dec_be64(s) : dec_le64(s));
    }
}

void pcm_to_q31(const pcm_layout_t *l, const uint8_t *src, int32_t *dst, size_t n) {
    switch (l->kind) {
    case PCM_INT: int_to_q31(l, src, dst, n); break;
    case PCM_UINT:
        for (size_t i = 0; i < n; i++) dst[i] = (int32_t)((uint32_t)(src[i] ^ 0x80u) << 24);
        break;
    case PCM_FLOAT: float_to_q31(l, src, dst, n); break;
    default: memset(dst, 0, n * sizeof *dst); break;
    }
}

// ------------------------------------------------------------------------------------ reader ----
#define PCM_RAW_BYTES 8192  // raw bytes read and converted per step

struct pcm_reader {
    core_stream_t *s;
    pcm_layout_t layout;
    uint8_t channels;
    uint32_t block_align;  // bytes per frame
    int64_t data_start;
    uint64_t total;        // frames
    uint64_t pos;          // next frame
    uint8_t raw[PCM_RAW_BYTES];
};

pcm_reader_t *pcm_reader_create(core_stream_t *s, const pcm_layout_t *l, uint8_t channels, int64_t data_start,
                                uint64_t frames, int *err) {
    if (!pcm_layout_valid(l) || channels == 0 || channels > AUDIO_MAX_CHANNELS || data_start < 0) {
        *err = CORE_EINVAL;
        return NULL;
    }
    pcm_reader_t *r = core_malloc(sizeof *r);
    if (!r) {
        *err = CORE_ENOMEM;
        return NULL;
    }
    r->s = s;
    r->layout = *l;
    r->channels = channels;
    r->block_align = (uint32_t)l->bytes * channels;  // at most 2 * 8 bytes, far below PCM_RAW_BYTES
    r->data_start = data_start;
    r->total = frames;
    r->pos = 0;
    int e = core_stream_seek(s, data_start, 0);
    if (e != CORE_OK) {
        core_free(r);
        *err = e;
        return NULL;
    }
    return r;
}

int32_t pcm_reader_read(void *reader, int32_t *out, uint32_t max_frames) {
    pcm_reader_t *r = reader;
    uint32_t done = 0;
    const uint32_t step = PCM_RAW_BYTES / r->block_align;
    while (done < max_frames && r->pos < r->total) {
        uint32_t n = CORE_MIN(max_frames - done, step);
        if (n > r->total - r->pos) n = (uint32_t)(r->total - r->pos);
        size_t want = (size_t)n * r->block_align, got = 0;
        while (got < want) {
            int32_t k = core_stream_read(r->s, r->raw + got, want - got);
            if (k < 0) return done ? (int32_t)done : k;
            if (k == 0) break;
            got += (size_t)k;
        }
        uint32_t frames = (uint32_t)(got / r->block_align);
        if (frames) pcm_to_q31(&r->layout, r->raw, out + (size_t)done * r->channels, (size_t)frames * r->channels);
        done += frames;
        r->pos += frames;
        if (got < want) {
            r->total = r->pos;  // truncated file: the header promised more than there is
            break;
        }
    }
    return (int32_t)done;
}

int pcm_reader_seek(void *reader, uint64_t frame) {
    pcm_reader_t *r = reader;
    if (frame > r->total) return CORE_EINVAL;
    int e = core_stream_seek(r->s, r->data_start + (int64_t)(frame * r->block_align), 0);
    if (e != CORE_OK) return e;
    r->pos = frame;
    return CORE_OK;
}

void pcm_reader_close(void *reader) { core_free(reader); }
