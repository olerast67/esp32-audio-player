// SPDX-License-Identifier: Apache-2.0
// Text encodings (UTF-8/16, Latin-1, CP1251, detection) and collation.
#include <stdlib.h>
#include <string.h>

#include "audio_player/collate.h"
#include "audio_player/text.h"
#include "library/lib_priv.h"
#include "test.h"

static char g_buf[4096];

static const char *conv(const char *in, size_t len, text_encoding_t enc) {
    text_to_utf8((const uint8_t *)in, len, enc, g_buf, sizeof g_buf);
    return g_buf;
}

static size_t read_data(const char *name, uint8_t *buf, size_t cap) {
    char path[512];
    snprintf(path, sizeof path, "%s/library/%s", TEST_DATA_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

TEST(utf8_validation) {
    CHECK(text_is_valid_utf8((const uint8_t *)"Привет, world", 20));
    CHECK(text_is_valid_utf8((const uint8_t *)"\xEF\xBF\xBD", 3));      // literal U+FFFD
    CHECK(!text_is_valid_utf8((const uint8_t *)"\xC0\x80", 2));         // overlong NUL
    CHECK(!text_is_valid_utf8((const uint8_t *)"\xE0\x80\xAF", 3));     // overlong '/'
    CHECK(!text_is_valid_utf8((const uint8_t *)"\xED\xA0\x80", 3));     // surrogate
    CHECK(!text_is_valid_utf8((const uint8_t *)"\xF4\x90\x80\x80", 4)); // > U+10FFFF
    CHECK(!text_is_valid_utf8((const uint8_t *)"ab\xD0", 3));           // truncated
    CHECK(!text_is_valid_utf8((const uint8_t *)"\xFF", 1));
    // Invalid sequences become U+FFFD, valid text around them survives.
    CHECK_STR(conv("a\xC0\x80z", 4, TEXT_ENC_UTF8), "a\xEF\xBF\xBD\xEF\xBF\xBDz");
    CHECK_STR(conv("\xEF\xBB\xBFhi", 5, TEXT_ENC_UTF8), "hi");  // BOM stripped
    CHECK_STR(conv("ab\xD0", 3, TEXT_ENC_UTF8), "ab\xEF\xBF\xBD");
    // Stops at NUL.
    CHECK_STR(conv("abc\0def", 7, TEXT_ENC_UTF8), "abc");
}

TEST(utf8_helpers) {
    char out[4];
    uint32_t cp;
    CHECK_EQ_INT(utf8_encode(0x41, out), 1);
    CHECK_EQ_INT(utf8_encode(0x0416, out), 2);  // Ж
    CHECK_EQ_INT(utf8_decode(out, &cp), 2);
    CHECK_EQ_INT(cp, 0x0416);
    CHECK_EQ_INT(utf8_encode(0x20AC, out), 3);
    CHECK_EQ_INT(utf8_encode(0x1F600, out), 4);
    CHECK_EQ_INT(utf8_decode(out, &cp), 4);
    CHECK_EQ_INT(cp, 0x1F600);
    CHECK_EQ_INT(utf8_encode(0xD800, out), 3);  // surrogate -> U+FFFD
    CHECK_EQ_INT(utf8_decode("\xD0", &cp), 1);
    CHECK_EQ_INT(cp, 0xFFFD);
    CHECK_EQ_INT(utf8_strlen("Ёжик"), 4);
    CHECK_EQ_INT(utf8_strlen(""), 0);
    char s[32] = "Привет";  // 12 bytes
    utf8_truncate(s, 5);
    CHECK_STR(s, "Пр");
    strcpy(s, "ab\xD0\xB0");
    utf8_truncate(s, 4);
    CHECK_STR(s, "ab\xD0\xB0");
    utf8_truncate(s, 3);
    CHECK_STR(s, "ab");
    strcpy(s, " \t Кино \r\n");
    text_trim(s);
    CHECK_STR(s, "Кино");
    strcpy(s, "   ");
    text_trim(s);
    CHECK_STR(s, "");
}

TEST(cp1251_latin1_utf16) {
    // "Привет, Ёжик №5 «кавычки» — тест" in CP1251.
    const char cp[] = "\xCF\xF0\xE8\xE2\xE5\xF2, \xA8\xE6\xE8\xEA \xB9"
                      "5 \xAB\xEA\xE0\xE2\xFB\xF7\xEA\xE8\xBB \x97 \xF2\xE5\xF1\xF2";
    CHECK_STR(conv(cp, sizeof cp - 1, TEXT_ENC_CP1251), "Привет, Ёжик №5 «кавычки» — тест");
    // Ukrainian letters of CP1251.
    CHECK_STR(conv("\xB2\xB3\xAA\xBA\xAF\xBF\xA5\xB4", 8, TEXT_ENC_CP1251), "ІіЄєЇїҐґ");
    // Latin-1 with the Windows-1252 punctuation block.
    CHECK_STR(conv("\x93" "Caf\xE9\x94 \x80 \xFF", 10, TEXT_ENC_LATIN1), "“Café” € ÿ");
    CHECK_STR(conv("\x81", 1, TEXT_ENC_LATIN1), "\xEF\xBF\xBD");
    // UTF-16 with BOM (LE and BE), without BOM, surrogate pairs.
    CHECK_STR(conv("\xFF\xFE\x1F\x04\x40\x04\x38\x04", 8, TEXT_ENC_UTF16_BOM), "При");
    CHECK_STR(conv("\xFE\xFF\x04\x1F\x04\x40", 6, TEXT_ENC_UTF16_BOM), "Пр");
    CHECK_STR(conv("\x41\x00\x42\x00", 4, TEXT_ENC_UTF16_BOM), "AB");  // no BOM: little-endian
    CHECK_STR(conv("\x04\x1F\x00\x41", 4, TEXT_ENC_UTF16BE), "ПA");
    CHECK_STR(conv("\xFF\xFE\x3D\xD8\x00\xDE", 6, TEXT_ENC_UTF16_BOM), "\xF0\x9F\x98\x80");  // U+1F600
    CHECK_STR(conv("\xFF\xFE\x3D\xD8\x41\x00", 6, TEXT_ENC_UTF16_BOM), "\xEF\xBF\xBD" "A");  // lone surrogate
    CHECK_STR(conv("\xFF\xFE\x41\x00\x00\x00\x42\x00", 8, TEXT_ENC_UTF16_BOM), "A");         // stops at NUL
    CHECK_STR(conv("\xFF\xFE\x41", 3, TEXT_ENC_UTF16_BOM), "");                              // odd byte ignored
    // Output never splits a code point.
    char small[5];
    CHECK_EQ_INT(text_to_utf8((const uint8_t *)"\xCF\xF0\xE8", 3, TEXT_ENC_CP1251, small, sizeof small), 4);
    CHECK_STR(small, "Пр");
    CHECK_EQ_INT(text_to_utf8((const uint8_t *)"abc", 3, TEXT_ENC_UTF8, small, 1), 0);
    CHECK_STR(small, "");
}

TEST(detect_phrases) {
    // CP1251 bytes of typical Russian tags.
    static const char *const ru[] = {
        "\xCA\xE8\xED\xEE",                                  // Кино
        "\xC3\xF0\xF3\xEF\xEF\xE0 \xEA\xF0\xEE\xE2\xE8",     // Группа крови
        "\xC4\xC4\xD2",                                      // ДДТ
        "\xDF",                                              // Я
        "\xC2\xE8\xEA\xF2\xEE\xF0 \xD6\xEE\xE9",             // Виктор Цой
        "\xCA\xE8\xED\xEE (Live 1988)",                      // Кино (Live 1988)
        "DJ \xC3\xF0\xF3\xE2",                               // DJ Грув
        "\xA8\xEB\xEA\xE0",                                  // Ёлка
        "\xC1\xE8-2",                                        // Би-2
    };
    for (size_t i = 0; i < sizeof ru / sizeof *ru; i++) {
        text_encoding_t e = text_detect_legacy((const uint8_t *)ru[i], strlen(ru[i]));
        if (e != TEXT_ENC_CP1251) printf("  ru[%u] detected as %d\n", (unsigned)i, (int)e);
        CHECK_EQ_INT(e, TEXT_ENC_CP1251);
    }
    // Latin-1 bytes of Western tags.
    static const char *const west[] = {
        "Bj\xF6rk", "M\xF6tley Cr\xFC" "e", "Sigur R\xF3s", "D\xE9j\xE0 vu", "Caf\xE9 del Mar",
        "Voyage \xE0 Paris", "Gr\xF6\xDF" "e", "\xC6nima", "Ma\xF1" "ana", "Beyonc\xE9",
    };
    for (size_t i = 0; i < sizeof west / sizeof *west; i++) {
        text_encoding_t e = text_detect_legacy((const uint8_t *)west[i], strlen(west[i]));
        if (e != TEXT_ENC_LATIN1) printf("  west[%u] detected as %d\n", (unsigned)i, (int)e);
        CHECK_EQ_INT(e, TEXT_ENC_LATIN1);
    }
    CHECK_EQ_INT(text_detect_legacy((const uint8_t *)"Pink Floyd", 10), TEXT_ENC_UTF8);  // ASCII
    CHECK_EQ_INT(text_detect_legacy((const uint8_t *)"Кино", 8), TEXT_ENC_UTF8);
    CHECK_EQ_INT(text_detect_legacy((const uint8_t *)"Björk", 6), TEXT_ENC_UTF8);
    // AUTO converts accordingly.
    CHECK_STR(conv("\xCA\xE8\xED\xEE", 4, TEXT_ENC_AUTO_LEGACY), "Кино");
    CHECK_STR(conv("Bj\xF6rk", 5, TEXT_ENC_AUTO_LEGACY), "Björk");
    CHECK_STR(conv("Кино", 8, TEXT_ENC_AUTO_LEGACY), "Кино");
}

TEST(detect_samples) {
    static uint8_t buf[8192];
    size_t n = read_data("ru_cp1251.txt", buf, sizeof buf);
    CHECK(n > 100);
    CHECK_EQ_INT(text_detect_legacy(buf, n), TEXT_ENC_CP1251);
    static char utf[16384];
    text_to_utf8(buf, n, TEXT_ENC_AUTO_LEGACY, utf, sizeof utf);
    static uint8_t ref[16384];
    size_t rn = read_data("ru_utf8.txt", ref, sizeof ref - 1);
    ref[rn] = 0;
    CHECK_STR(utf, (const char *)ref);
    CHECK_EQ_INT(text_detect_legacy(ref, rn), TEXT_ENC_UTF8);
    n = read_data("west_latin1.txt", buf, sizeof buf);
    CHECK(n > 100);
    CHECK_EQ_INT(text_detect_legacy(buf, n), TEXT_ENC_LATIN1);
    text_to_utf8(buf, n, TEXT_ENC_AUTO_LEGACY, utf, sizeof utf);
    CHECK(strstr(utf, "Mötley Crüe") != NULL);
    CHECK(strstr(utf, "Ágætis byrjun") != NULL);
    n = read_data("en_ascii.txt", buf, sizeof buf);
    CHECK(n > 50);
    CHECK_EQ_INT(text_detect_legacy(buf, n), TEXT_ENC_UTF8);
    // Each Russian line alone is detected too.
    size_t lines = 0, good = 0;
    n = read_data("ru_cp1251.txt", buf, sizeof buf - 1);
    buf[n] = 0;
    for (char *line = (char *)buf; *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        lines++;
        if (text_detect_legacy((const uint8_t *)line, strlen(line)) == TEXT_ENC_CP1251) good++;
        if (!end) break;
        line = end + 1;
    }
    CHECK(lines >= 10);
    CHECK_EQ_INT(good, lines);
}

// ------------------------------------------------------------- collation ----
static int cmp_plain(const void *a, const void *b) {
    return collate_cmp(*(const char *const *)a, *(const char *const *)b, NULL);
}

TEST(collation_order) {
    const char *expected[] = {
        "",           "!!!",        "2 Unlimited", "10 Years",  "50 Cent", "ABBA",       "Äpfel",
        "apple",      "Bjork",      "Björk",       "bjork",     "Bravo",   "Đà Lạt",     "Ewa",
        "The Cure",   "Track 2",    "Track 10",    "Zebra",     "Ария",    "Ёлка",       "Ель",
        "Жуки",       "Иван",       "Йод",         "Кино",      "Яблоко",  "Ωmega",      "你好",
    };
    enum { N = sizeof expected / sizeof *expected };
    const char *list[N];
    // Deterministic shuffle.
    for (int i = 0; i < N; i++) list[i] = expected[(i * 11 + 5) % N];
    qsort(list, N, sizeof *list, cmp_plain);
    for (int i = 0; i < N; i++) {
        if (strcmp(list[i], expected[i]) != 0) printf("  pos %d: got \"%s\", want \"%s\"\n", i, list[i], expected[i]);
        CHECK_STR(list[i], expected[i]);
    }
    // Letters never go backwards along the sorted list.
    uint32_t prev = 0;
    for (int i = 0; i < N; i++) {
        uint32_t r = collate_letter_rank(collate_index_letter(expected[i], NULL));
        CHECK(r >= prev);
        prev = r;
        // Primary keys agree with collate_cmp.
        if (i) {
            uint8_t ka[64], kb[64];
            size_t la = collate_key(expected[i - 1], NULL, ka, sizeof ka);
            size_t lb = collate_key(expected[i], NULL, kb, sizeof kb);
            CHECK(collate_key_cmp(ka, la, kb, lb) <= 0);
        }
    }
    CHECK(collate_cmp("abc", "abc", NULL) == 0);
    CHECK(collate_cmp(NULL, "", NULL) == 0);
}

TEST(collation_rules) {
    collate_opts_t art = {true};
    // Case, diacritics, ё = е, punctuation runs.
    CHECK_EQ_INT(collate_cmp_primary("Pink Floyd", "pink floyd", NULL), 0);
    CHECK_EQ_INT(collate_cmp_primary("AC/DC", "AC - DC", NULL), 0);
    CHECK_EQ_INT(collate_cmp_primary("Ёлка", "елка", NULL), 0);
    CHECK_EQ_INT(collate_cmp_primary("Sigur Rós", "Sigur Ros", NULL), 0);
    CHECK_EQ_INT(collate_cmp_primary("Đà Lạt", "da lat", NULL), 0);
    CHECK_EQ_INT(collate_cmp_primary("Phở Ưng Ơi", "pho ung oi", NULL), 0);
    CHECK_EQ_INT(collate_cmp_primary("Straße", "strase", NULL), 0);
    CHECK_EQ_INT(collate_cmp_primary("Ｔｏｋｙｏ", "tokyo", NULL), 0);  // fullwidth
    // Decomposed (NFD, macOS file names).
    CHECK_EQ_INT(collate_cmp_primary("Cafe\xCC\x81", "Café", NULL), 0);
    CHECK_EQ_INT(collate_cmp_primary("\xD0\x95\xCC\x88\xD0\xBB\xD0\xBA\xD0\xB0", "Ёлка", NULL), 0);
    CHECK_EQ_INT(collate_cmp_primary("\xD0\xB8\xCC\x86\xD0\xBE\xD0\xB4", "йод", NULL), 0);
    CHECK(collate_cmp("и", "й", NULL) < 0);
    // Natural numbers.
    CHECK(collate_cmp("Track 2", "Track 10", NULL) < 0);
    CHECK(collate_cmp("Track 9", "Track 10", NULL) < 0);
    CHECK_EQ_INT(collate_cmp_primary("Track 02", "Track 2", NULL), 0);
    CHECK(collate_cmp("Track 02", "Track 2", NULL) != 0);  // total order: raw bytes break the tie
    CHECK(collate_cmp("1.mp3", "10.mp3", NULL) < 0);
    CHECK(collate_cmp("99", "100", NULL) < 0);
    CHECK(collate_cmp("12345678901234567", "12345678901234568", NULL) < 0);  // long run
    CHECK(collate_cmp("123456789012345678", "12345678901234567", NULL) > 0);
    // Separators sort before letters; shorter first.
    CHECK(collate_cmp("A B", "AB", NULL) < 0);
    CHECK(collate_cmp("A", "A B", NULL) < 0);
    // Ukrainian letters next to their Russian neighbours.
    CHECK(collate_cmp("и", "і", NULL) < 0);
    CHECK(collate_cmp("і", "ї", NULL) < 0);
    CHECK(collate_cmp("ї", "й", NULL) < 0);
    CHECK(collate_cmp("г", "ґ", NULL) < 0);
    CHECK(collate_cmp("ґ", "д", NULL) < 0);
    // Articles.
    CHECK(collate_cmp("The Beatles", "Cars", &art) < 0);
    CHECK(collate_cmp("The Beatles", "Abba", &art) > 0);
    CHECK(collate_cmp("The Beatles", "Cars", NULL) > 0);
    CHECK_EQ_INT(collate_index_letter("The Beatles", &art), 'B');
    CHECK_EQ_INT(collate_index_letter("A Tribe Called Quest", &art), 'T');
    CHECK_EQ_INT(collate_index_letter("An Horse", &art), 'H');
    CHECK_EQ_INT(collate_index_letter("The", &art), 'T');       // nothing after the article
    CHECK_EQ_INT(collate_index_letter("Theatre", &art), 'T');
    CHECK_EQ_INT(collate_index_letter("A-ha", &art), 'A');      // no space after "A"
    CHECK_EQ_INT(collate_index_letter("  The Doors", &art), 'D');
    CHECK_EQ_INT(collate_cmp_primary("The Beatles", "Beatles", &art), 0);
}

TEST(fold_and_letters) {
    CHECK_EQ_INT(collate_fold('A'), 'a');
    CHECK_EQ_INT(collate_fold(0x00C9), 'e');   // É
    CHECK_EQ_INT(collate_fold(0x0401), 0x0435); // Ё -> е
    CHECK_EQ_INT(collate_fold(0x0451), 0x0435); // ё -> е
    CHECK_EQ_INT(collate_fold(0x0416), 0x0436); // Ж -> ж
    CHECK_EQ_INT(collate_fold(0x1EA1), 'a');   // ạ
    CHECK_EQ_INT(collate_fold(0x1EF9), 'y');   // ỹ
    CHECK_EQ_INT(collate_fold(0x01B0), 'u');   // ư
    CHECK_EQ_INT(collate_fold(0x0111), 'd');   // đ
    CHECK_EQ_INT(collate_fold(0x0301), 0);     // combining acute: ignorable
    CHECK_EQ_INT(collate_fold(0x200B), 0);     // zero width space
    CHECK_EQ_INT(collate_fold(0x03A9), 0x03C9); // Ω -> ω
    CHECK_EQ_INT(collate_index_letter("123 Go", NULL), '#');
    CHECK_EQ_INT(collate_index_letter("(hed) p.e.", NULL), 'H');
    CHECK_EQ_INT(collate_index_letter("ёжик", NULL), 0x0415);  // Е
    CHECK_EQ_INT(collate_index_letter("Über", NULL), 'U');
    CHECK_EQ_INT(collate_index_letter("ωmega", NULL), 0x03A9);
    CHECK_EQ_INT(collate_index_letter("", NULL), '#');
    CHECK_EQ_INT(collate_index_letter("!!!", NULL), '#');
    CHECK_EQ_INT(collate_index_letter("你好", NULL), 0x4F60);
    CHECK_EQ_INT(collate_index_letter("їжак", NULL), 0x0418);  // И
    // Decomposed "й" (macOS file names: и + U+0306) has the letter it sorts under.
    CHECK_EQ_INT(collate_index_letter("\xD0\xB8\xCC\x86" "од", NULL), collate_index_letter("йод", NULL));
    CHECK(collate_index_letter("\xD0\xB8\xCC\x86" "од", NULL) != collate_index_letter("игра", NULL));
    CHECK_EQ_INT(collate_cmp_primary("\xD0\xB8\xCC\x86" "од", "йод", NULL), 0);
    CHECK(collate_letter_rank('#') < collate_letter_rank('A'));
    CHECK(collate_letter_rank('A') < collate_letter_rank('Z'));
    CHECK(collate_letter_rank('Z') < collate_letter_rank(0x0410));
    CHECK(collate_letter_rank(0x0410) < collate_letter_rank(0x042F));
    CHECK(collate_letter_rank(0x042F) < collate_letter_rank(0x03A9));
    CHECK_EQ_INT(collate_letter_rank(0x0401), collate_letter_rank(0x0415));  // Ё jumps with Е
    CHECK_EQ_INT(collate_letter_rank('a'), collate_letter_rank('A'));
}

TEST(collation_fuzz) {
    // Random bytes (invalid UTF-8 included) must compare consistently and never crash.
    uint32_t seed = 12345;
    char a[40], b[40];
    for (int it = 0; it < 20000; it++) {
        for (int i = 0; i < 39; i++) {
            seed = seed * 1103515245u + 12345u;
            a[i] = (char)((seed >> 16) & 0xFF);
            seed = seed * 1103515245u + 12345u;
            b[i] = (char)((seed >> 16) & 0xFF);
        }
        a[(seed >> 8) % 40] = 0;
        a[39] = b[39] = 0;
        int x = collate_cmp(a, b, NULL), y = collate_cmp(b, a, NULL);
        CHECK(x == -y);
        (void)collate_index_letter(a, NULL);
        uint8_t key[16];
        (void)collate_key(a, NULL, key, sizeof key);
        char out[64];
        text_to_utf8((const uint8_t *)a, 39, TEXT_ENC_AUTO_LEGACY, out, sizeof out);
        CHECK(text_is_valid_utf8((const uint8_t *)out, strlen(out)));
        if (g_test_failures) break;
    }
}

TEST_MAIN(RUN(utf8_validation) RUN(utf8_helpers) RUN(cp1251_latin1_utf16) RUN(detect_phrases) RUN(detect_samples)
              RUN(collation_order) RUN(collation_rules) RUN(fold_and_letters) RUN(collation_fuzz))
