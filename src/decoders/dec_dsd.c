// SPDX-License-Identifier: Apache-2.0
// DSD backends: DSF (Sony) and DSDIFF/DFF (Philips), uncompressed DSD64 and DSD128, delivered as DoP
// (DSD over PCM v1.1). Each output frame carries 16 DSD bits per channel, oldest bit first, below a
// marker byte that alternates 0x05 / 0xFA from frame to frame (0x05 on even absolute frames):
//
//   Q31 word = marker << 24 | first DSD byte << 16 | second DSD byte << 8     (fmt.bits = 24)
//
// DSD64 (2.8224 MHz) becomes 176.4 kHz DoP, DSD128 (5.6448 MHz) becomes 352.8 kHz DoP; whether the
// output can carry 352.8 kHz is the sink's decision. DST-compressed DFF is not supported.
#include "decoders/dec_internal.h"

#define TAG "dec_dsd"

#define DSD64_RATE 2822400u
#define DSD128_RATE 5644800u
#define DSF_MAX_BLOCK (64 * 1024)  // bytes per channel per block (the DSF spec fixes 4096)
#define DFF_RAW_BYTES 8192         // raw bytes read per step
#define DFF_MAX_CHUNKS 1024

// Bit reversal: DSF with 1 bit per sample stores the oldest bit in the LSB, DoP wants it in the MSB.
#define R2(n) n, n + 2 * 64, n + 1 * 64, n + 3 * 64
#define R4(n) R2(n), R2(n + 2 * 16), R2(n + 1 * 16), R2(n + 3 * 16)
#define R6(n) R4(n), R4(n + 2 * 4), R4(n + 1 * 4), R4(n + 3 * 4)
static const uint8_t k_bitrev[256] = {R6(0), R6(2), R6(1), R6(3)};
#undef R2
#undef R4
#undef R6

static inline int32_t dop_word(uint64_t frame, uint8_t first, uint8_t second) {
    uint32_t marker = (frame & 1) ? 0xFAu : 0x05u;
    return (int32_t)(marker << 24 | (uint32_t)first << 16 | (uint32_t)second << 8);
}

static bool dop_rate(uint32_t dsd_rate, uint32_t *pcm_rate) {
    if (dsd_rate != DSD64_RATE && dsd_rate != DSD128_RATE) return false;
    *pcm_rate = dsd_rate / 16;
    return true;
}

static void fill_info(decoder_info_t *info, uint32_t dsd_rate, uint8_t channels, uint64_t frames) {
    info->fmt.sample_rate = dsd_rate / 16;
    info->fmt.channels = channels;
    info->fmt.bits = 24;
    info->fmt.dop = true;
    info->total_frames = frames;
    info->bitrate_kbps = dsd_rate / 1000 * channels;
    info->seekable = true;
}

// ------------------------------------------------------------------------------------------ DSF ----
// Layout: "DSD " chunk (28 bytes), "fmt " chunk (52 bytes), "data" chunk (12-byte header), then
// blocks of `block` bytes per channel: [ch0 block 0][ch1 block 0][ch0 block 1]...
typedef struct {
    core_stream_t *s;
    uint8_t channels;
    bool lsb_first;
    uint32_t block;
    int64_t data_start;
    uint64_t total;   // DoP frames
    uint64_t pos;     // next DoP frame
    uint64_t loaded;  // block group held in buf, UINT64_MAX if none
    uint8_t *buf;     // block * channels bytes
} dsf_t;

static bool dsf_probe(const uint8_t *h, size_t n, const char *ext) {
    (void)ext;
    return n >= 12 && dec_tag_eq(h, "DSD ") && dec_le64(h + 4) == 28;
}

static void dsf_close(void *st) {
    dsf_t *d = st;
    if (!d) return;
    core_free(d->buf);
    core_free(d);
}

static void *dsf_open(core_stream_t *s, decoder_info_t *info, int *err) {
    uint8_t h[28 + 52 + 12];
    if (dec_read_at(s, 0, h, sizeof h) != CORE_OK || !dsf_probe(h, sizeof h, "")) {
        *err = CORE_ECORRUPT;
        return NULL;
    }
    const uint8_t *f = h + 28;
    if (!dec_tag_eq(f, "fmt ") || dec_le64(f + 4) != 52) {
        *err = CORE_ECORRUPT;
        return NULL;
    }
    uint32_t format_id = dec_le32(f + 16), channels = dec_le32(f + 24), rate = dec_le32(f + 28);
    uint32_t bits = dec_le32(f + 32), block = dec_le32(f + 44);
    uint64_t samples = dec_le64(f + 36);  // DSD samples (bits) per channel
    const uint8_t *d = h + 28 + 52;
    uint32_t pcm_rate;
    if (!dec_tag_eq(d, "data") || dec_le64(d + 4) < 12 || (bits != 1 && bits != 8) || block < 2 || (block & 1) ||
        block > DSF_MAX_BLOCK || channels == 0) {
        *err = CORE_ECORRUPT;
        return NULL;
    }
    if (format_id != 0 || channels > AUDIO_MAX_CHANNELS || !dop_rate(rate, &pcm_rate)) {
        CORE_LOGW(TAG, "DSF not supported: format %u, %u ch, %u Hz", (unsigned)format_id, (unsigned)channels,
                  (unsigned)rate);
        *err = CORE_EUNSUPPORTED;
        return NULL;
    }
    int64_t data_start = 28 + 52 + 12;
    uint64_t data_size = dec_le64(d + 4) - 12;
    int64_t fsize = core_stream_size(s);
    if (fsize >= 0) data_size = CORE_MIN(data_size, (uint64_t)(fsize > data_start ? fsize - data_start : 0));
    uint64_t groups = data_size / ((uint64_t)block * channels);  // complete block groups present
    uint64_t total = CORE_MIN(samples / 16, groups * block / 2);

    dsf_t *st = core_calloc(1, sizeof *st);
    uint8_t *buf = core_malloc((size_t)block * channels);
    if (!st || !buf) {
        core_free(st);
        core_free(buf);
        *err = CORE_ENOMEM;
        return NULL;
    }
    st->s = s;
    st->buf = buf;
    st->channels = (uint8_t)channels;
    st->lsb_first = bits == 1;
    st->block = block;
    st->data_start = data_start;
    st->total = total;
    st->loaded = UINT64_MAX;
    fill_info(info, rate, st->channels, total);
    return st;
}

static int32_t dsf_read(void *state, int32_t *out, uint32_t max_frames) {
    dsf_t *d = state;
    uint32_t done = 0;
    const uint8_t *rev = d->lsb_first ? k_bitrev : NULL;
    while (done < max_frames && d->pos < d->total) {
        uint64_t byte = d->pos * 2;  // offset within each channel's byte stream
        uint64_t group = byte / d->block;
        uint32_t in = (uint32_t)(byte % d->block);
        if (group != d->loaded) {
            size_t len = (size_t)d->block * d->channels;
            int64_t at = d->data_start + (int64_t)(group * len);
            int r = dec_read_at(d->s, at, d->buf, len);
            if (r != CORE_OK) {
                if (r != CORE_EOF && !done) return r;
                d->total = d->pos;  // file shorter than its header says
                break;
            }
            d->loaded = group;
        }
        uint32_t n = CORE_MIN(max_frames - done, (d->block - in) / 2);
        if (n > d->total - d->pos) n = (uint32_t)(d->total - d->pos);
        int32_t *dst = out + (size_t)done * d->channels;
        for (uint8_t c = 0; c < d->channels; c++) {
            const uint8_t *src = d->buf + (size_t)c * d->block + in;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t b0 = src[2 * i], b1 = src[2 * i + 1];
                if (rev) {
                    b0 = rev[b0];
                    b1 = rev[b1];
                }
                dst[(size_t)i * d->channels + c] = dop_word(d->pos + i, b0, b1);
            }
        }
        d->pos += n;
        done += n;
    }
    return (int32_t)done;
}

static int dsf_seek(void *state, uint64_t frame) {
    dsf_t *d = state;
    if (frame > d->total) return CORE_EINVAL;
    d->pos = frame;  // the block group is loaded on the next read
    return CORE_OK;
}

const decoder_backend_t dec_dsf_backend = {CODEC_DSF, dsf_probe, dsf_open, dsf_read, dsf_seek, dsf_close};

// ------------------------------------------------------------------------------------------ DFF ----
// DSDIFF: "FRM8" <size64 BE> "DSD ", then chunks <id> <size64 BE> <data, padded to even>.
// "PROP"/"SND " holds "FS  " (rate), "CHNL" (channels) and "CMPR" ("DSD " = uncompressed).
// "DSD " holds the samples, one byte per channel in turn, oldest bit in the MSB.
typedef struct {
    core_stream_t *s;
    uint8_t channels;
    int64_t data_start;
    uint64_t total;  // DoP frames
    uint64_t pos;
    uint8_t raw[DFF_RAW_BYTES];
} dff_t;

static bool dff_probe(const uint8_t *h, size_t n, const char *ext) {
    (void)ext;
    return n >= 16 && dec_tag_eq(h, "FRM8") && dec_tag_eq(h + 12, "DSD ");
}

typedef struct {
    uint32_t rate;
    uint16_t channels;
    bool compressed, have_rate, have_channels;
} dff_prop_t;

static int dff_parse_prop(core_stream_t *s, int64_t pos, uint64_t size, dff_prop_t *p) {
    uint8_t b[12];
    if (size < 4 || dec_read_at(s, pos, b, 4) != CORE_OK) return CORE_ECORRUPT;
    if (!dec_tag_eq(b, "SND ")) return CORE_OK;  // some other property set
    int64_t end = pos + (int64_t)size;
    pos += 4;
    for (int guard = 0; guard < DFF_MAX_CHUNKS && pos + 12 <= end; guard++) {
        if (dec_read_at(s, pos, b, 12) != CORE_OK) return CORE_ECORRUPT;
        uint64_t len = dec_be64(b + 4);
        int64_t body = pos + 12;
        if (len > (uint64_t)(end - body)) return CORE_ECORRUPT;
        uint8_t v[4];
        if (dec_tag_eq(b, "FS  ") && len >= 4) {
            if (core_stream_read_exact(s, v, 4) != CORE_OK) return CORE_ECORRUPT;
            p->rate = dec_be32(v);
            p->have_rate = true;
        } else if (dec_tag_eq(b, "CHNL") && len >= 2) {
            if (core_stream_read_exact(s, v, 2) != CORE_OK) return CORE_ECORRUPT;
            p->channels = dec_be16(v);
            p->have_channels = true;
        } else if (dec_tag_eq(b, "CMPR") && len >= 4) {
            if (core_stream_read_exact(s, v, 4) != CORE_OK) return CORE_ECORRUPT;
            p->compressed = !dec_tag_eq(v, "DSD ");
        }
        pos = body + (int64_t)(len + (len & 1));
    }
    return CORE_OK;
}

static void *dff_open(core_stream_t *s, decoder_info_t *info, int *err) {
    uint8_t h[16];
    if (dec_read_at(s, 0, h, sizeof h) != CORE_OK || !dff_probe(h, sizeof h, "")) {
        *err = CORE_ECORRUPT;
        return NULL;
    }
    int64_t fsize = core_stream_size(s);
    uint64_t form = dec_be64(h + 4);
    int64_t end = 12 + (int64_t)CORE_MIN(form, (uint64_t)INT64_MAX / 2);
    if (fsize >= 0 && (end > fsize || form == 0)) end = fsize;  // size 0: writer never finalized it
    dff_prop_t prop = {0};
    int64_t data_start = -1, pos = 16;
    uint64_t data_size = 0;
    int r = CORE_OK;
    for (int guard = 0; guard < DFF_MAX_CHUNKS && pos + 12 <= end && r == CORE_OK; guard++) {
        uint8_t ck[12];
        if (dec_read_at(s, pos, ck, sizeof ck) != CORE_OK) break;
        uint64_t len = dec_be64(ck + 4);
        int64_t body = pos + 12;
        if (len > (uint64_t)INT64_MAX / 2) break;
        if (dec_tag_eq(ck, "PROP")) {
            r = dff_parse_prop(s, body, CORE_MIN(len, (uint64_t)(end - body)), &prop);
        } else if (dec_tag_eq(ck, "DSD ")) {
            data_start = body;
            data_size = CORE_MIN(len, (uint64_t)(end - body));
            break;
        } else if (dec_tag_eq(ck, "DST ")) {
            prop.compressed = true;
            break;
        }
        pos = body + (int64_t)(len + (len & 1));
    }
    uint32_t pcm_rate;
    if (r == CORE_OK && (prop.compressed || (prop.have_rate && !dop_rate(prop.rate, &pcm_rate)) ||
                         prop.channels > AUDIO_MAX_CHANNELS)) {
        CORE_LOGW(TAG, "DFF not supported: %s, %u ch, %u Hz", prop.compressed ? "DST" : "DSD", prop.channels,
                  (unsigned)prop.rate);
        r = CORE_EUNSUPPORTED;
    }
    if (r == CORE_OK && (data_start < 0 || !prop.have_rate || !prop.have_channels || prop.channels == 0)) {
        r = CORE_ECORRUPT;
    }
    if (r != CORE_OK) {
        *err = r;
        return NULL;
    }
    dff_t *d = core_calloc(1, sizeof *d);
    if (!d) {
        *err = CORE_ENOMEM;
        return NULL;
    }
    d->s = s;
    d->channels = (uint8_t)prop.channels;
    d->data_start = data_start;
    d->total = data_size / (2u * d->channels);
    if (core_stream_seek(s, data_start, DEC_SEEK_SET) != CORE_OK) {
        core_free(d);
        *err = CORE_EIO;
        return NULL;
    }
    fill_info(info, prop.rate, d->channels, d->total);
    return d;
}

static int32_t dff_read(void *state, int32_t *out, uint32_t max_frames) {
    dff_t *d = state;
    const uint32_t frame_bytes = 2u * d->channels;
    uint32_t done = 0;
    while (done < max_frames && d->pos < d->total) {
        uint32_t n = CORE_MIN(max_frames - done, DFF_RAW_BYTES / frame_bytes);
        if (n > d->total - d->pos) n = (uint32_t)(d->total - d->pos);
        size_t want = (size_t)n * frame_bytes, got = 0;
        while (got < want) {
            int32_t k = core_stream_read(d->s, d->raw + got, want - got);
            if (k < 0) return done ? (int32_t)done : k;
            if (k == 0) break;
            got += (size_t)k;
        }
        n = (uint32_t)(got / frame_bytes);
        int32_t *dst = out + (size_t)done * d->channels;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *src = d->raw + (size_t)i * frame_bytes;  // [c0 b0][c1 b0][c0 b1][c1 b1]
            for (uint8_t c = 0; c < d->channels; c++) {
                dst[(size_t)i * d->channels + c] = dop_word(d->pos + i, src[c], src[d->channels + c]);
            }
        }
        d->pos += n;
        done += n;
        if (got < want) {
            d->total = d->pos;
            break;
        }
    }
    return (int32_t)done;
}

static int dff_seek(void *state, uint64_t frame) {
    dff_t *d = state;
    if (frame > d->total) return CORE_EINVAL;
    int r = core_stream_seek(d->s, d->data_start + (int64_t)(frame * 2u * d->channels), DEC_SEEK_SET);
    if (r != CORE_OK) return r;
    d->pos = frame;
    return CORE_OK;
}

static void dff_close(void *state) { core_free(state); }

const decoder_backend_t dec_dff_backend = {CODEC_DFF, dff_probe, dff_open, dff_read, dff_seek, dff_close};
