// SPDX-License-Identifier: Apache-2.0
// Decoder golden tests over the vectors made by tools/gen_test_vectors.py (test/data).
//
// - every vector opens with the expected codec/format/length and decodes to the expected CRC
//   (lossless files: bit-exact against numpy's own conversion)
// - lossy files reach > 30 dB SNR against their lossless source, aligned at lag 0 (gapless)
// - seeking lands on the same samples as a continuous decode
// - the FLAC STREAMINFO MD5 matches our decoded output packed to the source bit depth
// - a gapless pair decodes to the continuous signal (FLAC exact, MP3 within tolerance)
// - DoP markers alternate and the DSD payload carries a recognisable 1 kHz tone
#include <stdlib.h>

#include "audio_player/decoder.h"
#include "decoders/dec_flac_internal.h"
#include "test.h"

// --------------------------------------------------------------- helpers ----
typedef struct {
    char name[64], codec[16], ref[64];
    unsigned rate, channels, bits, dop;
    unsigned long long frames;
    unsigned crc;
    bool has_crc;
} vector_t;

static vector_t g_vec[64];
static int g_nvec;

static const char *data_path(const char *name) {
    static char buf[4][512];
    static int slot;
    slot = (slot + 1) % 4;
    snprintf(buf[slot], sizeof buf[slot], "%s/%s", TEST_DATA_DIR, name);
    return buf[slot];
}

static bool load_manifest(void) {
    FILE *f = fopen(data_path("vectors.txt"), "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof line, f) && g_nvec < (int)CORE_ARRAY_SIZE(g_vec)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        vector_t *v = &g_vec[g_nvec];
        char crc[16];
        if (sscanf(line, "%63s %15s %u %u %u %u %llu %15s %63s", v->name, v->codec, &v->rate, &v->channels, &v->bits,
                   &v->dop, &v->frames, crc, v->ref) != 9) {
            continue;
        }
        v->has_crc = strcmp(crc, "-") != 0;
        v->crc = v->has_crc ? (unsigned)strtoul(crc, NULL, 16) : 0;
        g_nvec++;
    }
    fclose(f);
    return g_nvec > 0;
}

static const vector_t *find_vector(const char *name) {
    for (int i = 0; i < g_nvec; i++) {
        if (strcmp(g_vec[i].name, name) == 0) return &g_vec[i];
    }
    return NULL;
}

static uint32_t crc32_le(const int32_t *pcm, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)pcm[i];
        for (int b = 0; b < 4; b++) {
            crc ^= (v >> (8 * b)) & 0xFF;
            for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

static decoder_t *open_vector(const char *name, decoder_info_t *info, int *err) {
    core_stream_t *s = core_stream_open_file(data_path(name), 0);
    if (!s) {
        if (err) *err = CORE_ENOTFOUND;
        return NULL;
    }
    return decoder_open(s, name, info, err);
}

// Decodes to the end in chunks of varying size. Returns frames decoded, pcm owned by the caller.
static uint64_t decode_rest(decoder_t *d, unsigned channels, int32_t **pcm_out, int *err) {
    static const uint32_t k_chunks[] = {4096, 1, 777, 1152, 3, 8192, 100};
    size_t cap = 65536, n = 0;
    int32_t *pcm = malloc(cap * channels * sizeof *pcm);
    int e = CORE_OK;
    for (int i = 0; pcm; i++) {
        uint32_t want = k_chunks[i % CORE_ARRAY_SIZE(k_chunks)];
        if (n + want > cap) {
            cap *= 2;
            int32_t *p = realloc(pcm, cap * channels * sizeof *pcm);
            if (!p) break;
            pcm = p;
        }
        int32_t got = decoder_read(d, pcm + n * channels, want);
        if (got < 0) e = got;
        if (got <= 0) break;
        if ((uint32_t)got > want) {
            e = CORE_ERR;
            break;
        }
        n += (size_t)got;
    }
    if (err) *err = e;
    *pcm_out = pcm;
    return n;
}

static uint64_t decode_file(const char *name, decoder_info_t *info, int32_t **pcm, int *err) {
    decoder_t *d = open_vector(name, info, err);
    *pcm = NULL;
    if (!d) return 0;
    uint64_t n = decode_rest(d, info->fmt.channels, pcm, err);
    decoder_close(d);
    return n;
}

// Best lag (b relative to a) in [-max_lag, max_lag] by cross-correlation over all channels
// (the right channel of the test signal is a sweep, so the peak is unique).
static int best_lag(const int32_t *a, uint64_t na, const int32_t *b, uint64_t nb, unsigned ch, int max_lag) {
    double best = -1e300;
    int lag_best = 0;
    uint64_t n = CORE_MIN(na, nb);
    for (int lag = -max_lag; lag <= max_lag; lag++) {
        double acc = 0;
        for (uint64_t i = (uint64_t)max_lag; i + (uint64_t)max_lag < n; i += 7) {
            uint64_t j = (uint64_t)((int64_t)i + lag);
            for (unsigned c = 0; c < ch; c++) acc += (double)a[i * ch + c] * (double)b[j * ch + c];
        }
        if (acc > best) {
            best = acc;
            lag_best = lag;
        }
    }
    return lag_best;
}

// SNR in dB of b against a over [from, to) frames, b shifted by lag.
static double snr_db(const int32_t *a, const int32_t *b, unsigned ch, uint64_t from, uint64_t to, int lag) {
    double sig = 0, err = 0;
    for (uint64_t i = from; i < to; i++) {
        for (unsigned c = 0; c < ch; c++) {
            double x = a[i * ch + c], y = b[(uint64_t)((int64_t)i + lag) * ch + c];
            sig += x * x;
            err += (x - y) * (x - y);
        }
    }
    if (err == 0) return 999.0;
    return 10.0 * log10(sig / err);
}

// ------------------------------------------------------------------ MD5 ----
// RFC 1321, compact implementation for the STREAMINFO check.
typedef struct {
    uint32_t h[4];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} md5_t;

static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

static void md5_block(md5_t *m, const uint8_t *p) {
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t w[16];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 |
               (uint32_t)p[4 * i + 3] << 24;
    }
    uint32_t a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + rol(a + f + k[i] + w[g], r[i]);
        a = t;
    }
    m->h[0] += a;
    m->h[1] += b;
    m->h[2] += c;
    m->h[3] += d;
}

static void md5_init(md5_t *m) {
    m->h[0] = 0x67452301;
    m->h[1] = 0xefcdab89;
    m->h[2] = 0x98badcfe;
    m->h[3] = 0x10325476;
    m->len = 0;
    m->fill = 0;
}

static void md5_update(md5_t *m, const uint8_t *p, size_t n) {
    m->len += n;
    while (n--) {
        m->buf[m->fill++] = *p++;
        if (m->fill == 64) {
            md5_block(m, m->buf);
            m->fill = 0;
        }
    }
}

static void md5_final(md5_t *m, uint8_t out[16]) {
    uint64_t bits = m->len * 8;
    uint8_t pad = 0x80;
    md5_update(m, &pad, 1);
    pad = 0;
    while (m->fill != 56) md5_update(m, &pad, 1);
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(bits >> (8 * i));
        md5_update(m, &b, 1);
    }
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(m->h[i / 4] >> (8 * (i % 4)));
}

// ------------------------------------------------------------------ tests ----
TEST(manifest) {
    CHECK(load_manifest());
    CHECK(g_nvec >= 20);
}

TEST(formats_and_crc) {
    for (int i = 0; i < g_nvec; i++) {
        const vector_t *v = &g_vec[i];
        decoder_info_t info;
        int32_t *pcm = NULL;
        int err = 0;
        uint64_t n = decode_file(v->name, &info, &pcm, &err);
        g_test_checks++;
        if (err != CORE_OK || !pcm) {
            TEST_FAIL_("%s: decode failed: %s", v->name, core_err_name(err));
            free(pcm);
            continue;
        }
        CHECK_STR(codec_name(info.codec), v->codec);
        CHECK_EQ_INT(info.fmt.sample_rate, v->rate);
        CHECK_EQ_INT(info.fmt.channels, v->channels);
        CHECK_EQ_INT(info.fmt.bits, v->bits);
        CHECK_EQ_INT(info.fmt.dop, v->dop);
        CHECK(info.seekable);
        CHECK(info.bitrate_kbps > 0);
        CHECK_EQ_INT(codec_is_lossless(info.codec), v->has_crc);
        g_test_checks++;
        if (info.total_frames != v->frames || n != v->frames) {
            TEST_FAIL_("%s: frames header %llu decoded %llu expected %llu", v->name,
                       (unsigned long long)info.total_frames, (unsigned long long)n, v->frames);
        }
        if (v->has_crc) {
            uint32_t crc = crc32_le(pcm, (size_t)n * v->channels);
            g_test_checks++;
            if (crc != v->crc) TEST_FAIL_("%s: crc %08x expected %08x", v->name, crc, v->crc);
        }
        printf("  %-26s %-6s %6u Hz %u ch %2u bit %6llu frames %5u kbps\n", v->name, codec_name(info.codec),
               info.fmt.sample_rate, info.fmt.channels, info.fmt.bits, (unsigned long long)n, info.bitrate_kbps);
        free(pcm);
    }
}

TEST(lossy_snr_and_alignment) {
    for (int i = 0; i < g_nvec; i++) {
        const vector_t *v = &g_vec[i];
        if (strcmp(v->ref, "-") == 0 || strncmp(v->name, "gap_", 4) == 0) continue;
        decoder_info_t ia, ib;
        int32_t *a = NULL, *b = NULL;
        int ea = 0, eb = 0;
        uint64_t na = decode_file(v->ref, &ia, &a, &ea);
        uint64_t nb = decode_file(v->name, &ib, &b, &eb);
        CHECK_EQ_INT(ea, CORE_OK);
        CHECK_EQ_INT(eb, CORE_OK);
        if (a && b && ea == CORE_OK && eb == CORE_OK && ia.fmt.channels == ib.fmt.channels) {
            unsigned ch = ia.fmt.channels;
            int lag = best_lag(a, na, b, nb, ch, 2048);
            uint64_t n = CORE_MIN(na, nb);
            double snr = snr_db(a, b, ch, 2048, n - 2048, lag);
            printf("  %-26s vs %-18s lag %5d  SNR %6.1f dB\n", v->name, v->ref, lag, snr);
            CHECK_EQ_INT(lag, 0);  // gapless trimming puts the first sample where the source had it
            CHECK(snr > 30.0);
        }
        free(a);
        free(b);
    }
}

static bool frames_equal(const int32_t *a, const int32_t *b, size_t samples, int32_t tol) {
    for (size_t i = 0; i < samples; i++) {
        int64_t d = (int64_t)a[i] - b[i];
        if (d > tol || d < -tol) return false;
    }
    return true;
}

// Seeks a fresh decoder to `at` and compares `len` frames with the continuous decode `ref`.
static void check_seek(const vector_t *v, decoder_t *d, const int32_t *ref, uint64_t n, uint64_t at, bool exact) {
    unsigned ch = v->channels;
    int r = decoder_seek(d, at);
    g_test_checks++;
    if (r != CORE_OK) {
        TEST_FAIL_("%s: seek to %llu failed: %s", v->name, (unsigned long long)at, core_err_name(r));
        return;
    }
    CHECK_EQ_INT(decoder_tell(d), at);
    uint32_t len = (uint32_t)CORE_MIN((uint64_t)4096, n - at);
    int32_t *buf = malloc((size_t)len * ch * sizeof *buf);
    uint32_t got = 0;
    while (buf && got < len) {
        int32_t k = decoder_read(d, buf + (size_t)got * ch, len - got);
        if (k <= 0) break;
        got += (uint32_t)k;
    }
    CHECK_EQ_INT(got, len);
    CHECK_EQ_INT(decoder_tell(d), at + got);
    if (buf && got == len) {
        g_test_checks++;
        if (exact) {
            if (!frames_equal(ref + at * ch, buf, (size_t)len * ch, 0)) {
                TEST_FAIL_("%s: data after seek to %llu differs", v->name, (unsigned long long)at);
            }
        } else {
            // Approximate (VBR MP3 without an index yet): must land within 100 ms, same audio.
            int max_lag = (int)(v->rate / 10);
            int64_t lo = (int64_t)at - max_lag, hi = (int64_t)at + max_lag;
            double best = -1;
            int64_t best_pos = -1;
            uint64_t from = CORE_MIN((uint64_t)256, (uint64_t)len / 4);  // skip the decoder warm-up
            for (int64_t p = lo < 0 ? 0 : lo; p <= hi && p + len <= (int64_t)n; p++) {
                double s = snr_db(ref + (size_t)p * ch, buf, ch, from, len, 0);
                if (s > best) {
                    best = s;
                    best_pos = p;
                }
            }
            printf("  %-26s approximate seek to %llu landed at %lld (SNR %.1f dB)\n", v->name,
                   (unsigned long long)at, (long long)best_pos, best);
            if (best < 30.0) TEST_FAIL_("%s: approximate seek did not land within 100 ms", v->name);
        }
    }
    free(buf);
}

TEST(seek_matches_continuous_decode) {
    for (int i = 0; i < g_nvec; i++) {
        const vector_t *v = &g_vec[i];
        decoder_info_t info;
        int32_t *ref = NULL;
        int err = 0;
        uint64_t n = decode_file(v->name, &info, &ref, &err);
        if (!ref || err != CORE_OK || n < 8) {
            free(ref);
            continue;
        }
        bool mp3 = info.codec == CODEC_MP3;
        bool vbr_mp3 = mp3 && strstr(v->name, "cbr") == NULL;
        decoder_t *d = open_vector(v->name, &info, &err);
        CHECK(d != NULL);
        if (!d) {
            free(ref);
            continue;
        }
        // Fresh decoder: middle, then near the start, then back to 0 and to the end.
        check_seek(v, d, ref, n, n / 2 + 123, !vbr_mp3);
        check_seek(v, d, ref, n, 1, true);
        check_seek(v, d, ref, n, n - CORE_MIN(n, (uint64_t)100), !vbr_mp3);
        check_seek(v, d, ref, n, 0, true);
        // Deterministic pseudo-random positions (after a full pass MP3 seeks use the frame index).
        int32_t scratch[2 * 1024];
        while (decoder_read(d, scratch, 1024) > 0) {
        }
        uint32_t x = 12345u + (uint32_t)i;
        for (int k = 0; k < 12; k++) {
            x = x * 1103515245u + 12345u;
            check_seek(v, d, ref, n, (x >> 8) % n, true);
        }
        CHECK_EQ_INT(decoder_seek(d, n), CORE_OK);
        CHECK_EQ_INT(decoder_read(d, scratch, 16), 0);
        CHECK_EQ_INT(decoder_seek(d, n + 1), CORE_EINVAL);
        decoder_close(d);
        free(ref);
    }
}

TEST(flac_md5_matches_streaminfo) {
    for (int i = 0; i < g_nvec; i++) {
        const vector_t *v = &g_vec[i];
        if (strcmp(v->codec, "FLAC") != 0) continue;
        decoder_info_t info;
        int err = 0;
        decoder_t *d = open_vector(v->name, &info, &err);
        CHECK(d != NULL);
        if (!d) continue;
        uint8_t want[16], got[16];
        CHECK(decoder_flac_md5(d, want));
        int32_t *pcm = NULL;
        uint64_t n = decode_rest(d, info.fmt.channels, &pcm, &err);
        md5_t m;
        md5_init(&m);
        unsigned bytes = (v->bits + 7) / 8;
        for (uint64_t k = 0; pcm && k < n * info.fmt.channels; k++) {
            uint32_t s = (uint32_t)(pcm[k] >> (32 - v->bits));
            uint8_t le[4] = {(uint8_t)s, (uint8_t)(s >> 8), (uint8_t)(s >> 16), (uint8_t)(s >> 24)};
            md5_update(&m, le, bytes);
        }
        md5_final(&m, got);
        g_test_checks++;
        if (memcmp(want, got, 16) != 0) TEST_FAIL_("%s: decoded MD5 differs from STREAMINFO", v->name);
        free(pcm);
        decoder_close(d);
    }
    // Not a FLAC decoder: no MD5.
    decoder_info_t info;
    decoder_t *d = open_vector("wav_s16_44k.wav", &info, NULL);
    uint8_t md5[16];
    CHECK(d && !decoder_flac_md5(d, md5));
    decoder_close(d);
}

static uint64_t decode_pair(const char *a, const char *b, int32_t **out) {
    decoder_info_t ia, ib;
    int32_t *pa = NULL, *pb = NULL;
    int ea = 0, eb = 0;
    uint64_t na = decode_file(a, &ia, &pa, &ea), nb = decode_file(b, &ib, &pb, &eb);
    *out = NULL;
    if (ea != CORE_OK || eb != CORE_OK || !pa || !pb) {
        free(pa);
        free(pb);
        return 0;
    }
    int32_t *cat = malloc((size_t)(na + nb) * ia.fmt.channels * sizeof *cat);
    if (cat) {
        memcpy(cat, pa, (size_t)na * ia.fmt.channels * sizeof *cat);
        memcpy(cat + na * ia.fmt.channels, pb, (size_t)nb * ia.fmt.channels * sizeof *cat);
    }
    free(pa);
    free(pb);
    *out = cat;
    return cat ? na + nb : 0;
}

TEST(gapless_pair) {
    decoder_info_t info;
    int32_t *ref = NULL, *cat = NULL;
    int err = 0;
    uint64_t n = decode_file("gap_ref.wav", &info, &ref, &err);
    CHECK_EQ_INT(err, CORE_OK);
    const vector_t *va = find_vector("gap_a.flac");
    CHECK(va != NULL);
    uint64_t split = va ? va->frames : 0;

    uint64_t nf = decode_pair("gap_a.flac", "gap_b.flac", &cat);
    CHECK_EQ_INT(nf, n);
    CHECK(cat && ref && nf == n && memcmp(cat, ref, (size_t)n * sizeof *ref) == 0);
    free(cat);

    uint64_t nm = decode_pair("gap_a.mp3", "gap_b.mp3", &cat);
    CHECK_EQ_INT(nm, n);  // no samples lost or added at the joint
    if (cat && ref && nm == n) {
        double whole = snr_db(ref, cat, 1, 2048, n - 2048, 0);
        double after = snr_db(ref, cat, 1, split + 2048, n - 2048, 0);
        double joint = snr_db(ref, cat, 1, split - 1024, split + 1024, 0);
        printf("  MP3 pair: SNR whole %.1f dB, after joint %.1f dB, around joint %.1f dB\n", whole, after, joint);
        CHECK(whole > 30.0);
        CHECK(after > 30.0);  // a gap or an overlap would shift the phase and ruin this
        CHECK(joint > 15.0);
    }
    free(cat);
    free(ref);
}

TEST(dop_markers_and_payload) {
    const char *files[] = {"dsd64_1k.dsf", "dsd64_1k.dff", "dsd64_mono_msb.dsf"};
    int32_t *first = NULL;
    uint64_t first_n = 0;
    for (size_t f = 0; f < CORE_ARRAY_SIZE(files); f++) {
        decoder_info_t info;
        int32_t *pcm = NULL;
        int err = 0;
        uint64_t n = decode_file(files[f], &info, &pcm, &err);
        CHECK_EQ_INT(err, CORE_OK);
        if (!pcm || !n) {
            free(pcm);
            continue;
        }
        unsigned ch = info.fmt.channels;
        CHECK(info.fmt.dop);
        CHECK_EQ_INT(info.fmt.sample_rate, 176400);
        bool markers = true, low_zero = true;
        for (uint64_t i = 0; i < n; i++) {
            for (unsigned c = 0; c < ch; c++) {
                uint32_t w = (uint32_t)pcm[i * ch + c];
                markers &= (w >> 24) == ((i & 1) ? 0xFAu : 0x05u);
                low_zero &= (w & 0xFF) == 0;
            }
        }
        CHECK(markers);
        CHECK(low_zero);
        // Crude DSD -> PCM: density of ones over 64 DSD bits, correlated with a 1 kHz sine.
        for (unsigned c = 0; c < ch; c++) {
            double sxy = 0, sxx = 0, syy = 0;
            for (uint64_t i = 0; i + 4 <= n; i += 4) {
                int ones = 0;
                for (int k = 0; k < 4; k++) {
                    uint32_t bits = ((uint32_t)pcm[(i + k) * ch + c] >> 8) & 0xFFFF;
                    for (; bits; bits &= bits - 1) ones++;
                }
                double x = (ones - 32) / 32.0;
                double t = (double)(i + 2) / 176400.0;  // centre of the 64-bit window
                double y = sin(2 * 3.14159265358979 * 1000.0 * t);
                sxy += x * y;
                sxx += x * x;
                syy += y * y;
            }
            double corr = sxy / sqrt(sxx * syy);
            printf("  %-20s ch %u: correlation with 1 kHz %+.3f\n", files[f], c, corr);
            CHECK(c == 0 ? corr > 0.9 : corr < -0.9);  // right channel is the inverted tone
        }
        if (f == 0) {
            first = pcm;
            first_n = n;
            pcm = NULL;
        } else if (f == 1) {
            CHECK(n == first_n && memcmp(first, pcm, (size_t)n * ch * sizeof *pcm) == 0);  // DFF == DSF
        }
        free(pcm);
    }
    free(first);
}

TEST(read_after_end_and_close) {
    decoder_info_t info;
    int err = 0;
    decoder_t *d = open_vector("flac_s16_44k.flac", &info, &err);
    CHECK(d != NULL);
    int32_t *pcm = NULL;
    uint64_t n = decode_rest(d, info.fmt.channels, &pcm, &err);
    CHECK_EQ_INT(n, info.total_frames);
    int32_t buf[64];
    CHECK_EQ_INT(decoder_read(d, buf, 16), 0);  // stays at the end
    CHECK_EQ_INT(decoder_read(d, buf, 16), 0);
    CHECK_EQ_INT(decoder_tell(d), n);
    CHECK(decoder_info(d) != NULL && decoder_info(d)->codec == CODEC_FLAC);
    free(pcm);
    decoder_close(d);
}

TEST_MAIN(RUN(manifest) RUN(formats_and_crc) RUN(lossy_snr_and_alignment) RUN(seek_matches_continuous_decode)
              RUN(flac_md5_matches_streaminfo) RUN(gapless_pair) RUN(dop_markers_and_payload)
                  RUN(read_after_end_and_close))
