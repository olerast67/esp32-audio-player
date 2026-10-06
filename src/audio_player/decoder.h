// SPDX-License-Identifier: Apache-2.0
/// \file
/// Audio decoders behind one interface. Probe by magic bytes first, extension second.
///
/// Backends (v0.1): WAV/RF64/W64? (WAV + RF64), AIFF/AIFC(none/sowt), FLAC (dr_flac),
/// MP3 (minimp3, with Xing/LAME/Info gapless trimming), Ogg Vorbis (stb_vorbis),
/// DSF -> DoP64. Later: Opus, ALAC/M4A, AAC (esp_audio_codec on device), WavPack, APE.
#pragma once

#include "audio_player/audio.h"
#include "audio_player/stream.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CODEC_UNKNOWN = 0,
    CODEC_WAV,
    CODEC_AIFF,
    CODEC_FLAC,
    CODEC_MP3,
    CODEC_VORBIS,
    CODEC_OPUS,
    CODEC_AAC,
    CODEC_ALAC,
    CODEC_WAVPACK,
    CODEC_APE,
    CODEC_DSF,
    CODEC_DFF,
    CODEC_COUNT
} codec_id_t;

const char *codec_name(codec_id_t id);   ///< "FLAC", "MP3", ...
bool codec_is_lossless(codec_id_t id);

/// Extension -> codec guess ("flac" -> CODEC_FLAC). Case-insensitive, no dot.
codec_id_t codec_from_extension(const char *ext);
/// True if the file extension is one we list in the browser (supported or planned).
bool codec_is_audio_extension(const char *ext);

typedef struct {
    codec_id_t codec;
    audio_format_t fmt;        ///< output PCM format (dop=true for DSD via DoP)
    uint64_t total_frames;     ///< after gapless trimming; 0 if unknown
    uint32_t bitrate_kbps;     ///< average, 0 if unknown
    bool seekable;
} decoder_info_t;

typedef struct decoder decoder_t;

/// Probe the stream (position is restored). path_hint may be NULL.
codec_id_t decoder_probe(core_stream_t *s, const char *path_hint);

/// Open a decoder. Takes ownership of the stream in all cases (closes it on failure).
/// Returns NULL on failure; *err (optional) receives the reason.
decoder_t *decoder_open(core_stream_t *s, const char *path_hint, decoder_info_t *info, int *err);

/// Decode up to max_frames frames into out (interleaved Q31, info.fmt.channels).
/// Returns frames written (>0), 0 at end of stream, or a negative core_err_t.
/// Gapless trimming (encoder delay/padding) is applied here.
int32_t decoder_read(decoder_t *d, int32_t *out, uint32_t max_frames);

/// Seek to an absolute frame (after trimming). Returns CORE_OK or error.
int decoder_seek(decoder_t *d, uint64_t frame);
uint64_t decoder_tell(decoder_t *d);          ///< current frame
const decoder_info_t *decoder_info(const decoder_t *d);
void decoder_close(decoder_t *d);

// ---------------------------------------------------------------------------
/// Backend interface (for implementers of src/decoders/*.c).
// ---------------------------------------------------------------------------
typedef struct {
    codec_id_t id;
    /// Return true if the first bytes look like this format. ext may be "".
    bool (*probe)(const uint8_t *head, size_t head_len, const char *ext);
    /// Parse headers, fill info, return backend state or NULL.
    void *(*open)(core_stream_t *s, decoder_info_t *info, int *err);
    int32_t (*read)(void *state, int32_t *out, uint32_t max_frames);
    int (*seek)(void *state, uint64_t frame);
    void (*close)(void *state);  ///< must NOT close the stream (the frontend does)
} decoder_backend_t;

#ifdef __cplusplus
}
#endif
