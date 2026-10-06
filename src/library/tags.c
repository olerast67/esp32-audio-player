// SPDX-License-Identifier: Apache-2.0
// Tag reading front end: container detection and helpers shared by the parsers.
//
// Container parsers live in tags_id3.c (ID3v2/ID3v1/APEv2 + MPEG audio), tags_xiph.c
// (FLAC, Ogg Vorbis/Opus/FLAC), tags_riff.c (WAV/RF64, AIFF, DSF, DFF, WavPack, APE)
// and tags_mp4.c (MP4/M4A/M4B). They read through core_stream_t with small bounded
// buffers: a tag field is never read beyond TAG_RAW_MAX bytes and large frames
// (covers, lyrics) are skipped, so a corrupt size in a file cannot make us allocate.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio_player/decoder.h"
#include "audio_player/tags.h"
#include "audio_player/text.h"
#include "lib_priv.h"

#define TAG "tags"

void tags_clear(track_tags_t *t) {
    memset(t, 0, sizeof *t);
    t->rg_track_gain_db = NAN;
    t->rg_track_peak = NAN;
    t->rg_album_gain_db = NAN;
    t->rg_album_peak = NAN;
}

int tags_read_at(core_stream_t *s, int64_t off, void *buf, size_t len) {
    int r = core_stream_seek(s, off, 0);
    if (r) return r;
    return core_stream_read_exact(s, buf, len);
}

// ------------------------------------------------------------ tag sources ----
static int32_t region_read(tag_src_t *src, void *buf, size_t len) {
    tag_region_t *r = (tag_region_t *)src;
    if (len > r->left) len = (size_t)r->left;
    if (!len) return 0;
    int32_t n = core_stream_read(r->s, buf, len);
    if (n > 0) r->left -= (uint64_t)n;
    return n;
}

static int region_skip(tag_src_t *src, uint64_t len) {
    tag_region_t *r = (tag_region_t *)src;
    if (len > r->left) return CORE_EOF;
    int e = core_stream_skip(r->s, (int64_t)len);
    if (!e) r->left -= len;
    return e;
}

void tag_region_init(tag_region_t *r, core_stream_t *s, uint64_t len) {
    r->base.read = region_read;
    r->base.skip = region_skip;
    r->s = s;
    r->left = len;
}

// ---------------------------------------------------------- field helpers ----
void tagf_set(char *dst, size_t cap, const uint8_t *src, size_t len, text_encoding_t enc, bool overwrite) {
    if (!overwrite && dst[0]) return;
    char tmp[TAG_TEXT_MAX * 2];
    text_to_utf8(src, len, enc, tmp, sizeof tmp);
    // Control characters (tabs, line breaks inside a title) become spaces.
    for (char *p = tmp; *p; p++) {
        if ((uint8_t)*p < 0x20) *p = ' ';
    }
    text_trim(tmp);
    if (!tmp[0]) return;
    core_strlcpy(dst, tmp, cap);
    utf8_truncate(dst, cap - 1);
    text_trim(dst);
}

size_t tagf_trim_partial_utf8(const uint8_t *p, size_t n) {
    // Walk back over at most 3 continuation bytes to the last lead byte.
    size_t i = n, back = 0;
    while (i > 0 && back < 4 && (p[i - 1] & 0xC0) == 0x80) {
        i--;
        back++;
    }
    if (i == 0) return n;
    uint8_t lead = p[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (lead < 0x80) return n;           // ASCII tail: complete
    if (back + 1 < need) return i - 1;   // incomplete sequence: drop it
    return n;
}

void tagf_set_utf8(char *dst, size_t cap, const char *src, bool overwrite) {
    if (!src) return;
    tagf_set(dst, cap, (const uint8_t *)src, strlen(src), TEXT_ENC_AUTO_LEGACY, overwrite);
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static bool parse_uint(const char **ps, uint32_t *out) {
    const char *s = *ps;
    uint32_t v = 0;
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        if (v < 100000000u) v = v * 10 + (uint32_t)(*s - '0');
        s++;
        n++;
    }
    if (!n) return false;
    *ps = s;
    *out = v;
    return true;
}

void tagf_parse_pair(const char *s, uint16_t *no, uint16_t *total, bool overwrite) {
    if (!s) return;
    s = skip_ws(s);
    uint32_t v;
    if (parse_uint(&s, &v) && v > 0 && v < 65536 && (overwrite || !*no)) *no = (uint16_t)v;
    s = skip_ws(s);
    if (*s == '/' || *s == '-' || *s == ':') {
        s = skip_ws(s + 1);
        if (total && parse_uint(&s, &v) && v > 0 && v < 65536 && (overwrite || !*total)) *total = (uint16_t)v;
    }
}

uint16_t tagf_parse_year(const char *s) {
    if (!s) return 0;
    for (; *s; s++) {
        if (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9' && s[3] >= '0' &&
            s[3] <= '9' && !(s[4] >= '0' && s[4] <= '9')) {
            uint16_t y = (uint16_t)((s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0'));
            if (y >= 1000 && y <= 2999) return y;
        }
        // Skip the rest of a longer digit group ("12345").
        while (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9') s++;
    }
    return 0;
}

float tagf_parse_float(const char *s, bool *ok) {
    *ok = false;
    if (!s) return 0.0f;
    s = skip_ws(s);
    float sign = 1.0f;
    if (*s == '+' || *s == '-') {
        if (*s == '-') sign = -1.0f;
        s++;
    }
    float v = 0.0f;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10.0f + (float)(*s - '0');
        s++;
        digits++;
    }
    if (*s == '.' || *s == ',') {
        s++;
        float scale = 0.1f;
        while (*s >= '0' && *s <= '9') {
            v += (float)(*s - '0') * scale;
            scale *= 0.1f;
            s++;
            digits++;
        }
    }
    if (!digits || v > 1000.0f) return 0.0f;
    *ok = true;
    return sign * v;
}

// ---------------------------------------------------------------- genres ----
static const char *const s_genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
    "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
    "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal",
    "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel",
    "Noise", "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop",
    "Instrumental Rock", "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance",
    "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
    "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock",
    // Winamp extensions
    "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebop", "Latin", "Revival", "Celtic",
    "Bluegrass", "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock",
    "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music",
    "Sonata", "Symphony", "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba",
    "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo",
    "A Cappella", "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore Techno", "Terror",
    "Indie", "BritPop", "Afro-Punk", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal", "Black Metal",
    "Crossover", "Contemporary Christian", "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "Jpop",
    "Synthpop", "Abstract", "Art Rock", "Baroque", "Bhangra", "Big Beat", "Breakbeat", "Chillout", "Downtempo",
    "Dub", "EBM", "Eclectic", "Electro", "Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM",
    "Illbient", "Industro-Goth", "Jam Band", "Krautrock", "Leftfield", "Lounge", "Math Rock", "New Romantic",
    "Nu-Breakz", "Post-Punk", "Post-Rock", "Psytrance", "Shoegaze", "Space Rock", "Trop Rock", "World Music",
    "Neoclassical", "Audiobook", "Audio Theatre", "Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk",
    "Dubstep", "Garage Rock", "Psybient",
};
_Static_assert(CORE_ARRAY_SIZE(s_genres) == 192, "ID3v1 genre table has 192 entries");

const char *tags_id3v1_genre(unsigned idx) { return idx < CORE_ARRAY_SIZE(s_genres) ? s_genres[idx] : NULL; }

void tags_normalize_genre(char *g, size_t cap) {
    char out[TAG_GENRE_MAX];
    const char *s = g;
    out[0] = 0;
    // v2.3 style: one or more "(n)" references, optionally followed by refinement text.
    const char *ref = NULL;
    char ref_buf[4] = {0};
    while (*s == '(') {
        if (s[1] == '(') {  // "((" escapes a literal parenthesis
            s++;
            break;
        }
        const char *close = strchr(s, ')');
        if (!close) break;
        size_t n = (size_t)(close - s - 1);
        if (!ref) {
            if (n == 2 && (strncmp(s + 1, "RX", 2) == 0 || strncmp(s + 1, "CR", 2) == 0)) {
                ref = s[1] == 'R' ? "Remix" : "Cover";
            } else if (n >= 1 && n <= 3) {
                bool digits = true;
                for (size_t i = 0; i < n; i++) digits &= s[1 + i] >= '0' && s[1 + i] <= '9';
                if (digits) {
                    memcpy(ref_buf, s + 1, n);
                    ref_buf[n] = 0;
                    ref = tags_id3v1_genre((unsigned)atoi(ref_buf));
                }
            }
        }
        s = close + 1;
    }
    while (*s == ' ') s++;
    if (*s) {
        // Refinement text wins; a bare number (v2.4 style) is a genre index.
        bool digits = true;
        size_t n = 0;
        for (const char *p = s; *p; p++, n++) digits &= *p >= '0' && *p <= '9';
        const char *name = (digits && n <= 3) ? tags_id3v1_genre((unsigned)atoi(s)) : NULL;
        core_strlcpy(out, name ? name : s, sizeof out);
    } else if (ref) {
        core_strlcpy(out, ref, sizeof out);
    }
    core_strlcpy(g, out, cap);
    utf8_truncate(g, cap - 1);
    text_trim(g);
}

bool tags_is_audiobook_genre(const char *genre) {
    static const char *const names[] = {"audiobook", "audiobooks", "audio book", "audio books", "аудиокнига",
                                        "аудиокниги", "audio theatre", "spoken word", "аудиоспектакль"};
    for (size_t i = 0; i < CORE_ARRAY_SIZE(names); i++) {
        if (collate_cmp_primary(genre, names[i], NULL) == 0) return true;
    }
    return false;
}

// ------------------------------------------------------ key/value mapping ----
static bool key_is(const char *key, const char *name) {
    for (; *key && *name; key++, name++) {
        char a = *key, b = *name;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (a != b) return false;
    }
    return *key == 0 && *name == 0;
}

static void set_gain(float *dst, const char *value, bool overwrite) {
    if (!overwrite && !isnan(*dst)) return;
    bool ok;
    float v = tagf_parse_float(value, &ok);
    if (ok) *dst = v;
}

void tags_apply_kv(track_tags_t *t, const char *key, const char *value, bool overwrite) {
    if (!key || !value || !*value) return;
    if (key_is(key, "TITLE")) {
        tagf_set_utf8(t->title, sizeof t->title, value, overwrite);
    } else if (key_is(key, "ARTIST")) {
        tagf_set_utf8(t->artist, sizeof t->artist, value, overwrite);
    } else if (key_is(key, "ALBUM")) {
        tagf_set_utf8(t->album, sizeof t->album, value, overwrite);
    } else if (key_is(key, "ALBUMARTIST") || key_is(key, "ALBUM ARTIST") || key_is(key, "ALBUM_ARTIST") ||
               key_is(key, "BAND")) {
        tagf_set_utf8(t->album_artist, sizeof t->album_artist, value, overwrite);
    } else if (key_is(key, "GENRE")) {
        if (overwrite || !t->genre[0]) {
            char g[TAG_GENRE_MAX];
            g[0] = 0;
            tagf_set_utf8(g, sizeof g, value, true);
            tags_normalize_genre(g, sizeof g);
            if (g[0]) core_strlcpy(t->genre, g, sizeof t->genre);
        }
    } else if (key_is(key, "COMPOSER")) {
        tagf_set_utf8(t->composer, sizeof t->composer, value, overwrite);
    } else if (key_is(key, "DATE") || key_is(key, "YEAR")) {
        uint16_t y = tagf_parse_year(value);
        if (y && (overwrite || !t->year)) t->year = y;
    } else if (key_is(key, "ORIGINALDATE") || key_is(key, "ORIGINALYEAR")) {
        uint16_t y = tagf_parse_year(value);
        if (y && !t->year) t->year = y;
    } else if (key_is(key, "TRACKNUMBER") || key_is(key, "TRACK")) {
        tagf_parse_pair(value, &t->track_no, &t->track_total, overwrite);
    } else if (key_is(key, "TRACKTOTAL") || key_is(key, "TOTALTRACKS")) {
        uint16_t dummy = 0;
        tagf_parse_pair(value, &t->track_total, &dummy, overwrite);
    } else if (key_is(key, "DISCNUMBER") || key_is(key, "DISC")) {
        tagf_parse_pair(value, &t->disc_no, &t->disc_total, overwrite);
    } else if (key_is(key, "DISCTOTAL") || key_is(key, "TOTALDISCS")) {
        uint16_t dummy = 0;
        tagf_parse_pair(value, &t->disc_total, &dummy, overwrite);
    } else if (key_is(key, "REPLAYGAIN_TRACK_GAIN")) {
        set_gain(&t->rg_track_gain_db, value, overwrite);
    } else if (key_is(key, "REPLAYGAIN_TRACK_PEAK")) {
        set_gain(&t->rg_track_peak, value, overwrite);
    } else if (key_is(key, "REPLAYGAIN_ALBUM_GAIN")) {
        set_gain(&t->rg_album_gain_db, value, overwrite);
    } else if (key_is(key, "REPLAYGAIN_ALBUM_PEAK")) {
        set_gain(&t->rg_album_peak, value, overwrite);
    } else if (key_is(key, "R128_TRACK_GAIN") || key_is(key, "R128_ALBUM_GAIN")) {
        // Opus: Q7.8 dB relative to -23 LUFS; ReplayGain reference is -18 LUFS.
        bool ok;
        float q = tagf_parse_float(value, &ok);
        float *dst = key_is(key, "R128_TRACK_GAIN") ? &t->rg_track_gain_db : &t->rg_album_gain_db;
        if (ok && (overwrite || isnan(*dst))) *dst = q / 256.0f + 5.0f;
    }
}

uint8_t tags_image_type(const uint8_t *h, size_t len) {
    if (len >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return 1;
    if (len >= 4 && h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G') return 2;
    return 0;
}

// -------------------------------------------------------------- front end ----
static bool is_mpeg_sync(const uint8_t *h) { return h[0] == 0xFF && (h[1] & 0xE0) == 0xE0; }

// Trailing APEv2 + ID3v1 (MP3, AAC, WavPack, APE, unknown). Fill-only: the leading
// tag (ID3v2) wins. Returns the offset where audio data ends.
static int64_t read_trailing_tags(core_stream_t *s, int64_t size, track_tags_t *t) {
    int64_t end = size;
    uint8_t tag[3];
    bool has_v1 = size >= 128 && tags_read_at(s, size - 128, tag, 3) == CORE_OK && memcmp(tag, "TAG", 3) == 0;
    if (has_v1) end = size - 128;
    // APEv2 (UTF-8, long fields) before ID3v1 (30 bytes, legacy code page).
    int64_t ape_start = end;
    if (tags_parse_apev2(s, end, t, &ape_start) == CORE_OK) end = ape_start;
    if (has_v1) tags_parse_id3v1(s, size, t);
    return end;
}

int tags_read_stream(core_stream_t *s, const char *path_hint, track_tags_t *out) {
    if (!s || !out) return CORE_EINVAL;
    tags_clear(out);
    const char *ext = path_hint ? core_path_ext(path_hint) : "";
    codec_id_t ext_codec = codec_from_extension(ext);
    int64_t size = core_stream_size(s);
    if (size < 0) size = 0;

    uint8_t h[16];
    int64_t start = 0;
    int r = tags_read_at(s, 0, h, sizeof h);
    if (r == CORE_EOF) {
        out->codec = ext_codec;
        return CORE_ECORRUPT;  // too short to be an audio file
    }
    if (r) return r;

    bool had_id3v2 = false;
    // Leading ID3v2 (possibly several, possibly with junk padding after them).
    for (int guard = 0; guard < 4 && memcmp(h, "ID3", 3) == 0; guard++) {
        uint32_t total = 0;
        tags_parse_id3v2(s, start, out, &total);
        if (!total) break;
        had_id3v2 = true;
        start += total;
        if (tags_read_at(s, start, h, sizeof h) != CORE_OK) {
            memset(h, 0, sizeof h);
            break;
        }
    }

    int result = CORE_OK;
    if (memcmp(h, "fLaC", 4) == 0) {
        out->codec = CODEC_FLAC;
        result = tags_parse_flac(s, start, out);
    } else if (memcmp(h, "OggS", 4) == 0) {
        result = tags_parse_ogg(s, start, size, out);
    } else if ((memcmp(h, "RIFF", 4) == 0 || memcmp(h, "RF64", 4) == 0 || memcmp(h, "BW64", 4) == 0) &&
               memcmp(h + 8, "WAVE", 4) == 0) {
        out->codec = CODEC_WAV;
        result = tags_parse_wav(s, start, out);
    } else if (memcmp(h, "FORM", 4) == 0 && (memcmp(h + 8, "AIFF", 4) == 0 || memcmp(h + 8, "AIFC", 4) == 0)) {
        out->codec = CODEC_AIFF;
        result = tags_parse_aiff(s, start, out);
    } else if (memcmp(h, "DSD ", 4) == 0) {
        out->codec = CODEC_DSF;
        result = tags_parse_dsf(s, start, out);
    } else if (memcmp(h, "FRM8", 4) == 0) {
        out->codec = CODEC_DFF;
        result = tags_parse_dff(s, start, out);
    } else if (memcmp(h + 4, "ftyp", 4) == 0) {
        result = tags_parse_mp4(s, start, size, out);
    } else if (memcmp(h, "wvpk", 4) == 0) {
        out->codec = CODEC_WAVPACK;
        tags_parse_wavpack(s, start, out);
        read_trailing_tags(s, size, out);
    } else if (memcmp(h, "MAC ", 4) == 0) {
        out->codec = CODEC_APE;
        tags_parse_ape(s, start, out);
        read_trailing_tags(s, size, out);
    } else if (is_mpeg_sync(h) || had_id3v2 || ext_codec == CODEC_MP3 || ext_codec == CODEC_AAC) {
        int64_t end = read_trailing_tags(s, size, out);
        result = tags_parse_mpeg(s, start, end, out);
        if (result == CORE_ENOTFOUND) {
            // No valid frame header: keep the tags we found, but say so for unknown files.
            result = (had_id3v2 || out->title[0]) ? CORE_OK : CORE_EUNSUPPORTED;
        }
    } else {
        result = CORE_EUNSUPPORTED;
    }
    if (out->codec == CODEC_UNKNOWN) out->codec = ext_codec;
    if (result == CORE_ECORRUPT || result == CORE_EOF) result = CORE_OK;  // partial tags are still useful

    if (out->genre[0] && tags_is_audiobook_genre(out->genre)) out->is_audiobook_hint = true;
    if (core_str_ends_with_ci(ext, "m4b")) out->is_audiobook_hint = true;
    return result;
}

int tags_read_file(const char *path, track_tags_t *out) {
    if (!path || !out) return CORE_EINVAL;
    core_stream_t *s = core_stream_open_file(path, 8 * 1024);
    if (!s) {
        tags_clear(out);
        out->codec = codec_from_extension(core_path_ext(path));
        return CORE_EIO;
    }
    int r = tags_read_stream(s, path, out);
    core_stream_close(s);
    return r;
}
