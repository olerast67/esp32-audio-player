// SPDX-License-Identifier: Apache-2.0
// WAV backend: RIFF/WAVE, RF64 and BW64 (ds64 sizes), integer PCM 8..32 bit, IEEE float 32/64 bit,
// WAVE_FORMAT_EXTENSIBLE. Unknown chunks are skipped (with the RIFF odd-size pad byte), "fmt " may
// follow "data", and unfinalized headers (data size 0 or 0xFFFFFFFF) play up to the end of the file.
#include "decoders/dec_internal.h"
#include "decoders/dec_pcm.h"

#define TAG "dec_wav"

#define WAV_MAX_CHUNKS 4096  // bound on the chunk walk for corrupt files

enum { WAV_FMT_PCM = 0x0001, WAV_FMT_FLOAT = 0x0003, WAV_FMT_EXTENSIBLE = 0xFFFE };

// KSDATAFORMAT_SUBTYPE_* GUIDs are {0000xxxx-0000-0010-8000-00AA00389B71}; xxxx is the format tag.
static const uint8_t k_subformat_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                             0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

typedef struct {
    uint16_t tag, channels, block_align, bits, valid_bits;
    uint32_t rate;
} wav_fmt_t;

static bool wav_probe(const uint8_t *h, size_t n, const char *ext) {
    (void)ext;
    return n >= 12 && (dec_tag_eq(h, "RIFF") || dec_tag_eq(h, "RF64") || dec_tag_eq(h, "BW64")) &&
           dec_tag_eq(h + 8, "WAVE");
}

static int parse_fmt(const uint8_t *b, uint32_t len, wav_fmt_t *f) {
    if (len < 16) return CORE_ECORRUPT;
    f->tag = dec_le16(b);
    f->channels = dec_le16(b + 2);
    f->rate = dec_le32(b + 4);
    f->block_align = dec_le16(b + 12);
    f->bits = dec_le16(b + 14);
    f->valid_bits = f->bits;
    if (f->tag == WAV_FMT_EXTENSIBLE) {
        if (len < 40 || dec_le16(b + 16) < 22) return CORE_ECORRUPT;
        f->valid_bits = dec_le16(b + 18);
        if (memcmp(b + 26, k_subformat_tail, sizeof k_subformat_tail) != 0) return CORE_EUNSUPPORTED;
        f->tag = dec_le16(b + 24);
        if (f->valid_bits == 0 || f->valid_bits > f->bits) f->valid_bits = f->bits;  // lenient writers
    }
    return CORE_OK;
}

// Validates the format and fills the sample layout and the number of significant bits.
static int layout_from_fmt(const wav_fmt_t *f, pcm_layout_t *l, unsigned *sig_bits) {
    if (f->channels == 0 || f->block_align == 0) return CORE_ECORRUPT;
    if (f->channels > AUDIO_MAX_CHANNELS) {
        CORE_LOGW(TAG, "%u channels not supported", f->channels);
        return CORE_EUNSUPPORTED;
    }
    if (f->rate < 1000 || f->rate > 768000) return CORE_EUNSUPPORTED;
    if (f->block_align % f->channels) return CORE_ECORRUPT;
    unsigned container = f->block_align / f->channels;
    l->big_endian = false;
    l->bytes = (uint8_t)CORE_MIN(container, 255u);
    switch (f->tag) {
    case WAV_FMT_PCM:
        if (f->bits == 0 || f->bits > 32 || container > 4 || container * 8 < f->bits) return CORE_ECORRUPT;
        l->kind = container == 1 ? PCM_UINT : PCM_INT;  // WAV stores 8-bit (and narrower) as unsigned
        *sig_bits = f->valid_bits;
        break;
    case WAV_FMT_FLOAT:
        if (container != 4 && container != 8) return CORE_EUNSUPPORTED;
        l->kind = PCM_FLOAT;
        *sig_bits = 32;
        break;
    default:
        CORE_LOGW(TAG, "format tag 0x%04x not supported", f->tag);
        return CORE_EUNSUPPORTED;
    }
    return pcm_layout_valid(l) ? CORE_OK : CORE_EUNSUPPORTED;
}

static void *wav_open(core_stream_t *s, decoder_info_t *info, int *err) {
    uint8_t h[12];
    if (dec_read_at(s, 0, h, sizeof h) != CORE_OK || !wav_probe(h, sizeof h, "")) {
        *err = CORE_ECORRUPT;
        return NULL;
    }
    bool is64 = !dec_tag_eq(h, "RIFF");
    uint32_t riff_size = dec_le32(h + 4);
    int64_t fsize = core_stream_size(s);

    wav_fmt_t fmt = {0};
    bool have_fmt = false, have_ds64 = false;
    uint64_t ds64_data = 0, data_size = 0;
    int64_t data_start = -1, pos = 12;
    int fmt_err = CORE_OK;

    for (int guard = 0; guard < WAV_MAX_CHUNKS; guard++) {
        uint8_t ck[8];
        if (fsize >= 0 && pos + 8 > fsize) break;
        if (dec_read_at(s, pos, ck, sizeof ck) != CORE_OK) break;
        uint64_t size = dec_le32(ck + 4);
        int64_t body = pos + 8;
        if (dec_tag_eq(ck, "ds64") && is64 && size >= 24) {
            uint8_t b[24];
            if (core_stream_read_exact(s, b, sizeof b) != CORE_OK) break;
            ds64_data = dec_le64(b + 8);
            have_ds64 = true;
        } else if (dec_tag_eq(ck, "fmt ") && !have_fmt) {
            uint8_t b[40] = {0};
            uint32_t n = (uint32_t)CORE_MIN(size, (uint64_t)sizeof b);
            if (core_stream_read_exact(s, b, n) != CORE_OK) break;
            fmt_err = parse_fmt(b, n, &fmt);
            have_fmt = true;
        } else if (dec_tag_eq(ck, "data") && data_start < 0) {
            if (is64 && size == 0xFFFFFFFFu && have_ds64) size = ds64_data;
            data_start = body;
            data_size = size;
            if (have_fmt || size == 0xFFFFFFFFu) break;
            // "fmt " after "data" is unusual but legal: keep walking past the audio.
        }
        uint64_t adv = size + (size & 1);  // chunks are padded to an even size
        if (adv > (uint64_t)(INT64_MAX - body)) break;
        pos = body + (int64_t)adv;
    }

    if (!have_fmt || data_start < 0) {
        *err = CORE_ECORRUPT;
        return NULL;
    }
    pcm_layout_t layout;
    unsigned sig_bits = 0;
    if (fmt_err == CORE_OK) fmt_err = layout_from_fmt(&fmt, &layout, &sig_bits);
    if (fmt_err != CORE_OK) {
        *err = fmt_err;
        return NULL;
    }

    if (fsize >= 0) {
        uint64_t avail = fsize > data_start ? (uint64_t)(fsize - data_start) : 0;
        bool unfinalized = (!is64 && data_size == 0xFFFFFFFFu) ||
                           (data_size == 0 && (riff_size == 0 || riff_size == 0xFFFFFFFFu));
        if (unfinalized || data_size > avail) data_size = avail;
    } else if (!is64 && data_size == 0xFFFFFFFFu) {
        data_size = UINT64_MAX;  // unknown length on an unsized stream: play until the stream ends
    }

    uint64_t frames = data_size / fmt.block_align;
    pcm_reader_t *r = pcm_reader_create(s, &layout, (uint8_t)fmt.channels, data_start, frames, err);
    if (!r) return NULL;

    info->fmt.sample_rate = fmt.rate;
    info->fmt.channels = (uint8_t)fmt.channels;
    info->fmt.bits = dec_bits_class(sig_bits);
    info->fmt.dop = false;
    info->total_frames = data_size == UINT64_MAX ? 0 : frames;
    info->bitrate_kbps = (uint32_t)((uint64_t)fmt.rate * fmt.block_align * 8 / 1000);
    info->seekable = fsize >= 0;
    return r;
}

const decoder_backend_t dec_wav_backend = {
    CODEC_WAV, wav_probe, wav_open, pcm_reader_read, pcm_reader_seek, pcm_reader_close,
};
