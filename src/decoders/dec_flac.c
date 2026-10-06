// SPDX-License-Identifier: Apache-2.0
// FLAC backend on dr_flac: native FLAC (optionally behind ID3v2 tags) and Ogg FLAC, 4..32 bit,
// mono/stereo, sample-exact seeking. dr_flac's s32 output is already left-justified, i.e. Q31.
// The vendored decoder expects two's-complement wrap on corrupt input (-fwrapv). The CMake
// builds pass the flag; the pragma covers builds that cannot set per-file flags (Arduino).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("wrapv")
#endif
#include "decoders/dec_flac_internal.h"
#include "decoders/dec_internal.h"

// dr_flac is compiled here and nowhere else. No stdio: every byte comes through core_stream.
// Allocations go to core_malloc (PSRAM on the device). Assertions are compiled out as in the host
// release build that the fuzz tests exercise: a corrupt file must end in an error, never an abort.
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_WCHAR
#define DRFLAC_ASSERT(expression) ((void)sizeof(expression))
#define DRFLAC_MALLOC(sz) core_malloc(sz)
#define DRFLAC_REALLOC(p, sz) core_realloc((p), (sz))
#define DRFLAC_FREE(p) core_free(p)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "third_party/dr_flac.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define TAG "dec_flac"

typedef struct {
    core_stream_t *s;
    int64_t base;  // offset of "fLaC" / "OggS" (after any ID3v2 tags)
    int64_t size;  // bytes from base to the end of the stream, -1 if unknown
    drflac *fl;
    uint8_t md5[16];
    bool has_md5;
} flac_t;

// Ogg page header (27 bytes) + one-segment lacing, then the Ogg FLAC mapping header:
// 0x7F "FLAC" major minor header-count(2) "fLaC" block-header(4) STREAMINFO(34).
static bool is_ogg_flac(const uint8_t *h, size_t n) {
    if (n < 28 || !dec_tag_eq(h, "OggS")) return false;
    size_t body = 27 + (size_t)h[26];
    return n >= body + 5 && h[body] == 0x7F && memcmp(h + body + 1, "FLAC", 4) == 0;
}

static bool flac_probe(const uint8_t *h, size_t n, const char *ext) {
    (void)ext;
    return (n >= 4 && dec_tag_eq(h, "fLaC")) || is_ogg_flac(h, n);
}

// ------------------------------------------------------------ stream glue ----
static size_t fl_read(void *user, void *buf, size_t len) {
    flac_t *f = user;
    size_t got = 0;
    while (got < len) {  // dr_flac treats a short read as the end of the stream
        int32_t n = core_stream_read(f->s, (uint8_t *)buf + got, len - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    return got;
}

static drflac_bool32 fl_seek(void *user, int offset, drflac_seek_origin origin) {
    flac_t *f = user;
    int64_t target;
    switch (origin) {
    case DRFLAC_SEEK_SET: target = f->base + offset; break;
    case DRFLAC_SEEK_CUR: target = core_stream_tell(f->s) + offset; break;
    case DRFLAC_SEEK_END:
        if (f->size < 0) return DRFLAC_FALSE;
        target = f->base + f->size + offset;
        break;
    default: return DRFLAC_FALSE;
    }
    if (target < f->base || (f->size >= 0 && target > f->base + f->size)) return DRFLAC_FALSE;
    return core_stream_seek(f->s, target, DEC_SEEK_SET) == CORE_OK;
}

static drflac_bool32 fl_tell(void *user, drflac_int64 *cursor) {
    flac_t *f = user;
    int64_t pos = core_stream_tell(f->s);
    if (pos < f->base) return DRFLAC_FALSE;
    *cursor = pos - f->base;
    return DRFLAC_TRUE;
}

static void *fl_malloc(size_t sz, void *user) {
    (void)user;
    return core_malloc(sz);
}

static void *fl_realloc(void *p, size_t sz, void *user) {
    (void)user;
    return core_realloc(p, sz);
}

static void fl_free(void *p, void *user) {
    (void)user;
    core_free(p);
}

// Reads the STREAMINFO MD5 without dr_flac's metadata callback (that path would load every
// metadata block, cover art included, into memory).
static void read_streaminfo_md5(flac_t *f) {
    uint8_t h[27 + 255 + 51];
    size_t n = 0;
    if (core_stream_seek(f->s, f->base, DEC_SEEK_SET) != CORE_OK) return;
    while (n < sizeof h) {
        int32_t k = core_stream_read(f->s, h + n, sizeof h - n);
        if (k <= 0) break;
        n += (size_t)k;
    }
    const uint8_t *si = NULL;  // start of the STREAMINFO metadata block header
    if (n >= 42 && dec_tag_eq(h, "fLaC")) {
        si = h + 4;
    } else if (is_ogg_flac(h, n)) {
        size_t body = 27 + (size_t)h[26];
        if (n >= body + 51 && dec_tag_eq(h + body + 9, "fLaC")) si = h + body + 13;
    }
    if (!si || (si[0] & 0x7F) != 0 || ((uint32_t)si[1] << 16 | (uint32_t)si[2] << 8 | si[3]) != 34) return;
    memcpy(f->md5, si + 4 + 18, 16);
    for (int i = 0; i < 16; i++) f->has_md5 |= f->md5[i] != 0;
}

static void flac_close(void *st) {
    flac_t *f = st;
    if (!f) return;
    if (f->fl) drflac_close(f->fl);
    core_free(f);
}

static void *flac_open(core_stream_t *s, decoder_info_t *info, int *err) {
    flac_t *f = core_calloc(1, sizeof *f);
    if (!f) {
        *err = CORE_ENOMEM;
        return NULL;
    }
    f->s = s;
    f->base = dec_skip_id3v2(s, 0);
    int64_t size = core_stream_size(s);
    f->size = size >= f->base ? size - f->base : -1;
    read_streaminfo_md5(f);

    static const drflac_allocation_callbacks k_alloc = {NULL, fl_malloc, fl_realloc, fl_free};
    if (core_stream_seek(s, f->base, DEC_SEEK_SET) == CORE_OK) {
        f->fl = drflac_open(fl_read, fl_seek, fl_tell, f, &k_alloc);
    }
    if (!f->fl) {
        flac_close(f);
        *err = CORE_ECORRUPT;
        return NULL;
    }
    drflac *fl = f->fl;
    if (fl->channels == 0 || fl->channels > AUDIO_MAX_CHANNELS || fl->sampleRate < 1000 || fl->sampleRate > 768000 ||
        fl->bitsPerSample < 4 || fl->bitsPerSample > 32) {
        CORE_LOGW(TAG, "unsupported stream: %u ch, %u Hz, %u bit", fl->channels, (unsigned)fl->sampleRate,
                  fl->bitsPerSample);
        flac_close(f);
        *err = CORE_EUNSUPPORTED;
        return NULL;
    }

    info->fmt.sample_rate = fl->sampleRate;
    info->fmt.channels = fl->channels;
    info->fmt.bits = dec_bits_class(fl->bitsPerSample);
    info->fmt.dop = false;
    info->total_frames = fl->totalPCMFrameCount;
    uint64_t first_frame = fl->firstFLACFramePosInBytes;
    uint64_t payload = (f->size > 0 && (uint64_t)f->size > first_frame) ? (uint64_t)f->size - first_frame : 0;
    info->bitrate_kbps = dec_bitrate_kbps(payload, fl->totalPCMFrameCount, fl->sampleRate);
    info->seekable = f->size >= 0;
    return f;
}

// On corrupt input some of dr_flac's failed-seek paths leave it "inside" a frame whose samples
// were never decoded (only the header was read), and the next read would dereference a NULL
// sample pointer. Drop such a frame: the next read starts with the next frame that decodes.
static void flac_drop_undecoded_frame(drflac *fl) {
    drflac_frame *fr = &fl->currentFLACFrame;
    if (fr->pcmFramesRemaining == 0) return;
    unsigned ch = drflac__get_channel_count_from_channel_assignment(fr->header.channelAssignment);
    bool valid = drflac__is_current_flac_frame_valid(fl) && ch == fl->channels;
    for (unsigned c = 0; valid && c < ch; c++) valid = fr->subframes[c].pSamplesS32 != NULL;
    if (!valid) fr->pcmFramesRemaining = 0;
}

// drflac_int32 is int, int32_t is long on Xtensa: same size and representation, different type.
_Static_assert(sizeof(drflac_int32) == sizeof(int32_t), "dr_flac sample type must be 32-bit");

static int32_t flac_read(void *st, int32_t *out, uint32_t max_frames) {
    flac_t *f = st;
    flac_drop_undecoded_frame(f->fl);
    return (int32_t)drflac_read_pcm_frames_s32(f->fl, max_frames, (drflac_int32 *)out);
}

static int flac_seek(void *st, uint64_t frame) {
    flac_t *f = st;
    bool ok = drflac_seek_to_pcm_frame(f->fl, frame);
    flac_drop_undecoded_frame(f->fl);
    return ok ? CORE_OK : CORE_ECORRUPT;
}

bool decoder_flac_md5(const decoder_t *d, uint8_t md5[16]) {
    const flac_t *f = decoder_backend_state(d, CODEC_FLAC);
    if (!f || !f->has_md5 || !md5) return false;
    memcpy(md5, f->md5, 16);
    return true;
}

const decoder_backend_t dec_flac_backend = {CODEC_FLAC, flac_probe, flac_open, flac_read, flac_seek, flac_close};
