// SPDX-License-Identifier: Apache-2.0
// Helpers shared by several decoder backends.
#include "decoders/dec_internal.h"

uint32_t dec_bitrate_kbps(uint64_t bytes, uint64_t frames, uint32_t sample_rate) {
    // kbps = bytes * 8 / seconds / 1000 = bytes * 8 * rate / frames / 1000. Files are < 2^36 bytes and
    // rates < 2^23 Hz, so the product fits in 64 bits; larger inputs are rejected rather than wrapped.
    if (!frames || !sample_rate || bytes >= (UINT64_C(1) << 36) || sample_rate >= (1u << 23)) return 0;
    uint64_t kbps = bytes * 8 * sample_rate / frames / 1000;
    return kbps > UINT32_MAX ? UINT32_MAX : (uint32_t)kbps;
}

uint32_t dec_id3v2_size(const uint8_t *p, size_t len) {
    if (len < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3') return 0;
    if (p[3] == 0xFF || p[4] == 0xFF) return 0;              // version bytes are never 0xFF
    if ((p[6] | p[7] | p[8] | p[9]) & 0x80) return 0;        // size is synchsafe
    uint32_t body = (uint32_t)p[6] << 21 | (uint32_t)p[7] << 14 | (uint32_t)p[8] << 7 | p[9];
    return 10 + body + ((p[5] & 0x10) ? 10 : 0);              // footer present flag
}

int64_t dec_skip_id3v2(core_stream_t *s, int64_t pos) {
    // Some taggers write several tags back to back; a small bound keeps corrupt input finite.
    for (int i = 0; i < 16; i++) {
        uint8_t h[10];
        if (dec_read_at(s, pos, h, sizeof h) != CORE_OK) break;
        uint32_t n = dec_id3v2_size(h, sizeof h);
        if (!n) break;
        pos += n;
    }
    return pos;
}

int dec_read_at(core_stream_t *s, int64_t pos, void *buf, size_t len) {
    int r = core_stream_seek(s, pos, DEC_SEEK_SET);
    if (r != CORE_OK) return r;
    return core_stream_read_exact(s, buf, len);
}

uint32_t dec_ogg_crc(uint32_t crc, const uint8_t *p, size_t len) {
    // Bitwise form: only used on header pages (a few KB per open), so a table is not worth 1 KB.
    while (len--) {
        crc ^= (uint32_t)*p++ << 24;
        for (int b = 0; b < 8; b++) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}
