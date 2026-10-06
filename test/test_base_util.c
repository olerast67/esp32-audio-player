// SPDX-License-Identifier: Apache-2.0
#include "audio_player/audio.h"
#include "audio_player/base.h"
#include "audio_player/events.h"
#include "audio_player/stream.h"
#include "test.h"

TEST(strings) {
    char buf[8];
    CHECK_EQ_INT(core_strlcpy(buf, "abcdefghij", sizeof buf), 10);
    CHECK_STR(buf, "abcdefg");
    CHECK_STR(core_path_ext("/music/a.b/Track.FLAC"), "FLAC");
    CHECK_STR(core_path_ext("/music/noext"), "");
    CHECK_STR(core_path_basename("/music/x/y.mp3"), "y.mp3");
    char dir[64];
    core_path_dirname("/music/x/y.mp3", dir, sizeof dir);
    CHECK_STR(dir, "/music/x");
    CHECK(core_str_ends_with_ci("song.Mp3", ".mp3"));
    char joined[32];
    CHECK_EQ_INT(core_path_join(joined, sizeof joined, "/sd", "a.flac"), CORE_OK);
    CHECK_STR(joined, "/sd/a.flac");
}

TEST(memory_stream) {
    static const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    core_stream_t *s = core_stream_open_memory(data, sizeof data, false);
    CHECK(s != NULL);
    uint32_t v32;
    CHECK_EQ_INT(core_stream_read_le32(s, &v32), CORE_OK);
    CHECK_EQ_INT(v32, 0x04030201);
    CHECK_EQ_INT(core_stream_read_be32(s, &v32), CORE_OK);
    CHECK_EQ_INT(v32, 0x05060708);
    CHECK_EQ_INT(core_stream_tell(s), 8);
    CHECK_EQ_INT(core_stream_seek(s, 1, 0), CORE_OK);
    uint16_t v16;
    CHECK_EQ_INT(core_stream_read_le16(s, &v16), CORE_OK);
    CHECK_EQ_INT(v16, 0x0302);
    CHECK_EQ_INT(core_stream_size(s), 9);
    core_stream_close(s);
}

TEST(rate_family) {
    CHECK_EQ_INT(audio_rate_family(44100), RATE_FAMILY_44K1);
    CHECK_EQ_INT(audio_rate_family(176400), RATE_FAMILY_44K1);
    CHECK_EQ_INT(audio_rate_family(48000), RATE_FAMILY_48K);
    CHECK_EQ_INT(audio_rate_family(192000), RATE_FAMILY_48K);
    CHECK_EQ_INT(audio_s16_to_q31(0x1234), 0x12340000);
    CHECK_EQ_INT(audio_ms_to_frames(1000, 48000), 48000);
}

static int g_events;
static void on_ev(const core_event_t *ev, void *user) {
    (void)user;
    if (ev->type == EV_QUEUE_CHANGED) g_events++;
}

TEST(events) {
    core_events_set_sink(on_ev, NULL);
    core_event_post_text(EV_QUEUE_CHANGED, 0, "hello");
    CHECK_EQ_INT(g_events, 1);
    core_events_set_sink(NULL, NULL);
}

TEST_MAIN(RUN(strings) RUN(memory_stream) RUN(rate_family) RUN(events))
