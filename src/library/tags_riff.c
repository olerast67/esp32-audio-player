// SPDX-License-Identifier: Apache-2.0
// Chunked containers: WAV / RF64 / BW64 (fmt, data, LIST/INFO, "id3 " chunk), AIFF /
// AIFC (COMM, NAME, AUTH, "ID3 " chunk), DSF (fmt + ID3v2 via the metadata pointer),
// DSDIFF (PROP/FS/CHNL, DSD size, ID3 chunk, DIIN title/artist), and the stream
// headers of WavPack and Monkey's Audio (their tags are APEv2 at the file end).
#include <string.h>

#include "audio_player/decoder.h"
#include "lib_priv.h"

#define MAX_CHUNKS 256

static void set_duration(track_tags_t *t, uint64_t frames, uint32_t rate) {
    if (rate && frames) t->duration_ms = (uint32_t)CORE_MIN(frames * 1000 / rate, (uint64_t)UINT32_MAX);
}

// Text chunk (INFO subchunk, AIFF NAME/AUTH, DSDIFF DITI/DIAR) of unknown code page.
static void chunk_text(core_stream_t *s, int64_t off, uint32_t len, char *dst, size_t cap) {
    uint8_t buf[TAG_TEXT_MAX * 2];
    size_t n = CORE_MIN((size_t)len, sizeof buf);
    if (tags_read_at(s, off, buf, n) != CORE_OK) return;
    if (len > n) n = tagf_trim_partial_utf8(buf, n);
    tagf_set(dst, cap, buf, n, TEXT_ENC_AUTO_LEGACY, false);
}

// ================================================================== WAV ======
static void wav_info(core_stream_t *s, int64_t pos, int64_t end, track_tags_t *t) {
    for (int i = 0; i < MAX_CHUNKS && pos + 8 <= end; i++) {
        uint8_t h[8];
        if (tags_read_at(s, pos, h, 8) != CORE_OK) return;
        uint32_t len = tags_rd_le32(h + 4);
        int64_t data = pos + 8;
        if (data + len > end) len = (uint32_t)(end - data);
        char tmp[TAG_TEXT_MAX];
        tmp[0] = 0;
        if (!memcmp(h, "INAM", 4)) {
            chunk_text(s, data, len, t->title, sizeof t->title);
        } else if (!memcmp(h, "IART", 4)) {
            chunk_text(s, data, len, t->artist, sizeof t->artist);
        } else if (!memcmp(h, "IPRD", 4) || !memcmp(h, "IALB", 4)) {
            chunk_text(s, data, len, t->album, sizeof t->album);
        } else if (!memcmp(h, "IGNR", 4)) {
            chunk_text(s, data, len, t->genre, sizeof t->genre);
        } else if (!memcmp(h, "IMUS", 4) || !memcmp(h, "ICMS", 4)) {
            chunk_text(s, data, len, t->composer, sizeof t->composer);
        } else if (!memcmp(h, "ICRD", 4) || !memcmp(h, "IYER", 4)) {
            chunk_text(s, data, len, tmp, sizeof tmp);
            if (!t->year) t->year = tagf_parse_year(tmp);
        } else if (!memcmp(h, "ITRK", 4) || !memcmp(h, "IPRT", 4) || !memcmp(h, "TRCK", 4)) {
            chunk_text(s, data, len, tmp, sizeof tmp);
            tagf_parse_pair(tmp, &t->track_no, &t->track_total, false);
        }
        pos = data + len + (len & 1);
    }
}

int tags_parse_wav(core_stream_t *s, int64_t start, track_tags_t *t) {
    uint8_t h[12];
    if (tags_read_at(s, start, h, 12) != CORE_OK) return CORE_ECORRUPT;
    bool rf64 = memcmp(h, "RIFF", 4) != 0;
    int64_t size = core_stream_size(s);
    int64_t end = size;
    if (!rf64) end = CORE_MIN(size, start + 8 + (int64_t)tags_rd_le32(h + 4));
    int64_t pos = start + 12;
    uint64_t data_size = 0, ds64_data = 0;
    uint16_t block_align = 0;
    bool have_fmt = false;
    for (int i = 0; i < MAX_CHUNKS && pos + 8 <= end; i++) {
        uint8_t ch[8];
        if (tags_read_at(s, pos, ch, 8) != CORE_OK) break;
        uint64_t len = tags_rd_le32(ch + 4);
        int64_t data = pos + 8;
        if (!memcmp(ch, "ds64", 4) && len >= 16) {
            uint8_t d[16];
            if (tags_read_at(s, data, d, 16) == CORE_OK) ds64_data = tags_rd_le64(d + 8);
        } else if (!memcmp(ch, "fmt ", 4) && len >= 16) {
            uint8_t f[40];
            memset(f, 0, sizeof f);
            if (tags_read_at(s, data, f, (size_t)CORE_MIN(len, (uint64_t)sizeof f)) == CORE_OK) {
                uint16_t fmt = tags_rd_le16(f);
                t->channels = (uint8_t)tags_rd_le16(f + 2);
                t->sample_rate = tags_rd_le32(f + 4);
                block_align = tags_rd_le16(f + 12);
                t->bits = (uint8_t)tags_rd_le16(f + 14);
                if (fmt == 0xFFFE && len >= 26) {
                    uint16_t valid = tags_rd_le16(f + 18);
                    if (valid && valid <= t->bits) t->bits = (uint8_t)valid;
                }
                have_fmt = true;
            }
        } else if (!memcmp(ch, "data", 4)) {
            data_size = (len == 0xFFFFFFFFu && ds64_data) ? ds64_data : len;
            if (rf64 && ds64_data) len = ds64_data;
            if ((int64_t)(data + data_size) > size) data_size = (uint64_t)(size - data);
        } else if (!memcmp(ch, "LIST", 4) && len >= 4) {
            uint8_t type[4];
            if (tags_read_at(s, data, type, 4) == CORE_OK && !memcmp(type, "INFO", 4)) {
                wav_info(s, data + 4, CORE_MIN(end, data + (int64_t)len), t);
            }
        } else if (!memcmp(ch, "id3 ", 4) || !memcmp(ch, "ID3 ", 4)) {
            uint32_t total;
            tags_parse_id3v2(s, data, t, &total);
        }
        pos = data + (int64_t)len + (int64_t)(len & 1);
    }
    if (!have_fmt) return CORE_ECORRUPT;
    if (block_align) set_duration(t, data_size / block_align, t->sample_rate);
    return CORE_OK;
}

// ================================================================= AIFF ======
// 80-bit IEEE extended sample rate -> integer Hz (integer math only).
static uint32_t ext80_to_u32(const uint8_t *p) {
    int exp = ((p[0] & 0x7F) << 8 | p[1]) - 16383;
    uint64_t mant = tags_rd_be64(p + 2);
    if ((p[0] & 0x80) || exp < 0 || exp > 31) return 0;
    return (uint32_t)(mant >> (63 - exp));
}

int tags_parse_aiff(core_stream_t *s, int64_t start, track_tags_t *t) {
    uint8_t h[12];
    if (tags_read_at(s, start, h, 12) != CORE_OK) return CORE_ECORRUPT;
    int64_t end = CORE_MIN(core_stream_size(s), start + 8 + (int64_t)tags_rd_be32(h + 4));
    int64_t pos = start + 12;
    uint32_t frames = 0;
    bool have_comm = false;
    for (int i = 0; i < MAX_CHUNKS && pos + 8 <= end; i++) {
        uint8_t ch[8];
        if (tags_read_at(s, pos, ch, 8) != CORE_OK) break;
        uint32_t len = tags_rd_be32(ch + 4);
        int64_t data = pos + 8;
        if (!memcmp(ch, "COMM", 4) && len >= 18) {
            uint8_t c[18];
            if (tags_read_at(s, data, c, sizeof c) == CORE_OK) {
                t->channels = (uint8_t)tags_rd_be16(c);
                frames = tags_rd_be32(c + 2);
                t->bits = (uint8_t)tags_rd_be16(c + 6);
                t->sample_rate = ext80_to_u32(c + 8);
                have_comm = true;
            }
        } else if (!memcmp(ch, "ID3 ", 4) || !memcmp(ch, "id3 ", 4)) {
            uint32_t total;
            tags_parse_id3v2(s, data, t, &total);
        } else if (!memcmp(ch, "NAME", 4)) {
            chunk_text(s, data, len, t->title, sizeof t->title);
        } else if (!memcmp(ch, "AUTH", 4)) {
            chunk_text(s, data, len, t->artist, sizeof t->artist);
        }
        pos = data + (int64_t)len + (len & 1);
    }
    if (!have_comm) return CORE_ECORRUPT;
    set_duration(t, frames, t->sample_rate);
    return CORE_OK;
}

// ================================================================== DSF ======
int tags_parse_dsf(core_stream_t *s, int64_t start, track_tags_t *t) {
    uint8_t d[28 + 52];
    if (tags_read_at(s, start, d, sizeof d) != CORE_OK) return CORE_ECORRUPT;
    uint64_t meta = tags_rd_le64(d + 20);
    const uint8_t *f = d + 28;
    if (memcmp(f, "fmt ", 4) != 0) return CORE_ECORRUPT;
    t->channels = (uint8_t)tags_rd_le32(f + 24);
    t->sample_rate = tags_rd_le32(f + 28);
    t->bits = 1;
    set_duration(t, tags_rd_le64(f + 36), t->sample_rate);
    int64_t size = core_stream_size(s);
    if (meta && (int64_t)meta + 10 <= size) {
        uint32_t total;
        tags_parse_id3v2(s, (int64_t)meta, t, &total);
    }
    return CORE_OK;
}

// ============================================================== DSDIFF =======
int tags_parse_dff(core_stream_t *s, int64_t start, track_tags_t *t) {
    uint8_t h[16];
    if (tags_read_at(s, start, h, 16) != CORE_OK || memcmp(h + 12, "DSD ", 4) != 0) return CORE_ECORRUPT;
    int64_t end = CORE_MIN(core_stream_size(s), start + 12 + (int64_t)tags_rd_be64(h + 4));
    int64_t pos = start + 16;
    uint64_t dsd_bytes = 0;
    t->bits = 1;
    for (int i = 0; i < MAX_CHUNKS && pos + 12 <= end; i++) {
        uint8_t ch[12];
        if (tags_read_at(s, pos, ch, 12) != CORE_OK) break;
        uint64_t len = tags_rd_be64(ch + 4);
        int64_t data = pos + 12;
        if (!memcmp(ch, "PROP", 4)) {
            // Sub-chunks after the 4-byte "SND " id.
            int64_t p = data + 4, pend = CORE_MIN(end, data + (int64_t)len);
            for (int k = 0; k < 32 && p + 12 <= pend; k++) {
                uint8_t sh[16];
                if (tags_read_at(s, p, sh, 16) != CORE_OK) break;
                uint64_t sl = tags_rd_be64(sh + 4);
                if (!memcmp(sh, "FS  ", 4)) t->sample_rate = tags_rd_be32(sh + 12);
                if (!memcmp(sh, "CHNL", 4)) t->channels = (uint8_t)tags_rd_be16(sh + 12);
                p += 12 + (int64_t)sl + (int64_t)(sl & 1);
            }
        } else if (!memcmp(ch, "DSD ", 4)) {
            dsd_bytes = len;
        } else if (!memcmp(ch, "ID3 ", 4)) {
            uint32_t total;
            tags_parse_id3v2(s, data, t, &total);
        } else if (!memcmp(ch, "DIIN", 4)) {
            int64_t p = data, pend = CORE_MIN(end, data + (int64_t)len);
            for (int k = 0; k < 16 && p + 12 <= pend; k++) {
                uint8_t sh[16];
                if (tags_read_at(s, p, sh, 16) != CORE_OK) break;
                uint64_t sl = tags_rd_be64(sh + 4);
                uint32_t count = tags_rd_be32(sh + 12);
                if (!memcmp(sh, "DITI", 4)) chunk_text(s, p + 16, count, t->title, sizeof t->title);
                if (!memcmp(sh, "DIAR", 4)) chunk_text(s, p + 16, count, t->artist, sizeof t->artist);
                p += 12 + (int64_t)sl + (int64_t)(sl & 1);
            }
        }
        pos = data + (int64_t)len + (int64_t)(len & 1);
    }
    if (t->channels && t->sample_rate) set_duration(t, dsd_bytes * 8 / t->channels, t->sample_rate);
    return CORE_OK;
}

// ============================================================== WavPack ======
int tags_parse_wavpack(core_stream_t *s, int64_t start, track_tags_t *t) {
    static const uint32_t rates[15] = {6000,  8000,  9600,  11025, 12000, 16000, 22050, 24000,
                                       32000, 44100, 48000, 64000, 88200, 96000, 192000};
    uint8_t h[32];
    if (tags_read_at(s, start, h, sizeof h) != CORE_OK) return CORE_ECORRUPT;
    uint32_t flags = tags_rd_le32(h + 24);
    uint32_t total = tags_rd_le32(h + 12);
    uint32_t sri = (flags >> 23) & 0xF;
    if (sri < 15) t->sample_rate = rates[sri];
    t->bits = (flags & 0x80) ? 32 : (uint8_t)(((flags & 3) + 1) * 8);
    t->channels = (flags & 4) ? 1 : 2;
    if (total != 0xFFFFFFFFu) {
        uint64_t samples = total | (uint64_t)h[11] << 32;  // total_samples_u8 (v4.07+)
        set_duration(t, samples, t->sample_rate);
    }
    return CORE_OK;
}

// ======================================================== Monkey's Audio ======
int tags_parse_ape(core_stream_t *s, int64_t start, track_tags_t *t) {
    uint8_t d[52];
    if (tags_read_at(s, start, d, sizeof d) != CORE_OK) return CORE_ECORRUPT;
    uint16_t version = tags_rd_le16(d + 4);
    if (version < 3980) return CORE_OK;  // old header layout: tags only
    uint32_t desc_bytes = tags_rd_le32(d + 8);
    uint8_t h[24];
    if (tags_read_at(s, start + desc_bytes, h, sizeof h) != CORE_OK) return CORE_ECORRUPT;
    uint32_t blocks_per_frame = tags_rd_le32(h + 4), final_blocks = tags_rd_le32(h + 8);
    uint32_t frames = tags_rd_le32(h + 12);
    t->bits = (uint8_t)tags_rd_le16(h + 16);
    t->channels = (uint8_t)tags_rd_le16(h + 18);
    t->sample_rate = tags_rd_le32(h + 20);
    if (frames) set_duration(t, (uint64_t)(frames - 1) * blocks_per_frame + final_blocks, t->sample_rate);
    return CORE_OK;
}
