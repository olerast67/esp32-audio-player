// SPDX-License-Identifier: Apache-2.0
// FLAC metadata blocks (STREAMINFO, VORBIS_COMMENT, PICTURE) and Ogg streams with
// Vorbis, Opus or FLAC: page parsing, packet reassembly across pages, comment
// header parsing and the duration from the last granule position.
#include <string.h>

#include "audio_player/decoder.h"
#include "lib_priv.h"

// ======================================================= Vorbis comments =====
int tags_parse_vorbis_comment(tag_src_t *src, track_tags_t *t) {
    uint8_t b4[4];
    if (src->read(src, b4, 4) != 4) return CORE_ECORRUPT;
    if (src->skip(src, tags_rd_le32(b4)) != CORE_OK) return CORE_ECORRUPT;  // vendor string
    if (src->read(src, b4, 4) != 4) return CORE_ECORRUPT;
    uint32_t count = tags_rd_le32(b4);
    uint8_t buf[TAG_RAW_MAX + 1];
    for (uint32_t i = 0; i < count && i < 100000; i++) {
        if (src->read(src, b4, 4) != 4) return CORE_ECORRUPT;
        uint32_t len = tags_rd_le32(b4);
        size_t n = CORE_MIN((size_t)len, (size_t)TAG_RAW_MAX);
        if (src->read(src, buf, n) != (int32_t)n) return CORE_ECORRUPT;
        if (len > n && src->skip(src, len - n) != CORE_OK) return CORE_ECORRUPT;
        if (len > n) n = tagf_trim_partial_utf8(buf, n);
        buf[n] = 0;
        char *eq = memchr(buf, '=', n);
        if (!eq || eq == (char *)buf || eq - (char *)buf > 64) continue;
        *eq = 0;
        // First value of a repeated key wins (ARTIST=a, ARTIST=b -> "a").
        tags_apply_kv(t, (const char *)buf, eq + 1, false);
    }
    return CORE_OK;
}

// ================================================================= FLAC ======
static void flac_streaminfo(const uint8_t *d, track_tags_t *t) {
    const uint8_t *b = d + 10;
    uint32_t rate = (uint32_t)b[0] << 12 | (uint32_t)b[1] << 4 | b[2] >> 4;
    uint8_t ch = (uint8_t)(((b[2] >> 1) & 7) + 1);
    uint8_t bps = (uint8_t)((((b[2] & 1) << 4) | (b[3] >> 4)) + 1);
    uint64_t total = (uint64_t)(b[3] & 0x0F) << 32 | tags_rd_be32(b + 4);
    if (!rate) return;
    t->sample_rate = rate;
    t->channels = ch;
    t->bits = bps;
    if (total) t->duration_ms = (uint32_t)(total * 1000 / rate);
}

// PICTURE block at the current stream position. Front cover (type 3) wins.
static void flac_picture(core_stream_t *s, uint32_t block_len, track_tags_t *t, bool *have_front) {
    int64_t base = core_stream_tell(s);
    uint8_t b[8];
    if (block_len < 32 || core_stream_read_exact(s, b, 8) != CORE_OK) return;
    uint32_t type = tags_rd_be32(b), mime_len = tags_rd_be32(b + 4);
    if (mime_len > 128 || 8u + mime_len + 4 > block_len) return;
    char mime[129];
    if (core_stream_read_exact(s, mime, mime_len) != CORE_OK) return;
    mime[mime_len] = 0;
    if (core_stream_read_exact(s, b, 4) != CORE_OK) return;
    uint32_t desc_len = tags_rd_be32(b);
    if ((uint64_t)8 + mime_len + 4 + desc_len + 20 > block_len) return;
    if (core_stream_skip(s, (int64_t)desc_len + 16) != CORE_OK) return;
    if (core_stream_read_exact(s, b, 4) != CORE_OK) return;
    uint32_t data_len = tags_rd_be32(b);
    int64_t data_off = core_stream_tell(s);
    if (data_len < 4 || data_off - base + data_len > block_len) return;
    uint8_t head[4];
    if (core_stream_read_exact(s, head, 4) != CORE_OK) return;
    uint8_t kind = tags_image_type(head, 4);
    if (!kind) kind = strstr(mime, "png") ? 2 : (strstr(mime, "jp") ? 1 : 0);
    if (!kind) return;
    bool front = type == 3;
    if (t->cover_size && (*have_front || !front)) return;
    t->cover_offset = (uint64_t)data_off;
    t->cover_size = data_len;
    t->cover_mime = kind;
    *have_front = front;
}

int tags_parse_flac(core_stream_t *s, int64_t start, track_tags_t *t) {
    t->codec = CODEC_FLAC;
    int64_t pos = start + 4;
    int64_t size = core_stream_size(s);
    bool have_front = false;
    for (int i = 0; i < 256; i++) {
        uint8_t bh[4];
        if (tags_read_at(s, pos, bh, 4) != CORE_OK) return CORE_ECORRUPT;
        bool last = bh[0] & 0x80;
        uint8_t type = bh[0] & 0x7F;
        uint32_t len = (uint32_t)bh[1] << 16 | (uint32_t)bh[2] << 8 | bh[3];
        pos += 4;
        if (type == 127 || (size > 0 && pos + len > size)) return CORE_ECORRUPT;
        if (type == 0 && len >= 34) {
            uint8_t d[34];
            if (tags_read_at(s, pos, d, sizeof d) != CORE_OK) return CORE_ECORRUPT;
            flac_streaminfo(d, t);
        } else if (type == 4) {
            if (core_stream_seek(s, pos, 0) != CORE_OK) return CORE_EIO;
            tag_region_t r;
            tag_region_init(&r, s, len);
            tags_parse_vorbis_comment(&r.base, t);
        } else if (type == 6) {
            if (core_stream_seek(s, pos, 0) != CORE_OK) return CORE_EIO;
            flac_picture(s, len, t, &have_front);
        }
        pos += len;
        if (last) break;
    }
    return CORE_OK;
}

// ================================================================== Ogg ======
typedef struct {
    tag_src_t base;
    core_stream_t *s;
    uint32_t serial;
    uint8_t lacing[255];
    int nseg, seg;       // segments of the current page, index of the next one
    uint32_t seg_left;   // unread bytes of the current segment
    bool seg_final;      // the current segment ends the packet (lacing < 255)
    bool packet_done;
    bool error;
    int pages;
} ogg_rd_t;

// Read the next page header of our logical stream (other streams are skipped).
static bool ogg_page(ogg_rd_t *r, bool first) {
    while (!r->error && r->pages < 4096) {
        uint8_t h[27];
        if (core_stream_read_exact(r->s, h, sizeof h) != CORE_OK || memcmp(h, "OggS", 4) != 0 || h[4] != 0) {
            r->error = true;
            return false;
        }
        int nseg = h[26];
        if (core_stream_read_exact(r->s, r->lacing, (size_t)nseg) != CORE_OK) {
            r->error = true;
            return false;
        }
        r->pages++;
        uint32_t serial = tags_rd_le32(h + 14);
        if (first) r->serial = serial;
        if (serial != r->serial) {
            uint32_t body = 0;
            for (int i = 0; i < nseg; i++) body += r->lacing[i];
            if (core_stream_skip(r->s, body) != CORE_OK) {
                r->error = true;
                return false;
            }
            continue;
        }
        r->nseg = nseg;
        r->seg = 0;
        return true;
    }
    r->error = true;
    return false;
}

static bool ogg_next_segment(ogg_rd_t *r) {
    while (r->seg >= r->nseg) {
        if (!ogg_page(r, false)) return false;
    }
    uint8_t l = r->lacing[r->seg++];
    r->seg_left = l;
    r->seg_final = l < 255;
    if (l == 0) r->packet_done = true;
    return true;
}

static int32_t ogg_read(tag_src_t *src, void *buf, size_t len) {
    ogg_rd_t *r = (ogg_rd_t *)src;
    uint8_t *out = buf;
    size_t done = 0;
    while (done < len && !r->error) {
        if (r->seg_left) {
            size_t n = CORE_MIN(len - done, (size_t)r->seg_left);
            if (core_stream_read_exact(r->s, out + done, n) != CORE_OK) {
                r->error = true;
                break;
            }
            done += n;
            r->seg_left -= (uint32_t)n;
            if (!r->seg_left && r->seg_final) r->packet_done = true;
            continue;
        }
        if (r->packet_done || !ogg_next_segment(r)) break;
    }
    return (int32_t)done;
}

static int ogg_skip(tag_src_t *src, uint64_t len) {
    ogg_rd_t *r = (ogg_rd_t *)src;
    while (len && !r->error) {
        if (r->seg_left) {
            uint32_t n = (uint32_t)CORE_MIN(len, (uint64_t)r->seg_left);
            if (core_stream_skip(r->s, n) != CORE_OK) {
                r->error = true;
                break;
            }
            len -= n;
            r->seg_left -= n;
            if (!r->seg_left && r->seg_final) r->packet_done = true;
            continue;
        }
        if (r->packet_done || !ogg_next_segment(r)) break;
    }
    return len ? CORE_EOF : CORE_OK;
}

// Finish the current packet and position at the start of the next one.
static bool ogg_next_packet(ogg_rd_t *r) {
    while (!r->packet_done && !r->error) {
        if (r->seg_left) {
            if (ogg_skip(&r->base, r->seg_left) != CORE_OK) return false;
        } else if (!ogg_next_segment(r)) {
            return false;
        }
    }
    r->packet_done = false;
    r->seg_left = 0;
    r->seg_final = false;
    return !r->error;
}

// Granule position of the last page of the stream (searching backwards from the end).
static bool ogg_last_granule(core_stream_t *s, int64_t size, uint32_t serial, uint64_t *granule) {
    uint8_t buf[4096 + 27];
    int64_t end = size;
    while (end > 0 && size - end < 512 * 1024) {
        int64_t from = end > 4096 ? end - 4096 : 0;
        size_t n = (size_t)(CORE_MIN(size, end + 27) - from);
        if (tags_read_at(s, from, buf, n) != CORE_OK) return false;
        for (size_t i = (size_t)(end - from); i-- > 0;) {
            if (i + 27 > n) continue;
            if (memcmp(buf + i, "OggS", 4) != 0 || buf[i + 4] != 0) continue;
            if (tags_rd_le32(buf + i + 14) != serial) continue;
            uint64_t g = tags_rd_le64(buf + i + 6);
            if (g == UINT64_MAX) continue;  // page without a finished packet
            *granule = g;
            return true;
        }
        end = from;
    }
    return false;
}

int tags_parse_ogg(core_stream_t *s, int64_t start, int64_t size, track_tags_t *t) {
    if (core_stream_seek(s, start, 0) != CORE_OK) return CORE_EIO;
    ogg_rd_t r;
    memset(&r, 0, sizeof r);
    r.base.read = ogg_read;
    r.base.skip = ogg_skip;
    r.s = s;
    if (!ogg_page(&r, true)) return CORE_ECORRUPT;

    uint8_t id[64];
    memset(id, 0, sizeof id);
    int32_t n = ogg_read(&r.base, id, sizeof id);
    if (n < 8) return CORE_ECORRUPT;
    uint32_t pre_skip = 0;
    if (n >= 16 && !memcmp(id, "\x01vorbis", 7)) {
        t->codec = CODEC_VORBIS;
        t->channels = id[11];
        t->sample_rate = tags_rd_le32(id + 12);
        if (!ogg_next_packet(&r)) return CORE_ECORRUPT;
        uint8_t magic[7];
        if (ogg_read(&r.base, magic, 7) != 7 || memcmp(magic, "\x03vorbis", 7) != 0) return CORE_ECORRUPT;
        tags_parse_vorbis_comment(&r.base, t);
    } else if (n >= 19 && !memcmp(id, "OpusHead", 8)) {
        t->codec = CODEC_OPUS;
        t->channels = id[9];
        pre_skip = tags_rd_le16(id + 10);
        t->sample_rate = 48000;  // Opus always decodes at 48 kHz
        if (!ogg_next_packet(&r)) return CORE_ECORRUPT;
        uint8_t magic[8];
        if (ogg_read(&r.base, magic, 8) != 8 || memcmp(magic, "OpusTags", 8) != 0) return CORE_ECORRUPT;
        tags_parse_vorbis_comment(&r.base, t);
    } else if (n >= 51 && !memcmp(id, "\x7F" "FLAC", 5) && !memcmp(id + 9, "fLaC", 4)) {
        t->codec = CODEC_FLAC;
        flac_streaminfo(id + 17, t);
        // Following header packets are FLAC metadata blocks; find VORBIS_COMMENT.
        for (int i = 0; i < 16; i++) {
            if (!ogg_next_packet(&r)) break;
            uint8_t bh[4];
            if (ogg_read(&r.base, bh, 4) != 4) break;
            if ((bh[0] & 0x7F) == 4) {
                tags_parse_vorbis_comment(&r.base, t);
                break;
            }
            if (bh[0] & 0x80) break;  // last metadata block
        }
    } else {
        return CORE_EUNSUPPORTED;  // Speex, Theora, ...
    }

    uint64_t g;
    if (t->sample_rate && ogg_last_granule(s, size, r.serial, &g)) {
        if (g > pre_skip) g -= pre_skip;
        t->duration_ms = (uint32_t)(g * 1000 / t->sample_rate);
    }
    return CORE_OK;
}
