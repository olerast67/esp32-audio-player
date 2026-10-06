// SPDX-License-Identifier: Apache-2.0
/// \file
/// Metadata readers: ID3v2.2/2.3/2.4 (+unsynchronisation, extended header), ID3v1,
/// FLAC Vorbis comments + PICTURE, Ogg Vorbis/Opus comments, RIFF INFO / id3 chunk,
/// AIFF ID3 chunk, APEv2 (optional). Legacy 8-bit text goes through
/// text_detect_legacy(), so CP1251 tags in old Russian MP3 files read correctly.
#pragma once

#include "audio_player/decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Field limits (bytes, UTF-8, including NUL). Longer values are truncated; this
/// keeps memory bounded when a file carries 10 KB+ tag fields.
#define TAG_TEXT_MAX 128
#define TAG_GENRE_MAX 48

typedef struct {
    char title[TAG_TEXT_MAX];
    char artist[TAG_TEXT_MAX];
    char album[TAG_TEXT_MAX];
    char album_artist[TAG_TEXT_MAX];
    char genre[TAG_GENRE_MAX];
    char composer[TAG_TEXT_MAX];
    uint16_t year;
    uint16_t track_no, track_total;
    uint16_t disc_no, disc_total;
    /// ReplayGain (NAN when absent)
    float rg_track_gain_db, rg_track_peak, rg_album_gain_db, rg_album_peak;
    /// Embedded cover: absolute file offset and size of the raw image, 0 if none.
    uint64_t cover_offset;
    uint32_t cover_size;
    uint8_t cover_mime;  ///< 0 none, 1 JPEG, 2 PNG
    /// From the stream when cheap to get (FLAC STREAMINFO, WAV header, MP3 Xing).
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint8_t bits;
    uint8_t channels;
    codec_id_t codec;
    bool is_audiobook_hint;  ///< genre "Audiobook"/"Аудиокнига" or M4B
} track_tags_t;

void tags_clear(track_tags_t *t);

/// Read tags from a file. Missing fields stay empty; if the title is empty the caller
/// may use the file name. Returns CORE_OK even when the file has no tags at all;
/// errors only for I/O problems or unsupported containers.
int tags_read_file(const char *path, track_tags_t *out);
int tags_read_stream(core_stream_t *s, const char *path_hint, track_tags_t *out);

#ifdef __cplusplus
}
#endif
