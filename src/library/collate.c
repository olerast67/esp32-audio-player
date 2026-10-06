// SPDX-License-Identifier: Apache-2.0
// Compact collation for Russian and English, with Latin diacritics (including
// Vietnamese) folded to the base letter.
//
// Every string maps to a primary key of bytes; memcmp order of two keys is the
// primary order, ties are broken by the raw bytes so the order is total and stable:
//
//   0x01            one separator for any run of spaces/punctuation/symbols between
//                   significant characters (leading and trailing runs are dropped)
//   0x10+L, digits  a run of digits compared by value ("2" < "10"); L = significant
//                   digit count (leading zeros skipped), digits as 0x30+d. L >= 15 uses
//                   0x1F, L, digits.
//   0x40..0x59      Latin a..z (case and diacritics folded: "É" = "e", "ơ" = "o")
//   0x60..0xDF      Cyrillic: 4 * index(а..я) + variant; ё = е, і/ї after и, ґ after г,
//                   є after е, ў after у (Ukrainian and Belarusian letters sort with
//                   their Russian neighbours)
//   0xE0+plane, hi, lo   any other letter (Greek, CJK, ...) by folded code point
//
// So digits < Latin < Cyrillic < other scripts, and the jump letter of a string is
// the first unit of its key: '#' for digits and symbols, 'A'..'Z', 'А'..'Я', or the
// (uppercase) letter itself for other scripts.
#include "audio_player/collate.h"

#include <string.h>

#include "audio_player/text.h"
#include "lib_priv.h"

typedef enum { CL_IGNORE = 0, CL_SYMBOL, CL_DIGIT, CL_LATIN, CL_CYRILLIC, CL_OTHER } char_class_t;

// Base letters for U+00C0..U+017F ('*' = symbol: × and ÷).
static const char s_latin_c0[] =
    "aaaaaaaceeeeiiiidnooooo*ouuuuyts"   // C0..DF
    "aaaaaaaceeeeiiiidnooooo*ouuuuyty"   // E0..FF
    "aaaaaaccccccccdd"                   // 0100
    "ddeeeeeeeeeegggg"                   // 0110
    "gggghhhhiiiiiiii"                   // 0120
    "iiiijjkkklllllll"                   // 0130
    "lllnnnnnnnnnoooo"                   // 0140
    "oooorrrrrrssssss"                   // 0150
    "ssttttttuuuuuuuu"                   // 0160
    "uuuuwwyyyzzzzzzs";                  // 0170

// Base letters for U+1E00..U+1EFF (Latin Extended Additional, incl. Vietnamese).
static const char s_latin_1e[] =
    // 1E00..1E95: pairs (upper, lower)
    "aabbbbbbccddddddddddeeeeeeeeeeffgghhhhhhhhhhiiiikkkkkkllllllll"
    "mmmmmmnnnnnnnnoooooooopppprrrrrrrrssssssssssttttttttuuuuuuuuuu"
    "vvvvwwwwwwwwwwxxxxyyzzzzzz"
    // 1E96..1E9F: singles
    "htwyassssd"
    // 1EA0..1EFF: Vietnamese pairs
    "aaaaaaaaaaaaaaaaaaaaaaaa"  // 12 pairs a
    "eeeeeeeeeeeeeeee"          // 8 pairs e
    "iiii"                      // 2 pairs i
    "oooooooooooooooooooooooo"  // 12 pairs o
    "uuuuuuuuuuuuuu"            // 7 pairs u
    "yyyyyyyy"                  // 4 pairs y
    "llvvyy";                   // 1EFA..1EFF

_Static_assert(sizeof s_latin_c0 == 0x17F - 0xC0 + 2, "s_latin_c0 size");
_Static_assert(sizeof s_latin_1e == 0x100 + 1, "s_latin_1e size");

// Latin Extended-B (U+0180..U+024F) letters that have a plain base letter.
static uint32_t fold_latin_ext_b(uint32_t cp) {
    switch (cp) {
    case 0x0180: case 0x0181: case 0x0183: return 'b';
    case 0x0187: case 0x0188: return 'c';
    case 0x0189: case 0x018A: case 0x018C: return 'd';
    case 0x018E: case 0x0190: case 0x01DD: return 'e';
    case 0x0191: case 0x0192: return 'f';
    case 0x0193: return 'g';
    case 0x0197: case 0x0196: return 'i';
    case 0x0198: case 0x0199: return 'k';
    case 0x019A: return 'l';
    case 0x019D: case 0x019E: return 'n';
    case 0x019F: case 0x01A0: case 0x01A1: return 'o';
    case 0x01A4: case 0x01A5: return 'p';
    case 0x01AB: case 0x01AC: case 0x01AD: case 0x01AE: return 't';
    case 0x01AF: case 0x01B0: return 'u';
    case 0x01B2: return 'v';
    case 0x01B3: case 0x01B4: return 'y';
    case 0x01B5: case 0x01B6: return 'z';
    default: break;
    }
    if (cp >= 0x01CD && cp <= 0x01DC) {
        static const char t[] = "aaiioouuuuuuuuuu";  // Ǎǎ Ǐǐ Ǒǒ Ǔǔ Ǖǖ Ǘǘ Ǚǚ Ǜǜ
        return (uint32_t)t[cp - 0x01CD];
    }
    if (cp >= 0x01DE && cp <= 0x01FF) {
        static const char t[] = "aaaaaaggggkkoooo**jdddgg**nnaaaaoo";
        _Static_assert(sizeof t == 0x01FF - 0x01DE + 2, "table size");
        char c = t[cp - 0x01DE];
        return c == '*' ? 0 : (uint32_t)c;
    }
    if (cp >= 0x0200 && cp <= 0x0233) {
        static const char t[] = "aaaaeeeeiiiioooorrrruuuusstt**hhnd**zzaaeeooooooooyy";
        _Static_assert(sizeof t == 0x0233 - 0x0200 + 2, "table size");
        char c = t[cp - 0x0200];
        return c == '*' ? 0 : (uint32_t)c;
    }
    return 0;
}

// Cyrillic letter -> 4 * base index + variant, or -1.
static int cyr_weight(uint32_t cp) {
    if (cp >= 0x0430 && cp <= 0x044F) return (int)(cp - 0x0430) * 4;
    enum { A = 0, B, V, G, D, E, ZH, Z, I, J, K, L, M, N, O, P, R, S, T, U, F, H, C, CH };
    switch (cp) {
    case 0x0450: case 0x0451: return E * 4;      // ѐ ё
    case 0x0452: return D * 4 + 1;               // ђ
    case 0x0453: return G * 4 + 2;               // ѓ
    case 0x0454: return E * 4 + 1;               // є
    case 0x0455: return Z * 4 + 1;               // ѕ
    case 0x0456: return I * 4 + 1;               // і
    case 0x0457: return I * 4 + 2;               // ї
    case 0x0458: return J * 4 + 1;               // ј
    case 0x0459: return L * 4 + 1;               // љ
    case 0x045A: return N * 4 + 1;               // њ
    case 0x045B: return T * 4 + 1;               // ћ
    case 0x045C: return K * 4 + 1;               // ќ
    case 0x045D: return I * 4;                   // ѝ
    case 0x045E: return U * 4 + 1;               // ў
    case 0x045F: return CH * 4 + 1;              // џ
    case 0x0491: return G * 4 + 1;               // ґ
    case 0x0493: return G * 4 + 3;               // ғ
    case 0x0497: return ZH * 4 + 1;              // җ
    case 0x0499: return Z * 4 + 2;               // ҙ
    case 0x049B: return K * 4 + 2;               // қ
    case 0x04A1: return K * 4 + 3;               // ҡ
    case 0x04A3: return N * 4 + 2;               // ң
    case 0x04AB: return S * 4 + 1;               // ҫ
    case 0x04AF: return U * 4 + 3;               // ү
    case 0x04B1: return U * 4 + 2;               // ұ
    case 0x04BB: return H * 4 + 1;               // һ
    case 0x04D9: return A * 4 + 1;               // ә
    case 0x04E9: return O * 4 + 1;               // ө
    default: return -1;
    }
}

static bool is_ignorable(uint32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp < 0xA0) || cp == 0xAD || (cp >= 0x0300 && cp <= 0x036F) ||
           (cp >= 0x0483 && cp <= 0x0489) ||
           (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x200B && cp <= 0x200F) ||
           (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || (cp >= 0xE0000 && cp <= 0xE01EF);
}

static bool is_symbol_block(uint32_t cp) {
    return cp < 0x80 || (cp >= 0xA0 && cp <= 0xBF) || cp == 0xD7 || cp == 0xF7 || (cp >= 0x02B0 && cp <= 0x02FF) ||
           (cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x2E00 && cp <= 0x2E7F) || (cp >= 0x3000 && cp <= 0x303F) ||
           (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xFE10 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF0F) ||
           (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65) ||
           (cp >= 0xFFF0 && cp <= 0xFFFF) || (cp >= 0x1F000 && cp <= 0x1FAFF);
}

// Fold one code point: lowercase, diacritics removed, ё -> е. Sets the class.
static uint32_t fold_classify(uint32_t cp, char_class_t *cls) {
    if (is_ignorable(cp)) {
        *cls = CL_IGNORE;
        return 0;
    }
    if (cp < 0x80) {
        if (cp >= '0' && cp <= '9') {
            *cls = CL_DIGIT;
            return cp;
        }
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        if (cp >= 'a' && cp <= 'z') {
            *cls = CL_LATIN;
            return cp;
        }
        *cls = CL_SYMBOL;
        return cp;
    }
    if (cp >= 0xFF10 && cp <= 0xFF19) {  // fullwidth digits
        *cls = CL_DIGIT;
        return '0' + (cp - 0xFF10);
    }
    if ((cp >= 0xFF21 && cp <= 0xFF3A) || (cp >= 0xFF41 && cp <= 0xFF5A)) {  // fullwidth Latin
        *cls = CL_LATIN;
        return 'a' + ((cp - 0xFF21) & 0x1F);
    }
    if (cp >= 0xC0 && cp <= 0x17F) {
        char c = s_latin_c0[cp - 0xC0];
        if (c == '*') {
            *cls = CL_SYMBOL;
            return cp;
        }
        *cls = CL_LATIN;
        return (uint32_t)c;
    }
    if (cp >= 0x180 && cp <= 0x24F) {
        uint32_t b = fold_latin_ext_b(cp);
        if (b) {
            *cls = CL_LATIN;
            return b;
        }
        *cls = CL_OTHER;
        return cp;
    }
    if (cp >= 0x1E00 && cp <= 0x1EFF) {
        *cls = CL_LATIN;
        return (uint32_t)s_latin_1e[cp - 0x1E00];
    }
    if (cp >= 0x0400 && cp <= 0x04FF) {
        uint32_t lc = cp;
        if (cp >= 0x0400 && cp <= 0x040F) lc = cp + 0x50;
        else if (cp >= 0x0410 && cp <= 0x042F) lc = cp + 0x20;
        else if (cp >= 0x0460 && !(cp & 1)) lc = cp + 1;  // extended block: even = upper
        if (lc == 0x0451 || lc == 0x0450) lc = 0x0435;    // ё ѐ -> е
        if (lc == 0x045D) lc = 0x0438;                    // ѝ -> и
        *cls = cyr_weight(lc) >= 0 ? CL_CYRILLIC : CL_OTHER;
        return lc;
    }
    if (cp >= 0x0370 && cp <= 0x03FF) {  // Greek: lowercase, tonos/dialytika folded
        switch (cp) {
        case 0x0386: case 0x03AC: cp = 0x03B1; break;
        case 0x0388: case 0x03AD: cp = 0x03B5; break;
        case 0x0389: case 0x03AE: cp = 0x03B7; break;
        case 0x038A: case 0x03AF: case 0x0390: case 0x03AA: case 0x03CA: cp = 0x03B9; break;
        case 0x038C: case 0x03CC: cp = 0x03BF; break;
        case 0x038E: case 0x03CD: case 0x03B0: case 0x03AB: case 0x03CB: cp = 0x03C5; break;
        case 0x038F: case 0x03CE: cp = 0x03C9; break;
        case 0x03C2: cp = 0x03C3; break;
        default:
            if (cp >= 0x0391 && cp <= 0x03A9) cp += 0x20;
            break;
        }
        *cls = (cp >= 0x03B1 && cp <= 0x03C9) ? CL_OTHER : CL_SYMBOL;
        return cp;
    }
    if (is_symbol_block(cp)) {
        *cls = CL_SYMBOL;
        return cp;
    }
    *cls = CL_OTHER;
    return cp;
}

uint32_t collate_fold(uint32_t cp) {
    char_class_t cls;
    uint32_t f = fold_classify(cp, &cls);
    return cls == CL_IGNORE ? 0 : f;
}

// ----------------------------------------------------------- key iterator ----
typedef struct {
    const uint8_t *p;      // next unread byte of the string
    uint8_t buf[6];        // pending key bytes of the current unit
    uint8_t n, i;
    uint32_t digits_left;  // digit bytes of the current run still to emit
    const uint8_t *dp;     // next digit of the current run
    bool started;          // a significant unit was produced
    bool sep;              // symbols seen since the last significant unit
} ckey_t;

static uint32_t next_cp(const uint8_t **p) {
    uint32_t cp;
    size_t n = utf8_decode((const char *)*p, &cp);
    *p += n;
    return cp;
}

static bool ascii_ieq(uint8_t a, char b) { return (a | 0x20) == (uint8_t)b; }

// Skip a leading English article when something significant follows it.
static const uint8_t *skip_article(const uint8_t *s) {
    static const char *const articles[] = {"the", "an", "a"};
    for (size_t k = 0; k < CORE_ARRAY_SIZE(articles); k++) {
        const char *a = articles[k];
        size_t len = strlen(a), i = 0;
        while (i < len && s[i] && ascii_ieq(s[i], a[i])) i++;
        if (i != len || s[len] != ' ') continue;
        const uint8_t *q = s + len;
        while (*q == ' ') q++;
        // Require a significant character after the article.
        const uint8_t *r = q;
        while (*r) {
            char_class_t cls;
            fold_classify(next_cp(&r), &cls);
            if (cls >= CL_DIGIT) return q;
        }
        return s;
    }
    return s;
}

static void ckey_init(ckey_t *k, const char *s, const collate_opts_t *opts) {
    memset(k, 0, sizeof *k);
    k->p = (const uint8_t *)(s ? s : "");
    if (opts && opts->ignore_articles) {
        // Articles are looked for after leading symbols: "  The Doors", "'The' Band".
        const uint8_t *q = k->p;
        while (*q) {
            const uint8_t *save = q;
            char_class_t cls;
            fold_classify(next_cp(&q), &cls);
            if (cls >= CL_DIGIT) {
                q = save;
                break;
            }
        }
        k->p = skip_article(q);
    }
}

static bool is_digit_at(const uint8_t *p, uint32_t *value) {
    const uint8_t *q = p;
    uint32_t cp = next_cp(&q);
    char_class_t cls;
    uint32_t f = fold_classify(cp, &cls);
    if (cls != CL_DIGIT) return false;
    *value = f - '0';
    return true;
}

static void advance_digit(const uint8_t **p) { (void)next_cp(p); }

// Next key byte, or -1 at the end.
static int ckey_next(ckey_t *k) {
    if (k->i < k->n) return k->buf[k->i++];
    if (k->digits_left) {
        uint32_t d = 0;
        is_digit_at(k->dp, &d);
        advance_digit(&k->dp);
        k->digits_left--;
        return (int)(0x30 + d);
    }
    for (;;) {
        if (!*k->p) return -1;
        const uint8_t *start = k->p;
        uint32_t cp = next_cp(&k->p);
        char_class_t cls;
        uint32_t f = fold_classify(cp, &cls);
        if (cls == CL_IGNORE) continue;
        if (cls == CL_SYMBOL) {
            if (k->started) k->sep = true;
            continue;
        }
        k->n = k->i = 0;
        if (k->sep) k->buf[k->n++] = 0x01;
        k->sep = false;
        k->started = true;
        if (cls == CL_DIGIT) {
            // Measure the run: skip leading zeros, count significant digits.
            const uint8_t *q = start;
            uint32_t d;
            while (is_digit_at(q, &d) && d == 0) advance_digit(&q);
            const uint8_t *first = q;
            uint32_t len = 0;
            while (is_digit_at(q, &d)) {
                advance_digit(&q);
                len++;
            }
            k->p = q;
            if (len < 15) {
                k->buf[k->n++] = (uint8_t)(0x10 + len);
            } else {
                k->buf[k->n++] = 0x1F;
                k->buf[k->n++] = (uint8_t)CORE_MIN(len, 255u);
            }
            k->dp = first;
            k->digits_left = len;
        } else if (cls == CL_LATIN) {
            k->buf[k->n++] = (uint8_t)(0x40 + (f - 'a'));
        } else if (cls == CL_CYRILLIC) {
            // Decomposed "й" (и + combining breve, as written by macOS).
            if (f == 0x0438) {
                const uint8_t *q = k->p;
                if (next_cp(&q) == 0x0306) {
                    f = 0x0439;
                    k->p = q;
                }
            }
            k->buf[k->n++] = (uint8_t)(0x60 + cyr_weight(f));
        } else {
            k->buf[k->n++] = (uint8_t)(0xE0 + (f >> 16));
            k->buf[k->n++] = (uint8_t)(f >> 8);
            k->buf[k->n++] = (uint8_t)f;
        }
        return k->buf[k->i++];
    }
}

int collate_cmp_primary(const char *a, const char *b, const collate_opts_t *opts) {
    ckey_t ka, kb;
    ckey_init(&ka, a, opts);
    ckey_init(&kb, b, opts);
    for (;;) {
        int x = ckey_next(&ka), y = ckey_next(&kb);
        if (x != y) return x < y ? -1 : 1;
        if (x < 0) return 0;
    }
}

int collate_cmp(const char *a, const char *b, const collate_opts_t *opts) {
    int r = collate_cmp_primary(a, b, opts);
    if (r) return r;
    r = strcmp(a ? a : "", b ? b : "");
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

size_t collate_key(const char *s, const collate_opts_t *opts, uint8_t *out, size_t max) {
    ckey_t k;
    ckey_init(&k, s, opts);
    size_t n = 0;
    for (int c; (c = ckey_next(&k)) >= 0; n++) {
        if (n < max) out[n] = (uint8_t)c;
    }
    return n;
}

int collate_key_cmp(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen) {
    size_t m = alen < blen ? alen : blen;
    int r = m ? memcmp(a, b, m) : 0;
    if (r) return r < 0 ? -1 : 1;
    return alen < blen ? -1 : alen > blen ? 1 : 0;
}

uint32_t collate_hash_primary(const char *s, const collate_opts_t *opts) {
    ckey_t k;
    ckey_init(&k, s, opts);
    uint32_t h = LIB_FNV32_INIT;
    for (int c; (c = ckey_next(&k)) >= 0;) {
        uint8_t b = (uint8_t)c;
        h = lib_fnv1a32(h, &b, 1);
    }
    return h;
}

// Uppercase jump letter for a folded letter of the given class.
static uint32_t letter_of(uint32_t f, char_class_t cls) {
    switch (cls) {
    case CL_LATIN: return 'A' + (f - 'a');
    case CL_CYRILLIC: return 0x0410 + (uint32_t)(cyr_weight(f) / 4);
    case CL_OTHER:
        if (f >= 0x03B1 && f <= 0x03C9) return f - 0x20;  // Greek
        return f;
    default: return '#';
    }
}

uint32_t collate_index_letter(const char *s, const collate_opts_t *opts) {
    ckey_t k;
    ckey_init(&k, s, opts);
    const uint8_t *p = k.p;
    while (*p) {
        uint32_t cp = next_cp(&p);
        char_class_t cls;
        uint32_t f = fold_classify(cp, &cls);
        if (cls == CL_IGNORE || cls == CL_SYMBOL) continue;
        // Decomposed "й" (и + combining breve) sorts as "й" in ckey_next(): same letter here,
        // so jump letters and the sort order agree.
        if (cls == CL_CYRILLIC && f == 0x0438) {
            const uint8_t *q = p;
            if (next_cp(&q) == 0x0306) f = 0x0439;
        }
        return letter_of(f, cls);
    }
    return '#';
}

uint32_t collate_letter_rank(uint32_t letter) {
    char_class_t cls;
    uint32_t f = fold_classify(letter, &cls);
    switch (cls) {
    case CL_LATIN: return (uint32_t)(0x40 + (f - 'a')) << 16;
    case CL_CYRILLIC: return (uint32_t)(0x60 + (cyr_weight(f) & ~3)) << 16;
    case CL_OTHER: return (uint32_t)(0xE0 + (f >> 16)) << 16 | (f & 0xFFFF);
    default: return 0;
    }
}
