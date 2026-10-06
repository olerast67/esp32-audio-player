// SPDX-License-Identifier: Apache-2.0
// Queue and commands:
// - queue edits: play, append, play next, remove (before/at the current entry), jump, clear,
//   display order, capacity limit, enqueue into an empty queue
// - shuffle: seeded Fisher-Yates with the current entry first, deterministic, a permutation,
//   off restores the original order without interrupting the track, played in shuffled order
// - repeat off/all/one, next at the end, "previous" restarts after 3 s
// - command ring: merged volume/seek commands, overflow is harmless
// - thread-safety: random commands between iterations, and a real second thread posting
//   commands and reading the status while the audio thread runs (real locks installed)
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define mkdir_(p) _mkdir(p)
#else
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <time.h>
#define mkdir_(p) mkdir(p, 0775)
#endif

#include "support/player_sinks.h"
#include "audio_player/events.h"
#include "audio_player/player.h"
#include "player/player_priv.h"
#include "test.h"

#define TMP "tmp_player_queue"
#define NFILES 8
#define SHORT_FRAMES 2000u   // 0.25 s at 8 kHz
#define LONG_FRAMES 40000u   // 5 s at 8 kHz

// ---------------------------------------------------------------------------- helpers ----
static core_event_t g_ev[8192];
static int g_nev;

static void ev_sink(const core_event_t *ev, void *user) {
    (void)user;
    if (g_nev < (int)CORE_ARRAY_SIZE(g_ev)) g_ev[g_nev++] = *ev;
}

static int ev_count(core_event_type_t t) {
    int n = 0;
    for (int i = 0; i < g_nev; i++) n += g_ev[i].type == t;
    return n;
}

static char g_short[NFILES][64], g_mid[NFILES][64], g_long[2][64];

static int32_t const_gen(uint64_t i, unsigned ch, void *user) {
    (void)i;
    (void)ch;
    return (int32_t)(intptr_t)user;
}

static int32_t ramp_gen(uint64_t i, unsigned ch, void *user) {
    (void)ch;
    return (int32_t)((i * 3 + (uint64_t)(intptr_t)user) % 50000) - 25000;
}

static void make_files(void) {
    mkdir_(TMP);
    for (int i = 0; i < NFILES; i++) {
        snprintf(g_short[i], sizeof g_short[i], TMP "/s%d.wav", i);
        wavgen_write(g_short[i], 8000, 1, 16, SHORT_FRAMES, const_gen, (void *)(intptr_t)((i + 1) * 1000), NULL);
        snprintf(g_mid[i], sizeof g_mid[i], TMP "/t%d.wav", i);
        wavgen_write(g_mid[i], 8000, 1, 16, LONG_FRAMES, const_gen, (void *)(intptr_t)((i + 1) * 1000), NULL);
    }
    for (int i = 0; i < 2; i++) {
        snprintf(g_long[i], sizeof g_long[i], TMP "/long%d.wav", i);
        wavgen_write(g_long[i], 8000, 1, 16, LONG_FRAMES, ramp_gen, (void *)(intptr_t)(i * 7777), NULL);
    }
}

static player_t *new_player(mem_sink_t *m) {
    player_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.sink = m ? &m->sink : NULL;
    cfg.volume_default_db = 0.0f;
    cfg.volume_max_db = 0.0f;
    cfg.volume_step_db = 1.0f;
    g_nev = 0;
    return player_create(&cfg);
}

// Entries for the short (0.25 s) or the 5 s files.
static void fill_set(queue_item_t *it, const int *idx, uint32_t n, bool mid) {
    memset(it, 0, n * sizeof *it);
    for (uint32_t i = 0; i < n; i++) {
        it[i].track_id = LIB_ID_NONE;
        core_strlcpy(it[i].path, mid ? g_mid[idx[i]] : g_short[idx[i]], sizeof it[i].path);
    }
}

static void fill_items(queue_item_t *it, const int *idx, uint32_t n) { fill_set(it, idx, n, false); }
static void fill_mid(queue_item_t *it, const int *idx, uint32_t n) { fill_set(it, idx, n, true); }

static player_status_t status_of(player_t *p) {
    player_status_t st;
    player_get_status(p, &st);
    return st;
}

static void run_n(player_t *p, int n) {
    for (int i = 0; i < n; i++) player_run_once(p);
}

static int run_to_stop(player_t *p, int max_iter) {
    int i;
    for (i = 0; i < max_iter; i++) {
        player_run_once(p);
        if (status_of(p).state == PLAYER_STOPPED) break;
    }
    return i;
}

// Index of the short file an entry points to, -1 if none.
static int entry_file(player_t *p, uint32_t index) {
    queue_item_t q;
    if (player_queue_get(p, index, &q) != CORE_OK) return -1;
    for (int i = 0; i < NFILES; i++) {
        if (strcmp(q.path, g_short[i]) == 0 || strcmp(q.path, g_mid[i]) == 0) return i;
    }
    return -1;
}

// Sequence of files in the recording (each plays `len` constant frames).
static int output_sequence_len(const mem_sink_t *m, int *seq, int max, uint64_t len) {
    int n = 0;
    uint64_t i = 0;
    while (i < m->frames && n < max) {
        int32_t v = m->pcm[i * 2];
        uint64_t run = 0;
        while (i + run < m->frames && m->pcm[(i + run) * 2] == v) run++;
        int file = (int)((uint32_t)v >> 16) / 1000 - 1;
        for (uint64_t k = 0; k < run / len && n < max; k++) seq[n++] = file;
        if (run % len) seq[n++] = -100;  // partial: not expected
        i += run;
    }
    return n;
}

static int output_sequence(const mem_sink_t *m, int *seq, int max) {
    return output_sequence_len(m, seq, max, SHORT_FRAMES);
}

// ------------------------------------------------------------------------------ tests ----
TEST(queue_ops) {
    mem_sink_t m;
    mem_sink_init(&m, "mem", SINK_CAP_BITPERFECT);
    player_t *p = new_player(&m);
    queue_item_t it[8];
    int first[] = {0, 1, 2, 3, 4};
    fill_mid(it, first, 5);
    player_cmd_play_items(p, it, 5, 2, false);
    player_run_once(p);
    player_status_t st = status_of(p);
    CHECK_EQ_INT(st.queue_length, 5);
    CHECK_EQ_INT(st.queue_index, 2);
    CHECK_STR(st.title, "t2");
    CHECK(st.state == PLAYER_PLAYING);
    for (uint32_t i = 0; i < 5; i++) CHECK_EQ_INT(entry_file(p, i), i);
    int more[] = {5, 6};
    fill_mid(it, more, 2);
    player_cmd_enqueue(p, it, 2, false);
    int nxt[] = {7};
    fill_mid(it, nxt, 1);
    player_cmd_enqueue(p, it, 1, true);
    player_run_once(p);
    CHECK_EQ_INT(player_queue_count(p), 8);
    static const int expect1[] = {0, 1, 2, 7, 3, 4, 5, 6};
    for (uint32_t i = 0; i < 8; i++) CHECK_EQ_INT(entry_file(p, i), expect1[i]);
    CHECK_EQ_INT(status_of(p).queue_index, 2);
    // Remove before the current entry: the index follows.
    player_cmd_queue_remove(p, 0);
    player_run_once(p);
    st = status_of(p);
    CHECK_EQ_INT(st.queue_index, 1);
    CHECK_STR(st.title, "t2");
    // Remove the current entry: the next one plays.
    player_cmd_queue_remove(p, 1);
    player_run_once(p);
    st = status_of(p);
    CHECK_EQ_INT(st.queue_length, 6);
    CHECK_EQ_INT(st.queue_index, 1);
    CHECK_STR(st.title, "t7");
    CHECK(st.state == PLAYER_PLAYING);
    player_cmd_queue_jump(p, 4);
    player_run_once(p);
    st = status_of(p);
    CHECK_EQ_INT(st.queue_index, 4);
    CHECK_STR(st.title, "t5");
    queue_item_t q;
    CHECK_EQ_INT(player_queue_get(p, 100, &q), CORE_ENOTFOUND);
    CHECK_EQ_INT(player_queue_get(NULL, 0, &q), CORE_EINVAL);
    player_cmd_queue_jump(p, 99);  // ignored
    player_run_once(p);
    CHECK_EQ_INT(status_of(p).queue_index, 4);
    CHECK(ev_count(EV_QUEUE_CHANGED) >= 4);
    player_cmd_queue_clear(p);
    player_run_once(p);
    st = status_of(p);
    CHECK_EQ_INT(st.queue_length, 0);
    CHECK(st.state == PLAYER_STOPPED);
    CHECK_STR(st.title, "");
    // Enqueue into an empty queue: nothing starts, resume plays the first entry.
    fill_mid(it, more, 2);
    player_cmd_enqueue(p, it, 2, true);
    player_run_once(p);
    st = status_of(p);
    CHECK_EQ_INT(st.queue_length, 2);
    CHECK(st.state == PLAYER_STOPPED);
    player_cmd_resume(p);
    player_run_once(p);
    st = status_of(p);
    CHECK(st.state == PLAYER_PLAYING);
    CHECK_STR(st.title, "t5");
    // Removing the last entry while it plays stops (repeat off).
    player_cmd_queue_jump(p, 1);
    player_cmd_queue_remove(p, 1);
    player_run_once(p);
    st = status_of(p);
    CHECK(st.state == PLAYER_STOPPED);
    CHECK_EQ_INT(st.queue_length, 1);
    player_destroy(p);
    mem_sink_free(&m);
}

TEST(queue_capacity) {
    player_t *p = new_player(NULL);
    uint32_t n = PLAYER_QUEUE_MAX + 5000;
    queue_item_t *it = calloc(n, sizeof *it);
    for (uint32_t i = 0; i < n; i++) {
        it[i].track_id = LIB_ID_NONE;
        snprintf(it[i].path, sizeof it[i].path, "/sdcard/Music/Artist %u/Album/%05u track.flac", i / 12, i);
    }
    player_cmd_play_items(p, it, n, 0, false);
    player_run_once(p);
    CHECK_EQ_INT(player_queue_count(p), PLAYER_QUEUE_MAX);
    player_cmd_enqueue(p, it, 10, false);
    player_run_once(p);
    CHECK_EQ_INT(player_queue_count(p), PLAYER_QUEUE_MAX);
    queue_item_t q;
    CHECK_EQ_INT(player_queue_get(p, PLAYER_QUEUE_MAX - 1, &q), CORE_OK);
    CHECK_STR(q.path, it[PLAYER_QUEUE_MAX - 1].path);
    // Entries without id and path are ignored; the start index follows them.
    queue_item_t two[3];
    memset(two, 0, sizeof two);
    two[0].track_id = LIB_ID_NONE;
    two[1].track_id = LIB_ID_NONE;
    core_strlcpy(two[1].path, g_short[1], sizeof two[1].path);
    two[2].track_id = LIB_ID_NONE;
    core_strlcpy(two[2].path, g_short[2], sizeof two[2].path);
    player_cmd_play_items(p, two, 3, 2, false);
    player_run_once(p);
    CHECK_EQ_INT(player_queue_count(p), 2);
    CHECK_EQ_INT(status_of(p).queue_index, 1);
    free(it);
    player_destroy(p);
}

TEST(shuffle_determinism) {
    mem_sink_t m;
    mem_sink_init(&m, "mem", SINK_CAP_BITPERFECT);
    queue_item_t it[NFILES];
    int all[NFILES] = {0, 1, 2, 3, 4, 5, 6, 7};
    fill_mid(it, all, NFILES);
    int order[3][NFILES];
    uint32_t seeds[3] = {1234, 1234, 999};
    for (int k = 0; k < 3; k++) {
        player_t *p = new_player(&m);
        player_test_set_seed(p, seeds[k]);
        player_cmd_play_items(p, it, NFILES, 3, true);
        player_run_once(p);
        player_status_t st = status_of(p);
        CHECK(st.shuffle);
        CHECK_EQ_INT(st.queue_index, 0);
        CHECK_STR(st.title, "t3");
        int seen[NFILES] = {0};
        for (uint32_t i = 0; i < NFILES; i++) {
            order[k][i] = entry_file(p, i);
            if (order[k][i] >= 0) seen[order[k][i]]++;
        }
        CHECK_EQ_INT(order[k][0], 3);
        for (int i = 0; i < NFILES; i++) CHECK_EQ_INT(seen[i], 1);  // a permutation
        player_destroy(p);
    }
    CHECK(memcmp(order[0], order[1], sizeof order[0]) == 0);
    CHECK(memcmp(order[0], order[2], sizeof order[0]) != 0);
    bool identity = true;
    for (int i = 0; i < NFILES; i++) identity = identity && order[0][i] == i;
    CHECK(!identity);

    // Shuffle off: original order, same entry, playback not interrupted.
    mem_sink_reset_recording(&m);
    player_t *p = new_player(&m);
    player_test_set_seed(p, 1234);
    player_cmd_play_items(p, it, NFILES, 3, true);
    run_n(p, 1);
    unsigned flushes = m.flushes;
    uint64_t frames = m.frames;
    player_cmd_set_shuffle(p, false);
    player_run_once(p);
    player_status_t st = status_of(p);
    CHECK(!st.shuffle);
    CHECK_EQ_INT(st.queue_index, 3);
    CHECK_STR(st.title, "t3");
    CHECK_EQ_INT(m.flushes, flushes);
    CHECK(m.frames > frames);
    for (uint32_t i = 0; i < NFILES; i++) CHECK_EQ_INT(entry_file(p, i), i);
    // On again: the current entry moves to the front.
    player_cmd_set_shuffle(p, true);
    player_run_once(p);
    CHECK_EQ_INT(status_of(p).queue_index, 0);
    CHECK_EQ_INT(entry_file(p, 0), 3);
    int shuffled[NFILES];
    for (uint32_t i = 0; i < NFILES; i++) shuffled[i] = entry_file(p, i);
    // Plays in the shuffled order.
    mem_sink_reset_recording(&m);
    player_cmd_queue_jump(p, 0);
    run_to_stop(p, 1000);
    int seq[32];
    int n = output_sequence_len(&m, seq, 32, LONG_FRAMES);
    CHECK_EQ_INT(n, NFILES);
    for (int i = 0; i < n && i < NFILES; i++) CHECK_EQ_INT(seq[i], shuffled[i]);
    // Play next while shuffled goes right after the current entry.
    player_cmd_queue_jump(p, 2);
    int one[] = {0};
    queue_item_t extra;
    fill_mid(&extra, one, 1);
    player_cmd_enqueue(p, &extra, 1, true);
    player_run_once(p);
    CHECK_EQ_INT(entry_file(p, 3), 0);
    CHECK_EQ_INT(player_queue_count(p), NFILES + 1);
    player_destroy(p);
    mem_sink_free(&m);
}

TEST(repeat_modes) {
    mem_sink_t m;
    mem_sink_init(&m, "mem", SINK_CAP_BITPERFECT);
    queue_item_t it[2];
    int ab[] = {0, 1};
    fill_items(it, ab, 2);
    int seq[64];

    // Off: A B, then stopped and rewound.
    player_t *p = new_player(&m);
    player_cmd_play_items(p, it, 2, 0, false);
    run_to_stop(p, 1000);
    int n = output_sequence(&m, seq, 64);
    CHECK_EQ_INT(n, 2);
    CHECK(n == 2 && seq[0] == 0 && seq[1] == 1);
    CHECK_EQ_INT(status_of(p).queue_index, 0);
    CHECK_EQ_INT(status_of(p).position_ms, 0);
    player_destroy(p);

    // All: A B A B ...
    mem_sink_reset_recording(&m);
    p = new_player(&m);
    player_cmd_set_repeat(p, REPEAT_ALL);
    player_cmd_play_items(p, it, 2, 0, false);
    run_n(p, 12);  // 12 * 1024 frames = 6 entries
    n = output_sequence(&m, seq, 64);
    CHECK(n >= 5);
    for (int i = 0; i < n && i < 5; i++) CHECK_EQ_INT(seq[i], i % 2);
    CHECK(status_of(p).repeat == REPEAT_ALL);
    // Next at the end wraps with repeat all.
    player_cmd_queue_jump(p, 1);
    player_cmd_next(p);
    player_run_once(p);
    CHECK_EQ_INT(status_of(p).queue_index, 0);
    player_destroy(p);

    // One: A A A ... (continuous, no flush)
    mem_sink_reset_recording(&m);
    p = new_player(&m);
    player_cmd_set_repeat(p, REPEAT_ONE);
    player_cmd_play_items(p, it, 2, 0, false);
    unsigned flushes = m.flushes;
    g_nev = 0;
    run_n(p, 10);
    n = output_sequence(&m, seq, 64);
    CHECK(n >= 4);
    for (int i = 0; i < n && i < 4; i++) CHECK_EQ_INT(seq[i], 0);
    CHECK_EQ_INT(status_of(p).queue_index, 0);
    CHECK_EQ_INT(m.flushes, flushes);
    CHECK(ev_count(EV_TRACK_CHANGED) >= 4);
    // A user "next" still moves on.
    player_cmd_next(p);
    player_run_once(p);
    CHECK_EQ_INT(status_of(p).queue_index, 1);
    player_destroy(p);

    // Off: next on the last entry does nothing.
    p = new_player(&m);
    player_cmd_play_items(p, it, 2, 1, false);
    player_run_once(p);
    player_cmd_next(p);
    player_run_once(p);
    CHECK_EQ_INT(status_of(p).queue_index, 1);
    player_destroy(p);
    mem_sink_free(&m);
}

TEST(prev_logic) {
    uint64_t na = 0;
    int32_t *a = decode_all(g_long[0], &na, NULL);
    mem_sink_t m;
    mem_sink_init(&m, "mem", SINK_CAP_BITPERFECT);
    player_t *p = new_player(&m);
    queue_item_t it[2];
    memset(it, 0, sizeof it);
    for (int i = 0; i < 2; i++) {
        it[i].track_id = LIB_ID_NONE;
        core_strlcpy(it[i].path, g_long[i], sizeof it[i].path);
    }
    player_cmd_play_items(p, it, 2, 1, false);
    run_n(p, 5);  // 0.64 s into B
    player_cmd_prev(p);
    player_run_once(p);
    player_status_t st = status_of(p);
    CHECK_EQ_INT(st.queue_index, 0);
    CHECK(st.position_ms < 200);
    run_n(p, 30);  // 3.9 s into A
    CHECK(status_of(p).position_ms > 3000);
    uint64_t mark = m.frames;
    player_cmd_prev(p);
    player_run_once(p);
    st = status_of(p);
    CHECK_EQ_INT(st.queue_index, 0);
    CHECK(st.position_ms < 200);
    // The restart replays A from its first frame (mono upmixed).
    bool same = a && m.frames >= mark + 1024;
    for (uint64_t i = 0; same && i < 1024; i++) same = m.pcm[(mark + i) * 2] == a[i];
    CHECK(same);
    // At the first entry within 3 s: restart; with repeat all: the last entry.
    player_cmd_prev(p);
    player_run_once(p);
    CHECK_EQ_INT(status_of(p).queue_index, 0);
    player_cmd_set_repeat(p, REPEAT_ALL);
    player_cmd_prev(p);
    player_run_once(p);
    CHECK_EQ_INT(status_of(p).queue_index, 1);
    // Paused stays paused on prev/next, with the new entry prepared.
    player_cmd_pause(p);
    player_cmd_prev(p);
    player_run_once(p);
    st = status_of(p);
    CHECK(st.state == PLAYER_PAUSED);
    CHECK_EQ_INT(st.queue_index, 0);
    CHECK(st.duration_ms == 5000);
    free(a);
    player_destroy(p);
    mem_sink_free(&m);
}

TEST(command_merging_and_overflow) {
    mem_sink_t m;
    mem_sink_init(&m, "mem", SINK_CAP_BITPERFECT);
    player_t *p = new_player(&m);
    queue_item_t it;
    memset(&it, 0, sizeof it);
    it.track_id = LIB_ID_NONE;
    core_strlcpy(it.path, g_long[0], sizeof it.path);
    player_cmd_play_items(p, &it, 1, 0, false);
    player_run_once(p);
    g_nev = 0;
    for (int i = 0; i < 100; i++) player_cmd_volume_step(p, -1);
    player_run_once(p);
    CHECK_EQ_INT(ev_count(EV_VOLUME), 1);
    CHECK_NEAR(status_of(p).volume_db, PLAYER_VOLUME_MIN_DB, 1e-4);  // -100 dB clamps to the floor
    for (int i = 0; i < 100; i++) player_cmd_set_volume_db(p, -0.5f * (float)i);
    g_nev = 0;
    player_run_once(p);
    CHECK_NEAR(status_of(p).volume_db, -49.5, 1e-4);
    CHECK_EQ_INT(ev_count(EV_VOLUME), 1);
    unsigned flushes = m.flushes;
    uint32_t pos = status_of(p).position_ms;
    for (int i = 0; i < 100; i++) player_cmd_seek_relative_ms(p, 10);
    player_run_once(p);
    CHECK_EQ_INT(m.flushes, flushes + 1);
    uint32_t now = status_of(p).position_ms;
    CHECK(now >= pos + 1000 && now <= pos + 1000 + 200);
    // 200 alternating commands overflow the ring: the rest is dropped, nothing breaks.
    for (int i = 0; i < 200; i++) {
        if (i & 1) {
            player_cmd_resume(p);
        } else {
            player_cmd_pause(p);
        }
    }
    player_run_once(p);
    player_cmd_resume(p);
    run_n(p, 2);
    CHECK(status_of(p).state == PLAYER_PLAYING);
    player_destroy(p);
    mem_sink_free(&m);
}

// Random commands between iterations; invariants after each step.
TEST(random_commands) {
    mem_sink_t a, b;
    mem_sink_init(&a, "wired", SINK_CAP_BITPERFECT | SINK_CAP_HW_VOLUME);
    mem_sink_init(&b, "bt", 0);
    b.fixed_rate = 44100;
    b.fixed_bits = 16;
    player_t *p = new_player(&a);
    uint32_t rng = 42;
    queue_item_t it[NFILES];
    int all[NFILES] = {0, 1, 2, 3, 4, 5, 6, 7};
    fill_items(it, all, NFILES);
    int bad = 0;
    for (int step = 0; step < 3000; step++) {
        uint32_t r = pq_rand(&rng);
        uint32_t cnt = player_queue_count(p);
        switch (r % 24) {
        case 0: player_cmd_play_items(p, it, 1 + (r >> 8) % NFILES, (r >> 16) % NFILES, (r >> 20) & 1); break;
        case 1: player_cmd_enqueue(p, it, 1 + (r >> 8) % 3, (r >> 12) & 1); break;
        case 2: player_cmd_queue_remove(p, cnt ? (r >> 8) % (cnt + 1) : 0); break;
        case 3: player_cmd_queue_jump(p, cnt ? (r >> 8) % cnt : 0); break;
        case 4: player_cmd_next(p); break;
        case 5: player_cmd_prev(p); break;
        case 6: player_cmd_seek_ms(p, (r >> 8) % 400); break;
        case 7: player_cmd_seek_relative_ms(p, (int32_t)((r >> 8) % 400) - 200); break;
        case 8: player_cmd_toggle_pause(p); break;
        case 9: player_cmd_stop(p); break;
        case 10: player_cmd_resume(p); break;
        case 11: player_cmd_set_shuffle(p, (r >> 8) & 1); break;
        case 12: player_cmd_set_repeat(p, (repeat_mode_t)((r >> 8) % 3)); break;
        case 13: player_cmd_volume_step(p, (int)((r >> 8) % 5) - 2); break;
        case 14: player_cmd_set_gapless(p, (r >> 8) & 1); break;
        case 15: {
            dsp_config_t d;
            dsp_config_defaults(&d);
            d.crossfeed_enabled = (r >> 8) & 1;
            d.rg_mode = (rg_mode_t)((r >> 9) % 4);
            player_cmd_set_dsp(p, &d);
            break;
        }
        case 16: player_cmd_set_sink(p, (r >> 8) % 3 == 0 ? NULL : ((r >> 8) & 1) ? &a.sink : &b.sink); break;
        case 17: if ((r >> 8) % 20 == 0) player_cmd_queue_clear(p); break;
        case 18: player_cmd_set_sleep_timer(p, (r >> 8) % 2); break;
        case 19: player_set_chunk_frames(p, 64u << ((r >> 8) % 6)); break;
        default: break;
        }
        int iters = 1 + (int)((r >> 24) % 3);
        for (int i = 0; i < iters; i++) player_run_once(p);
        player_status_t st = status_of(p);
        cnt = player_queue_count(p);
        if (st.queue_length != cnt) bad++;
        if (cnt && st.queue_index >= cnt) bad++;
        if ((unsigned)st.state > PLAYER_PAUSED) bad++;
        if (cnt == 0 && st.state != PLAYER_STOPPED) bad++;
        for (uint32_t i = 0; i < cnt; i++) {
            if (entry_file(p, i) < 0) bad++;
        }
        mem_sink_reset_recording(&a);
        mem_sink_reset_recording(&b);
    }
    CHECK_EQ_INT(bad, 0);
    player_destroy(p);
    mem_sink_free(&a);
    mem_sink_free(&b);
}

// Edits prepared on a copy of the queue by the commanding thread give exactly the queue the
// audio thread's in-place edits give: same entries, order, position and shuffle state. Several
// edits in one batch exercise the fallback (the queue changed since the copy) and a current
// entry that moved between the copy and the swap.
static bool same_queue(player_t *a, player_t *b) {
    player_status_t sa = status_of(a), sb = status_of(b);
    uint32_t n = player_queue_count(a);
    if (n != player_queue_count(b) || sa.queue_index != sb.queue_index || sa.shuffle != sb.shuffle ||
        sa.state != sb.state || strcmp(sa.title, sb.title) != 0) {
        return false;
    }
    queue_item_t x, y;
    for (uint32_t i = 0; i < n; i++) {
        if (player_queue_get(a, i, &x) != CORE_OK || player_queue_get(b, i, &y) != CORE_OK) return false;
        if (strcmp(x.path, y.path) != 0) return false;
    }
    return true;
}

TEST(prepared_edits_equal_in_place) {
    mem_sink_t ma, mb;
    mem_sink_init(&ma, "mem", SINK_CAP_BITPERFECT);
    mem_sink_init(&mb, "mem", SINK_CAP_BITPERFECT);
    ma.discard = mb.discard = true;
    player_t *pa = new_player(&ma), *pb = new_player(&mb);
    player_test_set_prep_min(pa, 0);           // every edit prepared
    player_test_set_prep_min(pb, UINT32_MAX);  // every edit in place
    player_test_set_seed(pa, 77);
    player_test_set_seed(pb, 77);
    enum { N = 48 };
    queue_item_t it[N];
    int idx[N];
    for (int i = 0; i < N; i++) idx[i] = i % NFILES;
    fill_items(it, idx, N);
    player_t *ps[2] = {pa, pb};
    for (int k = 0; k < 2; k++) player_cmd_play_items(ps[k], it, N, 5, false);
    run_n(pa, 1);
    run_n(pb, 1);
    // The prepared path is really taken: the command carries the edited copy.
    player_cmd_queue_remove(pa, 3);
    CHECK(pa->ring_count == 1 && pa->ring[pa->ring_head].ptr != NULL);
    player_cmd_queue_remove(pb, 3);
    CHECK(pb->ring_count == 1 && pb->ring[pb->ring_head].ptr == NULL);
    run_n(pa, 1);
    run_n(pb, 1);
    CHECK(same_queue(pa, pb));
    uint32_t rng = 4242;
    int bad = 0;
    for (int step = 0; step < 1500; step++) {
        const uint32_t r = pq_rand(&rng);
        const int cmds = 1 + (int)((r >> 28) % 3);  // up to three commands before the audio thread runs
        uint32_t rr = r;
        for (int c = 0; c < cmds; c++) {
            rr = pq_rand(&rr);
            const uint32_t cnt = player_queue_count(pa);
            for (int k = 0; k < 2; k++) {
                player_t *p = ps[k];
                switch (rr % 10) {
                case 0:
                    if ((rr >> 8) % 8 == 0) player_cmd_play_items(p, it, 1 + (rr >> 12) % N, (rr >> 20) % N, (rr >> 4) & 1);
                    break;
                case 1: player_cmd_enqueue(p, it + (rr >> 8) % 8, 1 + (rr >> 12) % 3, (rr >> 16) & 1); break;
                case 2:
                case 3: player_cmd_queue_remove(p, cnt ? (rr >> 8) % cnt : 0); break;
                case 4: player_cmd_set_shuffle(p, (rr >> 8) & 1); break;
                case 5: player_cmd_next(p); break;
                case 6: player_cmd_prev(p); break;
                case 7: player_cmd_queue_jump(p, cnt ? (rr >> 8) % cnt : 0); break;
                case 8: player_cmd_set_repeat(p, (repeat_mode_t)((rr >> 8) % 3)); break;
                default: break;
                }
            }
        }
        const int iters = 1 + (int)((r >> 24) % 3);
        for (int i = 0; i < iters; i++) {
            player_run_once(pa);
            player_run_once(pb);
        }
        if (!same_queue(pa, pb)) {
            if (!bad) {
                player_status_t sa = status_of(pa), sb = status_of(pb);
                printf("  queues differ after step %d: %u/%u entries, index %u/%u\n", step,
                       (unsigned)player_queue_count(pa), (unsigned)player_queue_count(pb), (unsigned)sa.queue_index,
                       (unsigned)sb.queue_index);
            }
            bad++;
        }
        if (player_queue_count(pa) < 8) {
            for (int k = 0; k < 2; k++) player_cmd_enqueue(ps[k], it, N, false);  // keep it long
        }
    }
    CHECK_EQ_INT(bad, 0);
    player_destroy(pa);
    player_destroy(pb);
    mem_sink_free(&ma);
    mem_sink_free(&mb);
}

// ------------------------------------------------------------------- real threads ----
#if defined(_WIN32)
static void *lk_create(void) {
    CRITICAL_SECTION *c = malloc(sizeof *c);
    if (c) InitializeCriticalSection(c);
    return c;
}
static void lk_lock(void *m) { EnterCriticalSection((CRITICAL_SECTION *)m); }
static void lk_unlock(void *m) { LeaveCriticalSection((CRITICAL_SECTION *)m); }
static void lk_destroy(void *m) {
    DeleteCriticalSection((CRITICAL_SECTION *)m);
    free(m);
}
#else
static void *lk_create(void) {
    pthread_mutex_t *m = malloc(sizeof *m);
    if (m) pthread_mutex_init(m, NULL);
    return m;
}
static void lk_lock(void *m) { pthread_mutex_lock((pthread_mutex_t *)m); }
static void lk_unlock(void *m) { pthread_mutex_unlock((pthread_mutex_t *)m); }
static void lk_destroy(void *m) {
    pthread_mutex_destroy((pthread_mutex_t *)m);
    free(m);
}
#endif

typedef struct {
    player_t *p;
    volatile int stop;
    volatile long iterations;
} audio_ctx_t;

#if defined(_WIN32)
static DWORD WINAPI audio_thread(LPVOID arg) {
#else
static void *audio_thread(void *arg) {
#endif
    audio_ctx_t *c = arg;
    while (!c->stop) {
        player_run_once(c->p);
        c->iterations++;
    }
    return 0;
}

static volatile long g_events;
static void count_events(const core_event_t *ev, void *user) {
    (void)ev;
    (void)user;
#if defined(_WIN32)
    InterlockedIncrement(&g_events);
#else
    __atomic_add_fetch(&g_events, 1, __ATOMIC_RELAXED);
#endif
}

TEST(threads_smoke) {
    static const core_lock_ops_t ops = {lk_create, lk_lock, lk_unlock, lk_destroy};
    core_set_lock_ops(&ops);
    core_events_set_sink(count_events, NULL);
    mem_sink_t m;
    mem_sink_init(&m, "mem", SINK_CAP_BITPERFECT);
    m.discard = true;  // the audio thread may write a lot: count only
    player_t *p = new_player(&m);
    audio_ctx_t ctx = {p, 0, 0};
    queue_item_t it[NFILES];
    int all[NFILES] = {0, 1, 2, 3, 4, 5, 6, 7};
    fill_items(it, all, NFILES);
    player_cmd_set_repeat(p, REPEAT_ALL);
#if defined(_WIN32)
    HANDLE th = CreateThread(NULL, 0, audio_thread, &ctx, 0, NULL);
    CHECK(th != NULL);
#else
    pthread_t th;
    CHECK(pthread_create(&th, NULL, audio_thread, &ctx) == 0);
#endif
    uint32_t rng = 7;
    int bad = 0;
    // At least 20000 commands, and until the audio thread has run a good while alongside.
    for (int i = 0; i < 2000000 && (i < 20000 || ctx.iterations < 3000); i++) {
        if ((i & 63) == 0) {
#if defined(_WIN32)
            Sleep(0);
#else
            sched_yield();
#endif
        }
        uint32_t r = pq_rand(&rng);
        switch (r % 10) {
        case 0: player_cmd_play_items(p, it, 1 + (r >> 8) % NFILES, 0, (r >> 12) & 1); break;
        case 1: player_cmd_enqueue(p, it, 2, (r >> 8) & 1); break;
        case 2: player_cmd_next(p); break;
        case 3: player_cmd_seek_ms(p, (r >> 8) % 200); break;
        case 4: player_cmd_volume_step(p, (r >> 8) & 1 ? 1 : -1); break;
        case 5: player_cmd_set_shuffle(p, (r >> 8) & 1); break;
        case 6: player_cmd_queue_remove(p, (r >> 8) % 4); break;
        default: break;
        }
        player_status_t st;
        player_get_status(p, &st);
        if (st.queue_length && st.queue_index >= st.queue_length) bad++;
        queue_item_t q;
        uint32_t n = player_queue_count(p);
        if (n) player_queue_get(p, (r >> 4) % n, &q);
    }
    ctx.stop = 1;
#if defined(_WIN32)
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
#else
    pthread_join(th, NULL);
#endif
    CHECK_EQ_INT(bad, 0);
    CHECK(ctx.iterations >= 3000);
    CHECK(g_events > 0);
    player_destroy(p);
    mem_sink_free(&m);
    core_events_set_sink(ev_sink, NULL);
    core_set_lock_ops(NULL);
}

TEST_MAIN(make_files(); core_events_set_sink(ev_sink, NULL);
          RUN(queue_ops) RUN(queue_capacity) RUN(shuffle_determinism) RUN(repeat_modes) RUN(prev_logic)
              RUN(command_merging_and_overflow) RUN(random_commands) RUN(prepared_edits_equal_in_place)
                  RUN(threads_smoke))
