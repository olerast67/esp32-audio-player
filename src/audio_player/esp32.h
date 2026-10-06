// SPDX-License-Identifier: Apache-2.0
/// \file
/// ESP32 runtime: connects the portable core to ESP-IDF and runs the player in its own task.
///
///   audio_player_esp32_config_t cfg = AUDIO_PLAYER_ESP32_CONFIG_DEFAULT();
///   cfg.sink = audio_i2s_sink_create(&i2s_cfg);
///   player_t *p = audio_player_start(&cfg);
///   queue_item_t item = {.track_id = LIB_ID_NONE, .path = "/sdcard/music/song.flac"};
///   player_cmd_play_items(p, &item, 1, 0, false);
///
/// audio_player_port_init() installs the platform hooks of audio_player/base.h:
///  - memory: core_malloc() (decoder state, read buffers, FIFOs) prefers PSRAM when the board
///    has it, core_malloc_fast() uses internal RAM;
///  - logging through ESP_LOG with the core's tags;
///  - FreeRTOS recursive mutexes for the core locks;
///  - esp_timer for core_now_ms().
/// audio_player_start() calls it, so most applications never call it themselves.
#pragma once

#include "audio_player/events.h"
#include "audio_player/player.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Idempotent. max_level limits the core's log output (CORE_LOG_INFO is a good default).
void audio_player_port_init(core_log_level_t max_level);

typedef struct {
    audio_sink_t *sink;           ///< output, may be NULL and set later with player_cmd_set_sink()
    library_t *library;           ///< optional music library index
    const char *state_dir;        ///< resume state and scrobble log directory, NULL = none
    bool scrobble_log;            ///< write a Rockbox-format .scrobbler.log into state_dir
    float volume_default_db;      ///< first volume when there is no saved state
    float volume_max_db;          ///< upper volume limit (hearing protection), <= 0
    float volume_step_db;         ///< player_cmd_volume_step() increment
    uint32_t task_stack;          ///< bytes of internal RAM for the audio task; the MP3 decoder alone
                                  ///< takes about 16 KB of it per frame (minimp3 scratch)
    uint8_t task_priority;        ///< FreeRTOS priority of the audio task
    int8_t task_core;             ///< core to pin the audio task to, -1 = any
    core_event_sink_t on_event;   ///< optional: player events (called from the audio task)
    void *on_event_user;
    core_log_level_t log_level;
} audio_player_esp32_config_t;

/// The audio task runs on the second core of dual-core chips, away from Wi-Fi and Bluetooth.
#define AUDIO_PLAYER_ESP32_CONFIG_DEFAULT()                                                                   \
    {                                                                                                         \
        .sink = NULL, .library = NULL, .state_dir = NULL, .scrobble_log = false, .volume_default_db = -20.0f, \
        .volume_max_db = 0.0f, .volume_step_db = 1.0f, .task_stack = 24576, .task_priority = 18,              \
        .task_core = 1, .on_event = NULL, .on_event_user = NULL, .log_level = CORE_LOG_INFO,                  \
    }

/// Initialise the port, create the player and start its audio task. NULL on failure.
/// With state_dir set, call player_restore_state() next to continue where the last session ended.
player_t *audio_player_start(const audio_player_esp32_config_t *cfg);
/// Stop the audio task and destroy the player. The sink stays: destroy it afterwards. Not from
/// on_event (it runs on the audio task). With Bluetooth, call a2dp_xq_deinit() first: its
/// remote callback holds the player pointer.
void audio_player_stop(player_t *p);

#ifdef __cplusplus
}
#endif
