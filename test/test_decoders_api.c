// SPDX-License-Identifier: Apache-2.0
// Decoder frontend and header edge cases: codec helpers, probing, argument checks, synthetic
// WAV/AIFF/DSF/DFF headers built in memory, allocator routing (every byte through core_malloc,
// nothing allocated while decoding, no leaks) and allocation-failure injection.
#include <stdlib.h>

#include "audio_player/decoder.h"
#include "test.h"

static const char *data_path(const char *name) {
    static char buf[512];
    snprintf(buf, sizeof buf, "%s/%s", TEST_DATA_DIR, name);
    return buf;
}

// ------------------------------------------------------------ byte builder ----
typedef struct {
    uint8_t d[32768];
    size_t n;
} buf_t;

static void put(buf_t *b, const void *p, size_t n) {
    if (b->n + n > sizeof b->d) {  // a test that outgrows the builder must fail loudly
        TEST_FAIL_("byte builder overflow at %zu", b->n);
        return;
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void put_str(buf_t *b, const char *s) { put(b, s, strlen(s)); }
static void put_le16(buf_t *b, uint32_t v) {
    uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    put(b, x, 2);
}
static void put_le32(buf_t *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(b, x, 4);
}
static void put_le64(buf_t *b, uint64_t v) {
    put_le32(b, (uint32_t)v);
    put_le32(b, (uint32_t)(v >> 32));
}
static void put_be16(buf_t *b, uint32_t v) {
    uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    put(b, x, 2);
}
static void put_be32(buf_t *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put(b, x, 4);
}
static void put_be64(buf_t *b, uint64_t v) {
    put_be32(b, (uint32_t)(v >> 32));
    put_be32(b, (uint32_t)v);
}
static void set_le32(buf_t *b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b->d[at + i] = (uint8_t)(v >> (8 * i));
}
static void set_be32(buf_t *b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b->d[at + i] = (uint8_t)(v >> (24 - 8 * i));
}

static core_stream_t *mem(const buf_t *b) { return core_stream_open_memory(b->d, b->n, false); }

static decoder_t *open_buf(const buf_t *b, const char *name, decoder_info_t *info, int *err) {
    return decoder_open(mem(b), name, info, err);
}

// PCM fmt chunk body for WAVE_FORMAT_PCM or IEEE float.
static void wav_fmt(buf_t *b, uint16_t tag, uint16_t ch, uint32_t rate, uint16_t bits) {
    uint16_t align = (uint16_t)(ch * ((bits + 7) / 8));
    put_str(b, "fmt ");
    put_le32(b, 16);
    put_le16(b, tag);
    put_le16(b, ch);
    put_le32(b, rate);
    put_le32(b, rate * align);
    put_le16(b, align);
    put_le16(b, bits);
}

static void riff_begin(buf_t *b) {
    b->n = 0;
    put_str(b, "RIFF");
    put_le32(b, 0);
    put_str(b, "WAVE");
}

static void riff_end(buf_t *b) { set_le32(b, 4, (uint32_t)(b->n - 8)); }

static void put_pcm16(buf_t *b, int frames, int ch) {
    for (int i = 0; i < frames * ch; i++) put_le16(b, (uint16_t)(int16_t)(i * 37 - 1000));
}

// ------------------------------------------------------------------ tests ----
TEST(codec_helpers) {
    CHECK_STR(codec_name(CODEC_FLAC), "FLAC");
    CHECK_STR(codec_name(CODEC_VORBIS), "Vorbis");
    CHECK_STR(codec_name(CODEC_DFF), "DFF");
    CHECK_STR(codec_name(CODEC_UNKNOWN), "Unknown");
    CHECK_STR(codec_name((codec_id_t)99), "Unknown");
    CHECK(codec_is_lossless(CODEC_FLAC) && codec_is_lossless(CODEC_WAV) && codec_is_lossless(CODEC_DSF));
    CHECK(codec_is_lossless(CODEC_ALAC) && codec_is_lossless(CODEC_WAVPACK) && codec_is_lossless(CODEC_APE));
    CHECK(!codec_is_lossless(CODEC_MP3) && !codec_is_lossless(CODEC_VORBIS) && !codec_is_lossless(CODEC_OPUS));
    CHECK(!codec_is_lossless(CODEC_AAC) && !codec_is_lossless(CODEC_UNKNOWN));
    CHECK_EQ_INT(codec_from_extension("flac"), CODEC_FLAC);
    CHECK_EQ_INT(codec_from_extension("FLAC"), CODEC_FLAC);
    CHECK_EQ_INT(codec_from_extension(".Mp3"), CODEC_MP3);
    CHECK_EQ_INT(codec_from_extension("aif"), CODEC_AIFF);
    CHECK_EQ_INT(codec_from_extension("ogg"), CODEC_VORBIS);
    CHECK_EQ_INT(codec_from_extension("opus"), CODEC_OPUS);
    CHECK_EQ_INT(codec_from_extension("m4a"), CODEC_AAC);
    CHECK_EQ_INT(codec_from_extension("aac"), CODEC_AAC);
    CHECK_EQ_INT(codec_from_extension("alac"), CODEC_ALAC);
    CHECK_EQ_INT(codec_from_extension("wv"), CODEC_WAVPACK);
    CHECK_EQ_INT(codec_from_extension("ape"), CODEC_APE);
    CHECK_EQ_INT(codec_from_extension("dsf"), CODEC_DSF);
    CHECK_EQ_INT(codec_from_extension("DFF"), CODEC_DFF);
    CHECK_EQ_INT(codec_from_extension("txt"), CODEC_UNKNOWN);
    CHECK_EQ_INT(codec_from_extension("flac2"), CODEC_UNKNOWN);
    CHECK_EQ_INT(codec_from_extension(""), CODEC_UNKNOWN);
    CHECK_EQ_INT(codec_from_extension(NULL), CODEC_UNKNOWN);
    CHECK(codec_is_audio_extension("wav") && codec_is_audio_extension("OPUS") && codec_is_audio_extension("m4a"));
    CHECK(!codec_is_audio_extension("jpg") && !codec_is_audio_extension("cue") && !codec_is_audio_extension("m3u"));
}

static codec_id_t probe_bytes(const void *p, size_t n, const char *name) {
    core_stream_t *s = core_stream_open_memory(p, n, false);
    codec_id_t id = decoder_probe(s, name);
    core_stream_close(s);
    return id;
}

TEST(probe_by_magic_and_extension) {
    uint8_t junk[64];
    for (int i = 0; i < 64; i++) junk[i] = (uint8_t)(i * 7 + 3);
    CHECK_EQ_INT(probe_bytes("RIFF\0\0\0\0WAVEfmt ", 16, NULL), CODEC_WAV);
    CHECK_EQ_INT(probe_bytes("RF64\xff\xff\xff\xffWAVEds64", 16, NULL), CODEC_WAV);
    CHECK_EQ_INT(probe_bytes("FORM\0\0\0\0AIFCFVER", 16, NULL), CODEC_AIFF);
    CHECK_EQ_INT(probe_bytes("fLaC\0\0\0\x22", 8, "x.mp3"), CODEC_FLAC);  // magic wins over the name
    CHECK_EQ_INT(probe_bytes("DSD \x1c\0\0\0\0\0\0\0", 12, NULL), CODEC_DSF);
    CHECK_EQ_INT(probe_bytes("FRM8\0\0\0\0\0\0\0\0DSD ", 16, NULL), CODEC_DFF);
    CHECK_EQ_INT(probe_bytes("wvpk\0\0\0\0", 8, NULL), CODEC_WAVPACK);
    CHECK_EQ_INT(probe_bytes("MAC \0\0\0\0", 8, NULL), CODEC_APE);
    CHECK_EQ_INT(probe_bytes("\0\0\0\x20" "ftypM4A \0\0\0\0", 16, NULL), CODEC_AAC);
    CHECK_EQ_INT(probe_bytes("\xff\xfb\x90\x64\0\0\0\0", 8, NULL), CODEC_MP3);  // MPEG-1 Layer III sync
    CHECK_EQ_INT(probe_bytes("\xff\xf1\x50\x80\0\0\0\0", 8, NULL), CODEC_AAC);  // ADTS, not MP3
    CHECK_EQ_INT(probe_bytes(junk, sizeof junk, NULL), CODEC_UNKNOWN);
    CHECK_EQ_INT(probe_bytes(junk, sizeof junk, "/music/a.mp3"), CODEC_MP3);
    CHECK_EQ_INT(probe_bytes(junk, sizeof junk, "/music/b.FLAC"), CODEC_FLAC);
    CHECK_EQ_INT(probe_bytes("", 0, "song.ogg"), CODEC_VORBIS);

    // Ogg: first packet decides.
    uint8_t ogg[64] = "OggS";
    ogg[5] = 2;
    ogg[26] = 1;
    ogg[27] = 30;
    memcpy(ogg + 28, "\x01vorbis", 7);
    CHECK_EQ_INT(probe_bytes(ogg, sizeof ogg, NULL), CODEC_VORBIS);
    memcpy(ogg + 28, "\x7f" "FLAC\x01\x00", 7);
    CHECK_EQ_INT(probe_bytes(ogg, sizeof ogg, NULL), CODEC_FLAC);
    memcpy(ogg + 28, "OpusHead", 8);
    CHECK_EQ_INT(probe_bytes(ogg, sizeof ogg, NULL), CODEC_OPUS);

    // ID3v2 in front: FLAC behind it is FLAC, anything else is MP3.
    buf_t b = {0};
    put_str(&b, "ID3\x03");
    put(&b, "\0\0\0\0\0\x14", 6);  // flags, synchsafe size 20
    for (int i = 0; i < 20; i++) put(&b, "\0", 1);
    size_t tag = b.n;
    put_str(&b, "fLaC");
    CHECK_EQ_INT(probe_bytes(b.d, b.n, "a.flac"), CODEC_FLAC);
    b.n = tag;
    put(&b, junk, 16);
    CHECK_EQ_INT(probe_bytes(b.d, b.n, NULL), CODEC_MP3);

    // The stream position is restored.
    core_stream_t *s = core_stream_open_memory("RIFF\0\0\0\0WAVE", 12, false);
    core_stream_seek(s, 5, 0);
    CHECK_EQ_INT(decoder_probe(s, NULL), CODEC_WAV);
    CHECK_EQ_INT(core_stream_tell(s), 5);
    core_stream_close(s);
    CHECK_EQ_INT(decoder_probe(NULL, "a.flac"), CODEC_UNKNOWN);
}

TEST(open_errors) {
    int err = 0;
    decoder_info_t info;
    CHECK(decoder_open(NULL, "a.flac", &info, &err) == NULL);
    CHECK_EQ_INT(err, CORE_EINVAL);

    uint8_t junk[256];
    for (int i = 0; i < 256; i++) junk[i] = (uint8_t)(i * 13 + 5);
    CHECK(decoder_open(core_stream_open_memory(junk, sizeof junk, false), NULL, &info, &err) == NULL);
    CHECK_EQ_INT(err, CORE_EUNSUPPORTED);  // not recognised at all
    CHECK(decoder_open(core_stream_open_memory(junk, sizeof junk, false), "x.flac", &info, &err) == NULL);
    CHECK_EQ_INT(err, CORE_ECORRUPT);  // claims to be FLAC but is not
    CHECK(decoder_open(core_stream_open_memory(junk, sizeof junk, false), "x.mp3", NULL, NULL) == NULL);

    // Recognised but not implemented yet.
    uint8_t ogg[64] = "OggS";
    ogg[26] = 1;
    ogg[27] = 19;
    memcpy(ogg + 28, "OpusHead", 8);
    CHECK(decoder_open(core_stream_open_memory(ogg, sizeof ogg, false), "a.opus", &info, &err) == NULL);
    CHECK_EQ_INT(err, CORE_EUNSUPPORTED);
    CHECK(decoder_open(core_stream_open_memory("wvpk\0\0\0\0", 8, false), "a.wv", &info, &err) == NULL);
    CHECK_EQ_INT(err, CORE_EUNSUPPORTED);
    CHECK(decoder_open(core_stream_open_memory("\0\0\0\x20" "ftypM4A ", 12, false), "a.m4a", &info, &err) == NULL);
    CHECK_EQ_INT(err, CORE_EUNSUPPORTED);
    CHECK(decoder_open(core_stream_open_memory("", 0, false), "a.ape", &info, &err) == NULL);
    CHECK_EQ_INT(err, CORE_EUNSUPPORTED);

    // NULL-safe API.
    int32_t out[8];
    CHECK_EQ_INT(decoder_read(NULL, out, 1), CORE_EINVAL);
    CHECK_EQ_INT(decoder_seek(NULL, 0), CORE_EINVAL);
    CHECK_EQ_INT(decoder_tell(NULL), 0);
    CHECK(decoder_info(NULL) == NULL);
    decoder_close(NULL);

    buf_t b;
    riff_begin(&b);
    wav_fmt(&b, 1, 2, 44100, 16);
    put_str(&b, "data");
    put_le32(&b, 40);
    put_pcm16(&b, 10, 2);
    riff_end(&b);
    decoder_t *d = open_buf(&b, NULL, &info, &err);
    CHECK(d != NULL);
    CHECK_EQ_INT(decoder_read(d, NULL, 4), CORE_EINVAL);
    CHECK_EQ_INT(decoder_read(d, out, 0), CORE_EINVAL);
    CHECK_EQ_INT(decoder_read(d, out, 4), 4);
    CHECK_EQ_INT(decoder_tell(d), 4);
    CHECK_EQ_INT(decoder_seek(d, 11), CORE_EINVAL);
    CHECK_EQ_INT(decoder_seek(d, 10), CORE_OK);
    CHECK_EQ_INT(decoder_read(d, out, 4), 0);
    CHECK_EQ_INT(decoder_seek(d, 9), CORE_OK);
    CHECK_EQ_INT(decoder_read(d, out, 4), 1);
    decoder_close(d);
}

TEST(wav_header_variants) {
    buf_t b;
    decoder_info_t info;
    int err = 0;
    int32_t out[512];

    // Unfinalized header (RIFF and data sizes 0, as left by an interrupted recorder).
    riff_begin(&b);
    set_le32(&b, 4, 0);
    wav_fmt(&b, 1, 2, 48000, 16);
    put_str(&b, "data");
    put_le32(&b, 0);
    put_pcm16(&b, 100, 2);
    decoder_t *d = open_buf(&b, NULL, &info, &err);
    CHECK(d != NULL);
    CHECK_EQ_INT(info.total_frames, 100);
    CHECK_EQ_INT(decoder_read(d, out, 256), 100);
    decoder_close(d);

    // Data size larger than the file (truncated copy): plays what is there.
    riff_begin(&b);
    wav_fmt(&b, 1, 1, 44100, 16);
    put_str(&b, "data");
    put_le32(&b, 1000000);
    put_pcm16(&b, 50, 1);
    put(&b, "\x01", 1);  // half a sample at the end
    d = open_buf(&b, NULL, &info, &err);
    CHECK(d != NULL);
    CHECK_EQ_INT(info.total_frames, 50);
    CHECK_EQ_INT(info.fmt.channels, 1);
    CHECK_EQ_INT(decoder_read(d, out, 256), 50);
    CHECK_EQ_INT(out[1], (int32_t)((uint32_t)(uint16_t)(37 - 1000) << 16));
    decoder_close(d);

    // "fmt " after "data".
    riff_begin(&b);
    put_str(&b, "data");
    put_le32(&b, 8);
    put_pcm16(&b, 2, 2);
    wav_fmt(&b, 1, 2, 44100, 16);
    riff_end(&b);
    d = open_buf(&b, NULL, &info, &err);
    CHECK(d != NULL);
    CHECK_EQ_INT(info.total_frames, 2);
    CHECK_EQ_INT(decoder_read(d, out, 8), 2);
    CHECK_EQ_INT(out[0], (int32_t)((uint32_t)(uint16_t)(-1000) << 16));
    decoder_close(d);

    // Multichannel, compressed and unknown sub-formats are refused cleanly.
    static const struct {
        uint16_t tag, ch, bits;
        int err;
    } k_bad[] = {
        {1, 6, 16, CORE_EUNSUPPORTED}, {7, 1, 8, CORE_EUNSUPPORTED},  {0x55, 2, 0, CORE_ECORRUPT},
        {1, 0, 16, CORE_ECORRUPT},     {3, 2, 24, CORE_EUNSUPPORTED}, {1, 2, 40, CORE_ECORRUPT},
    };
    for (size_t i = 0; i < CORE_ARRAY_SIZE(k_bad); i++) {
        riff_begin(&b);
        wav_fmt(&b, k_bad[i].tag, k_bad[i].ch, 44100, k_bad[i].bits);
        put_str(&b, "data");
        put_le32(&b, 16);
        put_pcm16(&b, 4, 2);
        riff_end(&b);
        CHECK(open_buf(&b, NULL, &info, &err) == NULL);
        CHECK_EQ_INT(err, k_bad[i].err);
    }

    // WAVE_FORMAT_EXTENSIBLE with a sub-format GUID we do not know (e.g. ambisonics).
    riff_begin(&b);
    put_str(&b, "fmt ");
    put_le32(&b, 40);
    put_le16(&b, 0xFFFE);
    put_le16(&b, 2);
    put_le32(&b, 48000);
    put_le32(&b, 48000 * 4);
    put_le16(&b, 4);
    put_le16(&b, 16);
    put_le16(&b, 22);
    put_le16(&b, 16);
    put_le32(&b, 3);
    put(&b, "\x01\x00\x00\x00\x21\x07\xd3\x11\x86\x44\xc8\xc1\xca\x00\x00\x00", 16);
    put_str(&b, "data");
    put_le32(&b, 16);
    put_pcm16(&b, 4, 2);
    riff_end(&b);
    CHECK(open_buf(&b, NULL, &info, &err) == NULL);
    CHECK_EQ_INT(err, CORE_EUNSUPPORTED);

    // No data chunk at all.
    riff_begin(&b);
    wav_fmt(&b, 1, 2, 44100, 16);
    riff_end(&b);
    CHECK(open_buf(&b, NULL, &info, &err) == NULL);
    CHECK_EQ_INT(err, CORE_ECORRUPT);
}

// AIFF with a given 80-bit rate field.
static void aiff_build(buf_t *b, const uint8_t rate80[10], const char *compression, uint16_t bits) {
    b->n = 0;
    put_str(b, "FORM");
    put_be32(b, 0);
    put_str(b, compression ? "AIFC" : "AIFF");
    put_str(b, "COMM");
    put_be32(b, compression ? 24 : 18);
    put_be16(b, 1);
    put_be32(b, 4);
    put_be16(b, bits);
    put(b, rate80, 10);
    if (compression) {
        put_str(b, compression);
        put(b, "\0\0", 2);
    }
    put_str(b, "SSND");
    put_be32(b, 8 + 4 * ((bits + 7) / 8));
    put_be32(b, 0);
    put_be32(b, 0);
    for (int i = 0; i < 4 * ((bits + 7) / 8); i++) put(b, i == 0 ? "\x80" : "\x01", 1);
    set_be32(b, 4, (uint32_t)(b->n - 8));
}

static void ext80(uint32_t hz, uint8_t out[10]) {
    int exp = 31;
    while (!(hz & (1u << exp))) exp--;
    uint64_t mant = (uint64_t)hz << (63 - exp);
    uint16_t e = (uint16_t)(16383 + exp);
    out[0] = (uint8_t)(e >> 8);
    out[1] = (uint8_t)e;
    for (int i = 0; i < 8; i++) out[2 + i] = (uint8_t)(mant >> (56 - 8 * i));
}

TEST(aiff_rates_and_compression) {
    static const uint32_t k_rates[] = {8000, 11025, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
    buf_t b;
    decoder_info_t info;
    int err = 0;
    int32_t out[8];
    for (size_t i = 0; i < CORE_ARRAY_SIZE(k_rates); i++) {
        uint8_t r[10];
        ext80(k_rates[i], r);
        aiff_build(&b, r, NULL, 16);
        decoder_t *d = open_buf(&b, "a.aiff", &info, &err);
        CHECK(d != NULL);
        CHECK_EQ_INT(info.fmt.sample_rate, k_rates[i]);
        CHECK_EQ_INT(decoder_read(d, out, 8), 4);
        CHECK_EQ_INT(out[0], (int32_t)0x80010000u);  // big-endian 0x8001, left-justified
        decoder_close(d);
    }
    // 44100.5 Hz (fractional) rounds; 8-bit AIFF is signed.
    uint8_t r[10];
    ext80(44100, r);
    r[9] |= 1;  // tiny fraction below the rounding threshold
    aiff_build(&b, r, NULL, 8);
    decoder_t *d = open_buf(&b, "a.aiff", &info, &err);
    CHECK(d != NULL);
    CHECK_EQ_INT(info.fmt.sample_rate, 44100);
    CHECK_EQ_INT(info.fmt.bits, 16);
    CHECK_EQ_INT(decoder_read(d, out, 8), 4);
    CHECK_EQ_INT(out[0], INT32_MIN);  // 0x80 signed
    CHECK_EQ_INT(out[1], 0x01000000);
    decoder_close(d);

    // Bad rates and unsupported compression.
    memset(r, 0, sizeof r);
    aiff_build(&b, r, NULL, 16);
    CHECK(open_buf(&b, "a.aiff", &info, &err) == NULL);
    CHECK_EQ_INT(err, CORE_EUNSUPPORTED);
    ext80(44100, r);
    r[0] |= 0x80;  // negative
    aiff_build(&b, r, NULL, 16);
    CHECK(open_buf(&b, "a.aiff", &info, &err) == NULL);
    ext80(44100, r);
    aiff_build(&b, r, "ulaw", 16);
    CHECK(open_buf(&b, "a.aifc", &info, &err) == NULL);
    CHECK_EQ_INT(err, CORE_EUNSUPPORTED);
    aiff_build(&b, r, "sowt", 16);
    d = open_buf(&b, "a.aifc", &info, &err);
    CHECK(d != NULL);
    CHECK_EQ_INT(decoder_read(d, out, 8), 4);
    CHECK_EQ_INT(out[0], 0x01800000);  // little-endian 0x0180
    decoder_close(d);
}

TEST(dsd_header_variants) {
    buf_t b;
    decoder_info_t info;
    int err = 0;
    // DSF DSD256 is refused, DSD128 becomes 352.8 kHz DoP.
    for (int pass = 0; pass < 2; pass++) {
        uint32_t rate = pass ? 5644800 : 11289600;
        b.n = 0;
        put_str(&b, "DSD ");
        put_le64(&b, 28);
        put_le64(&b, 0);
        put_le64(&b, 0);
        put_str(&b, "fmt ");
        put_le64(&b, 52);
        put_le32(&b, 1);
        put_le32(&b, 0);
        put_le32(&b, 2);
        put_le32(&b, 2);
        put_le32(&b, rate);
        put_le32(&b, 1);
        put_le64(&b, 4096 * 8);
        put_le32(&b, 4096);
        put_le32(&b, 0);
        put_str(&b, "data");
        put_le64(&b, 12 + 8192);
        for (int i = 0; i < 8192; i++) put(&b, "\x69", 1);
        decoder_t *d = open_buf(&b, "a.dsf", &info, &err);
        if (pass == 0) {
            CHECK(d == NULL);
            CHECK_EQ_INT(err, CORE_EUNSUPPORTED);
        } else {
            CHECK(d != NULL);
            CHECK_EQ_INT(info.fmt.sample_rate, 352800);
            CHECK_EQ_INT(info.total_frames, 2048);
            int32_t out[2 * 64];
            CHECK_EQ_INT(decoder_read(d, out, 64), 64);
            CHECK_EQ_INT((uint32_t)out[0], 0x05969600u);  // 0x69 bit-reversed is 0x96
            CHECK_EQ_INT((uint32_t)out[2], 0xFA969600u);
            decoder_close(d);
        }
    }
    // DFF with DST compression is refused.
    b.n = 0;
    put_str(&b, "FRM8");
    put_be64(&b, 0);
    put_str(&b, "DSD ");
    put_str(&b, "PROP");
    put_be64(&b, 4 + 12 + 4 + 12 + 10 + 12 + 4);
    put_str(&b, "SND ");
    put_str(&b, "FS  ");
    put_be64(&b, 4);
    put_be32(&b, 2822400);
    put_str(&b, "CHNL");
    put_be64(&b, 10);
    put_be16(&b, 2);
    put_str(&b, "SLFTSRGT");
    put_str(&b, "CMPR");
    put_be64(&b, 4);
    put_str(&b, "DST ");
    put_str(&b, "DST ");
    put_be64(&b, 4);
    put_be32(&b, 0);
    CHECK(open_buf(&b, "a.dff", &info, &err) == NULL);
    CHECK_EQ_INT(err, CORE_EUNSUPPORTED);
}

// --------------------------------------------------------- allocator tests ----
// Counting allocator with a size header, optional failure after N allocations.
static long g_live, g_total_allocs;
static long g_fail_after = -1;

static void *count_alloc(size_t n) {
    if (g_fail_after >= 0 && g_total_allocs >= g_fail_after) return NULL;
    size_t *p = malloc(n + 16);
    if (!p) return NULL;
    p[0] = n;
    g_live++;
    g_total_allocs++;
    return (uint8_t *)p + 16;
}

static void count_free(void *ptr) {
    if (!ptr) return;
    g_live--;
    free((uint8_t *)ptr - 16);
}

static void *count_realloc(void *ptr, size_t n) {
    if (!ptr) return count_alloc(n);
    if (g_fail_after >= 0 && g_total_allocs >= g_fail_after) return NULL;
    size_t *p = realloc((uint8_t *)ptr - 16, n + 16);
    if (!p) return NULL;
    p[0] = n;
    g_total_allocs++;
    return (uint8_t *)p + 16;
}

static const core_allocator_t k_counting = {count_alloc, count_alloc, count_realloc, count_free};

static const char *const k_files[] = {
    "wav_s16_44k.wav",  "aiff_s24_48k_mono.aiff", "flac_s16_44k.flac",     "flac_id3_s16_44k.flac",
    "mp3_vbr_44k.mp3",  "mp3_tagged_44k.mp3",     "ogg_44k.ogg",           "ogg_bigcomment_44k.ogg",
    "dsd64_1k.dsf",     "dsd64_1k.dff",           "wav_s32_48k_rf64.wav",
};

TEST(allocations_routed_no_leaks_none_while_decoding) {
    core_set_allocator(&k_counting);
    static int32_t out[2 * 4096];
    for (size_t f = 0; f < CORE_ARRAY_SIZE(k_files); f++) {
        g_live = g_total_allocs = 0;
        g_fail_after = -1;
        decoder_info_t info;
        int err = 0;
        decoder_t *d = decoder_open(core_stream_open_file(data_path(k_files[f]), 0), k_files[f], &info, &err);
        g_test_checks++;
        if (!d) {
            TEST_FAIL_("%s: open failed: %s", k_files[f], core_err_name(err));
            continue;
        }
        long after_open = g_total_allocs;
        CHECK(after_open > 0);
        uint64_t n = 0;
        int32_t k;
        while ((k = decoder_read(d, out, 4096)) > 0) n += (uint64_t)k;
        CHECK_EQ_INT(n, info.total_frames);
        g_test_checks++;
        if (g_total_allocs != after_open) {
            TEST_FAIL_("%s: %ld allocations while decoding", k_files[f], g_total_allocs - after_open);
        }
        decoder_close(d);
        g_test_checks++;
        if (g_live != 0) TEST_FAIL_("%s: %ld blocks leaked", k_files[f], g_live);
    }
    core_set_allocator(NULL);
}

TEST(allocation_failures_are_clean) {
    core_set_allocator(&k_counting);
    core_set_log_sink(NULL, CORE_LOG_ERROR);  // every injected failure logs a warning otherwise
    static int32_t out[2 * 1024];
    for (size_t f = 0; f < CORE_ARRAY_SIZE(k_files); f++) {
        // Count the allocations a successful open needs, then fail each one in turn.
        g_live = g_total_allocs = 0;
        g_fail_after = -1;
        decoder_t *d = decoder_open(core_stream_open_file(data_path(k_files[f]), 0), k_files[f], NULL, NULL);
        long needed = g_total_allocs;
        decoder_close(d);
        for (long fail = 0; fail < needed; fail++) {
            g_live = g_total_allocs = 0;
            g_fail_after = -1;
            core_stream_t *s = core_stream_open_file(data_path(k_files[f]), 0);
            g_fail_after = g_total_allocs + fail;  // allocation number `fail` after the stream fails
            int err = 0;
            d = decoder_open(s, k_files[f], NULL, &err);
            if (d) {
                while (decoder_read(d, out, 1024) > 0) {
                }
                decoder_close(d);
            } else {
                CHECK(err != CORE_OK);
            }
            g_fail_after = -1;
            g_test_checks++;
            if (g_live != 0) TEST_FAIL_("%s: fail at %ld leaked %ld blocks", k_files[f], fail, g_live);
        }
    }
    g_fail_after = -1;
    core_set_allocator(NULL);
    core_set_log_sink(NULL, CORE_LOG_INFO);
}

TEST_MAIN(RUN(codec_helpers) RUN(probe_by_magic_and_extension) RUN(open_errors) RUN(wav_header_variants)
              RUN(aiff_rates_and_compression) RUN(dsd_header_variants)
                  RUN(allocations_routed_no_leaks_none_while_decoding) RUN(allocation_failures_are_clean))
