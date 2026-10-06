// SPDX-License-Identifier: Apache-2.0
/// \file
/// Audio output interface. Implemented by the ESP32 port (I2S DAC, Bluetooth through
/// esp32-a2dp-xq), by tests (memory/WAV writer) or by the application (any other output).
#pragma once

#include "audio_player/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Output resolution. The negotiated bits describe the format for the bit-perfect check; the
/// capabilities say how many bits of each Q31 sample really reach the DAC, which sets the
/// DSP dither: SINK_CAP_DITHERS -> no DSP dither (Q31 out), SINK_CAP_BITPERFECT -> at least
/// 24 bits, neither -> the negotiated bits.
enum {
    SINK_CAP_HW_VOLUME = 1u << 0,   ///< set_volume_db works; DSP software volume stays off
    SINK_CAP_DOP = 1u << 1,         ///< accepts DoP frames
    /// Passes the Q31 samples unchanged (at least their top 24 bits, e.g. 32-bit I2S slots)
    /// whatever the negotiated bits, so it is bit-perfect when the rates match.
    SINK_CAP_BITPERFECT = 1u << 2,
    SINK_CAP_RATE_SWITCH = 1u << 3, ///< can change sample rate between tracks
    SINK_CAP_DITHERS = 1u << 4,     ///< the sink reduces Q31 to its bit depth with its own TPDF dither
};

typedef struct audio_sink audio_sink_t;

struct audio_sink {
    const char *name;   ///< "wired", "bluetooth", "null", "wav"
    uint32_t caps;
    void *ctx;

    /// Given the decoded format, fill *out with the format the sink will accept.
    /// Wired: same rate if the DAC supports it. Bluetooth: 44100/48000, 16 bits.
    int (*negotiate)(audio_sink_t *s, const audio_format_t *src, audio_format_t *out);
    /// Start or reconfigure for fmt. May mute and block briefly (clock switch).
    int (*open)(audio_sink_t *s, const audio_format_t *fmt);
    /// Write interleaved Q31 frames. Blocks up to timeout_ms when the output buffer is
    /// full. Returns frames accepted (may be fewer than requested) or <0.
    int32_t (*write)(audio_sink_t *s, const int32_t *pcm, uint32_t frames, uint32_t timeout_ms);
    void (*pause)(audio_sink_t *s, bool paused);   ///< pause output without dropping data
    void (*flush)(audio_sink_t *s);                ///< drop queued audio (seek, skip)
    int (*set_volume_db)(audio_sink_t *s, float db);  ///< hardware volume; -1 if unsupported
    uint32_t (*buffered_frames)(audio_sink_t *s);  ///< queued but not yet played
    /// Human-readable detail for the status bar, e.g. "SBC-XQ 452 kbps" or "PCM5102A".
    void (*describe)(audio_sink_t *s, char *out, size_t out_size);
    void (*close)(audio_sink_t *s);
};

#ifdef __cplusplus
}
#endif
