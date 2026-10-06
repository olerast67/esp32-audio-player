// SPDX-License-Identifier: Apache-2.0
// ID3v2.2 / 2.3 / 2.4, ID3v1 / 1.1, APEv2 and MPEG audio stream info (MP3 Xing/Info,
// LAME delay/padding, VBRI, CBR estimate; ADTS AAC basics).
//
// ID3v2 is parsed as a stream: frames we do not need are skipped by size, wanted
// text frames are read up to TAG_RAW_MAX bytes. Unsynchronisation is undone on the
// fly (tag-wide in v2.2/v2.3, per frame in v2.4). Text frames with encoding 0 are
// collected raw and decoded together at the end, so the CP1251/Latin-1 heuristic
// sees all fields of the tag at once ("Кино" alone is short; title + artist + album
// is a reliable sample).
#include <math.h>
#include <string.h>

#include "audio_player/decoder.h"
#include "lib_priv.h"

// ================================================================ ID3v2 ======
typedef struct {
    core_stream_t *s;
    int64_t pos;   // absolute raw position of the next byte to fetch into buf
    int64_t end;   // absolute raw end of the tag body
    bool unsync;   // tag-wide unsynchronisation (v2.2 / v2.3)
    bool prev_ff;  // last delivered byte was 0xFF (for dropping the stuffed 0x00)
    bool error;
    uint8_t buf[512];
    size_t blen, bpos;
} id3r_t;

static int64_t id3r_raw_pos(const id3r_t *r) { return r->pos - (int64_t)(r->blen - r->bpos); }
static int64_t id3r_raw_left(const id3r_t *r) { return r->end - id3r_raw_pos(r); }

static bool id3r_fill(id3r_t *r) {
    if (r->bpos < r->blen) return true;
    if (r->error || r->pos >= r->end) return false;
    size_t want = (size_t)CORE_MIN((int64_t)sizeof r->buf, r->end - r->pos);
    if (core_stream_seek(r->s, r->pos, 0) != CORE_OK) {
        r->error = true;
        return false;
    }
    int32_t n = core_stream_read(r->s, r->buf, want);
    if (n <= 0) {
        r->error = true;
        return false;
    }
    r->pos += n;
    r->blen = (size_t)n;
    r->bpos = 0;
    return true;
}

// Deliver up to len bytes (de-unsynchronised when needed). out may be NULL (skip).
static size_t id3r_read(id3r_t *r, uint8_t *out, size_t len) {
    size_t done = 0;
    while (done < len && id3r_fill(r)) {
        if (!r->unsync) {
            size_t n = CORE_MIN(len - done, r->blen - r->bpos);
            if (out) memcpy(out + done, r->buf + r->bpos, n);
            r->bpos += n;
            done += n;
            continue;
        }
        uint8_t b = r->buf[r->bpos++];
        if (r->prev_ff && b == 0x00) {
            r->prev_ff = false;
            continue;
        }
        r->prev_ff = b == 0xFF;
        if (out) out[done] = b;
        done++;
    }
    return done;
}

static void id3r_skip(id3r_t *r, uint64_t len) {
    if (r->unsync) {
        while (len && !r->error) {
            size_t n = id3r_read(r, NULL, (size_t)CORE_MIN(len, (uint64_t)4096));
            if (!n) break;
            len -= n;
        }
        return;
    }
    size_t inbuf = r->blen - r->bpos;
    if (len <= inbuf) {
        r->bpos += (size_t)len;
        return;
    }
    len -= inbuf;
    r->bpos = r->blen = 0;
    r->pos = (int64_t)CORE_MIN((uint64_t)r->end, (uint64_t)r->pos + len);
}

static uint32_t syncsafe32(const uint8_t *p) {
    return (uint32_t)(p[0] & 0x7F) << 21 | (uint32_t)(p[1] & 0x7F) << 14 | (uint32_t)(p[2] & 0x7F) << 7 | (p[3] & 0x7F);
}

enum { F_TITLE = 0, F_ARTIST, F_ALBUM, F_ALBUM_ARTIST, F_GENRE, F_COMPOSER, F_COUNT };
#define RAW_FIELD_MAX (TAG_TEXT_MAX * 2)

typedef struct {
    track_tags_t *t;
    int version;
    uint8_t raw[F_COUNT][RAW_FIELD_MAX];  // encoding-0 values, decoded together at the end
    uint16_t raw_len[F_COUNT];
    bool cover_front;
    uint8_t data[TAG_RAW_MAX];
} id3ctx_t;

static bool frame_id_char(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

// Map v2.2 three-letter ids to their v2.3 equivalents.
static void id_from_v22(const uint8_t *id3, char out[5]) {
    static const char *const map[][2] = {
        {"TT2", "TIT2"}, {"TP1", "TPE1"}, {"TAL", "TALB"}, {"TP2", "TPE2"}, {"TCO", "TCON"}, {"TCM", "TCOM"},
        {"TYE", "TYER"}, {"TRK", "TRCK"}, {"TPA", "TPOS"}, {"TXX", "TXXX"}, {"PIC", "PIC "}, {"TOR", "TORY"},
    };
    memcpy(out, "????", 5);
    for (size_t i = 0; i < CORE_ARRAY_SIZE(map); i++) {
        if (memcmp(id3, map[i][0], 3) == 0) {
            memcpy(out, map[i][1], 5);
            return;
        }
    }
}

static int text_field_of(const char *id) {
    if (!strcmp(id, "TIT2")) return F_TITLE;
    if (!strcmp(id, "TPE1")) return F_ARTIST;
    if (!strcmp(id, "TALB")) return F_ALBUM;
    if (!strcmp(id, "TPE2")) return F_ALBUM_ARTIST;
    if (!strcmp(id, "TCON")) return F_GENRE;
    if (!strcmp(id, "TCOM")) return F_COMPOSER;
    return -1;
}

static bool is_wanted(const char *id) {
    return text_field_of(id) >= 0 || !strcmp(id, "TYER") || !strcmp(id, "TDRC") || !strcmp(id, "TDOR") ||
           !strcmp(id, "TORY") || !strcmp(id, "TRCK") || !strcmp(id, "TPOS") || !strcmp(id, "TXXX") ||
           !strcmp(id, "APIC") || !strcmp(id, "PIC ");
}

static text_encoding_t id3_enc(uint8_t e) {
    switch (e) {
    case 1: return TEXT_ENC_UTF16_BOM;
    case 2: return TEXT_ENC_UTF16BE;
    case 3: return TEXT_ENC_AUTO_LEGACY;  // declared UTF-8; invalid bytes fall back to detection
    default: return TEXT_ENC_AUTO_LEGACY;
    }
}

// Length of a terminated string in the given ID3 encoding (without terminator) and
// the number of bytes consumed including it.
static size_t id3_strlen(const uint8_t *p, size_t len, uint8_t enc, size_t *consumed) {
    if (enc == 1 || enc == 2) {
        for (size_t i = 0; i + 1 < len; i += 2) {
            if (p[i] == 0 && p[i + 1] == 0) {
                *consumed = i + 2;
                return i;
            }
        }
    } else {
        for (size_t i = 0; i < len; i++) {
            if (p[i] == 0) {
                *consumed = i + 1;
                return i;
            }
        }
    }
    *consumed = len;
    return len;
}

static char *field_ptr(track_tags_t *t, int f, size_t *cap) {
    switch (f) {
    case F_TITLE: *cap = sizeof t->title; return t->title;
    case F_ARTIST: *cap = sizeof t->artist; return t->artist;
    case F_ALBUM: *cap = sizeof t->album; return t->album;
    case F_ALBUM_ARTIST: *cap = sizeof t->album_artist; return t->album_artist;
    case F_GENRE: *cap = sizeof t->genre; return t->genre;
    default: *cap = sizeof t->composer; return t->composer;
    }
}

static void handle_text(id3ctx_t *c, const char *id, const uint8_t *d, size_t len) {
    if (len < 2) return;
    uint8_t enc = d[0];
    d++;
    len--;
    track_tags_t *t = c->t;
    int f = text_field_of(id);
    if (f >= 0) {
        if (enc == 0) {
            size_t consumed;
            size_t n = id3_strlen(d, len, 0, &consumed);
            if (n > RAW_FIELD_MAX) n = tagf_trim_partial_utf8(d, RAW_FIELD_MAX);
            memcpy(c->raw[f], d, n);
            c->raw_len[f] = (uint16_t)n;
            return;
        }
        size_t cap;
        char *dst = field_ptr(t, f, &cap);
        char tmp[TAG_TEXT_MAX];
        tmp[0] = 0;
        tagf_set(tmp, sizeof tmp, d, len, id3_enc(enc), true);
        if (!tmp[0]) return;
        core_strlcpy(dst, tmp, cap);
        c->raw_len[f] = 0;  // a later Unicode frame replaces an earlier Latin-1 one
        return;
    }
    char v[64];
    v[0] = 0;
    tagf_set(v, sizeof v, d, len, id3_enc(enc), true);
    if (!strcmp(id, "TYER") || !strcmp(id, "TDRC")) {
        uint16_t y = tagf_parse_year(v);
        if (y) t->year = y;
    } else if (!strcmp(id, "TDOR") || !strcmp(id, "TORY")) {
        uint16_t y = tagf_parse_year(v);
        if (y && !t->year) t->year = y;
    } else if (!strcmp(id, "TRCK")) {
        tagf_parse_pair(v, &t->track_no, &t->track_total, true);
    } else if (!strcmp(id, "TPOS")) {
        tagf_parse_pair(v, &t->disc_no, &t->disc_total, true);
    }
}

static void handle_txxx(id3ctx_t *c, const uint8_t *d, size_t len) {
    if (len < 2) return;
    uint8_t enc = d[0];
    d++;
    len--;
    size_t consumed;
    size_t dl = id3_strlen(d, len, enc, &consumed);
    char key[64], value[TAG_TEXT_MAX];
    key[0] = value[0] = 0;
    tagf_set(key, sizeof key, d, dl, id3_enc(enc), true);
    tagf_set(value, sizeof value, d + consumed, len - consumed, id3_enc(enc), true);
    if (!key[0] || !value[0]) return;
    // ReplayGain from TXXX is authoritative; other keys only fill gaps.
    bool rg = strncmp(key, "REPLAYGAIN_", 11) == 0 || strncmp(key, "replaygain_", 11) == 0;
    tags_apply_kv(c->t, key, value, rg);
}

static void handle_picture(id3ctx_t *c, bool v22, const uint8_t *d, size_t len, uint32_t full_len, int64_t file_off) {
    if (len < 4) return;
    uint8_t enc = d[0];
    size_t p = 1;
    uint8_t mime_type = 0;
    if (v22) {
        if (!memcmp(d + 1, "JPG", 3)) mime_type = 1;
        else if (!memcmp(d + 1, "PNG", 3)) mime_type = 2;
        p = 4;
    } else {
        size_t consumed;
        size_t ml = id3_strlen(d + p, len - p, 0, &consumed);
        // The MIME string is not NUL-terminated inside the buffer when it runs to its end:
        // search a bounded copy.
        char mime[32];
        size_t mn = CORE_MIN(ml, sizeof mime - 1);
        memcpy(mime, d + p, mn);
        mime[mn] = 0;
        if (strstr(mime, "jpeg") || strstr(mime, "jpg")) mime_type = 1;
        else if (strstr(mime, "png")) mime_type = 2;
        p += consumed;
    }
    if (p >= len) return;
    uint8_t pic_type = d[p++];
    size_t consumed;
    id3_strlen(d + p, len - p, enc, &consumed);
    p += consumed;
    if (p >= len || p >= full_len) return;  // description longer than what we read
    uint8_t kind = tags_image_type(d + p, len - p);
    if (!kind) kind = mime_type;
    if (!kind) return;
    bool front = pic_type == 3;
    track_tags_t *t = c->t;
    if (t->cover_size && (c->cover_front || !front)) return;
    t->cover_offset = (uint64_t)(file_off + (int64_t)p);
    t->cover_size = full_len - (uint32_t)p;
    t->cover_mime = kind;
    c->cover_front = front;
}

// v2.4 frame size: syncsafe by the spec, but some writers (old iTunes) stored plain
// integers. Pick the interpretation after which the next frame header makes sense.
static bool next_header_ok(core_stream_t *s, int64_t at, int64_t end) {
    if (at == end) return true;
    if (at > end || at + 10 > end) return false;
    uint8_t b[4];
    if (tags_read_at(s, at, b, 4) != CORE_OK) return false;
    if (b[0] == 0) return true;  // padding
    return frame_id_char(b[0]) && frame_id_char(b[1]) && frame_id_char(b[2]) && frame_id_char(b[3]);
}

static uint32_t v24_frame_size(id3r_t *r, const uint8_t *sz) {
    uint32_t plain = tags_rd_be32(sz);
    if ((sz[0] | sz[1] | sz[2] | sz[3]) & 0x80) return plain;
    uint32_t ss = syncsafe32(sz);
    if (ss == plain) return ss;
    int64_t body = id3r_raw_pos(r);
    if (next_header_ok(r->s, body + ss, r->end)) return ss;
    if (next_header_ok(r->s, body + plain, r->end)) return plain;
    return ss;
}

static void finish_raw_fields(id3ctx_t *c) {
    // Fields in valid UTF-8 (taggers that ignore the encoding byte) are taken as UTF-8;
    // the code page of the others is detected on all of them at once.
    uint8_t all[F_COUNT * (RAW_FIELD_MAX + 1)];
    bool utf8[F_COUNT];
    size_t n = 0;
    for (int f = 0; f < F_COUNT; f++) {
        utf8[f] = c->raw_len[f] && text_detect_legacy(c->raw[f], c->raw_len[f]) == TEXT_ENC_UTF8;
        if (!c->raw_len[f] || utf8[f]) continue;
        memcpy(all + n, c->raw[f], c->raw_len[f]);
        n += c->raw_len[f];
        all[n++] = ' ';
    }
    text_encoding_t enc = n ? text_detect_legacy(all, n) : TEXT_ENC_UTF8;
    for (int f = 0; f < F_COUNT; f++) {
        if (!c->raw_len[f]) continue;
        size_t cap;
        char *dst = field_ptr(c->t, f, &cap);
        tagf_set(dst, cap, c->raw[f], c->raw_len[f], utf8[f] ? TEXT_ENC_UTF8 : enc, true);
    }
}

int tags_parse_id3v2(core_stream_t *s, int64_t start, track_tags_t *t, uint32_t *total_size) {
    *total_size = 0;
    uint8_t h[10];
    if (tags_read_at(s, start, h, sizeof h) != CORE_OK) return CORE_ENOTFOUND;
    if (memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4 || h[4] == 0xFF || ((h[6] | h[7] | h[8] | h[9]) & 0x80)) {
        return CORE_ENOTFOUND;
    }
    int version = h[3];
    uint8_t flags = h[5];
    uint32_t size = syncsafe32(h + 6);
    bool footer = version == 4 && (flags & 0x10);
    *total_size = 10 + size + (footer ? 10 : 0);
    if (version == 2 && (flags & 0x40)) return CORE_EUNSUPPORTED;  // v2.2 compression: never used in practice

    id3ctx_t *c = core_calloc(1, sizeof *c);
    id3r_t *r = core_calloc(1, sizeof *r);
    if (!c || !r) {
        core_free(c);
        core_free(r);
        return CORE_ENOMEM;
    }
    c->t = t;
    c->version = version;
    r->s = s;
    r->pos = start + 10;
    r->end = start + 10 + size;
    int64_t fsize = core_stream_size(s);
    if (fsize > 0 && r->end > fsize) r->end = fsize;
    r->unsync = (flags & 0x80) && version < 4;

    if (version >= 3 && (flags & 0x40)) {
        uint8_t eh[4];
        if (id3r_read(r, eh, 4) == 4) {
            uint32_t es = version == 3 ? tags_rd_be32(eh) : syncsafe32(eh);
            if (version == 4) es = es >= 4 ? es - 4 : 0;
            id3r_skip(r, es);
        }
    }

    const size_t hdr_len = version == 2 ? 6 : 10;
    for (int guard = 0; guard < 8192 && !r->error; guard++) {
        if (id3r_raw_left(r) < (int64_t)hdr_len) break;
        uint8_t fh[10];
        if (id3r_read(r, fh, hdr_len) != hdr_len) break;
        if (fh[0] == 0) break;  // padding
        char id[5];
        uint32_t fs;
        uint16_t ff = 0;
        if (version == 2) {
            if (!frame_id_char(fh[0]) || !frame_id_char(fh[1]) || !frame_id_char(fh[2])) break;
            id_from_v22(fh, id);
            fs = (uint32_t)fh[3] << 16 | (uint32_t)fh[4] << 8 | fh[5];
        } else {
            if (!frame_id_char(fh[0]) || !frame_id_char(fh[1]) || !frame_id_char(fh[2]) || !frame_id_char(fh[3])) break;
            memcpy(id, fh, 4);
            id[4] = 0;
            fs = version == 3 ? tags_rd_be32(fh + 4) : v24_frame_size(r, fh + 4);
            ff = tags_rd_be16(fh + 8);
        }
        // A frame never extends past the tag (the de-unsynchronised size is not larger
        // than the raw one): a bigger size means corruption. Keep what we have.
        if ((int64_t)fs > id3r_raw_left(r)) break;
        if (fs == 0) continue;
        bool skip = !is_wanted(id);
        size_t prefix = 0;
        bool frame_unsync = false;
        if (version == 3) {
            if (ff & 0x00C0) skip = true;  // compressed or encrypted
            if (ff & 0x0020) prefix += 1;  // grouping id
        } else if (version == 4) {
            if (ff & 0x000C) skip = true;  // compressed or encrypted
            if (ff & 0x0040) prefix += 1;
            if (ff & 0x0001) prefix += 4;  // data length indicator
            frame_unsync = (ff & 0x0002) || (flags & 0x80);
        }
        if (skip || fs <= prefix) {
            id3r_skip(r, fs);
            continue;
        }
        int64_t body_off = id3r_raw_pos(r);
        size_t want = CORE_MIN((size_t)fs, sizeof c->data);
        size_t got = id3r_read(r, c->data, want);
        if (got < want) break;
        if (fs > want) id3r_skip(r, fs - want);
        uint8_t *d = c->data + prefix;
        size_t len = got - prefix;
        if (fs > want) len = tagf_trim_partial_utf8(d, len);
        uint32_t full = fs - (uint32_t)prefix;
        if (frame_unsync) {
            size_t o = 0;
            for (size_t i = 0; i < len; i++) {
                if (i > 0 && d[i - 1] == 0xFF && d[i] == 0x00) continue;
                d[o++] = d[i];
            }
            len = o;
        }
        if (id[0] == 'T' && strcmp(id, "TXXX") != 0) {
            handle_text(c, id, d, len);
        } else if (!strcmp(id, "TXXX")) {
            handle_txxx(c, d, len);
        } else if (!r->unsync && !frame_unsync) {
            // Covers inside unsynchronised data are not stored contiguously in the file.
            handle_picture(c, version == 2, d, len, full, body_off + (int64_t)prefix);
        }
    }
    finish_raw_fields(c);
    if (t->genre[0]) tags_normalize_genre(t->genre, sizeof t->genre);
    core_free(r);
    core_free(c);
    return CORE_OK;
}

// ================================================================ ID3v1 ======
static size_t v1_field(const uint8_t *p, size_t n) {
    while (n && (p[n - 1] == 0 || p[n - 1] == ' ')) n--;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == 0) return i;
    }
    return n;
}

int tags_parse_id3v1(core_stream_t *s, int64_t size, track_tags_t *t) {
    if (size < 128) return CORE_ENOTFOUND;
    uint8_t b[128];
    if (tags_read_at(s, size - 128, b, sizeof b) != CORE_OK || memcmp(b, "TAG", 3) != 0) return CORE_ENOTFOUND;
    size_t lt = v1_field(b + 3, 30), la = v1_field(b + 33, 30), lb = v1_field(b + 63, 30);
    uint8_t all[96];
    size_t n = 0;
    memcpy(all + n, b + 3, lt);
    n += lt;
    all[n++] = ' ';
    memcpy(all + n, b + 33, la);
    n += la;
    all[n++] = ' ';
    memcpy(all + n, b + 63, lb);
    n += lb;
    text_encoding_t enc = text_detect_legacy(all, n);
    tagf_set(t->title, sizeof t->title, b + 3, lt, enc, false);
    tagf_set(t->artist, sizeof t->artist, b + 33, la, enc, false);
    tagf_set(t->album, sizeof t->album, b + 63, lb, enc, false);
    char year[5];
    memcpy(year, b + 93, 4);
    year[4] = 0;
    if (!t->year) t->year = tagf_parse_year(year);
    if (b[125] == 0 && b[126] != 0 && !t->track_no) t->track_no = b[126];  // ID3v1.1
    const char *g = tags_id3v1_genre(b[127]);
    if (g && !t->genre[0]) core_strlcpy(t->genre, g, sizeof t->genre);
    return CORE_OK;
}

// ================================================================ APEv2 ======
int tags_parse_apev2(core_stream_t *s, int64_t end, track_tags_t *t, int64_t *tag_start) {
    *tag_start = end;
    if (end < 32) return CORE_ENOTFOUND;
    uint8_t f[32];
    if (tags_read_at(s, end - 32, f, sizeof f) != CORE_OK || memcmp(f, "APETAGEX", 8) != 0) return CORE_ENOTFOUND;
    uint32_t size = tags_rd_le32(f + 12), count = tags_rd_le32(f + 16), flags = tags_rd_le32(f + 20);
    if (size < 32 || (int64_t)size > end || count > 4096) return CORE_ECORRUPT;
    int64_t items = end - size;
    *tag_start = (flags & 0x80000000u) && items >= 32 ? items - 32 : items;
    int64_t items_end = end - 32;
    int64_t pos = items;
    char value[TAG_RAW_MAX + 1];
    for (uint32_t i = 0; i < count && pos + 9 < items_end; i++) {
        uint8_t ih[8 + 256];
        size_t avail = (size_t)CORE_MIN((int64_t)sizeof ih, items_end - pos);
        if (tags_read_at(s, pos, ih, avail) != CORE_OK) break;
        uint32_t vlen = tags_rd_le32(ih), iflags = tags_rd_le32(ih + 4);
        size_t k = 8;
        while (k < avail && ih[k]) k++;
        if (k >= avail) break;  // key without terminator
        char key[256];
        memcpy(key, ih + 8, k - 8);
        key[k - 8] = 0;
        int64_t voff = pos + (int64_t)k + 1;
        if (voff + vlen > items_end) break;
        uint32_t type = (iflags >> 1) & 3;
        if (type == 0) {
            size_t n = CORE_MIN((size_t)vlen, sizeof value - 1);
            if (tags_read_at(s, voff, value, n) != CORE_OK) break;
            value[n] = 0;
            tags_apply_kv(t, key, value, false);  // fill only: a leading ID3v2 tag wins
        } else if (type == 1 && !t->cover_size && strstr(key, "Cover Art (Front)") == key) {
            // "file name\0" followed by the image.
            uint8_t head[256 + 8];
            size_t n = CORE_MIN((size_t)vlen, sizeof head);
            if (tags_read_at(s, voff, head, n) == CORE_OK) {
                size_t z = 0;
                while (z < n && head[z]) z++;
                if (z + 4 < n) {
                    uint8_t kind = tags_image_type(head + z + 1, n - z - 1);
                    if (kind) {
                        t->cover_offset = (uint64_t)(voff + (int64_t)z + 1);
                        t->cover_size = vlen - (uint32_t)(z + 1);
                        t->cover_mime = kind;
                    }
                }
            }
        }
        pos = voff + vlen;
    }
    return CORE_OK;
}

// ========================================================== MPEG audio =======
typedef struct {
    int ver;         // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
    int layer;       // 1..3
    uint32_t rate;
    uint32_t kbps;
    uint32_t len;    // frame length in bytes
    uint32_t spf;    // samples per frame
    uint8_t channels;
} mpa_hdr_t;

static bool mpa_parse(const uint8_t *b, mpa_hdr_t *h) {
    static const uint16_t br[2][3][16] = {
        {// MPEG1: L1, L2, L3
         {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
        {// MPEG2 / 2.5
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}},
    };
    static const uint32_t rates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return false;
    int ver = (b[1] >> 3) & 3, lbits = (b[1] >> 1) & 3;
    if (ver == 1 || lbits == 0) return false;
    int layer = 4 - lbits;
    int bri = b[2] >> 4, sri = (b[2] >> 2) & 3, pad = (b[2] >> 1) & 1;
    if (bri == 0 || bri == 15 || sri == 3) return false;
    h->ver = ver;
    h->layer = layer;
    h->rate = rates[ver][sri];
    h->kbps = br[ver == 3 ? 0 : 1][layer - 1][bri];
    h->channels = (b[3] >> 6) == 3 ? 1 : 2;
    if (layer == 1) {
        h->spf = 384;
        h->len = (12 * h->kbps * 1000 / h->rate + (uint32_t)pad) * 4;
    } else if (layer == 2 || ver == 3) {
        h->spf = 1152;
        h->len = 144 * h->kbps * 1000 / h->rate + (uint32_t)pad;
    } else {
        h->spf = 576;
        h->len = 72 * h->kbps * 1000 / h->rate + (uint32_t)pad;
    }
    return h->len >= 21;
}

typedef struct {
    uint32_t rate;
    uint8_t channels;
    uint32_t len;
} adts_hdr_t;

static bool adts_parse(const uint8_t *b, adts_hdr_t *h) {
    static const uint32_t rates[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                       16000, 12000, 11025, 8000,  7350,  0,     0,     0};
    if (b[0] != 0xFF || (b[1] & 0xF6) != 0xF0) return false;
    h->rate = rates[(b[2] >> 2) & 0xF];
    h->channels = (uint8_t)(((b[2] & 1) << 2) | (b[3] >> 6));
    h->len = ((uint32_t)(b[3] & 3) << 11) | ((uint32_t)b[4] << 3) | (b[5] >> 5);
    return h->rate && h->len >= 7;
}

static void mpeg_xing(core_stream_t *s, int64_t pos, const mpa_hdr_t *h, int64_t audio_bytes, track_tags_t *t) {
    uint8_t f[256];
    memset(f, 0, sizeof f);
    int64_t fsize = core_stream_size(s);
    size_t n = (size_t)CORE_MIN((int64_t)sizeof f, fsize - pos);
    if (tags_read_at(s, pos, f, n) != CORE_OK) return;
    size_t side = h->ver == 3 ? (h->channels == 1 ? 17 : 32) : (h->channels == 1 ? 9 : 17);
    size_t x = 4 + side;
    uint64_t samples = 0;
    if (x + 8 <= n && (!memcmp(f + x, "Xing", 4) || !memcmp(f + x, "Info", 4))) {
        uint32_t flags = tags_rd_be32(f + x + 4);
        size_t p = x + 8;
        uint32_t frames = 0;
        if (flags & 1) {
            if (p + 4 > n) return;
            frames = tags_rd_be32(f + p);
            p += 4;
        }
        if (flags & 2) p += 4;
        if (flags & 4) p += 100;
        if (flags & 8) p += 4;
        if (!frames) goto cbr;
        samples = (uint64_t)frames * h->spf;
        // LAME tag: encoder delay and padding (12 bits each) at +21.
        if (p + 24 <= n && (!memcmp(f + p, "LAME", 4) || !memcmp(f + p, "Lavf", 4) || !memcmp(f + p, "Lavc", 4))) {
            uint32_t delay = (uint32_t)f[p + 21] << 4 | f[p + 22] >> 4;
            uint32_t padding = (uint32_t)(f[p + 22] & 0x0F) << 8 | f[p + 23];
            if (delay + padding < samples) samples -= delay + padding;
        }
    } else if (4 + 32 + 18 <= n && !memcmp(f + 4 + 32, "VBRI", 4)) {
        uint32_t frames = tags_rd_be32(f + 4 + 32 + 14);
        samples = (uint64_t)frames * h->spf;
    }
    if (samples) {
        t->duration_ms = (uint32_t)(samples * 1000 / h->rate);
        return;
    }
cbr:
    if (h->kbps && audio_bytes > 0) t->duration_ms = (uint32_t)((uint64_t)audio_bytes * 8 / h->kbps);
}

int tags_parse_mpeg(core_stream_t *s, int64_t start, int64_t end, track_tags_t *t) {
    const int64_t limit = CORE_MIN(end, start + 256 * 1024);
    uint8_t buf[4096];
    int64_t off = start;
    while (off + 6 <= limit) {
        size_t n = (size_t)CORE_MIN((int64_t)sizeof buf, limit - off);
        if (tags_read_at(s, off, buf, n) != CORE_OK) return CORE_EIO;
        for (size_t i = 0; i + 6 <= n; i++) {
            if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
            int64_t pos = off + (int64_t)i;
            mpa_hdr_t h;
            adts_hdr_t a;
            uint8_t nb[6];
            if (mpa_parse(buf + i, &h)) {
                // Confirm with the next frame header (or the end of the data).
                int64_t next = pos + h.len;
                mpa_hdr_t h2;
                bool ok = next + 4 > end || (tags_read_at(s, next, nb, 4) == CORE_OK && mpa_parse(nb, &h2) &&
                                             h2.ver == h.ver && h2.layer == h.layer && h2.rate == h.rate);
                if (!ok) continue;
                if (t->codec == CODEC_UNKNOWN) t->codec = CODEC_MP3;
                t->sample_rate = h.rate;
                t->channels = h.channels;
                t->bits = 0;
                mpeg_xing(s, pos, &h, end - pos, t);
                return CORE_OK;
            }
            if (adts_parse(buf + i, &a)) {
                int64_t next = pos + a.len;
                adts_hdr_t a2;
                bool ok = next + 6 > end ||
                          (tags_read_at(s, next, nb, 6) == CORE_OK && adts_parse(nb, &a2) && a2.rate == a.rate);
                if (!ok) continue;
                t->codec = CODEC_AAC;
                t->sample_rate = a.rate;
                t->channels = a.channels ? a.channels : 2;
                // Estimate duration from the average length of the first frames (1024 samples each).
                uint64_t bytes = 0;
                uint32_t frames = 0;
                int64_t p = pos;
                while (frames < 64 && p + 6 <= end && tags_read_at(s, p, nb, 6) == CORE_OK && adts_parse(nb, &a2)) {
                    bytes += a2.len;
                    frames++;
                    p += a2.len;
                }
                if (frames && bytes) {
                    uint64_t total_frames = (uint64_t)(end - pos) * frames / bytes;
                    t->duration_ms = (uint32_t)(total_frames * 1024 * 1000 / a.rate);
                }
                return CORE_OK;
            }
        }
        if (n < sizeof buf) break;
        off += (int64_t)n - 5;
    }
    return CORE_ENOTFOUND;
}
