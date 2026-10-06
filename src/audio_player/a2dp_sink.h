// SPDX-License-Identifier: Apache-2.0
/// \file
/// Bluetooth headphones as a player output, through the esp32-a2dp-xq library
/// (https://github.com/olerast67/esp32-a2dp-xq; Arduino: install it from the Library Manager,
/// ESP-IDF: add olerast67/esp32-a2dp-xq to your idf_component.yml).
///
/// Header-only on purpose: include it in the one source file that sets up the player, so this
/// library builds without esp32-a2dp-xq when you do not use Bluetooth.
///
///   a2dp_xq_config_t bt = A2DP_XQ_CONFIG_DEFAULT();
///   bt.remote_cb = audio_player_a2dp_remote;   // buttons and volume on the headphones
///   bt.remote_user = player;                   // set after audio_player_start()
///   a2dp_xq_init(&bt);
///   player_cmd_set_sink(player, audio_player_a2dp_sink());
///
/// The player resamples everything to 44.1 kHz for this sink and keeps its DSP output at
/// 32 bits; the sink adds the one TPDF dither to 16 bits. Volume goes to the headphones over
/// AVRCP absolute volume when they support it (SINK_CAP_HW_VOLUME), so it is never applied
/// twice. DSD (DoP) cannot be sent over Bluetooth and is refused.
#pragma once

#include "a2dp_xq.h"
#include "audio_player/player.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline int ap_a2dp_negotiate(audio_sink_t *s, const audio_format_t *src, audio_format_t *out) {
    (void)s;
    if (!src || !out || !src->sample_rate || !src->channels) return CORE_EINVAL;
    if (src->dop) return CORE_EUNSUPPORTED;
    out->sample_rate = A2DP_XQ_SAMPLE_RATE;
    out->channels = 2;
    out->bits = 16;
    out->dop = false;
    return CORE_OK;
}

static inline int ap_a2dp_open(audio_sink_t *s, const audio_format_t *fmt) {
    (void)s;
    if (!fmt) return CORE_EINVAL;
    if (fmt->sample_rate != A2DP_XQ_SAMPLE_RATE || fmt->channels != 2 || fmt->dop) return CORE_EUNSUPPORTED;
    return a2dp_xq_start() == ESP_OK ? CORE_OK : CORE_EIO;
}

static inline int32_t ap_a2dp_write(audio_sink_t *s, const int32_t *pcm, uint32_t frames, uint32_t timeout_ms) {
    (void)s;
    int32_t n = a2dp_xq_write_q31(pcm, frames, timeout_ms);
    if (n < 0) return CORE_EINVAL;
    return n;
}

static inline void ap_a2dp_pause(audio_sink_t *s, bool paused) {
    (void)s;
    a2dp_xq_pause(paused);
}

static inline void ap_a2dp_flush(audio_sink_t *s) {
    (void)s;
    a2dp_xq_flush();
}

static inline int ap_a2dp_set_volume_db(audio_sink_t *s, float db) {
    (void)s;
    a2dp_xq_set_volume_db(db);
    return CORE_OK;
}

static inline uint32_t ap_a2dp_buffered_frames(audio_sink_t *s) {
    (void)s;
    return a2dp_xq_buffered_frames();
}

static inline void ap_a2dp_describe(audio_sink_t *s, char *out, size_t out_size) {
    (void)s;
    if (!out || !out_size) return;
    a2dp_xq_describe(out, out_size);
    if (!out[0]) core_strlcpy(out, "Bluetooth", out_size);
}

static inline void ap_a2dp_close(audio_sink_t *s) {
    (void)s;
    a2dp_xq_stop();
}

/// The sink. Call after a2dp_xq_init(); it stays valid until a2dp_xq_deinit().
static inline audio_sink_t *audio_player_a2dp_sink(void) {
    static audio_sink_t sink;
    sink.name = "bluetooth";
    sink.caps = SINK_CAP_HW_VOLUME | SINK_CAP_DITHERS;
    sink.ctx = NULL;
    sink.negotiate = ap_a2dp_negotiate;
    sink.open = ap_a2dp_open;
    sink.write = ap_a2dp_write;
    sink.pause = ap_a2dp_pause;
    sink.flush = ap_a2dp_flush;
    sink.set_volume_db = ap_a2dp_set_volume_db;
    sink.buffered_frames = ap_a2dp_buffered_frames;
    sink.describe = ap_a2dp_describe;
    sink.close = ap_a2dp_close;
    return &sink;
}

/// a2dp_xq_config_t.remote_cb with remote_user = the player: the headphone buttons control the
/// player, their own volume changes become the player volume, and a lost link pauses.
static inline void audio_player_a2dp_remote(a2dp_xq_remote_t cmd, int value, void *user) {
    player_t *p = (player_t *)user;
    if (!p) return;
    switch (cmd) {
        case A2DP_XQ_REMOTE_PLAY_PAUSE: player_cmd_toggle_pause(p); break;
        case A2DP_XQ_REMOTE_PLAY: player_cmd_resume(p); break;
        case A2DP_XQ_REMOTE_PAUSE: player_cmd_pause(p); break;
        case A2DP_XQ_REMOTE_STOP: player_cmd_pause(p); break;  ///< keeps the position, like most players
        case A2DP_XQ_REMOTE_NEXT: player_cmd_next(p); break;
        case A2DP_XQ_REMOTE_PREV: player_cmd_prev(p); break;
        case A2DP_XQ_REMOTE_VOL_UP: player_cmd_volume_step(p, 1); break;
        case A2DP_XQ_REMOTE_VOL_DOWN: player_cmd_volume_step(p, -1); break;
        case A2DP_XQ_REMOTE_VOLUME_ABS: player_cmd_set_volume_db(p, a2dp_xq_volume_to_db(value)); break;
        case A2DP_XQ_REMOTE_LINK_DOWN: player_cmd_pause(p); break;
    }
}

#ifdef __cplusplus
}
#endif
