// SPDX-License-Identifier: Apache-2.0
// Decoder frontend: codec names, extension mapping, probing by magic bytes (extension second),
// and the public decoder_* API on top of the backends in dec_*.c.
#include "decoders/dec_internal.h"

#define TAG "decoder"

#define PROBE_BYTES 64

struct decoder {
    const decoder_backend_t *be;
    void *state;
    core_stream_t *s;
    decoder_info_t info;
    uint64_t pos;  // current frame
    bool at_end;   // positioned at total_frames by decoder_seek()
};

static const decoder_backend_t *const k_backends[] = {
    &dec_wav_backend, &dec_aiff_backend, &dec_flac_backend, &dec_vorbis_backend,
    &dec_dsf_backend, &dec_dff_backend,  &dec_mp3_backend,
};

// ---------------------------------------------------------------- codecs ----
static const char *const k_codec_names[CODEC_COUNT] = {
    [CODEC_UNKNOWN] = "Unknown", [CODEC_WAV] = "WAV",     [CODEC_AIFF] = "AIFF",       [CODEC_FLAC] = "FLAC",
    [CODEC_MP3] = "MP3",         [CODEC_VORBIS] = "Vorbis", [CODEC_OPUS] = "Opus",     [CODEC_AAC] = "AAC",
    [CODEC_ALAC] = "ALAC",       [CODEC_WAVPACK] = "WavPack", [CODEC_APE] = "APE",     [CODEC_DSF] = "DSF",
    [CODEC_DFF] = "DFF",
};

const char *codec_name(codec_id_t id) {
    if ((unsigned)id >= CODEC_COUNT || !k_codec_names[id]) return k_codec_names[CODEC_UNKNOWN];
    return k_codec_names[id];
}

bool codec_is_lossless(codec_id_t id) {
    switch (id) {
    case CODEC_WAV:
    case CODEC_AIFF:
    case CODEC_FLAC:
    case CODEC_ALAC:
    case CODEC_WAVPACK:
    case CODEC_APE:
    case CODEC_DSF:
    case CODEC_DFF: return true;
    default: return false;
    }
}

static const struct {
    const char *ext;
    codec_id_t codec;
} k_extensions[] = {
    {"wav", CODEC_WAV},   {"wave", CODEC_WAV},    {"rf64", CODEC_WAV},  {"bwf", CODEC_WAV},  {"aif", CODEC_AIFF},
    {"aiff", CODEC_AIFF}, {"aifc", CODEC_AIFF},   {"flac", CODEC_FLAC}, {"fla", CODEC_FLAC}, {"mp3", CODEC_MP3},
    {"mp2", CODEC_MP3},   {"mpga", CODEC_MP3},    {"ogg", CODEC_VORBIS}, {"oga", CODEC_VORBIS},
    {"opus", CODEC_OPUS}, {"m4a", CODEC_AAC},     {"m4b", CODEC_AAC},   {"aac", CODEC_AAC},  {"alac", CODEC_ALAC},
    {"wv", CODEC_WAVPACK}, {"ape", CODEC_APE},    {"dsf", CODEC_DSF},   {"dff", CODEC_DFF},
};

static bool ext_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        if (ca != *b) return false;
    }
    return *a == 0 && *b == 0;
}

codec_id_t codec_from_extension(const char *ext) {
    if (!ext) return CODEC_UNKNOWN;
    if (*ext == '.') ext++;
    for (size_t i = 0; i < CORE_ARRAY_SIZE(k_extensions); i++) {
        if (ext_equal(ext, k_extensions[i].ext)) return k_extensions[i].codec;
    }
    return CODEC_UNKNOWN;
}

bool codec_is_audio_extension(const char *ext) { return codec_from_extension(ext) != CODEC_UNKNOWN; }

static const decoder_backend_t *backend_for(codec_id_t id) {
    for (size_t i = 0; i < CORE_ARRAY_SIZE(k_backends); i++) {
        if (k_backends[i]->id == id) return k_backends[i];
    }
    return NULL;
}

// ----------------------------------------------------------------- probe ----
// Formats we recognise but cannot decode yet, so the error says "unsupported", not "corrupt".
static codec_id_t probe_planned(const uint8_t *h, size_t n) {
    if (n >= 4 && dec_tag_eq(h, "wvpk")) return CODEC_WAVPACK;
    if (n >= 4 && dec_tag_eq(h, "MAC ")) return CODEC_APE;
    if (n >= 12 && dec_tag_eq(h + 4, "ftyp")) return CODEC_AAC;  // MP4 family: AAC or ALAC inside
    if (n >= 28 && dec_tag_eq(h, "OggS")) {
        size_t body = 27 + (size_t)h[26];
        if (n >= body + 8 && memcmp(h + body, "OpusHead", 8) == 0) return CODEC_OPUS;
    }
    if (n >= 4 && h[0] == 0xFF && (h[1] & 0xF6) == 0xF0) return CODEC_AAC;  // ADTS: sync + layer 00
    return CODEC_UNKNOWN;
}

static codec_id_t probe_head(const uint8_t *h, size_t n, const char *ext) {
    for (size_t i = 0; i < CORE_ARRAY_SIZE(k_backends); i++) {
        if (k_backends[i]->probe(h, n, ext)) return k_backends[i]->id;
    }
    return probe_planned(h, n);
}

// Reads up to len bytes at pos; returns the count (0 on error).
static size_t read_head(core_stream_t *s, int64_t pos, uint8_t *buf, size_t len) {
    if (core_stream_seek(s, pos, DEC_SEEK_SET) != CORE_OK) return 0;
    size_t got = 0;
    while (got < len) {
        int32_t k = core_stream_read(s, buf + got, len - got);
        if (k <= 0) break;
        got += (size_t)k;
    }
    return got;
}

codec_id_t decoder_probe(core_stream_t *s, const char *path_hint) {
    if (!s) return CODEC_UNKNOWN;
    const char *ext = path_hint ? core_path_ext(path_hint) : "";
    int64_t saved = core_stream_tell(s);
    uint8_t h[PROBE_BYTES];
    size_t n = read_head(s, 0, h, sizeof h);
    codec_id_t id = CODEC_UNKNOWN;
    if (dec_id3v2_size(h, n)) {
        // ID3v2 in front: FLAC (some taggers do this) or, almost always, MP3.
        int64_t after = dec_skip_id3v2(s, 0);
        n = read_head(s, after, h, sizeof h);
        id = probe_head(h, n, ext);
        if (id == CODEC_UNKNOWN) id = CODEC_MP3;
    } else {
        id = probe_head(h, n, ext);
    }
    if (id == CODEC_UNKNOWN) id = codec_from_extension(ext);
    if (saved >= 0) core_stream_seek(s, saved, DEC_SEEK_SET);
    return id;
}

// ------------------------------------------------------------------ API ----
static bool info_valid(const decoder_info_t *i) {
    return i->fmt.channels >= 1 && i->fmt.channels <= AUDIO_MAX_CHANNELS && i->fmt.sample_rate > 0 &&
           (i->fmt.bits == 16 || i->fmt.bits == 24 || i->fmt.bits == 32);
}

decoder_t *decoder_open(core_stream_t *s, const char *path_hint, decoder_info_t *info, int *err) {
    int e = CORE_OK;
    decoder_t *d = NULL;
    if (!s) {
        if (err) *err = CORE_EINVAL;
        return NULL;
    }
    codec_id_t id = decoder_probe(s, path_hint);
    const decoder_backend_t *be = backend_for(id);
    if (!be) {
        CORE_LOGW(TAG, "%s: %s not supported", path_hint ? path_hint : "stream", codec_name(id));
        e = CORE_EUNSUPPORTED;
        goto fail;
    }
    d = core_calloc(1, sizeof *d);
    if (!d) {
        e = CORE_ENOMEM;
        goto fail;
    }
    if (core_stream_seek(s, 0, DEC_SEEK_SET) != CORE_OK) {
        e = CORE_EIO;
        goto fail;
    }
    d->info.codec = id;
    d->state = be->open(s, &d->info, &e);
    if (!d->state) {
        if (e == CORE_OK) e = CORE_ECORRUPT;
        CORE_LOGW(TAG, "%s: cannot open %s: %s", path_hint ? path_hint : "stream", codec_name(id), core_err_name(e));
        goto fail;
    }
    if (!info_valid(&d->info)) {
        be->close(d->state);
        e = CORE_ECORRUPT;
        goto fail;
    }
    d->be = be;
    d->s = s;
    d->info.codec = id;
    if (info) *info = d->info;
    if (err) *err = CORE_OK;
    return d;

fail:
    core_free(d);
    core_stream_close(s);
    if (err) *err = e;
    return NULL;
}

int32_t decoder_read(decoder_t *d, int32_t *out, uint32_t max_frames) {
    if (!d || !out || !max_frames) return CORE_EINVAL;
    if (d->at_end) return 0;
    // Keep frames * channels and the int32 return value in range on 32-bit targets.
    max_frames = CORE_MIN(max_frames, (uint32_t)(INT32_MAX / AUDIO_MAX_CHANNELS));
    int32_t n = d->be->read(d->state, out, max_frames);
    if (n > 0) d->pos += (uint64_t)n;
    return n;
}

int decoder_seek(decoder_t *d, uint64_t frame) {
    if (!d) return CORE_EINVAL;
    if (!d->info.seekable) return CORE_EUNSUPPORTED;
    uint64_t total = d->info.total_frames;
    if (total && frame > total) return CORE_EINVAL;
    if (total && frame == total) {
        d->at_end = true;  // nothing left to decode; the backend need not position exactly at the end
        d->pos = frame;
        return CORE_OK;
    }
    int r = d->be->seek(d->state, frame);
    if (r == CORE_OK) {
        d->pos = frame;
        d->at_end = false;
    }
    return r;
}

uint64_t decoder_tell(decoder_t *d) { return d ? d->pos : 0; }

const decoder_info_t *decoder_info(const decoder_t *d) { return d ? &d->info : NULL; }

void decoder_close(decoder_t *d) {
    if (!d) return;
    d->be->close(d->state);
    core_stream_close(d->s);
    core_free(d);
}

void *decoder_backend_state(const decoder_t *d, codec_id_t codec) {
    return (d && d->be && d->be->id == codec) ? d->state : NULL;
}
