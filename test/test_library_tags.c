// SPDX-License-Identifier: Apache-2.0
// Tag readers on files crafted in memory: ID3v2.2/2.3/2.4 (unsynchronisation,
// extended header, iTunes sizes, APIC), ID3v1 in CP1251, APEv2, MP3 duration, FLAC,
// Ogg Vorbis/Opus (packets across pages), WAV, AIFF, DSF, MP4; plus fuzzing.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio_player/tags.h"
#include "audio_player/text.h"
#include "library/lib_priv.h"
#include "test.h"

// ------------------------------------------------------------ byte builder ----
typedef struct {
    uint8_t *d;
    size_t n, cap;
} bb_t;

static void bb_init(bb_t *b) {
    b->cap = 1 << 16;
    b->d = malloc(b->cap);
    b->n = 0;
}
static void bb_free(bb_t *b) { free(b->d); }
static void put(bb_t *b, const void *p, size_t n) {
    while (b->n + n > b->cap) {
        b->cap *= 2;
        b->d = realloc(b->d, b->cap);
    }
    if (p) memcpy(b->d + b->n, p, n);
    else memset(b->d + b->n, 0, n);
    b->n += n;
}
static void put_s(bb_t *b, const char *s) { put(b, s, strlen(s)); }
static void put_u8(bb_t *b, uint8_t v) { put(b, &v, 1); }
static void put_be16(bb_t *b, uint16_t v) { uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v}; put(b, x, 2); }
static void put_be24(bb_t *b, uint32_t v) {
    uint8_t x[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put(b, x, 3);
}
static void put_be32(bb_t *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put(b, x, 4);
}
static void put_be64(bb_t *b, uint64_t v) {
    put_be32(b, (uint32_t)(v >> 32));
    put_be32(b, (uint32_t)v);
}
static void put_le16(bb_t *b, uint16_t v) { uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; put(b, x, 2); }
static void put_le32(bb_t *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(b, x, 4);
}
static void put_le64(bb_t *b, uint64_t v) {
    put_le32(b, (uint32_t)v);
    put_le32(b, (uint32_t)(v >> 32));
}
static void put_syncsafe(bb_t *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)((v >> 21) & 0x7F), (uint8_t)((v >> 14) & 0x7F), (uint8_t)((v >> 7) & 0x7F),
                    (uint8_t)(v & 0x7F)};
    put(b, x, 4);
}
static void patch_be32(bb_t *b, size_t at, uint32_t v) {
    b->d[at] = (uint8_t)(v >> 24);
    b->d[at + 1] = (uint8_t)(v >> 16);
    b->d[at + 2] = (uint8_t)(v >> 8);
    b->d[at + 3] = (uint8_t)v;
}
static void patch_le32(bb_t *b, size_t at, uint32_t v) {
    b->d[at] = (uint8_t)v;
    b->d[at + 1] = (uint8_t)(v >> 8);
    b->d[at + 2] = (uint8_t)(v >> 16);
    b->d[at + 3] = (uint8_t)(v >> 24);
}
static void patch_syncsafe(bb_t *b, size_t at, uint32_t v) {
    b->d[at] = (uint8_t)((v >> 21) & 0x7F);
    b->d[at + 1] = (uint8_t)((v >> 14) & 0x7F);
    b->d[at + 2] = (uint8_t)((v >> 7) & 0x7F);
    b->d[at + 3] = (uint8_t)(v & 0x7F);
}

static const uint8_t k_jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 1, 1, 0, 0, 1, 0, 1};
static const uint8_t k_png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};

static int read_mem(const bb_t *b, const char *name, track_tags_t *t) {
    core_stream_t *s = core_stream_open_memory(b->d, b->n, false);
    int r = tags_read_stream(s, name, t);
    core_stream_close(s);
    return r;
}

// ------------------------------------------------------------------ ID3v2 ----
static size_t id3_begin(bb_t *b, int ver, uint8_t flags) {
    size_t at = b->n;
    put_s(b, "ID3");
    put_u8(b, (uint8_t)ver);
    put_u8(b, 0);
    put_u8(b, flags);
    put_syncsafe(b, 0);
    return at;
}
static void id3_end(bb_t *b, size_t at) { patch_syncsafe(b, at + 6, (uint32_t)(b->n - at - 10)); }

static void id3_frame(bb_t *b, int ver, const char *id, const void *data, size_t len, uint16_t flags) {
    if (ver == 2) {
        put(b, id, 3);
        put_be24(b, (uint32_t)len);
    } else {
        put(b, id, 4);
        if (ver == 4) put_syncsafe(b, (uint32_t)len);
        else put_be32(b, (uint32_t)len);
        put_be16(b, flags);
    }
    put(b, data, len);
}

static void id3_text(bb_t *b, int ver, const char *id, uint8_t enc, const void *text, size_t len) {
    uint8_t buf[1024];
    buf[0] = enc;
    memcpy(buf + 1, text, len);
    id3_frame(b, ver, id, buf, len + 1, 0);
}

static void id3_txxx(bb_t *b, int ver, const char *desc, const char *value) {
    uint8_t buf[256];
    size_t n = 0;
    buf[n++] = 0;
    memcpy(buf + n, desc, strlen(desc) + 1);
    n += strlen(desc) + 1;
    memcpy(buf + n, value, strlen(value));
    n += strlen(value);
    id3_frame(b, ver, "TXXX", buf, n, 0);
}

static void id3_apic(bb_t *b, int ver, uint8_t type, const uint8_t *img, size_t img_len) {
    bb_t f;
    bb_init(&f);
    put_u8(&f, 0);
    put_s(&f, "image/jpeg");
    put_u8(&f, 0);
    put_u8(&f, type);
    put_s(&f, "desc");
    put_u8(&f, 0);
    put(&f, img, img_len);
    id3_frame(b, ver, "APIC", f.d, f.n, 0);
    bb_free(&f);
}

// MPEG-1 Layer III 128 kbps 44.1 kHz stereo frames (417 bytes).
static void mp3_frames(bb_t *b, int n) {
    for (int i = 0; i < n; i++) {
        size_t at = b->n;
        put_be32(b, 0xFFFB9000);
        put(b, NULL, 417 - 4);
        (void)at;
    }
}

// Xing + LAME frame: 1000 frames, delay 576, padding 1000.
static void mp3_xing_frame(bb_t *b) {
    size_t at = b->n;
    put_be32(b, 0xFFFB9000);
    put(b, NULL, 32);  // side info
    put_s(b, "Xing");
    put_be32(b, 0x3);   // frames + bytes
    put_be32(b, 1000);  // frames
    put_be32(b, 417000);
    put_s(b, "LAME3.100");
    put(b, NULL, 12);  // revision .. bitrate (offsets 9..20)
    put_u8(b, (uint8_t)(576 >> 4));
    put_u8(b, (uint8_t)(((576 & 0xF) << 4) | (1000 >> 8)));
    put_u8(b, (uint8_t)(1000 & 0xFF));
    put(b, NULL, 417 - (b->n - at));
}

TEST(id3v23_full) {
    bb_t b;
    bb_init(&b);
    size_t at = id3_begin(&b, 3, 0);
    // UTF-16 with BOM title (Russian), Latin-1 artist, album artist, genre reference.
    const uint8_t title16[] = {0xFF, 0xFE, 0x1F, 0x04, 0x35, 0x04, 0x41, 0x04, 0x3D, 0x04, 0x4F, 0x04, 0, 0};
    id3_text(&b, 3, "TIT2", 1, title16, sizeof title16);
    id3_text(&b, 3, "TPE1", 0, "Motorhead", 9);
    id3_text(&b, 3, "TALB", 3, "Ace of Spades", 13);
    id3_text(&b, 3, "TPE2", 0, "Various", 7);
    id3_text(&b, 3, "TCON", 0, "(17)", 4);
    id3_text(&b, 3, "TCOM", 0, "Lemmy", 5);
    id3_text(&b, 3, "TRCK", 0, "3/12", 4);
    id3_text(&b, 3, "TPOS", 0, "1/2", 3);
    id3_text(&b, 3, "TYER", 0, "1980", 4);
    id3_text(&b, 3, "COMM", 0, "ignored", 7);
    id3_txxx(&b, 3, "REPLAYGAIN_TRACK_GAIN", "-6.54 dB");
    id3_txxx(&b, 3, "replaygain_track_peak", "0.988831");
    id3_txxx(&b, 3, "REPLAYGAIN_ALBUM_GAIN", "+1.50 dB");
    uint8_t big[5000];
    memset(big, 'x', sizeof big);
    id3_frame(&b, 3, "PRIV", big, sizeof big, 0);  // large unknown frame is skipped
    id3_apic(&b, 3, 0, k_png, sizeof k_png);        // "other" picture first
    size_t apic_at = b.n;
    id3_apic(&b, 3, 3, k_jpeg, sizeof k_jpeg);      // front cover wins
    put(&b, NULL, 64);                               // padding
    id3_end(&b, at);
    size_t audio_at = b.n;
    mp3_xing_frame(&b);
    mp3_frames(&b, 20);

    track_tags_t t;
    CHECK_EQ_INT(read_mem(&b, "a.mp3", &t), CORE_OK);
    CHECK_STR(t.title, "Песня");
    CHECK_STR(t.artist, "Motorhead");
    CHECK_STR(t.album, "Ace of Spades");
    CHECK_STR(t.album_artist, "Various");
    CHECK_STR(t.genre, "Rock");
    CHECK_STR(t.composer, "Lemmy");
    CHECK_EQ_INT(t.track_no, 3);
    CHECK_EQ_INT(t.track_total, 12);
    CHECK_EQ_INT(t.disc_no, 1);
    CHECK_EQ_INT(t.disc_total, 2);
    CHECK_EQ_INT(t.year, 1980);
    CHECK_NEAR(t.rg_track_gain_db, -6.54, 0.001);
    CHECK_NEAR(t.rg_track_peak, 0.988831, 0.0001);
    CHECK_NEAR(t.rg_album_gain_db, 1.5, 0.001);
    CHECK(isnan(t.rg_album_peak));
    CHECK_EQ_INT(t.cover_mime, 1);
    CHECK_EQ_INT(t.cover_size, sizeof k_jpeg);
    // APIC header: 10 (frame) + 1 + "image/jpeg\0" (11) + 1 + "desc\0" (5).
    CHECK_EQ_INT(t.cover_offset, apic_at + 10 + 1 + 11 + 1 + 5);
    CHECK_EQ_INT(memcmp(b.d + t.cover_offset, k_jpeg, sizeof k_jpeg), 0);
    CHECK_EQ_INT(t.codec, CODEC_MP3);
    CHECK_EQ_INT(t.sample_rate, 44100);
    CHECK_EQ_INT(t.channels, 2);
    // 1000 frames * 1152 - 576 - 1000 samples at 44.1 kHz.
    CHECK_EQ_INT(t.duration_ms, (1000u * 1152 - 1576) * 1000 / 44100);
    CHECK(!t.is_audiobook_hint);
    (void)audio_at;
    bb_free(&b);
}

TEST(id3v24_features) {
    bb_t b;
    bb_init(&b);
    size_t at = id3_begin(&b, 4, 0x40);  // extended header
    put_syncsafe(&b, 6);
    put_u8(&b, 1);
    put_u8(&b, 0);
    id3_text(&b, 4, "TIT2", 3, "Утро", strlen("Утро"));
    const char multi[] = "First\0Second";
    id3_text(&b, 4, "TPE1", 3, multi, sizeof multi - 1);
    id3_text(&b, 4, "TDRC", 3, "2004-05-01T10:00", 16);
    id3_text(&b, 4, "TCON", 3, "13", 2);
    // Long frame (> 127 bytes) exercises syncsafe sizes.
    char album[300];
    memset(album, 'A', sizeof album);
    id3_text(&b, 4, "TALB", 3, album, sizeof album);
    // Frame-level unsynchronisation + data length indicator.
    const uint8_t raw[] = {0x00, 'X', 0xFF, 0xE9};  // Latin-1 "Xÿé"
    uint8_t fr[16];
    size_t n = 0;
    fr[n++] = 0;  // DLI (syncsafe 4)
    fr[n++] = 0;
    fr[n++] = 0;
    fr[n++] = sizeof raw;
    fr[n++] = raw[0];
    fr[n++] = raw[1];
    fr[n++] = 0xFF;
    fr[n++] = 0x00;  // stuffed byte
    fr[n++] = 0xE9;
    id3_frame(&b, 4, "TCOM", fr, n, 0x0003);
    // Compressed frame is skipped.
    id3_frame(&b, 4, "TPE2", "\x00xxxx", 5, 0x0008);
    id3_end(&b, at);
    mp3_frames(&b, 10);

    track_tags_t t;
    CHECK_EQ_INT(read_mem(&b, "b.mp3", &t), CORE_OK);
    CHECK_STR(t.title, "Утро");
    CHECK_STR(t.artist, "First");
    CHECK_EQ_INT(t.year, 2004);
    CHECK_STR(t.genre, "Pop");
    CHECK_EQ_INT(strlen(t.album), TAG_TEXT_MAX - 1);  // truncated to the field
    CHECK_STR(t.composer, "Xÿé");
    CHECK_STR(t.album_artist, "");
    // CBR estimate: 10 frames of 417 bytes at 128 kbps.
    CHECK_EQ_INT(t.duration_ms, 10u * 417 * 8 / 128);
    bb_free(&b);
}

TEST(id3v24_itunes_sizes) {
    // Old iTunes wrote v2.4 frame sizes as plain integers.
    bb_t b;
    bb_init(&b);
    size_t at = id3_begin(&b, 4, 0);
    char text[256];
    text[0] = 3;
    memset(text + 1, 'a', 255);
    put_s(&b, "TIT2");
    put_be32(&b, 256);  // plain 0x00000100: as syncsafe it would read 128
    put_be16(&b, 0);
    put(&b, text, 256);
    id3_text(&b, 4, "TPE1", 3, "After", 5);
    id3_end(&b, at);
    mp3_frames(&b, 3);
    track_tags_t t;
    CHECK_EQ_INT(read_mem(&b, "c.mp3", &t), CORE_OK);
    CHECK_EQ_INT(strlen(t.title), TAG_TEXT_MAX - 1);
    CHECK_STR(t.artist, "After");
    bb_free(&b);
}

TEST(id3v23_unsync_and_v22) {
    // Tag-wide unsynchronisation (v2.3): build frames, then stuff 0x00 after 0xFF.
    bb_t fr;
    bb_init(&fr);
    id3_text(&fr, 3, "TIT2", 0, "Caf\xFF\xE9", 5);
    id3_text(&fr, 3, "TPE1", 0, "Artist", 6);
    id3_apic(&fr, 3, 3, k_jpeg, sizeof k_jpeg);  // contains FF D8 FF E0: cover must be skipped
    bb_t b;
    bb_init(&b);
    size_t at = id3_begin(&b, 3, 0x80);
    for (size_t i = 0; i < fr.n; i++) {
        put_u8(&b, fr.d[i]);
        if (fr.d[i] == 0xFF && (i + 1 == fr.n || fr.d[i + 1] >= 0xE0 || fr.d[i + 1] == 0)) put_u8(&b, 0);
    }
    id3_end(&b, at);
    mp3_frames(&b, 3);
    track_tags_t t;
    CHECK_EQ_INT(read_mem(&b, "d.mp3", &t), CORE_OK);
    CHECK_STR(t.title, "Cafÿé");
    CHECK_STR(t.artist, "Artist");
    CHECK_EQ_INT(t.cover_size, 0);
    bb_free(&b);
    bb_free(&fr);

    // ID3v2.2: three-letter frames, PIC with image format.
    bb_init(&b);
    at = id3_begin(&b, 2, 0);
    id3_text(&b, 2, "TT2", 0, "Old Tag", 7);
    id3_text(&b, 2, "TP1", 0, "Someone", 7);
    id3_text(&b, 2, "TAL", 0, "Album 22", 8);
    id3_text(&b, 2, "TCO", 0, "(13)Pop Rock", 12);
    id3_text(&b, 2, "TRK", 0, "7", 1);
    id3_text(&b, 2, "TYE", 0, "1999", 4);
    bb_t pic;
    bb_init(&pic);
    put_u8(&pic, 0);
    put_s(&pic, "PNG");
    put_u8(&pic, 3);
    put_u8(&pic, 0);
    put(&pic, k_png, sizeof k_png);
    size_t pic_at = b.n;
    id3_frame(&b, 2, "PIC", pic.d, pic.n, 0);
    bb_free(&pic);
    id3_end(&b, at);
    mp3_frames(&b, 3);
    CHECK_EQ_INT(read_mem(&b, "e.mp3", &t), CORE_OK);
    CHECK_STR(t.title, "Old Tag");
    CHECK_STR(t.artist, "Someone");
    CHECK_STR(t.album, "Album 22");
    CHECK_STR(t.genre, "Pop Rock");
    CHECK_EQ_INT(t.track_no, 7);
    CHECK_EQ_INT(t.year, 1999);
    CHECK_EQ_INT(t.cover_mime, 2);
    CHECK_EQ_INT(t.cover_offset, pic_at + 6 + 1 + 3 + 1 + 1);
    CHECK_EQ_INT(t.cover_size, sizeof k_png);
    bb_free(&b);
}

TEST(id3_cp1251_latin1) {
    // Encoding-0 text frames in CP1251 (old Russian MP3s): detected over the whole tag.
    bb_t b;
    bb_init(&b);
    size_t at = id3_begin(&b, 3, 0);
    id3_text(&b, 3, "TIT2", 0, "\xC3\xF0\xF3\xEF\xEF\xE0 \xEA\xF0\xEE\xE2\xE8", 12);  // Группа крови
    id3_text(&b, 3, "TPE1", 0, "\xCA\xE8\xED\xEE", 4);                               // Кино
    id3_text(&b, 3, "TALB", 0, "\xDF", 1);                                           // Я (alone ambiguous)
    id3_end(&b, at);
    mp3_frames(&b, 2);
    track_tags_t t;
    CHECK_EQ_INT(read_mem(&b, "f.mp3", &t), CORE_OK);
    CHECK_STR(t.title, "Группа крови");
    CHECK_STR(t.artist, "Кино");
    CHECK_STR(t.album, "Я");
    bb_free(&b);

    bb_init(&b);
    at = id3_begin(&b, 3, 0);
    id3_text(&b, 3, "TIT2", 0, "J\xF3ga", 4);
    id3_text(&b, 3, "TPE1", 0, "Bj\xF6rk", 5);
    id3_text(&b, 3, "TALB", 0, "Homogenic", 9);
    id3_text(&b, 3, "TCOM", 0, "Кино", strlen("Кино"));  // UTF-8 despite encoding 0
    id3_end(&b, at);
    mp3_frames(&b, 2);
    CHECK_EQ_INT(read_mem(&b, "g.mp3", &t), CORE_OK);
    CHECK_STR(t.title, "Jóga");
    CHECK_STR(t.artist, "Björk");
    CHECK_STR(t.composer, "Кино");
    bb_free(&b);
}

static void id3v1(bb_t *b, const char *title, const char *artist, const char *album, const char *year, uint8_t track,
                  uint8_t genre) {
    uint8_t tag[128];
    memset(tag, 0, sizeof tag);
    memcpy(tag, "TAG", 3);
    memcpy(tag + 3, title, strlen(title));
    memcpy(tag + 33, artist, strlen(artist));
    memcpy(tag + 63, album, strlen(album));
    memcpy(tag + 93, year, 4);
    tag[125] = 0;
    tag[126] = track;
    tag[127] = genre;
    put(b, tag, sizeof tag);
}

TEST(id3v1_cp1251_and_ape) {
    bb_t b;
    bb_init(&b);
    mp3_frames(&b, 5);
    // Кукушка / Кино / Звезда по имени Солнце, padded with spaces like old taggers do.
    id3v1(&b, "\xCA\xF3\xEA\xF3\xF8\xEA\xE0   ", "\xCA\xE8\xED\xEE",
          "\xC7\xE2\xE5\xE7\xE4\xE0 \xEF\xEE \xE8\xEC\xE5\xED\xE8 \xD1\xEE\xEB\xED\xF6\xE5", "1989", 7, 17);
    track_tags_t t;
    CHECK_EQ_INT(read_mem(&b, "h.mp3", &t), CORE_OK);
    CHECK_STR(t.title, "Кукушка");
    CHECK_STR(t.artist, "Кино");
    CHECK_STR(t.album, "Звезда по имени Солнце");
    CHECK_EQ_INT(t.year, 1989);
    CHECK_EQ_INT(t.track_no, 7);
    CHECK_STR(t.genre, "Rock");
    CHECK_EQ_INT(t.duration_ms, 5u * 417 * 8 / 128);  // audio ends before the tag
    bb_free(&b);

    // APEv2 (with header) before ID3v1: APE wins, ID3v1 fills the rest.
    bb_init(&b);
    mp3_frames(&b, 5);
    bb_t items;
    bb_init(&items);
    const char *kv[][2] = {{"Title", "Long APE Title That Does Not Fit Into ID3v1"},
                           {"Artist", "Ape Artist"},
                           {"Track", "4/9"},
                           {"REPLAYGAIN_TRACK_GAIN", "-2.00 dB"}};
    for (int i = 0; i < 4; i++) {
        put_le32(&items, (uint32_t)strlen(kv[i][1]));
        put_le32(&items, 0);
        put_s(&items, kv[i][0]);
        put_u8(&items, 0);
        put_s(&items, kv[i][1]);
    }
    // Binary cover item.
    put_le32(&items, (uint32_t)(6 + sizeof k_jpeg));
    put_le32(&items, 2);  // binary
    put_s(&items, "Cover Art (Front)");
    put_u8(&items, 0);
    size_t cover_rel = items.n;
    put_s(&items, "a.jpg");
    put_u8(&items, 0);
    put(&items, k_jpeg, sizeof k_jpeg);
    uint32_t size = (uint32_t)(items.n + 32);
    put_s(&b, "APETAGEX");
    put_le32(&b, 2000);
    put_le32(&b, size);
    put_le32(&b, 5);
    put_le32(&b, 0xA0000000u);  // header, this is the header
    put(&b, NULL, 8);
    size_t items_at = b.n;
    put(&b, items.d, items.n);
    put_s(&b, "APETAGEX");
    put_le32(&b, 2000);
    put_le32(&b, size);
    put_le32(&b, 5);
    put_le32(&b, 0x80000000u);
    put(&b, NULL, 8);
    id3v1(&b, "Short", "V1 Artist", "V1 Album", "2001", 1, 255);
    CHECK_EQ_INT(read_mem(&b, "i.mp3", &t), CORE_OK);
    CHECK_STR(t.title, "Long APE Title That Does Not Fit Into ID3v1");
    CHECK_STR(t.artist, "Ape Artist");
    CHECK_STR(t.album, "V1 Album");
    CHECK_EQ_INT(t.year, 2001);
    CHECK_EQ_INT(t.track_no, 4);
    CHECK_EQ_INT(t.track_total, 9);
    CHECK_STR(t.genre, "");
    CHECK_NEAR(t.rg_track_gain_db, -2.0, 0.001);
    CHECK_EQ_INT(t.cover_mime, 1);
    CHECK_EQ_INT(t.cover_offset, items_at + cover_rel + 6);
    CHECK_EQ_INT(t.duration_ms, 5u * 417 * 8 / 128);
    bb_free(&items);
    bb_free(&b);
}

// ------------------------------------------------------------------- FLAC ----
static void vorbis_comments(bb_t *b, const char *const *kv, int n) {
    put_le32(b, 9);
    put_s(b, "test 1.0!");
    put_le32(b, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        put_le32(b, (uint32_t)strlen(kv[i]));
        put_s(b, kv[i]);
    }
}

static void flac_streaminfo(bb_t *b, uint32_t rate, uint8_t ch, uint8_t bps, uint64_t total) {
    put_be16(b, 4096);
    put_be16(b, 4096);
    put_be24(b, 0);
    put_be24(b, 0);
    put_be64(b, (uint64_t)rate << 44 | (uint64_t)(ch - 1) << 41 | (uint64_t)(bps - 1) << 36 | total);
    put(b, NULL, 16);
}

TEST(flac_tags) {
    bb_t b;
    bb_init(&b);
    put_s(&b, "fLaC");
    put_u8(&b, 0);  // STREAMINFO
    put_be24(&b, 34);
    flac_streaminfo(&b, 96000, 2, 24, 96000ull * 30);
    const char *kv[] = {"TITLE=Прованс", "ARTIST=Ёлка", "ARTIST=Second", "ALBUMARTIST=Ёлка", "ALBUM=Точки расставлены",
                        "DATE=2011-10-01", "TRACKNUMBER=5", "TRACKTOTAL=12", "DISCNUMBER=1/2", "GENRE=Pop",
                        "REPLAYGAIN_ALBUM_GAIN=-8.1 dB", "REPLAYGAIN_ALBUM_PEAK=1.0", "NOEQUALSIGN", "=novalue"};
    bb_t vc;
    bb_init(&vc);
    vorbis_comments(&vc, kv, 14);
    put_u8(&b, 4);
    put_be24(&b, (uint32_t)vc.n);
    put(&b, vc.d, vc.n);
    bb_free(&vc);
    put_u8(&b, 1);  // PADDING
    put_be24(&b, 100);
    put(&b, NULL, 100);
    put_u8(&b, 0x80 | 6);  // PICTURE, last block
    bb_t pic;
    bb_init(&pic);
    put_be32(&pic, 3);
    put_be32(&pic, 9);
    put_s(&pic, "image/png");
    put_be32(&pic, 0);
    put_be32(&pic, 1);
    put_be32(&pic, 1);
    put_be32(&pic, 24);
    put_be32(&pic, 0);
    put_be32(&pic, sizeof k_png);
    size_t data_rel = pic.n;
    put(&pic, k_png, sizeof k_png);
    put_be24(&b, (uint32_t)pic.n);
    size_t pic_at = b.n;
    put(&b, pic.d, pic.n);
    bb_free(&pic);
    put(&b, "\xFF\xF8", 2);  // first audio frame
    put(&b, NULL, 200);

    track_tags_t t;
    CHECK_EQ_INT(read_mem(&b, "a.flac", &t), CORE_OK);
    CHECK_EQ_INT(t.codec, CODEC_FLAC);
    CHECK_STR(t.title, "Прованс");
    CHECK_STR(t.artist, "Ёлка");
    CHECK_STR(t.album_artist, "Ёлка");
    CHECK_STR(t.album, "Точки расставлены");
    CHECK_EQ_INT(t.year, 2011);
    CHECK_EQ_INT(t.track_no, 5);
    CHECK_EQ_INT(t.track_total, 12);
    CHECK_EQ_INT(t.disc_no, 1);
    CHECK_EQ_INT(t.disc_total, 2);
    CHECK_STR(t.genre, "Pop");
    CHECK_NEAR(t.rg_album_gain_db, -8.1, 0.001);
    CHECK_NEAR(t.rg_album_peak, 1.0, 0.001);
    CHECK_EQ_INT(t.sample_rate, 96000);
    CHECK_EQ_INT(t.bits, 24);
    CHECK_EQ_INT(t.channels, 2);
    CHECK_EQ_INT(t.duration_ms, 30000);
    CHECK_EQ_INT(t.cover_mime, 2);
    CHECK_EQ_INT(t.cover_offset, pic_at + data_rel);
    CHECK_EQ_INT(t.cover_size, sizeof k_png);
    bb_free(&b);
}

// -------------------------------------------------------------------- Ogg ----
// Lay packets out on pages of at most max_segs lacing values each.
static void ogg_stream(bb_t *b, uint32_t serial, const bb_t *packets, int npk, int max_segs, uint32_t *seq) {
    uint8_t lacing[4096];
    int nseg = 0;
    bb_t body;
    bb_init(&body);
    for (int p = 0; p < npk; p++) {
        size_t len = packets[p].n;
        while (len >= 255) {
            lacing[nseg++] = 255;
            len -= 255;
        }
        lacing[nseg++] = (uint8_t)len;
        put(&body, packets[p].d, packets[p].n);
    }
    int s = 0;
    size_t off = 0;
    bool continued = false;
    while (s < nseg) {
        int take = nseg - s < max_segs ? nseg - s : max_segs;
        size_t blen = 0;
        for (int i = 0; i < take; i++) blen += lacing[s + i];
        put_s(b, "OggS");
        put_u8(b, 0);
        put_u8(b, (uint8_t)((continued ? 1 : 0) | (*seq == 0 ? 2 : 0)));
        put_le64(b, 0);
        put_le32(b, serial);
        put_le32(b, (*seq)++);
        put_le32(b, 0);
        put_u8(b, (uint8_t)take);
        put(b, lacing + s, (size_t)take);
        put(b, body.d + off, blen);
        continued = lacing[s + take - 1] == 255;
        s += take;
        off += blen;
    }
    bb_free(&body);
}

static void ogg_final_page(bb_t *b, uint32_t serial, uint32_t seq, uint64_t granule) {
    put_s(b, "OggS");
    put_u8(b, 0);
    put_u8(b, 4);  // end of stream
    put_le64(b, granule);
    put_le32(b, serial);
    put_le32(b, seq);
    put_le32(b, 0);
    put_u8(b, 1);
    put_u8(b, 10);
    put(b, NULL, 10);
}

TEST(ogg_vorbis_opus) {
    bb_t pk[3];
    for (int i = 0; i < 3; i++) bb_init(&pk[i]);
    put_u8(&pk[0], 1);
    put_s(&pk[0], "vorbis");
    put_le32(&pk[0], 0);
    put_u8(&pk[0], 2);
    put_le32(&pk[0], 44100);
    put(&pk[0], NULL, 13);
    put_u8(&pk[1], 3);
    put_s(&pk[1], "vorbis");
    char longv[700];
    memset(longv, 'z', sizeof longv - 1);
    memcpy(longv, "COMMENT=", 8);
    longv[sizeof longv - 1] = 0;
    const char *kv[] = {longv, "TITLE=Хочешь?", "ARTIST=Земфира", "ALBUM=ПММЛ", "TRACKNUMBER=03", "GENRE=Rock"};
    vorbis_comments(&pk[1], kv, 6);
    put_u8(&pk[1], 1);  // framing bit
    put_u8(&pk[2], 5);
    put_s(&pk[2], "vorbis");
    put(&pk[2], NULL, 40);
    bb_t b;
    bb_init(&b);
    uint32_t seq = 0;
    ogg_stream(&b, 0x1234, pk, 1, 255, &seq);
    ogg_final_page(&b, 0x9999, 0, 777);           // page of another logical stream: skipped
    ogg_stream(&b, 0x1234, pk + 1, 2, 2, &seq);  // comment packet spread over several pages
    ogg_final_page(&b, 0x1234, seq++, 44100ull * 5 + 22050);
    ogg_final_page(&b, 0x9999, 1, 99999999);      // the last page belongs to another stream
    track_tags_t t;
    CHECK_EQ_INT(read_mem(&b, "a.ogg", &t), CORE_OK);
    CHECK_EQ_INT(t.codec, CODEC_VORBIS);
    CHECK_STR(t.title, "Хочешь?");
    CHECK_STR(t.artist, "Земфира");
    CHECK_STR(t.album, "ПММЛ");
    CHECK_EQ_INT(t.track_no, 3);
    CHECK_STR(t.genre, "Rock");
    CHECK_EQ_INT(t.sample_rate, 44100);
    CHECK_EQ_INT(t.channels, 2);
    CHECK_EQ_INT(t.duration_ms, 5500);
    bb_free(&b);
    for (int i = 0; i < 3; i++) bb_free(&pk[i]);

    // Opus: 48 kHz, pre-skip, R128 gain.
    bb_t op[2];
    bb_init(&op[0]);
    bb_init(&op[1]);
    put_s(&op[0], "OpusHead");
    put_u8(&op[0], 1);
    put_u8(&op[0], 2);
    put_le16(&op[0], 312);
    put_le32(&op[0], 44100);
    put_le16(&op[0], 0);
    put_u8(&op[0], 0);
    put_s(&op[1], "OpusTags");
    const char *okv[] = {"TITLE=Opus Song", "R128_TRACK_GAIN=-512"};
    vorbis_comments(&op[1], okv, 2);
    bb_init(&b);
    seq = 0;
    ogg_stream(&b, 7, op, 1, 255, &seq);
    ogg_stream(&b, 7, op + 1, 1, 255, &seq);
    ogg_final_page(&b, 7, seq, 48000ull * 3 + 312);
    CHECK_EQ_INT(read_mem(&b, "a.opus", &t), CORE_OK);
    CHECK_EQ_INT(t.codec, CODEC_OPUS);
    CHECK_STR(t.title, "Opus Song");
    CHECK_EQ_INT(t.sample_rate, 48000);
    CHECK_EQ_INT(t.duration_ms, 3000);
    CHECK_NEAR(t.rg_track_gain_db, 3.0, 0.001);  // -512/256 dB re -23 LUFS = +3 dB re ReplayGain
    bb_free(&b);
    bb_free(&op[0]);
    bb_free(&op[1]);
}

// ------------------------------------------------------- RIFF / AIFF / DSF ----
TEST(wav_aiff_dsf) {
    bb_t b;
    bb_init(&b);
    put_s(&b, "RIFF");
    put_le32(&b, 0);
    put_s(&b, "WAVE");
    put_s(&b, "fmt ");
    put_le32(&b, 16);
    put_le16(&b, 1);
    put_le16(&b, 2);
    put_le32(&b, 48000);
    put_le32(&b, 192000);
    put_le16(&b, 4);
    put_le16(&b, 16);
    put_s(&b, "data");
    put_le32(&b, 96000);
    put(&b, NULL, 96000);
    put_s(&b, "LIST");
    size_t list_at = b.n;
    put_le32(&b, 0);
    put_s(&b, "INFO");
    put_s(&b, "INAM");
    put_le32(&b, 8);
    put(&b, "\xCF\xE5\xF1\xED\xFF\x00\x00\x00", 8);  // Песня + NUL padding (CP1251)
    put_s(&b, "IART");
    put_le32(&b, 5);
    put(&b, "Band\0\0", 6);  // odd size + pad byte
    put_s(&b, "ICRD");
    put_le32(&b, 4);
    put_s(&b, "2015");
    patch_le32(&b, list_at, (uint32_t)(b.n - list_at - 4));
    put_s(&b, "id3 ");
    size_t id3_len_at = b.n;
    put_le32(&b, 0);
    size_t tag = id3_begin(&b, 3, 0);
    id3_text(&b, 3, "TALB", 0, "From ID3", 8);
    id3_end(&b, tag);
    patch_le32(&b, id3_len_at, (uint32_t)(b.n - id3_len_at - 4));
    patch_le32(&b, 4, (uint32_t)(b.n - 8));
    track_tags_t t;
    CHECK_EQ_INT(read_mem(&b, "a.wav", &t), CORE_OK);
    CHECK_EQ_INT(t.codec, CODEC_WAV);
    CHECK_STR(t.title, "Песня");
    CHECK_STR(t.artist, "Band");
    CHECK_STR(t.album, "From ID3");
    CHECK_EQ_INT(t.year, 2015);
    CHECK_EQ_INT(t.sample_rate, 48000);
    CHECK_EQ_INT(t.bits, 16);
    CHECK_EQ_INT(t.channels, 2);
    CHECK_EQ_INT(t.duration_ms, 500);
    bb_free(&b);

    // AIFF with an ID3 chunk.
    bb_init(&b);
    put_s(&b, "FORM");
    put_be32(&b, 0);
    put_s(&b, "AIFF");
    put_s(&b, "COMM");
    put_be32(&b, 18);
    put_be16(&b, 2);
    put_be32(&b, 44100 * 2);
    put_be16(&b, 24);
    put(&b, "\x40\x0E\xAC\x44\x00\x00\x00\x00\x00\x00", 10);  // 44100 as 80-bit extended
    put_s(&b, "SSND");
    put_be32(&b, 8 + 60);
    put(&b, NULL, 8 + 60);
    put_s(&b, "ID3 ");
    size_t len_at = b.n;
    put_be32(&b, 0);
    tag = id3_begin(&b, 4, 0);
    id3_text(&b, 4, "TIT2", 3, "AIFF title", 10);
    id3_end(&b, tag);
    patch_be32(&b, len_at, (uint32_t)(b.n - len_at - 4));
    if (b.n & 1) put_u8(&b, 0);
    patch_be32(&b, 4, (uint32_t)(b.n - 8));
    CHECK_EQ_INT(read_mem(&b, "a.aiff", &t), CORE_OK);
    CHECK_EQ_INT(t.codec, CODEC_AIFF);
    CHECK_STR(t.title, "AIFF title");
    CHECK_EQ_INT(t.sample_rate, 44100);
    CHECK_EQ_INT(t.bits, 24);
    CHECK_EQ_INT(t.duration_ms, 2000);
    bb_free(&b);

    // DSF: ID3v2 at the metadata pointer.
    bb_init(&b);
    put_s(&b, "DSD ");
    put_le64(&b, 28);
    put_le64(&b, 0);
    size_t meta_at = b.n;
    put_le64(&b, 0);
    put_s(&b, "fmt ");
    put_le64(&b, 52);
    put_le32(&b, 1);
    put_le32(&b, 0);
    put_le32(&b, 2);
    put_le32(&b, 2);
    put_le32(&b, 2822400);
    put_le32(&b, 1);
    put_le64(&b, 2822400ull * 3);
    put_le32(&b, 4096);
    put_le32(&b, 0);
    put_s(&b, "data");
    put_le64(&b, 12 + 8192);
    put(&b, NULL, 8192);
    size_t tag_at = b.n;
    tag = id3_begin(&b, 3, 0);
    id3_text(&b, 3, "TIT2", 0, "DSD track", 9);
    id3_end(&b, tag);
    patch_le32(&b, meta_at, (uint32_t)tag_at);
    CHECK_EQ_INT(read_mem(&b, "a.dsf", &t), CORE_OK);
    CHECK_EQ_INT(t.codec, CODEC_DSF);
    CHECK_STR(t.title, "DSD track");
    CHECK_EQ_INT(t.sample_rate, 2822400);
    CHECK_EQ_INT(t.bits, 1);
    CHECK_EQ_INT(t.duration_ms, 3000);
    bb_free(&b);
}

static void apev2_footer_tag(bb_t *b, const char *key, const char *value) {
    bb_t it;
    bb_init(&it);
    put_le32(&it, (uint32_t)strlen(value));
    put_le32(&it, 0);
    put_s(&it, key);
    put_u8(&it, 0);
    put_s(&it, value);
    put(b, it.d, it.n);
    put_s(b, "APETAGEX");
    put_le32(b, 2000);
    put_le32(b, (uint32_t)(it.n + 32));
    put_le32(b, 1);
    put_le32(b, 0);
    put(b, NULL, 8);
    bb_free(&it);
}

TEST(wavpack_ape_dff) {
    bb_t b;
    bb_init(&b);
    put_s(&b, "wvpk");
    put_le32(&b, 1000);
    put_le16(&b, 0x0410);
    put_u8(&b, 0);
    put_u8(&b, 0);
    put_le32(&b, 441000);  // 10 s
    put_le32(&b, 0);
    put_le32(&b, 44100);
    put_le32(&b, 1u | (9u << 23));  // 16 bit, stereo, 44.1 kHz
    put_le32(&b, 0);
    put(&b, NULL, 500);
    apev2_footer_tag(&b, "Title", "WavPack title");
    track_tags_t t;
    CHECK_EQ_INT(read_mem(&b, "a.wv", &t), CORE_OK);
    CHECK_EQ_INT(t.codec, CODEC_WAVPACK);
    CHECK_STR(t.title, "WavPack title");
    CHECK_EQ_INT(t.sample_rate, 44100);
    CHECK_EQ_INT(t.bits, 16);
    CHECK_EQ_INT(t.channels, 2);
    CHECK_EQ_INT(t.duration_ms, 10000);
    bb_free(&b);

    bb_init(&b);
    put_s(&b, "MAC ");
    put_le16(&b, 3990);
    put_le16(&b, 0);
    put_le32(&b, 52);
    put_le32(&b, 24);
    put(&b, NULL, 52 - 16);
    put_le16(&b, 2000);
    put_le16(&b, 0);
    put_le32(&b, 73728);
    put_le32(&b, 1000);
    put_le32(&b, 10);
    put_le16(&b, 24);
    put_le16(&b, 2);
    put_le32(&b, 48000);
    put(&b, NULL, 300);
    apev2_footer_tag(&b, "Artist", "Monkey");
    CHECK_EQ_INT(read_mem(&b, "a.ape", &t), CORE_OK);
    CHECK_EQ_INT(t.codec, CODEC_APE);
    CHECK_STR(t.artist, "Monkey");
    CHECK_EQ_INT(t.bits, 24);
    CHECK_EQ_INT(t.sample_rate, 48000);
    CHECK_EQ_INT(t.duration_ms, (9ull * 73728 + 1000) * 1000 / 48000);
    bb_free(&b);

    // DSDIFF: PROP/FS/CHNL, DIIN title, DSD chunk size gives the duration.
    bb_init(&b);
    put_s(&b, "FRM8");
    put_be64(&b, 0);
    put_s(&b, "DSD ");
    put_s(&b, "FVER");
    put_be64(&b, 4);
    put_be32(&b, 0x01050000);
    put_s(&b, "PROP");
    size_t prop = b.n;
    put_be64(&b, 0);
    put_s(&b, "SND ");
    put_s(&b, "FS  ");
    put_be64(&b, 4);
    put_be32(&b, 2822400);
    put_s(&b, "CHNL");
    put_be64(&b, 10);
    put_be16(&b, 2);
    put_s(&b, "SLFTSRGT");
    size_t prop_len = b.n - prop - 8;
    for (int i = 0; i < 8; i++) b.d[prop + i] = (uint8_t)((uint64_t)prop_len >> (56 - 8 * i));
    put_s(&b, "DIIN");
    put_be64(&b, 12 + 4 + 10);
    put_s(&b, "DITI");
    put_be64(&b, 4 + 10);
    put_be32(&b, 10);
    put_s(&b, "DSD title!");
    put_s(&b, "DSD ");
    put_be64(&b, 352800ull * 2 * 2);  // 2 s stereo; the data itself is not needed
    put(&b, NULL, 64);
    for (int i = 0; i < 8; i++) b.d[4 + i] = (uint8_t)((uint64_t)(352800ull * 4 + 200) >> (56 - 8 * i));
    CHECK_EQ_INT(read_mem(&b, "a.dff", &t), CORE_OK);
    CHECK_EQ_INT(t.codec, CODEC_DFF);
    CHECK_STR(t.title, "DSD title!");
    CHECK_EQ_INT(t.sample_rate, 2822400);
    CHECK_EQ_INT(t.channels, 2);
    CHECK_EQ_INT(t.bits, 1);
    CHECK_EQ_INT(t.duration_ms, 2000);
    bb_free(&b);
}

// -------------------------------------------------------------------- MP4 ----
static size_t box_begin(bb_t *b, const char *type) {
    size_t at = b->n;
    put_be32(b, 0);
    put(b, type, 4);
    return at;
}
static void box_end(bb_t *b, size_t at) { patch_be32(b, at, (uint32_t)(b->n - at)); }

static void ilst_text(bb_t *b, const char *type, const char *value) {
    size_t item = box_begin(b, type);
    size_t data = box_begin(b, "data");
    put_be32(b, 1);
    put_be32(b, 0);
    put_s(b, value);
    box_end(b, data);
    box_end(b, item);
}

static void ilst_bin(bb_t *b, const char *type, uint32_t dtype, const void *v, size_t n) {
    size_t item = box_begin(b, type);
    size_t data = box_begin(b, "data");
    put_be32(b, dtype);
    put_be32(b, 0);
    put(b, v, n);
    box_end(b, data);
    box_end(b, item);
}

static size_t build_mp4(bb_t *b, const char *brand, bool book) {
    size_t ftyp = box_begin(b, "ftyp");
    put_s(b, brand);
    put_be32(b, 0);
    put_s(b, "isommp42");
    box_end(b, ftyp);
    size_t mdat = box_begin(b, "mdat");  // moov after mdat, as many encoders write it
    put(b, NULL, 1000);
    box_end(b, mdat);
    size_t moov = box_begin(b, "moov");
    size_t mvhd = box_begin(b, "mvhd");
    put_be32(b, 0);
    put_be32(b, 0);
    put_be32(b, 0);
    put_be32(b, 600);
    put_be32(b, 600 * 185);  // 185 s
    put(b, NULL, 80);
    box_end(b, mvhd);
    size_t trak = box_begin(b, "trak");
    size_t mdia = box_begin(b, "mdia");
    size_t minf = box_begin(b, "minf");
    size_t stbl = box_begin(b, "stbl");
    size_t stsd = box_begin(b, "stsd");
    put_be32(b, 0);
    put_be32(b, 1);
    size_t entry = box_begin(b, "mp4a");
    put(b, NULL, 6);
    put_be16(b, 1);
    put(b, NULL, 8);
    put_be16(b, 2);
    put_be16(b, 16);
    put_be32(b, 0);
    put_be32(b, 44100u << 16);
    box_end(b, entry);
    box_end(b, stsd);
    box_end(b, stbl);
    box_end(b, minf);
    box_end(b, mdia);
    box_end(b, trak);
    size_t udta = box_begin(b, "udta");
    size_t meta = box_begin(b, "meta");
    put_be32(b, 0);
    size_t hdlr = box_begin(b, "hdlr");
    put(b, NULL, 25);
    box_end(b, hdlr);
    size_t ilst = box_begin(b, "ilst");
    ilst_text(b, "\xA9nam", "Название");
    ilst_text(b, "\xA9" "ART", "Исполнитель");
    ilst_text(b, "aART", "Album Artist");
    ilst_text(b, "\xA9" "alb", "Album");
    ilst_text(b, "\xA9" "day", "2010-01-01T00:00:00Z");
    ilst_text(b, "\xA9wrt", "Composer");
    const uint8_t trkn[] = {0, 0, 0, 3, 0, 12, 0, 0}, disk[] = {0, 0, 0, 1, 0, 2}, gnre[] = {0, 18};
    ilst_bin(b, "trkn", 0, trkn, sizeof trkn);
    ilst_bin(b, "disk", 0, disk, sizeof disk);
    ilst_bin(b, "gnre", 0, gnre, sizeof gnre);
    if (book) ilst_bin(b, "stik", 21, "\x02", 1);
    size_t cover_item = b->n;
    ilst_bin(b, "covr", 13, k_jpeg, sizeof k_jpeg);
    size_t ff = box_begin(b, "----");
    size_t mean = box_begin(b, "mean");
    put_be32(b, 0);
    put_s(b, "com.apple.iTunes");
    box_end(b, mean);
    size_t name = box_begin(b, "name");
    put_be32(b, 0);
    put_s(b, "replaygain_track_gain");
    box_end(b, name);
    size_t data = box_begin(b, "data");
    put_be32(b, 1);
    put_be32(b, 0);
    put_s(b, "-3.21 dB");
    box_end(b, data);
    box_end(b, ff);
    box_end(b, ilst);
    box_end(b, meta);
    box_end(b, udta);
    box_end(b, moov);
    return cover_item + 8 + 8 + 8;  // item header, data header, type + locale
}

TEST(mp4_tags) {
    bb_t b;
    bb_init(&b);
    size_t cover = build_mp4(&b, "M4A ", false);
    track_tags_t t;
    CHECK_EQ_INT(read_mem(&b, "a.m4a", &t), CORE_OK);
    CHECK_EQ_INT(t.codec, CODEC_AAC);
    CHECK_STR(t.title, "Название");
    CHECK_STR(t.artist, "Исполнитель");
    CHECK_STR(t.album_artist, "Album Artist");
    CHECK_STR(t.album, "Album");
    CHECK_STR(t.composer, "Composer");
    CHECK_EQ_INT(t.year, 2010);
    CHECK_EQ_INT(t.track_no, 3);
    CHECK_EQ_INT(t.track_total, 12);
    CHECK_EQ_INT(t.disc_no, 1);
    CHECK_EQ_INT(t.disc_total, 2);
    CHECK_STR(t.genre, "Rock");
    CHECK_EQ_INT(t.duration_ms, 185000);
    CHECK_EQ_INT(t.sample_rate, 44100);
    CHECK_EQ_INT(t.channels, 2);
    CHECK_EQ_INT(t.cover_mime, 1);
    CHECK_EQ_INT(t.cover_offset, cover);
    CHECK_EQ_INT(t.cover_size, sizeof k_jpeg);
    CHECK_NEAR(t.rg_track_gain_db, -3.21, 0.001);
    CHECK(!t.is_audiobook_hint);
    bb_free(&b);
    bb_init(&b);
    build_mp4(&b, "M4B ", false);
    CHECK_EQ_INT(read_mem(&b, "book.m4b", &t), CORE_OK);
    CHECK(t.is_audiobook_hint);
    bb_free(&b);
    bb_init(&b);
    build_mp4(&b, "M4A ", true);  // stik = 2 (audiobook media kind)
    CHECK_EQ_INT(read_mem(&b, "book.m4a", &t), CORE_OK);
    CHECK(t.is_audiobook_hint);
    bb_free(&b);
}

// A box with a 64-bit size near 2^64 must not wrap its end below its start: at every level of
// nesting that would walk backwards and re-read the boxes before it, exponentially.
static void evil_box(bb_t *b) {
    put_be32(b, 1);  // 64-bit size follows
    put_s(b, "free");
    put_be64(b, 0xFFFFFFFFFFFFFFF0ull);
}

TEST(mp4_hostile_sizes) {
    bb_t b;
    bb_init(&b);
    size_t ftyp = box_begin(&b, "ftyp");
    put_s(&b, "M4A ");
    put_be32(&b, 0);
    box_end(&b, ftyp);
    size_t moov = box_begin(&b, "moov");
    size_t udta = box_begin(&b, "udta");
    size_t meta = box_begin(&b, "meta");
    put_be32(&b, 0);
    size_t ilst = box_begin(&b, "ilst");
    ilst_text(&b, "\xA9nam", "Safe");
    evil_box(&b);
    ilst_text(&b, "\xA9" "ART", "After");
    box_end(&b, ilst);
    box_end(&b, meta);
    box_end(&b, udta);
    static const char *const levels[] = {"trak", "mdia", "minf", "stbl", "udta", "udta", "udta"};
    size_t at[7];
    for (int i = 0; i < 7; i++) at[i] = box_begin(&b, levels[i]);
    for (int i = 6; i >= 0; i--) {
        evil_box(&b);
        box_end(&b, at[i]);
    }
    evil_box(&b);
    box_end(&b, moov);
    track_tags_t t;
    uint32_t t0 = core_now_ms();
    CHECK_EQ_INT(read_mem(&b, "evil.m4a", &t), CORE_OK);
    CHECK(core_now_ms() - t0 < 2000);
    CHECK_STR(t.title, "Safe");
    CHECK_STR(t.artist, "");  // the bad box ends the list: nothing after it is trusted
    bb_free(&b);
}

TEST(genres_and_misc) {
    char g[TAG_GENRE_MAX];
    strcpy(g, "(13)");
    tags_normalize_genre(g, sizeof g);
    CHECK_STR(g, "Pop");
    strcpy(g, "(13)Pop Rock");
    tags_normalize_genre(g, sizeof g);
    CHECK_STR(g, "Pop Rock");
    strcpy(g, "17");
    tags_normalize_genre(g, sizeof g);
    CHECK_STR(g, "Rock");
    strcpy(g, "(RX)");
    tags_normalize_genre(g, sizeof g);
    CHECK_STR(g, "Remix");
    strcpy(g, "((Weird)");
    tags_normalize_genre(g, sizeof g);
    CHECK_STR(g, "(Weird)");
    strcpy(g, "(183)");
    tags_normalize_genre(g, sizeof g);
    CHECK_STR(g, "Audiobook");
    strcpy(g, "(999)");
    tags_normalize_genre(g, sizeof g);
    CHECK_STR(g, "");
    strcpy(g, "2000s");
    tags_normalize_genre(g, sizeof g);
    CHECK_STR(g, "2000s");
    CHECK(tags_is_audiobook_genre("Аудиокнига"));
    CHECK(tags_is_audiobook_genre("AUDIOBOOK"));
    CHECK(!tags_is_audiobook_genre("Rock"));
    bool ok;
    CHECK_NEAR(tagf_parse_float(" -6,54 dB", &ok), -6.54, 0.001);
    CHECK(ok);
    tagf_parse_float("dB", &ok);
    CHECK(!ok);
    CHECK_EQ_INT(tagf_parse_year("released 1999-05"), 1999);
    CHECK_EQ_INT(tagf_parse_year("12345"), 0);
    CHECK_EQ_INT(tagf_parse_year(""), 0);

    // Audiobook genre sets the hint.
    bb_t b;
    bb_init(&b);
    size_t at = id3_begin(&b, 3, 0);
    id3_text(&b, 3, "TCON", 3, "Аудиокнига", strlen("Аудиокнига"));
    id3_end(&b, at);
    mp3_frames(&b, 2);
    track_tags_t t;
    CHECK_EQ_INT(read_mem(&b, "ch01.mp3", &t), CORE_OK);
    CHECK(t.is_audiobook_hint);
    bb_free(&b);

    // Missing file, unknown container, empty file.
    CHECK_EQ_INT(tags_read_file("does/not/exist.flac", &t), CORE_EIO);
    CHECK_EQ_INT(t.codec, CODEC_FLAC);
    bb_init(&b);
    put_s(&b, "just some text, not audio at all........");
    CHECK_EQ_INT(read_mem(&b, "x.xyz", &t), CORE_EUNSUPPORTED);
    b.n = 3;
    CHECK(read_mem(&b, "x.mp3", &t) != CORE_OK);
    CHECK_EQ_INT(t.codec, CODEC_MP3);
    bb_free(&b);
}

// ---------------------------------------------------------------- fuzzing ----
static uint32_t g_seed = 0xC0FFEEu;
static uint32_t rnd(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static bool field_ok(const char *s, size_t cap) {
    size_t n = strnlen(s, cap);
    return n < cap && text_is_valid_utf8((const uint8_t *)s, n);
}

static void fuzz_one(const bb_t *orig, const char *name, int iterations) {
    bb_t b;
    bb_init(&b);
    for (int it = 0; it < iterations; it++) {
        b.n = 0;
        put(&b, orig->d, orig->n);
        int flips = 1 + (int)(rnd() % 8);
        for (int k = 0; k < flips; k++) {
            size_t pos = rnd() % b.n;
            uint32_t mode = rnd() % 4;
            b.d[pos] = mode == 0 ? 0xFF : mode == 1 ? 0x00 : mode == 2 ? 0x7F : (uint8_t)rnd();
        }
        if (rnd() % 4 == 0) b.n = rnd() % b.n;  // truncation
        track_tags_t t;
        read_mem(&b, name, &t);
        bool ok = field_ok(t.title, sizeof t.title) && field_ok(t.artist, sizeof t.artist) &&
                  field_ok(t.album, sizeof t.album) && field_ok(t.album_artist, sizeof t.album_artist) &&
                  field_ok(t.genre, sizeof t.genre) && field_ok(t.composer, sizeof t.composer) &&
                  (t.cover_size == 0 || t.cover_offset + t.cover_size <= orig->n + (uint64_t)UINT32_MAX);
        CHECK(ok);
        if (!ok) break;
    }
    bb_free(&b);
}

TEST(fuzz_containers) {
    bb_t b;
    // Build one sample per container and mutate it.
    bb_init(&b);
    size_t at = id3_begin(&b, 3, 0);
    id3_text(&b, 3, "TIT2", 1, "\xFF\xFE" "a\0b\0", 6);
    id3_txxx(&b, 3, "REPLAYGAIN_TRACK_GAIN", "-1 dB");
    id3_apic(&b, 3, 3, k_jpeg, sizeof k_jpeg);
    id3_end(&b, at);
    mp3_xing_frame(&b);
    mp3_frames(&b, 4);
    id3v1(&b, "t", "a", "b", "2000", 1, 1);
    fuzz_one(&b, "f.mp3", 3000);
    bb_free(&b);

    bb_init(&b);
    put_s(&b, "fLaC");
    put_u8(&b, 0);
    put_be24(&b, 34);
    flac_streaminfo(&b, 44100, 2, 16, 44100);
    const char *kv[] = {"TITLE=x", "ARTIST=y"};
    bb_t vc;
    bb_init(&vc);
    vorbis_comments(&vc, kv, 2);
    put_u8(&b, 0x84);
    put_be24(&b, (uint32_t)vc.n);
    put(&b, vc.d, vc.n);
    bb_free(&vc);
    fuzz_one(&b, "f.flac", 3000);
    bb_free(&b);

    bb_init(&b);
    build_mp4(&b, "M4A ", true);
    fuzz_one(&b, "f.m4a", 3000);
    bb_free(&b);

    bb_t pk[2];
    bb_init(&pk[0]);
    bb_init(&pk[1]);
    put_s(&pk[0], "OpusHead");
    put(&pk[0], "\x01\x02\x38\x01\x44\xAC\x00\x00\x00\x00\x00", 11);
    put_s(&pk[1], "OpusTags");
    vorbis_comments(&pk[1], kv, 2);
    bb_init(&b);
    uint32_t seq = 0;
    ogg_stream(&b, 1, pk, 2, 1, &seq);
    ogg_final_page(&b, 1, seq, 48000);
    fuzz_one(&b, "f.opus", 3000);
    bb_free(&b);
    bb_free(&pk[0]);
    bb_free(&pk[1]);

    // Pure noise with each extension.
    bb_init(&b);
    const char *names[] = {"n.mp3", "n.flac", "n.ogg", "n.wav", "n.m4a", "n.aiff", "n.dsf", "n.wv", "n.ape"};
    for (int it = 0; it < 2000; it++) {
        b.n = 0;
        size_t len = rnd() % 600;
        static const struct {
            const char *s;
            size_t n;
        } heads[] = {{"ID3\x03\x00\x00", 6}, {"fLaC", 4}, {"OggS", 4}, {"RIFF\x10\x00\x00\x00" "WAVE", 12},
                     {"FORM\x00\x00\x00\x40" "AIFF", 12}, {"DSD ", 4}, {"FRM8", 4}, {"wvpk", 4}, {"MAC ", 4},
                     {"\x00\x00\x00\x20" "ftyp", 8}};
        uint32_t hi = rnd() % (sizeof heads / sizeof *heads);
        put(&b, heads[hi].s, heads[hi].n);
        for (size_t i = 0; i < len; i++) put_u8(&b, (uint8_t)rnd());
        track_tags_t t;
        read_mem(&b, names[rnd() % 9], &t);
        CHECK(field_ok(t.title, sizeof t.title) && field_ok(t.artist, sizeof t.artist));
    }
    bb_free(&b);
}

TEST_MAIN(RUN(id3v23_full) RUN(id3v24_features) RUN(id3v24_itunes_sizes) RUN(id3v23_unsync_and_v22)
              RUN(id3_cp1251_latin1) RUN(id3v1_cp1251_and_ape) RUN(flac_tags) RUN(ogg_vorbis_opus) RUN(wav_aiff_dsf)
              RUN(wavpack_ape_dff) RUN(mp4_tags) RUN(mp4_hostile_sizes) RUN(genres_and_misc) RUN(fuzz_containers))
