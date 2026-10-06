// SPDX-License-Identifier: Apache-2.0
// Text encodings: UTF-8 validation and helpers, UTF-16 (BOM / BE), Latin-1 (with the
// Windows-1252 punctuation block, which is what taggers really write), CP1251 and a
// heuristic that tells CP1251 from Latin-1 in 8-bit legacy text (ID3v1, ID3v2 with
// encoding 0, RIFF INFO, CUE sheets and M3U playlists from old Windows software).
#include "audio_player/text.h"

#include <string.h>

#define REPLACEMENT_CHAR 0xFFFDu

// Windows-1252 0x80..0x9F (0 = undefined). 0xA0..0xFF equal Latin-1.
static const uint16_t s_cp1252_80[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

// Windows-1251 0x80..0xBF (0 = undefined). 0xC0..0xFF map to U+0410..U+044F.
static const uint16_t s_cp1251_80[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,  // 80
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,  // 88
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,  // 90
    0,      0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,  // 98
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,  // A0
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,  // A8
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,  // B0
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,  // B8
};

// ------------------------------------------------------------------ UTF-8 ----
size_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = REPLACEMENT_CHAR;
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Decode one sequence from at most `avail` bytes. Strict: rejects overlong forms,
// surrogates and values above U+10FFFF. Never reads past a byte that is not a
// continuation byte, so a NUL inside a truncated sequence is never skipped.
static size_t utf8_decode_n(const uint8_t *s, size_t avail, uint32_t *cp) {
    uint8_t c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    size_t need;
    uint32_t v, min;
    if (c >= 0xC2 && c <= 0xDF) {
        need = 1;
        v = c & 0x1F;
        min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        v = c & 0x0F;
        min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        v = c & 0x07;
        min = 0x10000;
    } else {
        *cp = REPLACEMENT_CHAR;
        return 1;
    }
    for (size_t i = 1; i <= need; i++) {
        if (i >= avail || (s[i] & 0xC0) != 0x80) {
            *cp = REPLACEMENT_CHAR;
            return i;  // consume the valid prefix only
        }
        v = (v << 6) | (s[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        *cp = REPLACEMENT_CHAR;
        return need + 1;
    }
    *cp = v;
    return need + 1;
}

size_t utf8_decode(const char *s, uint32_t *cp) {
    if (!s || !*s) {
        *cp = 0;
        return 1;
    }
    return utf8_decode_n((const uint8_t *)s, 4, cp);
}

size_t utf8_strlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (*s) {
        uint32_t cp;
        s += utf8_decode(s, &cp);
        n++;
    }
    return n;
}

void utf8_truncate(char *s, size_t max_bytes) {
    if (!s) return;
    size_t len = strlen(s);
    if (len <= max_bytes) return;
    size_t cut = max_bytes;
    // Step back to the start of the code point that would be split.
    while (cut > 0 && ((uint8_t)s[cut] & 0xC0) == 0x80) cut--;
    s[cut] = 0;
}

bool text_is_valid_utf8(const uint8_t *in, size_t len) {
    if (!in) return true;
    size_t i = 0;
    while (i < len && in[i]) {
        if (in[i] < 0x80) {
            i++;
            continue;
        }
        uint32_t cp;
        size_t n = utf8_decode_n(in + i, len - i, &cp);
        if (cp == REPLACEMENT_CHAR) {
            // A literal U+FFFD (EF BF BD) is valid; anything else that decoded to it is not.
            if (!(n == 3 && in[i] == 0xEF && in[i + 1] == 0xBF && in[i + 2] == 0xBD)) return false;
        }
        i += n;
    }
    return true;
}

void text_trim(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    size_t a = 0;
    while (a < len && (s[a] == ' ' || (s[a] >= '\t' && s[a] <= '\r'))) a++;
    size_t b = len;
    while (b > a && (s[b - 1] == ' ' || (s[b - 1] >= '\t' && s[b - 1] <= '\r'))) b--;
    if (a > 0) memmove(s, s + a, b - a);
    s[b - a] = 0;
}

// ---------------------------------------------------- legacy detection ----
// Letters of CP1251 when the text is read as Cyrillic: 0xC0..0xFF, Ё/ё and the
// Ukrainian/Belarusian letters in the 0xA0..0xBF block.
static bool cp1251_is_letter(uint8_t c) {
    if (c >= 0xC0) return true;
    switch (c) {
    case 0xA8: case 0xB8:  // Ё ё
    case 0xA1: case 0xA2:  // Ў ў
    case 0xA5: case 0xB4:  // Ґ ґ
    case 0xAA: case 0xBA:  // Є є
    case 0xAF: case 0xBF:  // Ї ї
    case 0xB2: case 0xB3:  // І і
        return true;
    default:
        return false;
    }
}

// The 22 most frequent Russian letters (о е а и н т с р в л к м д п у я ы ь г з б ч),
// about 93 % of running text, as lowercase CP1251 bytes.
static bool cp1251_is_frequent(uint8_t c) {
    if (c >= 0xC0 && c <= 0xDF) c = (uint8_t)(c + 0x20);
    switch (c) {
    case 0xEE: case 0xE5: case 0xE0: case 0xE8: case 0xED: case 0xF2: case 0xF1: case 0xF0:
    case 0xE2: case 0xEB: case 0xEA: case 0xEC: case 0xE4: case 0xEF: case 0xF3: case 0xFF:
    case 0xFB: case 0xFC: case 0xE3: case 0xE7: case 0xE1: case 0xF7:
        return true;
    default:
        return false;
    }
}

static bool is_ascii_letter(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

text_encoding_t text_detect_legacy(const uint8_t *in, size_t len) {
    if (!in) return TEXT_ENC_UTF8;
    size_t n = 0;
    bool high = false;
    while (n < len && in[n]) {
        if (in[n] >= 0x80) high = true;
        n++;
    }
    if (!high || text_is_valid_utf8(in, n)) return TEXT_ENC_UTF8;

    // Word structure decides: Russian text in CP1251 consists of words made only of
    // bytes >= 0xC0, while accented Latin-1 letters sit inside ASCII words
    // ("Björk", "Déjà vu"). Letter frequency guards longer runs against random bytes.
    uint32_t pure_hi = 0, mixed_hi = 0, ascii_letters = 0, frequent = 0;
    size_t i = 0;
    while (i < n) {
        uint32_t w_hi = 0, w_ascii = 0, w_freq = 0;
        while (i < n && (is_ascii_letter(in[i]) || cp1251_is_letter(in[i]))) {
            if (is_ascii_letter(in[i])) {
                w_ascii++;
            } else {
                w_hi++;
                if (cp1251_is_frequent(in[i])) w_freq++;
            }
            i++;
        }
        if (w_hi && !w_ascii) {
            pure_hi += w_hi;
            frequent += w_freq;
        } else if (w_hi) {
            mixed_hi += w_hi;
        }
        ascii_letters += w_ascii;
        if (i < n && !(is_ascii_letter(in[i]) || cp1251_is_letter(in[i]))) i++;
    }
    if (pure_hi == 0 || pure_hi <= mixed_hi) return TEXT_ENC_LATIN1;
    // A single stray high letter next to ASCII words ("Voyage à Paris") is Latin-1;
    // text with no ASCII letters at all ("Я") is taken as Russian.
    if (pure_hi < 2 && ascii_letters > 0) return TEXT_ENC_LATIN1;
    // Long runs must look like Russian: real text has >= ~75 % frequent letters.
    if (pure_hi >= 8 && frequent * 2 < pure_hi) return TEXT_ENC_LATIN1;
    return TEXT_ENC_CP1251;
}

// -------------------------------------------------------------- convert ----
typedef struct {
    char *out;
    size_t cap;  // bytes available for text (out_size - 1)
    size_t len;
    bool full;
} utf8_writer_t;

static void put_cp(utf8_writer_t *w, uint32_t cp) {
    if (w->full) return;
    char tmp[4];
    size_t n = utf8_encode(cp, tmp);
    if (w->len + n > w->cap) {
        w->full = true;  // never split a code point
        return;
    }
    memcpy(w->out + w->len, tmp, n);
    w->len += n;
}

static uint32_t latin1_cp(uint8_t c) {
    if (c >= 0x80 && c < 0xA0) {
        uint16_t u = s_cp1252_80[c - 0x80];
        return u ? u : REPLACEMENT_CHAR;
    }
    return c;
}

static uint32_t cp1251_cp(uint8_t c) {
    if (c < 0x80) return c;
    if (c >= 0xC0) return 0x0410u + (c - 0xC0u);
    uint16_t u = s_cp1251_80[c - 0x80];
    return u ? u : REPLACEMENT_CHAR;
}

static void conv_utf16(utf8_writer_t *w, const uint8_t *in, size_t len, bool big_endian) {
    size_t i = 0;
    // BOM overrides the default byte order.
    if (len >= 2) {
        if (in[0] == 0xFF && in[1] == 0xFE) {
            big_endian = false;
            i = 2;
        } else if (in[0] == 0xFE && in[1] == 0xFF) {
            big_endian = true;
            i = 2;
        }
    }
    while (i + 1 < len && !w->full) {
        uint32_t u = big_endian ? (uint32_t)in[i] << 8 | in[i + 1] : (uint32_t)in[i + 1] << 8 | in[i];
        i += 2;
        if (u == 0) break;
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 < len) {
                uint32_t u2 = big_endian ? (uint32_t)in[i] << 8 | in[i + 1] : (uint32_t)in[i + 1] << 8 | in[i];
                if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                    i += 2;
                    put_cp(w, 0x10000u + ((u - 0xD800u) << 10) + (u2 - 0xDC00u));
                    continue;
                }
            }
            put_cp(w, REPLACEMENT_CHAR);
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            put_cp(w, REPLACEMENT_CHAR);
        } else if (u == 0xFEFF && w->len == 0) {
            continue;  // stray BOM
        } else {
            put_cp(w, u);
        }
    }
}

static void conv_utf8(utf8_writer_t *w, const uint8_t *in, size_t len) {
    size_t i = 0;
    if (len >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) i = 3;
    while (i < len && in[i] && !w->full) {
        uint32_t cp;
        i += utf8_decode_n(in + i, len - i, &cp);
        put_cp(w, cp);
    }
}

size_t text_to_utf8(const uint8_t *in, size_t len, text_encoding_t enc, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    utf8_writer_t w = {out, out_size - 1, 0, false};
    if (in && len) {
        if (enc == TEXT_ENC_AUTO_LEGACY) enc = text_detect_legacy(in, len);
        switch (enc) {
        case TEXT_ENC_UTF16_BOM: conv_utf16(&w, in, len, false); break;
        case TEXT_ENC_UTF16BE: conv_utf16(&w, in, len, true); break;
        case TEXT_ENC_UTF8: conv_utf8(&w, in, len); break;
        case TEXT_ENC_CP1251:
            for (size_t i = 0; i < len && in[i] && !w.full; i++) put_cp(&w, cp1251_cp(in[i]));
            break;
        case TEXT_ENC_LATIN1:
        default:
            for (size_t i = 0; i < len && in[i] && !w.full; i++) put_cp(&w, latin1_cp(in[i]));
            break;
        }
    }
    out[w.len] = 0;
    return w.len;
}
