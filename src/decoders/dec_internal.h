// SPDX-License-Identifier: Apache-2.0
// Private helpers shared by the decoder frontend (decoder.c) and the backends (dec_*.c).
#pragma once

#include <string.h>

#include "audio_player/decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

// Backends, in the order the frontend probes them (weak magic such as the MP3 sync word last).
extern const decoder_backend_t dec_wav_backend;
extern const decoder_backend_t dec_aiff_backend;
extern const decoder_backend_t dec_flac_backend;
extern const decoder_backend_t dec_vorbis_backend;
extern const decoder_backend_t dec_dsf_backend;
extern const decoder_backend_t dec_dff_backend;
extern const decoder_backend_t dec_mp3_backend;

// Backend state of an open decoder if it was opened by the backend for `codec`, else NULL.
// Used by backend-specific extensions such as decoder_flac_md5().
void *decoder_backend_state(const decoder_t *d, codec_id_t codec);

// stdio-style whence values for core_stream_seek().
enum { DEC_SEEK_SET = 0, DEC_SEEK_CUR = 1, DEC_SEEK_END = 2 };

// ------------------------------------------------------------- byte access ----
static inline uint16_t dec_le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static inline uint16_t dec_be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static inline uint32_t dec_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint32_t dec_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static inline uint64_t dec_le64(const uint8_t *p) { return (uint64_t)dec_le32(p + 4) << 32 | dec_le32(p); }
static inline uint64_t dec_be64(const uint8_t *p) { return (uint64_t)dec_be32(p) << 32 | dec_be32(p + 4); }
static inline bool dec_tag_eq(const uint8_t *p, const char *tag4) { return memcmp(p, tag4, 4) == 0; }

// --------------------------------------------------------- sample helpers ----
// float [-1, 1) -> Q31 with saturation. NaN becomes 0. float32 only (no double on the ESP32 FPU).
static inline int32_t dec_f32_to_q31(float x) {
    if (x >= 1.0f) return INT32_MAX;
    if (!(x > -1.0f)) return (x == x) ? INT32_MIN : 0;  // x <= -1 or NaN
    return (int32_t)(x * 2147483648.0f);
}

// fmt.bits for a source with `bits` significant bits: 16, 24 or 32.
static inline uint8_t dec_bits_class(unsigned bits) { return bits <= 16 ? 16 : bits <= 24 ? 24 : 32; }

// Average bitrate in kbit/s from a payload size and a duration; 0 if unknown.
uint32_t dec_bitrate_kbps(uint64_t bytes, uint64_t frames, uint32_t sample_rate);

// ------------------------------------------------------------------- tags ----
// Total size of the ID3v2 tag whose 10-byte header is at p (header + body + optional footer),
// or 0 if p does not start a valid ID3v2 header.
uint32_t dec_id3v2_size(const uint8_t *p, size_t len);

// Skips consecutive ID3v2 tags that start at `pos`. Returns the offset of the first byte after
// them (== pos if there is no tag). The stream position is unspecified afterwards.
int64_t dec_skip_id3v2(core_stream_t *s, int64_t pos);

// Reads exactly len bytes at absolute offset pos.
int dec_read_at(core_stream_t *s, int64_t pos, void *buf, size_t len);

// Ogg page CRC-32 (polynomial 0x04C11DB7, not reflected, init 0) over len bytes, continuing crc.
uint32_t dec_ogg_crc(uint32_t crc, const uint8_t *p, size_t len);

#ifdef __cplusplus
}
#endif
