// SPDX-License-Identifier: Apache-2.0
// MP3 backend on minimp3 (MPEG-1/2/2.5 Layer I/II/III), frame by frame from our own input window.
//
// - ID3v2 at the start and ID3v1 / APEv2 / Lyrics3v2 at the end are excluded from the frame data.
// - A Xing/Info or VBRI header frame gives the frame count and the byte size; it is never decoded.
// - A LAME/Lavc extension gives the encoder delay and padding: we drop delay + 529 samples (the
//   decoder delay) at the start and stop after exactly frames * spf - delay - padding samples.
// - Seeking: frames already played are found through a sparse index of frame offsets (exact);
//   CBR streams are positioned arithmetically (exact); VBR streams use the Xing TOC or the byte
//   ratio (approximate). A few frames before the target are decoded and dropped so the bit
//   reservoir and the overlap state are rebuilt before the first sample we return.
// The vendored decoder expects two's-complement wrap on corrupt input (-fwrapv). The CMake
// builds pass the flag; the pragma covers builds that cannot set per-file flags (Arduino).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("wrapv")
#endif
#include "decoders/dec_internal.h"

// minimp3 is compiled here and nowhere else. Float output keeps more than 16 bits of resolution.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "third_party/minimp3.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define TAG "dec_mp3"

#define MP3_BUF_SIZE (16 * 1024)         // input window
#define MP3_REFILL_AT (MP3_BUF_SIZE / 2)  // top the window up when less than this is left
#define MP3_DECODER_DELAY 529             // synthesis delay of the decoder, part of the LAME contract
#define MP3_MAX_JUNK (1024 * 1024)        // give up if no frame is found in this many leading bytes
#define MP3_INDEX_CAP 4096                // sparse frame index entries (16 KiB)
#define MP3_INDEX_WALK 256                // frames we walk past the last index entry before estimating
#define MP3_CBR_PROBE_FRAMES 24           // frames compared at open to decide whether a stream is CBR

typedef struct {
    core_stream_t *s;

    // Input window: buf[buf_pos, buf_len) are unread bytes, buf[0] is at file offset buf_off.
    // The stream position is always buf_off + buf_len.
    uint8_t buf[MP3_BUF_SIZE];
    size_t buf_len, buf_pos;
    int64_t buf_off;
    bool in_eof;

    int64_t audio_start;  // first audio frame (after tags and the Xing/VBRI frame)
    int64_t audio_end;    // end of frame data (before trailing tags)

    uint32_t rate, spf, kbps;  // from the first frame; kbps 0 = free format
    uint8_t channels;
    bool cbr;                  // every probed frame had the same bitrate (or an "Info" tag says so)
    uint32_t preroll;          // frames decoded and dropped before a seek target

    uint32_t xing_frames, xing_bytes;
    bool has_toc;
    uint8_t toc[100];
    int64_t toc_base;  // offset of the Xing/VBRI header frame, the origin of the TOC

    uint64_t delay;     // decoded samples dropped at the start (gapless), 0 without a LAME tag
    uint64_t total;     // output frames, 0 if unknown
    bool total_exact;   // stop exactly at `total` (LAME gapless info present)

    uint64_t frame_no;     // index of the next MPEG frame, counted from audio_start
    bool frame_no_exact;   // frame_no is known exactly (false after an estimated seek)
    int64_t last_frame_off;

    uint64_t dec_pos;      // decoded-sample index of the next frame's first sample
    uint64_t drop_until;   // decoded samples with a lower index are dropped
    uint64_t out_pos;      // frames returned so far on the trimmed timeline
    uint32_t pcm_frames, pcm_pos;
    uint8_t pcm_channels;

    uint32_t idx[MP3_INDEX_CAP];  // idx[i] = offset of frame i * idx_stride from audio_start
    uint32_t idx_count, idx_stride;

    mp3dec_t dec;
    float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
} mp3_t;

// --------------------------------------------------------------- headers ----
typedef struct {
    uint32_t rate, kbps, spf;
    uint8_t channels, layer;
    bool mpeg1, crc;
} mp3_hdr_t;

static bool parse_header(const uint8_t *h, mp3_hdr_t *o) {
    static const uint16_t k_rates[3] = {44100, 48000, 32000};
    static const uint16_t k_kbps[2][3][15] = {
        {// MPEG-1: layer I, II, III
         {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
        {// MPEG-2 and 2.5: layer I, II, III
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}},
    };
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
    unsigned ver = (h[1] >> 3) & 3, lay = (h[1] >> 1) & 3, br = h[2] >> 4, sr = (h[2] >> 2) & 3;
    if (ver == 1 || lay == 0 || br == 15 || sr == 3) return false;
    o->mpeg1 = ver == 3;
    o->layer = (uint8_t)(4 - lay);
    o->rate = (uint32_t)k_rates[sr] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    o->kbps = k_kbps[o->mpeg1 ? 0 : 1][o->layer - 1][br];
    o->spf = o->layer == 1 ? 384 : (o->layer == 3 && !o->mpeg1) ? 576 : 1152;
    o->channels = (h[3] >> 6) == 3 ? 1 : 2;
    o->crc = !(h[1] & 1);
    return true;
}

static uint32_t samples_per_frame(int layer, int hz) {
    return layer == 1 ? 384 : (layer == 3 && hz < 32000) ? 576 : 1152;  // LSF Layer III has half frames
}

// ------------------------------------------------------------ input window ----
static int fill(mp3_t *m) {
    if (m->buf_pos) {
        m->buf_len -= m->buf_pos;
        memmove(m->buf, m->buf + m->buf_pos, m->buf_len);
        m->buf_off += (int64_t)m->buf_pos;
        m->buf_pos = 0;
    }
    while (!m->in_eof && m->buf_len < MP3_BUF_SIZE) {
        int64_t at = m->buf_off + (int64_t)m->buf_len;
        if (at >= m->audio_end) {
            m->in_eof = true;
            break;
        }
        size_t want = MP3_BUF_SIZE - m->buf_len;
        if ((int64_t)want > m->audio_end - at) want = (size_t)(m->audio_end - at);
        int32_t n = core_stream_read(m->s, m->buf + m->buf_len, want);
        if (n < 0) return n;
        if (n == 0) {
            m->in_eof = true;
            break;
        }
        m->buf_len += (size_t)n;
    }
    return CORE_OK;
}

// Moves the input window to byte offset pos (clamped to the frame data) and resets the decoder.
static int reposition(mp3_t *m, int64_t pos) {
    pos = CORE_CLAMP(pos, m->audio_start, m->audio_end);
    int r = core_stream_seek(m->s, pos, DEC_SEEK_SET);
    if (r != CORE_OK) return r;
    m->buf_off = pos;
    m->buf_len = m->buf_pos = 0;
    m->in_eof = false;
    mp3dec_init(&m->dec);
    return CORE_OK;
}

// ------------------------------------------------------------ frame index ----
static void index_add(mp3_t *m, uint64_t frame_no, int64_t off) {
    if (frame_no != (uint64_t)m->idx_count * m->idx_stride) return;
    int64_t rel = off - m->audio_start;
    if (rel < 0 || rel > (int64_t)UINT32_MAX) return;
    if (m->idx_count == MP3_INDEX_CAP) {
        // Full: keep every other entry and double the stride. frame_no is then the next slot again.
        for (uint32_t i = 0; i < MP3_INDEX_CAP / 2; i++) m->idx[i] = m->idx[2 * i];
        m->idx_count = MP3_INDEX_CAP / 2;
        m->idx_stride *= 2;
    }
    m->idx[m->idx_count++] = (uint32_t)rel;
}

// Parses (decode == false) or decodes (decode == true, into m->pcm) the next MPEG frame.
// Returns samples per channel (> 0), 0 at the end of the frame data, or a negative error.
// A frame whose header is valid but whose data cannot be decoded (bit reservoir not available
// after a seek, corrupt payload) yields silence so the timeline stays intact.
static int next_frame(mp3_t *m, bool decode) {
    for (;;) {
        if (!m->in_eof && m->buf_len - m->buf_pos < MP3_REFILL_AT) {
            int r = fill(m);
            if (r != CORE_OK) return r;
        }
        size_t avail = m->buf_len - m->buf_pos;
        if (avail == 0) return 0;
        mp3dec_frame_info_t fi;
        memset(&fi, 0, sizeof fi);
        int n = mp3dec_decode_frame(&m->dec, m->buf + m->buf_pos, (int)avail, decode ? m->pcm : NULL, &fi);
        if (fi.frame_bytes <= 0) {
            // No complete frame at the window start. At the end of the data this is a truncated
            // last frame; otherwise step over one byte so we always make progress.
            if (m->in_eof) {
                m->buf_pos = m->buf_len;
                return 0;
            }
            m->buf_pos++;
            continue;
        }
        int64_t frame_off = m->buf_off + (int64_t)m->buf_pos + fi.frame_offset;
        m->buf_pos += (size_t)fi.frame_bytes;
        if (fi.hz == 0) continue;  // skipped bytes that do not start a frame
        if (n <= 0) {
            n = (int)samples_per_frame(fi.layer, fi.hz);
            if (decode) memset(m->pcm, 0, (size_t)n * (size_t)fi.channels * sizeof m->pcm[0]);
        }
        if (m->frame_no_exact) index_add(m, m->frame_no, frame_off);
        m->last_frame_off = frame_off;
        m->frame_no++;
        m->pcm_channels = (uint8_t)fi.channels;
        return n;
    }
}

// --------------------------------------------------------------- tags ----
// Returns the end of the frame data: file size minus ID3v1, APEv2 and Lyrics3v2 tags.
static int64_t trailing_tags_start(core_stream_t *s, int64_t start, int64_t end) {
    for (int pass = 0; pass < 3; pass++) {
        uint8_t b[32];
        bool found = false;
        if (end - start >= 128 && dec_read_at(s, end - 128, b, 3) == CORE_OK && memcmp(b, "TAG", 3) == 0) {
            end -= 128;
            found = true;
        }
        if (end - start >= 32 && dec_read_at(s, end - 32, b, 32) == CORE_OK && memcmp(b, "APETAGEX", 8) == 0) {
            uint64_t size = dec_le32(b + 12) + ((dec_le32(b + 20) & 0x80000000u) ? 32u : 0u);
            if (size >= 32 && (int64_t)size <= end - start) {
                end -= (int64_t)size;
                found = true;
            }
        }
        if (end - start >= 15 && dec_read_at(s, end - 15, b, 15) == CORE_OK &&
            memcmp(b + 6, "LYRICS200", 9) == 0) {
            uint32_t size = 0;
            bool digits = true;
            for (int i = 0; i < 6; i++) {
                digits &= b[i] >= '0' && b[i] <= '9';
                size = size * 10 + (uint32_t)(b[i] - '0');
            }
            if (digits && (int64_t)size + 15 <= end - start) {
                end -= (int64_t)size + 15;
                found = true;
            }
        }
        if (!found) break;
    }
    return end;
}

// Parses a Xing/Info (+ LAME) or VBRI header in the first frame. Returns true if the frame is a
// header frame (it carries no audio and must be skipped).
static bool parse_vbr_header(mp3_t *m, const uint8_t *f, size_t len, const mp3_hdr_t *h) {
    size_t side = h->mpeg1 ? (h->channels == 1 ? 17 : 32) : (h->channels == 1 ? 9 : 17);
    size_t o = 4 + (h->crc ? 2 : 0) + side;
    if (h->layer == 3 && len >= o + 8 && (dec_tag_eq(f + o, "Xing") || dec_tag_eq(f + o, "Info"))) {
        bool info_tag = dec_tag_eq(f + o, "Info");
        uint32_t flags = dec_be32(f + o + 4);
        size_t p = o + 8;
        if ((flags & 1) && len >= p + 4) {
            m->xing_frames = dec_be32(f + p);
            p += 4;
        }
        if ((flags & 2) && len >= p + 4) {
            m->xing_bytes = dec_be32(f + p);
            p += 4;
        }
        if ((flags & 4) && len >= p + 100) {
            memcpy(m->toc, f + p, 100);
            m->has_toc = true;
            p += 100;
        }
        if (flags & 8) p += 4;
        // LAME extension: 9-byte encoder string, ..., 12-bit delay and 12-bit padding at +21.
        if (m->xing_frames && len >= p + 24 &&
            (dec_tag_eq(f + p, "LAME") || dec_tag_eq(f + p, "Lavf") || dec_tag_eq(f + p, "Lavc"))) {
            uint32_t delay = (uint32_t)f[p + 21] << 4 | f[p + 22] >> 4;
            uint32_t padding = (uint32_t)(f[p + 22] & 0x0F) << 8 | f[p + 23];
            uint64_t decoded = (uint64_t)m->xing_frames * m->spf;
            uint64_t skip = (uint64_t)delay + MP3_DECODER_DELAY;
            if (decoded > skip) {
                m->delay = skip;
                m->total = CORE_MIN(decoded - CORE_MIN(decoded, (uint64_t)delay + padding), decoded - skip);
                m->total_exact = true;
            }
        }
        if (info_tag) m->cbr = true;
        return true;
    }
    if (len >= 36 + 26 && dec_tag_eq(f + 36, "VBRI")) {
        m->xing_bytes = dec_be32(f + 36 + 10);
        m->xing_frames = dec_be32(f + 36 + 14);
        return true;
    }
    return false;
}

// True if the first frames in the window all share one bitrate (then seeking can be arithmetic).
static bool probe_cbr(const uint8_t *p, size_t len, const mp3_hdr_t *first) {
    size_t pos = 0;
    for (int i = 0; i < MP3_CBR_PROBE_FRAMES; i++) {
        mp3_hdr_t h;
        if (pos + 4 > len) return i > 1;
        if (!parse_header(p + pos, &h) || h.kbps != first->kbps || h.rate != first->rate || !h.kbps) return false;
        size_t size = (size_t)h.spf / 8 * h.kbps * 1000 / h.rate;
        if (h.layer == 1) size = size / 4 * 4;
        size += (p[pos + 2] & 2) ? (h.layer == 1 ? 4u : 1u) : 0u;
        pos += size;
    }
    return true;
}

// ------------------------------------------------------------------ open ----
static bool mp3_probe(const uint8_t *h, size_t n, const char *ext) {
    mp3_hdr_t hdr;
    if (n < 4 || !parse_header(h, &hdr)) return false;
    // A sync word alone is weak evidence; accept it at offset 0 only for Layer III or an MP3 name.
    return hdr.layer == 3 || (ext && (core_str_ends_with_ci(ext, "mp3") || core_str_ends_with_ci(ext, "mp2")));
}

static void mp3_close(void *st) { core_free(st); }

static void *mp3_open(core_stream_t *s, decoder_info_t *info, int *err) {
    mp3_t *m = core_calloc(1, sizeof *m);
    if (!m) {
        *err = CORE_ENOMEM;
        return NULL;
    }
    m->s = s;
    m->idx_stride = 1;
    int64_t size = core_stream_size(s);
    m->audio_start = dec_skip_id3v2(s, 0);
    m->audio_end = size >= 0 ? trailing_tags_start(s, m->audio_start, size) : INT64_MAX;
    int r = reposition(m, m->audio_start);

    // Find the first frame with minimp3's own sync logic (several consecutive headers must match).
    mp3dec_frame_info_t fi;
    memset(&fi, 0, sizeof fi);
    int64_t first = -1;
    while (r == CORE_OK && first < 0) {
        r = fill(m);
        if (r != CORE_OK) break;
        size_t avail = m->buf_len - m->buf_pos;
        if (avail < 4 || m->buf_off + (int64_t)m->buf_pos - m->audio_start > MP3_MAX_JUNK) break;
        memset(&fi, 0, sizeof fi);
        int n = mp3dec_decode_frame(&m->dec, m->buf + m->buf_pos, (int)avail, NULL, &fi);
        if (n > 0) {
            first = m->buf_off + (int64_t)m->buf_pos + fi.frame_offset;
            m->buf_pos += (size_t)fi.frame_offset;
            r = fill(m);  // move the frame to the window start with as much data after it as possible
        } else if (fi.frame_bytes > 0) {
            m->buf_pos += (size_t)fi.frame_bytes;
        } else if (m->in_eof) {
            break;
        } else {
            m->buf_pos++;
        }
    }
    mp3_hdr_t h;
    if (r != CORE_OK || first < 0 || !parse_header(m->buf + m->buf_pos, &h)) {
        mp3_close(m);
        *err = r != CORE_OK ? r : CORE_ECORRUPT;
        return NULL;
    }
    m->rate = h.rate;
    m->channels = h.channels;
    m->spf = h.spf;
    m->kbps = h.kbps;
    size_t frame_len = (size_t)(fi.frame_bytes - fi.frame_offset);
    const uint8_t *fp = m->buf + m->buf_pos;
    size_t in_window = m->buf_len - m->buf_pos;
    bool header_frame = parse_vbr_header(m, fp, CORE_MIN(frame_len, in_window), &h);
    m->toc_base = first;
    m->audio_start = header_frame ? first + (int64_t)frame_len : first;
    if (!m->cbr && !m->xing_frames) {
        size_t skip = header_frame ? frame_len : 0;
        m->cbr = in_window > skip && probe_cbr(fp + skip, in_window - skip, &h);
    }

    uint64_t data_bytes = m->audio_end > m->audio_start ? (uint64_t)(m->audio_end - m->audio_start) : 0;
    if (m->xing_frames && !m->total_exact) m->total = (uint64_t)m->xing_frames * m->spf;
    if (!m->xing_frames && m->kbps) {
        uint64_t frame_bytes_x = (uint64_t)m->spf * m->kbps * 125;  // bytes per frame * rate
        m->total = data_bytes * m->rate / frame_bytes_x * m->spf;
    }
    uint64_t avg_bytes = m->kbps ? (uint64_t)m->spf * m->kbps * 125 / m->rate : 0;
    if (m->xing_frames && m->xing_bytes) avg_bytes = m->xing_bytes / m->xing_frames;
    m->preroll = h.layer == 3 ? 2 + (uint32_t)(avg_bytes ? CORE_MIN((511 + avg_bytes - 1) / avg_bytes, 8u) : 8u) : 2;

    r = reposition(m, m->audio_start);
    if (r != CORE_OK) {
        mp3_close(m);
        *err = r;
        return NULL;
    }
    m->frame_no_exact = true;
    m->drop_until = m->delay;

    info->fmt.sample_rate = m->rate;
    info->fmt.channels = m->channels;
    info->fmt.bits = 24;  // float synthesis: more resolution than 16 bits survives into Q31
    info->fmt.dop = false;
    info->total_frames = m->total;
    if (m->xing_frames && m->xing_bytes) {
        info->bitrate_kbps = dec_bitrate_kbps(m->xing_bytes, (uint64_t)m->xing_frames * m->spf, m->rate);
    } else if (m->total && !m->cbr) {
        info->bitrate_kbps = dec_bitrate_kbps(data_bytes, m->total, m->rate);
    } else {
        info->bitrate_kbps = m->kbps;
    }
    info->seekable = size >= 0;
    return m;
}

// ------------------------------------------------------------------ read ----
static void emit(const float *src, uint8_t src_ch, int32_t *dst, uint8_t dst_ch, uint32_t frames) {
    if (src_ch == dst_ch) {
        uint32_t n = frames * dst_ch;
        for (uint32_t i = 0; i < n; i++) dst[i] = dec_f32_to_q31(src[i]);
    } else if (src_ch == 1) {  // mono frame in a stereo stream
        for (uint32_t i = 0; i < frames; i++) dst[2 * i] = dst[2 * i + 1] = dec_f32_to_q31(src[i]);
    } else {  // stereo frame in a mono stream
        for (uint32_t i = 0; i < frames; i++) dst[i] = dec_f32_to_q31((src[2 * i] + src[2 * i + 1]) * 0.5f);
    }
}

static int32_t mp3_read(void *st, int32_t *out, uint32_t max_frames) {
    mp3_t *m = st;
    uint32_t done = 0;
    while (done < max_frames) {
        if (m->total_exact && m->out_pos >= m->total) break;
        if (m->pcm_pos >= m->pcm_frames) {
            m->dec_pos += m->pcm_frames;
            m->pcm_frames = m->pcm_pos = 0;
            int n = next_frame(m, true);
            if (n < 0) return done ? (int32_t)done : n;
            if (n == 0) break;
            m->pcm_frames = (uint32_t)n;
            if (m->drop_until > m->dec_pos) m->pcm_pos = (uint32_t)CORE_MIN(m->drop_until - m->dec_pos, (uint64_t)n);
            continue;
        }
        uint32_t take = CORE_MIN(m->pcm_frames - m->pcm_pos, max_frames - done);
        if (m->total_exact && take > m->total - m->out_pos) take = (uint32_t)(m->total - m->out_pos);
        emit(m->pcm + (size_t)m->pcm_pos * m->pcm_channels, m->pcm_channels, out + (size_t)done * m->channels,
             m->channels, take);
        m->pcm_pos += take;
        m->out_pos += take;
        done += take;
    }
    return (int32_t)done;
}

// ------------------------------------------------------------------ seek ----
// The Xing TOC maps percent of the duration to 1/256 of `bytes`, both counted from the header
// frame itself. toc_offset() goes from an MPEG frame to a byte offset, toc_frame() back.
static uint64_t toc_bytes(const mp3_t *m) {
    return m->xing_bytes ? m->xing_bytes : (uint64_t)(m->audio_end - m->toc_base);
}

static int64_t toc_offset(const mp3_t *m, uint64_t kp) {
    uint64_t pct = kp * 100 * 65536 / m->xing_frames;  // 16.16 fixed point
    if (pct >= 100 * 65536) pct = 100 * 65536 - 1;
    uint32_t a = (uint32_t)(pct >> 16), frac = (uint32_t)(pct & 0xFFFF);
    uint32_t fa = m->toc[a], fb = a < 99 ? m->toc[a + 1] : 256;
    if (fb < fa) fb = fa;
    uint64_t pos = (uint64_t)fa * 65536 + (uint64_t)(fb - fa) * frac;  // in 1/(256 * 65536) of the bytes
    return m->toc_base + (int64_t)(pos * toc_bytes(m) / (256u * 65536u));
}

static uint64_t toc_frame(const mp3_t *m, int64_t off) {
    uint64_t bytes = toc_bytes(m);
    if (off <= m->toc_base || !bytes) return 0;
    uint64_t pos = (uint64_t)(off - m->toc_base) * 256 * 65536 / bytes;
    uint32_t a = 0;
    while (a < 99 && (uint64_t)m->toc[a + 1] * 65536 <= pos) a++;
    uint64_t lo = (uint64_t)m->toc[a] * 65536, hi = (uint64_t)(a < 99 ? m->toc[a + 1] : 256) * 65536;
    uint64_t frac = hi > lo ? CORE_MIN((pos - CORE_MIN(pos, lo)) * 65536 / (hi - lo), (uint64_t)65535) : 0;
    uint64_t pct = (uint64_t)a * 65536 + frac;
    return (pct * m->xing_frames + 50 * 65536) / (100 * 65536);
}

static int mp3_seek(void *st, uint64_t frame) {
    mp3_t *m = st;
    uint64_t target = frame + m->delay;  // decoded-sample index of the first sample to return
    uint64_t k = target / m->spf;
    uint64_t kp = k > m->preroll ? k - m->preroll : 0;
    int r;

    uint32_t e = m->idx_count ? (uint32_t)CORE_MIN(kp / m->idx_stride, (uint64_t)m->idx_count - 1) : 0;
    uint64_t e_frame = (uint64_t)e * m->idx_stride;
    if (kp == 0 || (m->idx_count && kp >= e_frame && kp - e_frame <= CORE_MAX(m->idx_stride, MP3_INDEX_WALK))) {
        // Exact: start of the data or a known frame offset, then walk the headers forward.
        int64_t off = kp == 0 ? m->audio_start : m->audio_start + m->idx[e];
        r = reposition(m, off);
        if (r != CORE_OK) return r;
        m->frame_no = kp == 0 ? 0 : e_frame;
        m->frame_no_exact = true;
    } else {
        // Estimate a byte offset a little before frame kp, resync on the next header, work out
        // which frame that is, then walk forward. CBR arithmetic is exact; TOC and ratio are not.
        uint64_t span = (uint64_t)(m->audio_end - m->audio_start);
        uint64_t mpeg_frames = m->xing_frames ? m->xing_frames : (m->total + m->delay) / m->spf;
        uint64_t num = (uint64_t)m->spf * m->kbps * 125;  // CBR: bytes per frame * rate
        uint64_t aim = kp > 2 ? kp - 2 : 0;
        bool cbr = m->cbr && m->kbps, toc = m->has_toc && m->xing_frames;
        int64_t off;
        if (cbr) {
            off = m->audio_start + (int64_t)(aim * num / m->rate);
        } else if (toc) {
            off = toc_offset(m, aim);
        } else if (mpeg_frames) {
            off = m->audio_start + (int64_t)(span * aim / mpeg_frames);
        } else {
            return CORE_EUNSUPPORTED;
        }
        r = reposition(m, off);
        if (r != CORE_OK) return r;
        m->frame_no_exact = false;
        int n = next_frame(m, false);
        if (n < 0) return n;
        m->frame_no = kp;
        if (n > 0) {
            int64_t q = m->last_frame_off;
            m->buf_pos = (size_t)(q - m->buf_off);  // un-read the frame we synced on
            uint64_t rel = (uint64_t)(q - m->audio_start);
            if (cbr) {
                m->frame_no = (rel * m->rate * 2 + num) / (2 * num);  // sizes differ by the padding byte only
                m->frame_no_exact = true;
            } else if (toc) {
                m->frame_no = toc_frame(m, q);
            } else if (span) {
                m->frame_no = rel * mpeg_frames / span;
            }
        }
    }
    // Walk forward to frame kp without decoding (cheap), then decode from there.
    while (m->frame_no < kp) {
        int n = next_frame(m, false);
        if (n < 0) return n;
        if (n == 0) break;
    }
    mp3dec_init(&m->dec);
    m->dec_pos = m->frame_no * m->spf;
    m->pcm_frames = m->pcm_pos = 0;
    m->drop_until = CORE_MAX(target, m->dec_pos);
    m->out_pos = frame;
    return CORE_OK;
}

const decoder_backend_t dec_mp3_backend = {CODEC_MP3, mp3_probe, mp3_open, mp3_read, mp3_seek, mp3_close};
