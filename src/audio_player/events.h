// SPDX-License-Identifier: Apache-2.0
/// \file
/// Events from the player to the application. The player calls the sink installed with
/// core_events_set_sink() from the thread that runs player_run_once() (or from the thread that
/// issued a command). Keep the sink short: copy the event into a queue, set a flag, or log it.
#pragma once

#include "audio_player/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_NONE = 0,
    /// player
    EV_PLAYER_STATE,        ///< a = player_state_t
    EV_TRACK_CHANGED,       ///< a = queue index
    EV_POSITION,            ///< a = position ms (about 4 times per second)
    EV_VOLUME,              ///< f = volume dB
    EV_QUEUE_CHANGED,
    EV_PLAYBACK_ERROR,      ///< a = core_err_t, text = file name
    EV_SLEEP_TIMER,         ///< a = seconds left (0 = fired and paused, -1 = cancelled)
} core_event_type_t;

typedef struct {
    core_event_type_t type;
    int32_t a;
    int32_t b;
    float f;
    char text[64];
} core_event_t;

typedef void (*core_event_sink_t)(const core_event_t *ev, void *user);
void core_events_set_sink(core_event_sink_t sink, void *user);
void core_event_post(const core_event_t *ev);
void core_event_post_simple(core_event_type_t type, int32_t a, int32_t b);
void core_event_post_text(core_event_type_t type, int32_t a, const char *text);

#ifdef __cplusplus
}
#endif
