// SPDX-License-Identifier: Apache-2.0
// Ogg Vorbis backend on stb_vorbis (pull API, sample-accurate seeking).
//
// stb_vorbis pulls bytes through stdio. This file points its FILE calls at a small virtual stream
// (vio) over core_stream, so nothing touches a real FILE and seeking/length work unchanged.
// The virtual stream starts with header pages we rebuild ourselves:
//   - the identification packet as found in the file,
//   - an empty comment packet (the real one can carry megabytes of cover art that stb_vorbis would
//     copy into memory; tags are read by the library module, not here),
//   - the setup packet, checked first so codebook sizes cannot overflow stb_vorbis' int sizes,
// followed by the file's audio pages as they are. All stb_vorbis memory, including the per-frame
// scratch it would otherwise take with alloca(), comes from one arena from core_malloc.
#ifndef NDEBUG
#define NDEBUG  // stb_vorbis uses assert(); a corrupt file must end in an error, never an abort
#endif
// The vendored decoder expects two's-complement wrap on corrupt input (-fwrapv). The CMake
// builds pass the flag; the pragma covers builds that cannot set per-file flags (Arduino).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("wrapv")
#endif
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER) || defined(__MINGW32__)
#include <malloc.h>
#endif
#if defined(__linux__) || defined(__linux) || defined(__sun__) || defined(__EMSCRIPTEN__) || defined(__NEWLIB__)
#include <alloca.h>
#endif

#include "decoders/dec_internal.h"

#define TAG "dec_vorbis"

#define VIO_BUF 4096                         // read cache of the virtual stream
#define VORBIS_MAX_SETUP (512 * 1024)        // largest setup packet we buffer (real ones: 3..60 KB)
#define VORBIS_MAX_HEADER_PAGES 4096         // pages scanned for the three header packets
#define VORBIS_MAX_BOOK_VALUES (1u << 22)    // entries * dimensions of one codebook
#define VORBIS_ARENA_MIN (128 * 1024)
#define VORBIS_ARENA_MAX (2 * 1024 * 1024)
#define VORBIS_ARENA_SLACK (16 * 1024)

// ------------------------------------------------------------ virtual stream ----
typedef struct vorbis_io {
    core_stream_t *s;
    const uint8_t *hdr;  // rebuilt header pages, virtual offsets [0, hdr_len)
    uint32_t hdr_len;
    int64_t audio_off;   // file offset that maps to virtual offset hdr_len
    uint32_t vlen, vpos;
    uint32_t buf_vstart, buf_len;  // cache holds virtual [buf_vstart, buf_vstart + buf_len)
    uint8_t buf[VIO_BUF];
} vorbis_io_t;

static bool vio_fill(vorbis_io_t *v) {
    uint32_t want = CORE_MIN((uint32_t)VIO_BUF, v->vlen - v->vpos);
    v->buf_vstart = v->vpos;
    v->buf_len = 0;
    if (core_stream_seek(v->s, v->audio_off + (v->vpos - v->hdr_len), DEC_SEEK_SET) != CORE_OK) return false;
    while (v->buf_len < want) {
        int32_t n = core_stream_read(v->s, v->buf + v->buf_len, want - v->buf_len);
        if (n <= 0) break;
        v->buf_len += (uint32_t)n;
    }
    return v->buf_len > 0;
}

static int vio_getc(vorbis_io_t *v) {
    if (v->vpos < v->hdr_len) return v->hdr[v->vpos++];
    if (v->vpos >= v->vlen) return EOF;
    if (v->vpos < v->buf_vstart || v->vpos - v->buf_vstart >= v->buf_len) {
        if (!vio_fill(v)) return EOF;
    }
    return v->buf[v->vpos++ - v->buf_vstart];
}

static size_t vio_read(void *dst, size_t size, size_t count, vorbis_io_t *v) {
    if (!size || !count) return 0;
    size_t total = size * count, done = 0;
    uint8_t *out = dst;
    while (done < total) {
        if (v->vpos < v->hdr_len) {
            size_t n = CORE_MIN(total - done, (size_t)(v->hdr_len - v->vpos));
            memcpy(out + done, v->hdr + v->vpos, n);
            v->vpos += (uint32_t)n;
            done += n;
            continue;
        }
        if (v->vpos >= v->vlen) break;
        if (v->vpos < v->buf_vstart || v->vpos - v->buf_vstart >= v->buf_len) {
            if (!vio_fill(v)) break;
        }
        uint32_t off = v->vpos - v->buf_vstart;
        size_t n = CORE_MIN(total - done, (size_t)(v->buf_len - off));
        memcpy(out + done, v->buf + off, n);
        v->vpos += (uint32_t)n;
        done += n;
    }
    return done / size;
}

static int vio_seek(vorbis_io_t *v, long off, int whence) {
    int64_t base = whence == SEEK_CUR ? (int64_t)v->vpos : whence == SEEK_END ? (int64_t)v->vlen : 0;
    int64_t target = base + off;
    if (target < 0) return -1;
    v->vpos = (uint32_t)CORE_MIN(target, (int64_t)v->vlen);  // reads past the end report EOF
    return 0;
}

static long vio_tell(vorbis_io_t *v) { return (long)v->vpos; }

// ------------------------------------------------------------ stb_vorbis ----
// stb_vorbis is compiled here and nowhere else, with its stdio and heap calls redirected.
#define FILE vorbis_io_t
#define fgetc(f) vio_getc(f)
#define fread(p, size, count, f) vio_read((p), (size), (count), (f))
#define fseek(f, off, whence) vio_seek((f), (long)(off), (whence))
#define ftell(f) vio_tell(f)
#define fclose(f) ((void)(f))
#define fopen(name, mode) ((vorbis_io_t *)NULL)
#define fopen_s(pf, name, mode) (*(pf) = NULL, 1)
#define malloc(sz) core_malloc(sz)
#define realloc(p, sz) core_realloc((p), (sz))
#define free(p) core_free(p)
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#define STB_VORBIS_MAX_CHANNELS AUDIO_MAX_CHANNELS
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wshadow"
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wtautological-compare"
#else
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#endif
#include "third_party/stb_vorbis.c.inc"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef FILE
#undef fgetc
#undef fread
#undef fseek
#undef ftell
#undef fclose
#undef fopen
#undef fopen_s
#undef malloc
#undef realloc
#undef free

// ------------------------------------------------------------ Ogg headers ----
typedef struct {
    uint8_t id[30];      // identification packet
    uint8_t *setup;      // setup packet
    uint32_t setup_len, setup_cap;
    uint32_t serial;
    int64_t audio_off;   // first page after the header packets
} ogg_headers_t;

static bool vorbis_probe(const uint8_t *h, size_t n, const char *ext) {
    (void)ext;
    if (n < 28 || !dec_tag_eq(h, "OggS")) return false;
    size_t body = 27 + (size_t)h[26];
    return n >= body + 7 && h[body] == 0x01 && memcmp(h + body + 1, "vorbis", 6) == 0;
}

static int ogg_setup_append(ogg_headers_t *h, core_stream_t *s, int64_t pos, uint32_t len) {
    if (h->setup_len + len > VORBIS_MAX_SETUP) return CORE_EUNSUPPORTED;
    if (h->setup_len + len > h->setup_cap) {
        uint32_t cap = CORE_MAX(h->setup_cap * 2, 16u * 1024);
        while (cap < h->setup_len + len) cap *= 2;
        uint8_t *p = core_realloc(h->setup, cap);
        if (!p) return CORE_ENOMEM;
        h->setup = p;
        h->setup_cap = cap;
    }
    int r = dec_read_at(s, pos, h->setup + h->setup_len, len);
    if (r == CORE_OK) h->setup_len += len;
    return r == CORE_OK ? CORE_OK : CORE_ECORRUPT;
}

// Walks the first pages of the logical stream and collects the identification and setup packets.
// Pages of other logical streams are skipped; the comment packet is skipped without reading it.
static int ogg_read_headers(core_stream_t *s, ogg_headers_t *h) {
    int64_t pos = 0;
    int packet = 0;        // 0 identification, 1 comment, 2 setup
    uint32_t pkt_len = 0;  // bytes of the current packet so far
    uint8_t magic[7];      // start of the comment packet
    for (int page = 0; page < VORBIS_MAX_HEADER_PAGES; page++) {
        uint8_t ph[27 + 255];
        if (dec_read_at(s, pos, ph, 27) != CORE_OK || !dec_tag_eq(ph, "OggS") || ph[4] != 0) return CORE_ECORRUPT;
        uint8_t nseg = ph[26];
        if (core_stream_read_exact(s, ph + 27, nseg) != CORE_OK) return CORE_ECORRUPT;
        int64_t data = pos + 27 + nseg;
        uint32_t body = 0;
        for (int i = 0; i < nseg; i++) body += ph[27 + i];
        uint32_t serial = dec_le32(ph + 14);
        if (page == 0) {
            if (!(ph[5] & 0x02)) return CORE_ECORRUPT;  // first page must begin the stream
            h->serial = serial;
        }
        if (serial == h->serial) {
            int64_t seg = data;
            for (int i = 0; i < nseg; i++) {
                uint32_t len = ph[27 + i];
                if (packet == 0) {
                    if (pkt_len + len > sizeof h->id || dec_read_at(s, seg, h->id + pkt_len, len) != CORE_OK) {
                        return CORE_ECORRUPT;
                    }
                } else if (packet == 1) {
                    uint32_t want = pkt_len < sizeof magic ? CORE_MIN(len, (uint32_t)sizeof magic - pkt_len) : 0;
                    if (want && dec_read_at(s, seg, magic + pkt_len, want) != CORE_OK) return CORE_ECORRUPT;
                } else {
                    int r = ogg_setup_append(h, s, seg, len);
                    if (r != CORE_OK) return r;
                }
                pkt_len += len;
                seg += len;
                if (len == 255) continue;  // packet continues in the next segment
                if (packet == 0 && pkt_len != sizeof h->id) return CORE_ECORRUPT;
                if (packet == 1 && (pkt_len < sizeof magic || magic[0] != 3 || memcmp(magic + 1, "vorbis", 6))) {
                    return CORE_ECORRUPT;
                }
                packet++;
                pkt_len = 0;
                if (packet == 3) {
                    // Vorbis I requires the setup packet to end its page; audio starts on a fresh page.
                    if (i != nseg - 1) return CORE_EUNSUPPORTED;
                    h->audio_off = data + body;
                    return CORE_OK;
                }
            }
        }
        pos = data + body;
    }
    return CORE_ECORRUPT;
}

// Vorbis bit reader (LSB first) for the setup check.
typedef struct {
    const uint8_t *p;
    size_t len, bit;
    bool eop;
} vb_bits_t;

static uint32_t vb_get_bits(vb_bits_t *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++, b->bit++) {
        size_t byte = b->bit >> 3;
        if (byte >= b->len) {
            b->eop = true;
            return 0;
        }
        v |= (uint32_t)((b->p[byte] >> (b->bit & 7)) & 1) << i;
    }
    return v;
}

static int vb_ilog(uint32_t v) {
    int n = 0;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

// Largest r with r^dims <= entries (the Vorbis lookup1_values function), by bisection.
static uint32_t vb_lookup1_values(uint32_t entries, uint32_t dims) {
    uint32_t lo = 1, hi = entries;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        uint64_t acc = 1;
        for (uint32_t d = 0; d < dims && acc <= entries; d++) acc *= mid;
        if (acc <= entries) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

// Walks the codebooks of the setup packet and rejects sizes stb_vorbis would multiply into a
// truncated int allocation. The remaining setup sections are validated by stb_vorbis itself.
static bool vorbis_setup_is_sane(const uint8_t *p, size_t len) {
    if (len < 8 || p[0] != 5 || memcmp(p + 1, "vorbis", 6) != 0) return false;
    vb_bits_t b = {p + 7, len - 7, 0, false};
    uint32_t books = vb_get_bits(&b, 8) + 1;
    for (uint32_t i = 0; i < books && !b.eop; i++) {
        if (vb_get_bits(&b, 24) != 0x564342) return false;
        uint32_t dims = vb_get_bits(&b, 16);
        uint32_t entries = vb_get_bits(&b, 24);
        if ((uint64_t)entries * dims > VORBIS_MAX_BOOK_VALUES || (dims == 0 && entries != 0)) return false;
        if (vb_get_bits(&b, 1)) {  // ordered code lengths
            uint32_t cur = 0, length = vb_get_bits(&b, 5) + 1;
            while (cur < entries && !b.eop) {
                if (length++ > 32) return false;
                uint32_t n = vb_get_bits(&b, vb_ilog(entries - cur));
                if (n > entries - cur) return false;
                cur += n;
            }
        } else {
            bool sparse = vb_get_bits(&b, 1) != 0;
            for (uint32_t j = 0; j < entries && !b.eop; j++) {
                if (!sparse || vb_get_bits(&b, 1)) vb_get_bits(&b, 5);
            }
        }
        uint32_t lookup = vb_get_bits(&b, 4);
        if (lookup > 2) return false;
        if (lookup) {
            vb_get_bits(&b, 32);  // minimum value
            vb_get_bits(&b, 32);  // delta value
            uint32_t value_bits = vb_get_bits(&b, 4) + 1;
            vb_get_bits(&b, 1);  // sequence_p
            uint64_t values = lookup == 1 ? vb_lookup1_values(entries, dims) : (uint64_t)entries * dims;
            uint64_t skip = values * value_bits;
            if (skip > (uint64_t)(b.len * 8 - CORE_MIN(b.bit, b.len * 8))) return false;
            b.bit += (size_t)skip;
        }
    }
    return !b.eop;
}

// Writes one Ogg page at p and returns its size.
static uint32_t ogg_write_page(uint8_t *p, uint8_t flags, uint64_t granule, uint32_t serial, uint32_t seq,
                           const uint8_t *lacing, uint8_t nseg, const uint8_t *data, uint32_t len) {
    memcpy(p, "OggS", 4);
    p[4] = 0;
    p[5] = flags;
    for (int i = 0; i < 8; i++) p[6 + i] = (uint8_t)(granule >> (8 * i));
    for (int i = 0; i < 4; i++) p[14 + i] = (uint8_t)(serial >> (8 * i));
    for (int i = 0; i < 4; i++) p[18 + i] = (uint8_t)(seq >> (8 * i));
    memset(p + 22, 0, 4);
    p[26] = nseg;
    memcpy(p + 27, lacing, nseg);
    memcpy(p + 27 + nseg, data, len);
    uint32_t size = 27 + nseg + len;
    uint32_t crc = dec_ogg_crc(0, p, size);
    for (int i = 0; i < 4; i++) p[22 + i] = (uint8_t)(crc >> (8 * i));
    return size;
}

// Builds the header pages: identification, empty comment, setup (split into as many pages as
// its lacing needs). Returns the buffer (core_malloc) and its length, or NULL.
static uint8_t *vorbis_build_header_pages(const ogg_headers_t *h, uint32_t *out_len) {
    static const uint8_t k_comment[16] = {3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    uint32_t segs = h->setup_len / 255 + 1;  // the last lacing value is < 255 (possibly 0)
    uint32_t pages = (segs + 254) / 255;
    uint32_t size = (27 + 1 + 30) + (27 + 1 + 16) + pages * 27 + segs + h->setup_len;
    uint8_t *buf = core_malloc(size);
    if (!buf) return NULL;
    uint8_t lacing[255];
    uint32_t n = 0, seq = 0;
    lacing[0] = 30;
    n += ogg_write_page(buf + n, 0x02, 0, h->serial, seq++, lacing, 1, h->id, 30);
    lacing[0] = 16;
    n += ogg_write_page(buf + n, 0x00, 0, h->serial, seq++, lacing, 1, k_comment, sizeof k_comment);
    uint32_t done = 0, seg_left = segs;
    for (uint32_t pg = 0; pg < pages; pg++) {
        uint8_t nseg = (uint8_t)CORE_MIN(seg_left, 255u);
        uint32_t bytes = 0;
        for (uint8_t i = 0; i < nseg; i++) {
            uint32_t len = CORE_MIN(h->setup_len - done - bytes, 255u);
            lacing[i] = (uint8_t)len;
            bytes += len;
        }
        bool last = pg + 1 == pages;
        n += ogg_write_page(buf + n, pg ? 0x01 : 0x00, last ? 0 : UINT64_MAX, h->serial, seq++, lacing, nseg,
                        h->setup + done, bytes);
        done += bytes;
        seg_left -= nseg;
    }
    *out_len = n;
    return buf;
}

// ---------------------------------------------------------------- backend ----
typedef struct {
    vorbis_io_t io;
    uint8_t *hdr;
    stb_vorbis *vb;
    char *arena;
    uint8_t channels;
    float **frame;  // channel pointers of the current frame (owned by stb_vorbis)
    uint32_t frame_len, frame_pos;
} vorbis_t;

static int vorbis_map_error(int e) {
    switch (e) {
    case VORBIS_outofmem: return CORE_ENOMEM;
    case VORBIS_too_many_channels:
    case VORBIS_feature_not_supported:
    case VORBIS_ogg_skeleton_not_supported: return CORE_EUNSUPPORTED;
    default: return CORE_ECORRUPT;
    }
}

// Opens stb_vorbis with an arena of `size` bytes. On success the arena is owned by v.
static stb_vorbis *vorbis_open_in_arena(vorbis_t *v, int size, int *verr) {
    char *arena = core_malloc((size_t)size);
    if (!arena) {
        *verr = VORBIS_outofmem;
        return NULL;
    }
    stb_vorbis_alloc a = {arena, size};
    v->io.vpos = 0;
    stb_vorbis *vb = stb_vorbis_open_file(&v->io, 0, verr, &a);
    if (!vb) {
        core_free(arena);
        return NULL;
    }
    v->arena = arena;
    return vb;
}

static void vorbis_close(void *st) {
    vorbis_t *v = st;
    if (!v) return;
    if (v->vb) stb_vorbis_close(v->vb);  // arena mode: frees nothing, the arena goes below
    core_free(v->arena);
    core_free(v->hdr);
    core_free(v);
}

static void *vorbis_open(core_stream_t *s, decoder_info_t *info, int *err) {
    ogg_headers_t h;
    memset(&h, 0, sizeof h);
    int r = ogg_read_headers(s, &h);
    if (r == CORE_OK && (h.id[0] != 1 || memcmp(h.id + 1, "vorbis", 6) != 0 || dec_le32(h.id + 7) != 0)) {
        r = CORE_ECORRUPT;
    }
    if (r == CORE_OK && (h.id[11] == 0 || dec_le32(h.id + 12) == 0)) r = CORE_ECORRUPT;
    if (r == CORE_OK && h.id[11] > AUDIO_MAX_CHANNELS) r = CORE_EUNSUPPORTED;
    if (r == CORE_OK && !vorbis_setup_is_sane(h.setup, h.setup_len)) r = CORE_ECORRUPT;
    int64_t fsize = core_stream_size(s);
    if (r == CORE_OK && (fsize < h.audio_off || fsize - h.audio_off > INT32_MAX - (1 << 20))) r = CORE_EUNSUPPORTED;

    vorbis_t *v = NULL;
    if (r == CORE_OK) {
        v = core_calloc(1, sizeof *v);
        if (!v) r = CORE_ENOMEM;
    }
    if (r == CORE_OK) {
        v->hdr = vorbis_build_header_pages(&h, &v->io.hdr_len);
        if (!v->hdr) r = CORE_ENOMEM;
    }
    core_free(h.setup);
    if (r != CORE_OK) {
        vorbis_close(v);
        *err = r;
        return NULL;
    }
    v->io.s = s;
    v->io.hdr = v->hdr;
    v->io.audio_off = h.audio_off;
    v->io.vlen = v->io.hdr_len + (uint32_t)(fsize - h.audio_off);

    // Grow the arena until setup fits, then retry once with just what it reported to need.
    int verr = VORBIS__no_error, size = VORBIS_ARENA_MIN;
    for (;;) {
        v->vb = vorbis_open_in_arena(v, size, &verr);
        if (v->vb || verr != VORBIS_outofmem || size >= VORBIS_ARENA_MAX) break;
        size *= 2;
    }
    if (!v->vb) {
        vorbis_close(v);
        *err = vorbis_map_error(verr);
        return NULL;
    }
    stb_vorbis_info vi = stb_vorbis_get_info(v->vb);
    uint32_t need = vi.setup_memory_required + CORE_MAX(vi.setup_temp_memory_required, vi.temp_memory_required) +
                    VORBIS_ARENA_SLACK;
    CORE_LOGD(TAG, "arena %d bytes: setup %u, setup temp %u, frame temp %u", size, vi.setup_memory_required,
              vi.setup_temp_memory_required, vi.temp_memory_required);
    if (need + 2 * VORBIS_ARENA_SLACK < (uint32_t)size) {
        // Setup is deterministic, so reopening in a right-sized arena normally succeeds; the
        // reported temp figures do not cover every setup scratch, so fall back to the size that worked.
        stb_vorbis_close(v->vb);
        core_free(v->arena);
        v->arena = NULL;
        v->vb = vorbis_open_in_arena(v, (int)need, &verr);
        if (!v->vb) v->vb = vorbis_open_in_arena(v, size, &verr);
        if (!v->vb) {
            vorbis_close(v);
            *err = vorbis_map_error(verr);
            return NULL;
        }
    }

    v->channels = (uint8_t)vi.channels;
    uint32_t total = stb_vorbis_stream_length_in_samples(v->vb);
    int32_t nominal = (int32_t)dec_le32(h.id + 20);
    info->fmt.sample_rate = vi.sample_rate;
    info->fmt.channels = v->channels;
    info->fmt.bits = 24;  // float synthesis
    info->fmt.dop = false;
    info->total_frames = total;
    info->bitrate_kbps = nominal > 0 ? (uint32_t)nominal / 1000
                                     : dec_bitrate_kbps((uint64_t)(fsize - h.audio_off), total, vi.sample_rate);
    info->seekable = total > 0;
    return v;
}

static int32_t vorbis_read(void *st, int32_t *out, uint32_t max_frames) {
    vorbis_t *v = st;
    uint32_t done = 0;
    int failures = 0;
    while (done < max_frames) {
        if (v->frame_pos >= v->frame_len) {
            unsigned before = stb_vorbis_get_file_offset(v->vb);
            int ch = 0;
            float **pcm = NULL;
            int n = stb_vorbis_get_frame_float(v->vb, &ch, &pcm);
            if (n > 0 && pcm && ch == v->channels) {
                v->frame = pcm;
                v->frame_len = (uint32_t)n;
                v->frame_pos = 0;
                failures = 0;
                continue;
            }
            // End of stream, or a packet that did not decode: skip it if the reader moved on.
            if (v->vb->eof || stb_vorbis_get_file_offset(v->vb) == before || ++failures > 16) break;
            continue;
        }
        uint32_t take = CORE_MIN(v->frame_len - v->frame_pos, max_frames - done);
        int32_t *dst = out + (size_t)done * v->channels;
        for (uint8_t c = 0; c < v->channels; c++) {
            const float *src = v->frame[c] + v->frame_pos;
            for (uint32_t i = 0; i < take; i++) dst[(size_t)i * v->channels + c] = dec_f32_to_q31(src[i]);
        }
        v->frame_pos += take;
        done += take;
    }
    return (int32_t)done;
}

static int vorbis_seek(void *st, uint64_t frame) {
    vorbis_t *v = st;
    if (frame >= UINT32_MAX) return CORE_EINVAL;
    v->frame_len = v->frame_pos = 0;
    if (frame == 0) {
        stb_vorbis_seek_start(v->vb);
        return CORE_OK;
    }
    if (!stb_vorbis_seek_frame(v->vb, (unsigned)frame)) return CORE_ECORRUPT;
    // The next frame contains the target; drop the samples before it.
    uint32_t skip = 0;
    if (v->vb->current_loc_valid && v->vb->current_loc <= frame) skip = (uint32_t)(frame - v->vb->current_loc);
    for (int guard = 0; guard < 64; guard++) {
        int ch = 0;
        float **pcm = NULL;
        int n = stb_vorbis_get_frame_float(v->vb, &ch, &pcm);
        if (n <= 0 || !pcm || ch != v->channels) return v->vb->eof ? CORE_OK : CORE_ECORRUPT;
        if ((uint32_t)n > skip) {
            v->frame = pcm;
            v->frame_len = (uint32_t)n;
            v->frame_pos = skip;
            return CORE_OK;
        }
        skip -= (uint32_t)n;
    }
    return CORE_ECORRUPT;
}

const decoder_backend_t dec_vorbis_backend = {
    CODEC_VORBIS, vorbis_probe, vorbis_open, vorbis_read, vorbis_seek, vorbis_close,
};
