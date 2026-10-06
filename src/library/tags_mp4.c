// SPDX-License-Identifier: Apache-2.0
// MP4 / M4A / M4B: iTunes-style metadata (moov/udta/meta/ilst), stream info from the
// first sample entry (mp4a -> AAC, alac -> ALAC with bit depth and rate), duration from
// mvhd, audiobook detection by brand ("M4B ") or media kind (stik = 2).
// Only box headers and small leaf boxes are read; mdat and sample tables are skipped.
#include <string.h>

#include "audio_player/decoder.h"
#include "lib_priv.h"

#define MP4_MAX_DEPTH 8
#define MP4_MAX_BOXES 512    // boxes per level
#define MP4_MAX_READS 4096   // box headers per file: a crafted file cannot keep the scan busy

typedef struct {
    core_stream_t *s;
    track_tags_t *t;
    bool got_entry;
    uint32_t reads;          // box headers read so far
} mp4ctx_t;

typedef struct {
    char type[4];
    int64_t body;  // first byte after the header
    int64_t end;   // first byte after the box
} box_t;

// Reads the box header at pos inside [pos, end). The box always ends after its header and
// no later than end, so every loop over boxes moves forward: a 64-bit size near 2^64 would
// otherwise wrap pos + size below pos and walk backwards (exponentially, through the levels).
static bool box_read(mp4ctx_t *c, int64_t pos, int64_t end, box_t *b) {
    uint8_t h[16];
    if (pos < 0 || pos + 8 > end || ++c->reads > MP4_MAX_READS) return false;
    if (tags_read_at(c->s, pos, h, 8) != CORE_OK) return false;
    uint64_t size = tags_rd_be32(h);
    int64_t hdr = 8;
    memcpy(b->type, h + 4, 4);
    if (size == 1) {
        if (pos + 16 > end || tags_read_at(c->s, pos + 8, h + 8, 8) != CORE_OK) return false;
        size = tags_rd_be64(h + 8);
        hdr = 16;
    } else if (size == 0) {
        size = (uint64_t)(end - pos);
    }
    if (size < (uint64_t)hdr) return false;
    const uint64_t room = (uint64_t)(end - pos);
    b->body = pos + hdr;
    b->end = pos + (int64_t)CORE_MIN(size, room);  // truncated files: clamp to the parent
    return b->end > pos && b->end >= b->body;
}

static bool is_type(const box_t *b, const char *t) { return memcmp(b->type, t, 4) == 0; }

// Read a text value (UTF-8 by the spec) into dst; fill or overwrite.
static void data_text(mp4ctx_t *c, int64_t off, int64_t len, char *dst, size_t cap) {
    uint8_t buf[TAG_TEXT_MAX * 2];
    size_t n = (size_t)CORE_MIN(len, (int64_t)sizeof buf);
    if (n == 0 || tags_read_at(c->s, off, buf, n) != CORE_OK) return;
    if (len > (int64_t)n) n = tagf_trim_partial_utf8(buf, n);
    tagf_set(dst, cap, buf, n, TEXT_ENC_AUTO_LEGACY, true);
}

static void ilst_item(mp4ctx_t *c, const box_t *item) {
    track_tags_t *t = c->t;
    char name[64];
    name[0] = 0;
    int64_t pos = item->body;
    for (int i = 0; i < 16; i++) {
        box_t b;
        if (!box_read(c, pos, item->end, &b)) break;
        pos = b.end;
        if (is_type(&b, "name") && b.end - b.body > 4) {
            size_t n = (size_t)CORE_MIN(b.end - b.body - 4, (int64_t)sizeof name - 1);
            if (tags_read_at(c->s, b.body + 4, name, n) == CORE_OK) name[n] = 0;
            continue;
        }
        if (!is_type(&b, "data") || b.end - b.body < 8) continue;
        uint8_t dh[8];
        if (tags_read_at(c->s, b.body, dh, 8) != CORE_OK) break;
        uint32_t dtype = tags_rd_be32(dh) & 0x00FFFFFF;
        int64_t voff = b.body + 8, vlen = b.end - voff;
        const char *ty = item->type;
        if (!memcmp(ty, "\xA9nam", 4)) {
            data_text(c, voff, vlen, t->title, sizeof t->title);
        } else if (!memcmp(ty, "\xA9" "ART", 4)) {
            data_text(c, voff, vlen, t->artist, sizeof t->artist);
        } else if (!memcmp(ty, "\xA9" "alb", 4)) {
            data_text(c, voff, vlen, t->album, sizeof t->album);
        } else if (!memcmp(ty, "aART", 4)) {
            data_text(c, voff, vlen, t->album_artist, sizeof t->album_artist);
        } else if (!memcmp(ty, "\xA9wrt", 4)) {
            data_text(c, voff, vlen, t->composer, sizeof t->composer);
        } else if (!memcmp(ty, "\xA9gen", 4)) {
            data_text(c, voff, vlen, t->genre, sizeof t->genre);
        } else if (!memcmp(ty, "\xA9" "day", 4)) {
            char y[32];
            y[0] = 0;
            data_text(c, voff, vlen, y, sizeof y);
            uint16_t v = tagf_parse_year(y);
            if (v) t->year = v;
        } else if ((!memcmp(ty, "trkn", 4) || !memcmp(ty, "disk", 4)) && vlen >= 6) {
            uint8_t v[6];
            if (tags_read_at(c->s, voff, v, 6) == CORE_OK) {
                bool trk = !memcmp(ty, "trkn", 4);
                uint16_t no = tags_rd_be16(v + 2), total = tags_rd_be16(v + 4);
                if (no) *(trk ? &t->track_no : &t->disc_no) = no;
                if (total) *(trk ? &t->track_total : &t->disc_total) = total;
            }
        } else if (!memcmp(ty, "gnre", 4) && vlen >= 2 && !t->genre[0]) {
            uint8_t v[2];
            if (tags_read_at(c->s, voff, v, 2) == CORE_OK) {
                uint16_t g = tags_rd_be16(v);
                const char *nm = g ? tags_id3v1_genre(g - 1u) : NULL;
                if (nm) core_strlcpy(t->genre, nm, sizeof t->genre);
            }
        } else if (!memcmp(ty, "stik", 4) && vlen >= 1) {
            uint8_t v;
            if (tags_read_at(c->s, voff, &v, 1) == CORE_OK && v == 2) t->is_audiobook_hint = true;
        } else if (!memcmp(ty, "covr", 4) && vlen > 4 && !t->cover_size) {
            uint8_t head[4];
            if (tags_read_at(c->s, voff, head, 4) == CORE_OK) {
                uint8_t kind = tags_image_type(head, 4);
                if (!kind) kind = dtype == 13 ? 1 : dtype == 14 ? 2 : 0;
                if (kind) {
                    t->cover_offset = (uint64_t)voff;
                    t->cover_size = (uint32_t)CORE_MIN(vlen, (int64_t)UINT32_MAX);
                    t->cover_mime = kind;
                }
            }
        } else if (!memcmp(ty, "----", 4) && name[0] && dtype == 1) {
            char value[64];
            value[0] = 0;
            data_text(c, voff, vlen, value, sizeof value);
            tags_apply_kv(t, name, value, true);
        }
        break;  // first data box only
    }
    if (t->genre[0]) tags_normalize_genre(t->genre, sizeof t->genre);
}

static void sample_entry(mp4ctx_t *c, const box_t *stsd) {
    // stsd: version/flags(4), entry count(4), then the first sample entry box.
    box_t e;
    if (c->got_entry || !box_read(c, stsd->body + 8, stsd->end, &e)) return;
    uint8_t d[28];
    if (e.end - e.body < 28 || tags_read_at(c->s, e.body, d, sizeof d) != CORE_OK) return;
    track_tags_t *t = c->t;
    c->got_entry = true;
    t->channels = (uint8_t)tags_rd_be16(d + 16);
    t->sample_rate = tags_rd_be32(d + 24) >> 16;
    if (is_type(&e, "mp4a")) {
        t->codec = CODEC_AAC;
        t->bits = 0;
    } else if (is_type(&e, "alac")) {
        t->codec = CODEC_ALAC;
        t->bits = (uint8_t)tags_rd_be16(d + 18);
        // The "alac" child box holds the real configuration (rates above 65535 Hz).
        box_t cfg;
        if (box_read(c, e.body + 28, e.end, &cfg) && is_type(&cfg, "alac") && cfg.end - cfg.body >= 28) {
            uint8_t a[28];
            if (tags_read_at(c->s, cfg.body, a, sizeof a) == CORE_OK) {
                t->bits = a[4 + 5];
                t->channels = a[4 + 9];
                t->sample_rate = tags_rd_be32(a + 4 + 20);
            }
        }
    }
}

static void walk(mp4ctx_t *c, int64_t pos, int64_t end, int depth) {
    if (depth > MP4_MAX_DEPTH) return;
    for (int i = 0; i < MP4_MAX_BOXES; i++) {
        box_t b;
        if (!box_read(c, pos, end, &b)) return;
        pos = b.end;
        if (is_type(&b, "moov") || is_type(&b, "trak") || is_type(&b, "mdia") || is_type(&b, "minf") ||
            is_type(&b, "stbl") || is_type(&b, "udta")) {
            walk(c, b.body, b.end, depth + 1);
        } else if (is_type(&b, "meta")) {
            // ISO full box (version/flags first) or QuickTime style (children directly).
            uint8_t peek[8];
            int64_t child = b.body;
            if (b.end - b.body >= 8 && tags_read_at(c->s, b.body, peek, 8) == CORE_OK && tags_rd_be32(peek) == 0) {
                child += 4;
            }
            walk(c, child, b.end, depth + 1);
        } else if (is_type(&b, "ilst")) {
            int64_t p = b.body;
            for (int k = 0; k < MP4_MAX_BOXES; k++) {
                box_t item;
                if (!box_read(c, p, b.end, &item)) break;
                p = item.end;
                ilst_item(c, &item);
            }
        } else if (is_type(&b, "stsd")) {
            sample_entry(c, &b);
        } else if (is_type(&b, "mvhd")) {
            uint8_t d[32];
            size_t n = (size_t)CORE_MIN(b.end - b.body, (int64_t)sizeof d);
            if (n >= 20 && tags_read_at(c->s, b.body, d, n) == CORE_OK) {
                uint32_t scale;
                uint64_t dur;
                if (d[0] == 1 && n >= 32) {
                    scale = tags_rd_be32(d + 20);
                    dur = tags_rd_be64(d + 24);
                } else {
                    scale = tags_rd_be32(d + 12);
                    dur = tags_rd_be32(d + 16);
                }
                if (scale && dur != UINT64_MAX && dur != 0xFFFFFFFFu) {
                    c->t->duration_ms = (uint32_t)CORE_MIN(dur * 1000 / scale, (uint64_t)UINT32_MAX);
                }
            }
        } else if (is_type(&b, "ftyp")) {
            uint8_t d[64];
            size_t n = (size_t)CORE_MIN(b.end - b.body, (int64_t)sizeof d);
            if (n >= 4 && tags_read_at(c->s, b.body, d, n) == CORE_OK) {
                for (size_t k = 0; k + 4 <= n; k += 4) {
                    if (k == 4) continue;  // minor version
                    if (!memcmp(d + k, "M4B ", 4)) c->t->is_audiobook_hint = true;
                }
            }
        }
    }
}

int tags_parse_mp4(core_stream_t *s, int64_t start, int64_t file_size, track_tags_t *t) {
    mp4ctx_t c = {s, t, false, 0};
    walk(&c, start, file_size, 0);
    if (t->codec == CODEC_UNKNOWN) t->codec = CODEC_AAC;
    return CORE_OK;
}
