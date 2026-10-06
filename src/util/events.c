// SPDX-License-Identifier: Apache-2.0
#include "audio_player/events.h"

#include <string.h>

static core_event_sink_t s_sink;
static void *s_user;

void core_events_set_sink(core_event_sink_t sink, void *user) {
    s_sink = sink;
    s_user = user;
}

void core_event_post(const core_event_t *ev) {
    if (s_sink && ev) s_sink(ev, s_user);
}

void core_event_post_simple(core_event_type_t type, int32_t a, int32_t b) {
    core_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.a = a;
    ev.b = b;
    core_event_post(&ev);
}

void core_event_post_text(core_event_type_t type, int32_t a, const char *text) {
    core_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.a = a;
    if (text) core_strlcpy(ev.text, text, sizeof ev.text);
    core_event_post(&ev);
}
