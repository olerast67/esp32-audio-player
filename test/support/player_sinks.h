// SPDX-License-Identifier: Apache-2.0
// Host audio sinks for the player, header-only so player_cli and the player tests share them:
//
//   mem_sink_t  records every frame written (tests). Configurable: fixed rate/bits (to act
//               like Bluetooth), DoP support, hardware volume, a fake output buffer level,
//               short writes, refused writes, failing open.
//   wav_sink_t  writes a WAV file (player_cli): 24-bit PCM at the source rate, or 16-bit at a
//               fixed rate with TPDF dither like the Bluetooth sink. The first open fixes the
//               file's rate; later tracks are resampled to it by the player.
//
// Plus small helpers used by the tests and the CLI: WAV file generation with RIFF INFO tags,
// decoding a whole file, CRC-32.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_player/decoder.h"
#include "audio_player/dsp.h"
#include "audio_player/sink.h"

// ------------------------------------------------------------------------ memory sink ----
typedef struct {
    audio_sink_t sink;
    // behaviour (set before use)
    uint32_t fixed_rate;      // 0: accept the source rate
    uint8_t fixed_bits;       // 0: follow the source
    bool accept_dop;          // negotiate DoP as DoP (requires SINK_CAP_DOP in caps)
    uint32_t max_write;       // frames accepted per write call, 0 = all
    uint32_t level;           // value reported by buffered_frames()
    int fail_open;            // open() returns this when non-zero
    int32_t write_error;      // write() returns this when negative
    uint32_t refuse_writes;   // this many write() calls accept nothing
    bool discard;             // count frames without recording them
    // recording
    int32_t *pcm;             // interleaved Q31, sink channels
    uint64_t frames, cap;
    uint8_t ch;
    audio_format_t fmt;
    bool is_open, paused;
    unsigned opens, closes, flushes, pause_calls, writes;
    audio_format_t open_fmts[32];
    uint64_t open_at[32];     // recorded frames when each open happened
    float volume_db;
    unsigned volume_calls;
    float volume_log[256];
} mem_sink_t;

static inline int mem_negotiate(audio_sink_t *s, const audio_format_t *src, audio_format_t *out) {
    mem_sink_t *m = (mem_sink_t *)s->ctx;
    if (!src || !out || !src->sample_rate) return CORE_EINVAL;
    *out = *src;
    out->channels = 2;
    if (src->dop) {
        if (!m->accept_dop || !(s->caps & SINK_CAP_DOP)) return CORE_EUNSUPPORTED;
        return CORE_OK;
    }
    if (m->fixed_rate) out->sample_rate = m->fixed_rate;
    if (m->fixed_bits) out->bits = m->fixed_bits;
    out->dop = false;
    return CORE_OK;
}

static inline int mem_open(audio_sink_t *s, const audio_format_t *fmt) {
    mem_sink_t *m = (mem_sink_t *)s->ctx;
    if (m->fail_open) return m->fail_open;
    if (m->opens < 32) {
        m->open_fmts[m->opens] = *fmt;
        m->open_at[m->opens] = m->frames;
    }
    m->opens++;
    m->fmt = *fmt;
    if (m->ch && m->ch != fmt->channels && m->frames) return CORE_EUNSUPPORTED;  // recording keeps one layout
    m->ch = fmt->channels;
    m->is_open = true;
    return CORE_OK;
}

static inline int32_t mem_write(audio_sink_t *s, const int32_t *pcm, uint32_t frames, uint32_t timeout_ms) {
    (void)timeout_ms;
    mem_sink_t *m = (mem_sink_t *)s->ctx;
    m->writes++;
    if (!m->is_open) return CORE_EINVAL;
    if (m->write_error < 0) return m->write_error;
    if (m->refuse_writes) {
        m->refuse_writes--;
        return 0;
    }
    if (m->max_write && frames > m->max_write) frames = m->max_write;
    if (m->discard) {
        m->frames += frames;
        return (int32_t)frames;
    }
    if (m->frames + frames > m->cap) {
        uint64_t cap = m->cap ? m->cap : 1u << 16;
        while (cap < m->frames + frames) cap *= 2;
        int32_t *np = (int32_t *)realloc(m->pcm, (size_t)cap * m->ch * sizeof(int32_t));
        if (!np) return CORE_ENOMEM;
        m->pcm = np;
        m->cap = cap;
    }
    memcpy(m->pcm + (size_t)m->frames * m->ch, pcm, (size_t)frames * m->ch * sizeof(int32_t));
    m->frames += frames;
    return (int32_t)frames;
}

static inline void mem_pause(audio_sink_t *s, bool paused) {
    mem_sink_t *m = (mem_sink_t *)s->ctx;
    m->paused = paused;
    m->pause_calls++;
}

static inline void mem_flush(audio_sink_t *s) { ((mem_sink_t *)s->ctx)->flushes++; }

static inline int mem_set_volume(audio_sink_t *s, float db) {
    mem_sink_t *m = (mem_sink_t *)s->ctx;
    if (!(s->caps & SINK_CAP_HW_VOLUME)) return -1;
    m->volume_db = db;
    if (m->volume_calls < 256) m->volume_log[m->volume_calls] = db;
    m->volume_calls++;
    return CORE_OK;
}

static inline uint32_t mem_buffered(audio_sink_t *s) { return ((mem_sink_t *)s->ctx)->level; }

static inline void mem_describe(audio_sink_t *s, char *out, size_t n) {
    mem_sink_t *m = (mem_sink_t *)s->ctx;
    snprintf(out, n, "mem %u/%u", (unsigned)m->fmt.bits, (unsigned)m->fmt.sample_rate);
}

static inline void mem_close(audio_sink_t *s) {
    mem_sink_t *m = (mem_sink_t *)s->ctx;
    m->closes++;
    m->is_open = false;
}

static inline void mem_sink_init(mem_sink_t *m, const char *name, uint32_t caps) {
    memset(m, 0, sizeof *m);
    audio_sink_t *s = &m->sink;
    s->name = name;
    s->caps = caps;
    s->ctx = m;
    s->negotiate = mem_negotiate;
    s->open = mem_open;
    s->write = mem_write;
    s->pause = mem_pause;
    s->flush = mem_flush;
    s->set_volume_db = mem_set_volume;
    s->buffered_frames = mem_buffered;
    s->describe = mem_describe;
    s->close = mem_close;
}

static inline void mem_sink_free(mem_sink_t *m) {
    free(m->pcm);
    m->pcm = NULL;
    m->frames = m->cap = 0;
}

// Forget the recording but keep the configuration.
static inline void mem_sink_reset_recording(mem_sink_t *m) {
    m->frames = 0;
    m->ch = m->is_open ? m->fmt.channels : 0;
}

// --------------------------------------------------------------------------- WAV sink ----
typedef struct {
    audio_sink_t sink;
    FILE *f;
    uint32_t force_rate;      // 0 = the first track's rate
    uint8_t out_bits;         // 24 or 16
    uint32_t rate;            // fixed by the first open
    bool dop;                 // file carries DoP (24-bit, like a DoP WAV)
    uint64_t frames;
    uint32_t dither;
    uint8_t *buf;
    size_t buf_cap;
    int err;
} wav_sink_t;

static inline void wav_le16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void wav_le32(uint8_t *p, uint32_t v) {
    wav_le16(p, v);
    wav_le16(p + 2, v >> 16);
}

// 44-byte canonical header for PCM (sizes filled in later).
static inline void wav_header(uint8_t h[44], uint32_t rate, unsigned ch, unsigned bits, uint64_t frames) {
    uint32_t block = ch * (bits / 8);
    uint64_t data = frames * block;
    if (data > 0xFFFFFFFFull - 36) data = 0xFFFFFFFFull - 36;
    memcpy(h, "RIFF", 4);
    wav_le32(h + 4, (uint32_t)(36 + data));
    memcpy(h + 8, "WAVEfmt ", 8);
    wav_le32(h + 16, 16);
    wav_le16(h + 20, 1);
    wav_le16(h + 22, ch);
    wav_le32(h + 24, rate);
    wav_le32(h + 28, rate * block);
    wav_le16(h + 32, block);
    wav_le16(h + 34, bits);
    memcpy(h + 36, "data", 4);
    wav_le32(h + 40, (uint32_t)data);
}

static inline int wavs_negotiate(audio_sink_t *s, const audio_format_t *src, audio_format_t *out) {
    wav_sink_t *w = (wav_sink_t *)s->ctx;
    *out = *src;
    out->channels = 2;
    if (src->dop) {
        // A DoP WAV only makes sense at the DoP rate and 24 bits.
        if (w->force_rate || w->out_bits != 24 || (w->rate && w->rate != src->sample_rate)) return CORE_EUNSUPPORTED;
        return CORE_OK;
    }
    out->dop = false;
    out->bits = w->out_bits;
    out->sample_rate = w->force_rate ? w->force_rate : (w->rate ? w->rate : src->sample_rate);
    return CORE_OK;
}

static inline int wavs_open(audio_sink_t *s, const audio_format_t *fmt) {
    wav_sink_t *w = (wav_sink_t *)s->ctx;
    if (!w->f || fmt->channels != 2) return CORE_EUNSUPPORTED;
    if (w->rate && w->rate != fmt->sample_rate) return CORE_EUNSUPPORTED;
    if (!w->rate) w->dop = fmt->dop;
    w->rate = fmt->sample_rate;
    return CORE_OK;
}

static inline int32_t wavs_write(audio_sink_t *s, const int32_t *pcm, uint32_t frames, uint32_t timeout_ms) {
    (void)timeout_ms;
    wav_sink_t *w = (wav_sink_t *)s->ctx;
    size_t bps = w->out_bits / 8, need = (size_t)frames * 2 * bps;
    if (need > w->buf_cap) {
        uint8_t *nb = (uint8_t *)realloc(w->buf, need);
        if (!nb) return CORE_ENOMEM;
        w->buf = nb;
        w->buf_cap = need;
    }
    uint8_t *o = w->buf;
    if (w->out_bits == 16) {
        // Like the Bluetooth sink: Q31 -> s16 with TPDF dither.
        int16_t tmp[512];
        for (uint32_t done = 0; done < frames * 2;) {
            uint32_t n = CORE_MIN(frames * 2 - done, 512u);
            pcm_q31_to_s16_dither(pcm + done, tmp, n, &w->dither);
            for (uint32_t i = 0; i < n; i++, o += 2) wav_le16(o, (uint16_t)tmp[i]);
            done += n;
        }
    } else {
        for (uint32_t i = 0; i < frames * 2; i++, o += 3) {
            uint32_t v = (uint32_t)pcm[i] >> 8;
            o[0] = (uint8_t)v;
            o[1] = (uint8_t)(v >> 8);
            o[2] = (uint8_t)(v >> 16);
        }
    }
    if (fwrite(w->buf, 1, need, w->f) != need) {
        w->err = CORE_EIO;
        return CORE_EIO;
    }
    w->frames += frames;
    return (int32_t)frames;
}

static inline void wavs_patch(wav_sink_t *w) {
    if (!w->f) return;
    uint8_t h[44];
    wav_header(h, w->rate ? w->rate : 44100u, 2, w->out_bits, w->frames);
    long end = ftell(w->f);
    fseek(w->f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, w->f);
    fseek(w->f, end, SEEK_SET);
    fflush(w->f);
}

static inline void wavs_close(audio_sink_t *s) { wavs_patch((wav_sink_t *)s->ctx); }
static inline void wavs_nop_bool(audio_sink_t *s, bool b) {
    (void)s;
    (void)b;
}
static inline void wavs_nop(audio_sink_t *s) { (void)s; }
static inline uint32_t wavs_buffered(audio_sink_t *s) {
    (void)s;
    return 0;
}
static inline void wavs_describe(audio_sink_t *s, char *out, size_t n) {
    wav_sink_t *w = (wav_sink_t *)s->ctx;
    snprintf(out, n, "WAV %u bit", (unsigned)w->out_bits);
}

// out_bits: 24 (default) or 16. force_rate: 0 = the source rate.
static inline int wav_sink_open(wav_sink_t *w, const char *path, uint32_t force_rate, uint8_t out_bits) {
    memset(w, 0, sizeof *w);
    w->f = fopen(path, "wb");
    if (!w->f) return CORE_EIO;
    w->force_rate = force_rate;
    w->out_bits = out_bits == 16 ? 16 : 24;
    w->dither = 0x1234567u;
    uint8_t h[44];
    wav_header(h, force_rate ? force_rate : 44100u, 2, w->out_bits, 0);
    fwrite(h, 1, sizeof h, w->f);
    audio_sink_t *s = &w->sink;
    s->name = "wav";
    // 24 bit: the top 24 bits of each sample are stored unchanged. 16 bit: dithered here like
    // the Bluetooth sink, so the DSP does not dither a second time.
    s->caps = w->out_bits == 16 ? SINK_CAP_DITHERS | SINK_CAP_RATE_SWITCH
                                : SINK_CAP_BITPERFECT | SINK_CAP_RATE_SWITCH | SINK_CAP_DOP;
    s->ctx = w;
    s->negotiate = wavs_negotiate;
    s->open = wavs_open;
    s->write = wavs_write;
    s->pause = wavs_nop_bool;
    s->flush = wavs_nop;
    s->set_volume_db = NULL;
    s->buffered_frames = wavs_buffered;
    s->describe = wavs_describe;
    s->close = wavs_close;
    return CORE_OK;
}

static inline int wav_sink_finish(wav_sink_t *w) {
    int r = w->err;
    if (w->f) {
        wavs_patch(w);
        if (fclose(w->f) != 0 && !r) r = CORE_EIO;
    }
    w->f = NULL;
    free(w->buf);
    w->buf = NULL;
    return r;
}

// ------------------------------------------------------------------------ WAV writer ----
typedef int32_t (*wavgen_fn)(uint64_t frame, unsigned ch, void *user);  // sample in `bits` range

typedef struct {
    const char *title, *artist, *album, *track;
} wavgen_tags_t;

static inline void wavgen_info(FILE *f, const char *id, const char *text) {
    if (!text || !*text) return;
    uint32_t len = (uint32_t)strlen(text) + 1;
    uint8_t h[8];
    memcpy(h, id, 4);
    wav_le32(h + 4, len);
    fwrite(h, 1, 8, f);
    fwrite(text, 1, len, f);
    if (len & 1) fputc(0, f);
}

// Writes a 16- or 24-bit PCM WAV. Returns false on I/O errors.
static inline bool wavgen_write(const char *path, uint32_t rate, unsigned ch, unsigned bits, uint64_t frames,
                                wavgen_fn gen, void *user, const wavgen_tags_t *tags) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    unsigned bps = bits / 8;
    uint64_t data = frames * ch * bps;
    // LIST/INFO size
    uint32_t info = 0;
    const char *ids[4] = {"INAM", "IART", "IPRD", "ITRK"};
    const char *vals[4] = {tags ? tags->title : NULL, tags ? tags->artist : NULL, tags ? tags->album : NULL,
                           tags ? tags->track : NULL};
    for (int i = 0; i < 4; i++) {
        if (vals[i] && *vals[i]) {
            uint32_t len = (uint32_t)strlen(vals[i]) + 1;
            info += 8 + len + (len & 1);
        }
    }
    uint32_t list = info ? 12 + info : 0;
    uint8_t h[44];
    wav_header(h, rate, ch, bits, frames);
    wav_le32(h + 4, (uint32_t)(36 + data + (data & 1) + list));
    fwrite(h, 1, sizeof h, f);
    uint8_t s[4];
    for (uint64_t i = 0; i < frames; i++) {
        for (unsigned c = 0; c < ch; c++) {
            uint32_t v = (uint32_t)gen(i, c, user);
            for (unsigned b = 0; b < bps; b++) s[b] = (uint8_t)(v >> (8 * b));
            fwrite(s, 1, bps, f);
        }
    }
    if (data & 1) fputc(0, f);
    if (list) {
        uint8_t lh[12];
        memcpy(lh, "LIST", 4);
        wav_le32(lh + 4, 4 + info);
        memcpy(lh + 8, "INFO", 4);
        fwrite(lh, 1, 12, f);
        for (int i = 0; i < 4; i++) wavgen_info(f, ids[i], vals[i]);
    }
    return fclose(f) == 0;
}

// ---------------------------------------------------------------------------- helpers ----
// Decodes a whole file to interleaved Q31 (malloc'ed). Returns NULL on failure.
static inline int32_t *decode_all(const char *path, uint64_t *frames, decoder_info_t *info) {
    core_stream_t *s = core_stream_open_file(path, 0);
    if (!s) return NULL;
    decoder_info_t di;
    decoder_t *d = decoder_open(s, path, &di, NULL);
    if (!d) return NULL;
    uint64_t cap = 1u << 15, n = 0;
    int32_t *buf = (int32_t *)malloc((size_t)cap * di.fmt.channels * sizeof(int32_t));
    for (;;) {
        if (!buf) break;
        if (n + 4096 > cap) {
            cap *= 2;
            int32_t *nb = (int32_t *)realloc(buf, (size_t)cap * di.fmt.channels * sizeof(int32_t));
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
        }
        int32_t r = decoder_read(d, buf + (size_t)n * di.fmt.channels, 4096);
        if (r <= 0) break;
        n += (uint64_t)r;
    }
    decoder_close(d);
    if (frames) *frames = n;
    if (info) *info = di;
    return buf;
}

// zlib CRC-32 (the one in vectors.txt).
static inline uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
