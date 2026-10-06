// SPDX-License-Identifier: Apache-2.0
// AIFF / AIFF-C backend. Big-endian integer PCM 1..32 bit ("NONE", "twos", "in24", "in32"),
// little-endian "sowt", unsigned 8-bit "raw ", float "fl32"/"FL32" and "fl64"/"FL64".
// The sample rate is an 80-bit IEEE 754 extended float, converted with integer math.
#include "decoders/dec_internal.h"
#include "decoders/dec_pcm.h"

#define TAG "dec_aiff"

#define AIFF_MAX_CHUNKS 4096  // bound on the chunk walk for corrupt files

typedef struct {
    uint16_t channels, bits;
    uint32_t frames;
    uint32_t rate;
    uint8_t compression[4];
} aiff_comm_t;

static bool aiff_probe(const uint8_t *h, size_t n, const char *ext) {
    (void)ext;
    return n >= 12 && dec_tag_eq(h, "FORM") && (dec_tag_eq(h + 8, "AIFF") || dec_tag_eq(h + 8, "AIFC"));
}

// 80-bit extended (sign, 15-bit exponent with bias 16383, 64-bit mantissa with an explicit integer
// bit) -> integer Hz, rounded to nearest. Returns 0 for values that are not a usable rate.
static uint32_t ext80_to_hz(const uint8_t b[10]) {
    if (b[0] & 0x80) return 0;
    int exp = ((b[0] & 0x7F) << 8 | b[1]) - 16383;
    uint64_t mant = dec_be64(b + 2);
    if (mant == 0 || exp < 0 || exp > 31) return 0;
    int shift = 63 - exp;  // 32..63
    uint64_t v = (mant >> shift) + ((mant >> (shift - 1)) & 1);
    return v > UINT32_MAX ? 0 : (uint32_t)v;
}

static int layout_from_comm(const aiff_comm_t *c, pcm_layout_t *l, unsigned *sig_bits) {
    if (c->channels == 0) return CORE_ECORRUPT;
    if (c->channels > AUDIO_MAX_CHANNELS) {
        CORE_LOGW(TAG, "%u channels not supported", c->channels);
        return CORE_EUNSUPPORTED;
    }
    if (c->rate < 1000 || c->rate > 768000) return CORE_EUNSUPPORTED;
    const uint8_t *t = c->compression;
    l->big_endian = true;
    l->kind = PCM_INT;
    if (dec_tag_eq(t, "NONE") || dec_tag_eq(t, "twos") || dec_tag_eq(t, "sowt") || dec_tag_eq(t, "in24") ||
        dec_tag_eq(t, "in32")) {
        unsigned bits = c->bits;
        if (dec_tag_eq(t, "in24")) bits = 24;
        if (dec_tag_eq(t, "in32")) bits = 32;
        if (bits == 0 || bits > 32) return CORE_ECORRUPT;
        l->bytes = (uint8_t)((bits + 7) / 8);  // samples are left-justified in whole bytes
        l->big_endian = !dec_tag_eq(t, "sowt");
        *sig_bits = bits;
    } else if (dec_tag_eq(t, "raw ")) {
        l->kind = PCM_UINT;
        l->bytes = 1;
        *sig_bits = 8;
    } else if (dec_tag_eq(t, "fl32") || dec_tag_eq(t, "FL32")) {
        l->kind = PCM_FLOAT;
        l->bytes = 4;
        *sig_bits = 32;
    } else if (dec_tag_eq(t, "fl64") || dec_tag_eq(t, "FL64")) {
        l->kind = PCM_FLOAT;
        l->bytes = 8;
        *sig_bits = 32;
    } else {
        CORE_LOGW(TAG, "AIFC compression '%c%c%c%c' not supported", t[0], t[1], t[2], t[3]);
        return CORE_EUNSUPPORTED;
    }
    return pcm_layout_valid(l) ? CORE_OK : CORE_EUNSUPPORTED;
}

static void *aiff_open(core_stream_t *s, decoder_info_t *info, int *err) {
    uint8_t h[12];
    if (dec_read_at(s, 0, h, sizeof h) != CORE_OK || !aiff_probe(h, sizeof h, "")) {
        *err = CORE_ECORRUPT;
        return NULL;
    }
    bool aifc = dec_tag_eq(h + 8, "AIFC");
    int64_t fsize = core_stream_size(s);

    aiff_comm_t comm = {0};
    bool have_comm = false;
    int64_t data_start = -1, pos = 12;
    uint64_t data_size = 0;

    for (int guard = 0; guard < AIFF_MAX_CHUNKS; guard++) {
        uint8_t ck[8];
        if (fsize >= 0 && pos + 8 > fsize) break;
        if (dec_read_at(s, pos, ck, sizeof ck) != CORE_OK) break;
        uint64_t size = dec_be32(ck + 4);
        int64_t body = pos + 8;
        if (dec_tag_eq(ck, "COMM") && !have_comm) {
            uint8_t b[22];
            uint32_t n = (uint32_t)CORE_MIN(size, (uint64_t)sizeof b);
            if (n < 18 || (aifc && n < 22)) {
                *err = CORE_ECORRUPT;
                return NULL;
            }
            if (core_stream_read_exact(s, b, n) != CORE_OK) break;
            comm.channels = dec_be16(b);
            comm.frames = dec_be32(b + 2);
            comm.bits = dec_be16(b + 6);
            comm.rate = ext80_to_hz(b + 8);
            memcpy(comm.compression, aifc ? b + 18 : (const uint8_t *)"NONE", 4);
            have_comm = true;
        } else if (dec_tag_eq(ck, "SSND") && data_start < 0) {
            uint8_t b[8];
            if (size < 8 || core_stream_read_exact(s, b, sizeof b) != CORE_OK) break;
            uint64_t offset = CORE_MIN((uint64_t)dec_be32(b), size - 8);
            data_start = body + 8 + (int64_t)offset;
            data_size = size - 8 - offset;
            if (have_comm) break;
        }
        uint64_t adv = size + (size & 1);  // chunks are padded to an even size
        if (adv > (uint64_t)(INT64_MAX - body)) break;
        pos = body + (int64_t)adv;
    }

    if (!have_comm || data_start < 0) {
        *err = CORE_ECORRUPT;
        return NULL;
    }
    pcm_layout_t layout;
    unsigned sig_bits = 0;
    int r = layout_from_comm(&comm, &layout, &sig_bits);
    if (r != CORE_OK) {
        *err = r;
        return NULL;
    }
    uint32_t block_align = (uint32_t)layout.bytes * comm.channels;
    if (fsize >= 0) {
        uint64_t avail = fsize > data_start ? (uint64_t)(fsize - data_start) : 0;
        if (data_size > avail) data_size = avail;
    }
    uint64_t frames = CORE_MIN((uint64_t)comm.frames, data_size / block_align);

    pcm_reader_t *rd = pcm_reader_create(s, &layout, (uint8_t)comm.channels, data_start, frames, err);
    if (!rd) return NULL;

    info->fmt.sample_rate = comm.rate;
    info->fmt.channels = (uint8_t)comm.channels;
    info->fmt.bits = dec_bits_class(sig_bits);
    info->fmt.dop = false;
    info->total_frames = frames;
    info->bitrate_kbps = (uint32_t)((uint64_t)comm.rate * block_align * 8 / 1000);
    info->seekable = true;
    return rd;
}

const decoder_backend_t dec_aiff_backend = {
    CODEC_AIFF, aiff_probe, aiff_open, pcm_reader_read, pcm_reader_seek, pcm_reader_close,
};
