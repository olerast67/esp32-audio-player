// SPDX-License-Identifier: Apache-2.0
// Persistence and scrobbling:
// - save/restore round trip: queue (with a CUE range) in shuffled order and its original
//   order, index, position, volume, repeat; restored paused at the position (or playing)
// - library entries are saved as paths and resolved back to ids (with and without the index)
// - library ids in the queue, the status and a save follow a rescan that renumbers them
// - a 20 000-entry queue, missing state, recovery of an interrupted replace
// - an unchanged queue is not rewritten (only player.ini), an edited or deleted one is
// - Rockbox .scrobbler.log: header, L/S rating, timestamps; deferred while the clock is unset
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define mkdir_(p) _mkdir(p)
#define rmdir_(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_(p) mkdir(p, 0775)
#define rmdir_(p) rmdir(p)
#endif

#include "support/player_sinks.h"
#include "audio_player/events.h"
#include "audio_player/library.h"
#include "audio_player/player.h"
#include "player/player_priv.h"
#include "test.h"

#define TMP "tmp_player_state"
#define RATE 8000u
#define LEN 40000u  // 5 s

// ---------------------------------------------------------------------------- helpers ----
static uint32_t g_now = 10000;
static uint32_t fake_clock(void) { return g_now; }
static int64_t g_wall;
static int64_t wall_clock(void) { return g_wall; }

static char g_files[6][96];

static int32_t ramp_gen(uint64_t i, unsigned ch, void *user) {
    (void)ch;
    return (int32_t)((i * 5 + (uint64_t)(intptr_t)user) % 60000) - 30000;
}

static int32_t tone_gen(uint64_t i, unsigned ch, void *user) {
    (void)ch;
    (void)user;
    return (int32_t)lrint(8000.0 * sin(2.0 * 3.14159265358979 * 440.0 * (double)i / RATE));
}

static void make_files(void) {
    mkdir_(TMP);
    for (int i = 0; i < 6; i++) {
        snprintf(g_files[i], sizeof g_files[i], TMP "/f%d.wav", i);
        wavgen_write(g_files[i], RATE, 1, 16, LEN, ramp_gen, (void *)(intptr_t)(i * 4999), NULL);
    }
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) n = 0;
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

static player_t *new_player(mem_sink_t *m, const char *state_dir, library_t *lib, bool scrobble) {
    player_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.sink = m ? &m->sink : NULL;
    cfg.state_dir = state_dir;
    cfg.library = lib;
    cfg.scrobble_log = scrobble;
    cfg.volume_default_db = 0.0f;
    cfg.volume_max_db = 0.0f;
    return player_create(&cfg);
}

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

static bool same_item(player_t *a, player_t *b, uint32_t i) {
    queue_item_t x, y;
    if (player_queue_get(a, i, &x) != CORE_OK || player_queue_get(b, i, &y) != CORE_OK) return false;
    return strcmp(x.path, y.path) == 0 && x.start_ms == y.start_ms && x.end_ms == y.end_ms;
}

static void rm(const char *dir, const char *name) {
    char p[256];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    remove(p);
    snprintf(p, sizeof p, "%s/%s.tmp", dir, name);
    remove(p);
}

// ------------------------------------------------------------------------------ tests ----
TEST(save_restore_roundtrip) {
    const char *dir = TMP "/state1";
    rm(dir, "queue.m3u8");
    rm(dir, "player.ini");
    mem_sink_t m1, m2;
    mem_sink_init(&m1, "mem", SINK_CAP_BITPERFECT);
    mem_sink_init(&m2, "mem", SINK_CAP_BITPERFECT | SINK_CAP_HW_VOLUME);  // samples stay untouched
    player_t *p1 = new_player(&m1, dir, NULL, false);
    queue_item_t it[6];
    memset(it, 0, sizeof it);
    for (int i = 0; i < 6; i++) {
        it[i].track_id = LIB_ID_NONE;
        core_strlcpy(it[i].path, g_files[i], sizeof it[i].path);
    }
    it[5].start_ms = 1000;  // a CUE-style range
    it[5].end_ms = 4000;
    player_test_set_seed(p1, 77);
    player_cmd_play_items(p1, it, 6, 1, true);
    player_cmd_set_repeat(p1, REPEAT_ALL);
    player_cmd_set_volume_db(p1, -12.0f);
    player_cmd_queue_jump(p1, 3);
    run_n(p1, 2);
    player_cmd_seek_ms(p1, 1234);
    player_cmd_pause(p1);
    player_run_once(p1);
    player_status_t s1 = status_of(p1);
    CHECK(s1.state == PLAYER_PAUSED);
    CHECK_EQ_INT(s1.position_ms, 1234);
    CHECK_EQ_INT(player_save_state(p1), CORE_OK);
    char *ini = read_file(TMP "/state1/player.ini");
    CHECK(ini && strstr(ini, "index = 3\n"));
    CHECK(ini && strstr(ini, "position_ms = 1234\n"));
    CHECK(ini && strstr(ini, "shuffle = 1\n"));
    CHECK(ini && strstr(ini, "repeat = all\n"));
    CHECK(ini && strstr(ini, "volume_db = -12.00\n"));
    free(ini);
    char *q = read_file(TMP "/state1/queue.m3u8");
    CHECK(q && strncmp(q, "#EXTM3U\n", 8) == 0);
    CHECK(q && strstr(q, ",1000,4000,-1\n"));
    free(q);

    player_t *p2 = new_player(&m2, dir, NULL, false);
    CHECK_EQ_INT(player_restore_state(p2, false), CORE_OK);
    player_run_once(p2);
    player_status_t s2 = status_of(p2);
    CHECK(s2.state == PLAYER_PAUSED);
    CHECK_EQ_INT(s2.queue_length, 6);
    CHECK_EQ_INT(s2.queue_index, 3);
    CHECK_EQ_INT(s2.position_ms, 1234);
    CHECK_NEAR(s2.volume_db, -12.0, 1e-4);
    CHECK(s2.shuffle);
    CHECK(s2.repeat == REPEAT_ALL);
    CHECK_STR(s2.path, s1.path);
    CHECK_EQ_INT(m2.frames, 0);  // paused: nothing written yet
    for (uint32_t i = 0; i < 6; i++) CHECK(same_item(p1, p2, i));
    // The original (unshuffled) order survives too.
    player_cmd_set_shuffle(p1, false);
    player_cmd_set_shuffle(p2, false);
    player_run_once(p1);
    player_run_once(p2);
    for (uint32_t i = 0; i < 6; i++) CHECK(same_item(p1, p2, i));
    queue_item_t first;
    player_queue_get(p2, 0, &first);
    CHECK_STR(first.path, g_files[0]);
    CHECK_EQ_INT(status_of(p2).queue_index, status_of(p1).queue_index);
    // Resume plays from the saved position.
    player_cmd_resume(p2);
    player_run_once(p2);
    CHECK(status_of(p2).state == PLAYER_PLAYING);
    uint64_t n = 0;
    int32_t *ref = decode_all(s1.path, &n, NULL);
    uint64_t at = 1234u * RATE / 1000u;
    bool same = ref && m2.frames >= 512;
    for (uint64_t i = 0; same && i < 512; i++) same = m2.pcm[i * 2] == ref[at + i];
    if (!same && ref) {
        long found = -1;
        for (uint64_t k = 0; k + 4 < n && found < 0; k++) {
            if (ref[k] == m2.pcm[0] && ref[k + 1] == m2.pcm[2] && ref[k + 2] == m2.pcm[4]) found = (long)k;
        }
        printf("  resumed at frame %ld, expected %llu (recorded %llu)\n", found, (unsigned long long)at,
               (unsigned long long)m2.frames);
    }
    CHECK(same);
    CHECK_NEAR(m2.volume_db, -12.0, 1e-4);
    free(ref);
    player_destroy(p1);
    player_destroy(p2);

    // Autoplay starts right away.
    mem_sink_reset_recording(&m2);
    p2 = new_player(&m2, dir, NULL, false);
    CHECK_EQ_INT(player_restore_state(p2, true), CORE_OK);
    player_run_once(p2);
    CHECK(status_of(p2).state == PLAYER_PLAYING);
    CHECK(m2.frames > 0);
    player_destroy(p2);
    mem_sink_free(&m1);
    mem_sink_free(&m2);
}

TEST(missing_and_recovered_state) {
    player_t *p = new_player(NULL, TMP "/nothing_here", NULL, false);
    CHECK_EQ_INT(player_restore_state(p, false), CORE_ENOTFOUND);
    player_destroy(p);
    p = new_player(NULL, NULL, NULL, false);
    CHECK_EQ_INT(player_restore_state(p, false), CORE_EINVAL);
    CHECK_EQ_INT(player_save_state(p), CORE_EINVAL);
    player_destroy(p);
    CHECK_EQ_INT(player_restore_state(NULL, false), CORE_EINVAL);

    // An interrupted replace leaves only the complete .tmp file: it is used.
    const char *dir = TMP "/state2";
    mkdir_(dir);
    rm(dir, "queue.m3u8");
    rm(dir, "player.ini");
    p = new_player(NULL, dir, NULL, false);
    queue_item_t it[2];
    memset(it, 0, sizeof it);
    for (int i = 0; i < 2; i++) {
        it[i].track_id = LIB_ID_NONE;
        core_strlcpy(it[i].path, g_files[i], sizeof it[i].path);
    }
    player_cmd_play_items(p, it, 2, 1, false);
    player_cmd_pause(p);
    player_run_once(p);
    CHECK_EQ_INT(player_save_state(p), CORE_OK);
    player_destroy(p);
    CHECK(rename(TMP "/state2/player.ini", TMP "/state2/player.ini.tmp") == 0);
    p = new_player(NULL, dir, NULL, false);
    CHECK_EQ_INT(player_restore_state(p, false), CORE_OK);
    player_run_once(p);
    CHECK_EQ_INT(status_of(p).queue_index, 1);
    CHECK_EQ_INT(status_of(p).queue_length, 2);
    player_destroy(p);
    FILE *f = fopen(TMP "/state2/player.ini", "rb");
    CHECK(f != NULL);  // renamed back into place
    if (f) fclose(f);

    // A hand-written playlist without our comments restores in its order.
    FILE *w = fopen(TMP "/state2/queue.m3u8", "wb");
    if (w) {
        fprintf(w, "#EXTM3U\n#EXTINF:5,Someone - Something\nf2.wav\n\n../f3.wav\n");
        fclose(w);
    }
    p = new_player(NULL, dir, NULL, false);
    CHECK_EQ_INT(player_restore_state(p, false), CORE_OK);
    player_run_once(p);
    queue_item_t qi;
    CHECK_EQ_INT(player_queue_count(p), 2);
    CHECK_EQ_INT(player_queue_get(p, 0, &qi), CORE_OK);
    CHECK_STR(qi.path, TMP "/state2/f2.wav");
    player_destroy(p);
}

// Periodic saves and track changes rewrite only player.ini: queue.m3u8 is written again
// after a queue edit or when the file is gone.
TEST(unchanged_queue_not_rewritten) {
    const char *dir = TMP "/state6";
    mkdir_(dir);
    rm(dir, "queue.m3u8");
    rm(dir, "player.ini");
    const char *qpath = TMP "/state6/queue.m3u8";
    const char *ipath = TMP "/state6/player.ini";
    player_t *p = new_player(NULL, dir, NULL, false);
    queue_item_t it[3];
    memset(it, 0, sizeof it);
    for (int i = 0; i < 3; i++) {
        it[i].track_id = LIB_ID_NONE;
        core_strlcpy(it[i].path, g_files[i], sizeof it[i].path);
    }
    player_cmd_play_items(p, it, 3, 0, false);
    player_cmd_pause(p);
    player_run_once(p);
    CHECK_EQ_INT(player_save_state(p), CORE_OK);

    // A marker at the end of the file survives a save that does not rewrite it.
    FILE *w = fopen(qpath, "ab");
    CHECK(w != NULL);
    if (w) {
        fputs("#MARK\n", w);
        fclose(w);
    }
    player_cmd_next(p);
    player_run_once(p);
    CHECK_EQ_INT(status_of(p).queue_index, 1);
    CHECK_EQ_INT(player_save_state(p), CORE_OK);
    char *s = read_file(qpath);
    CHECK(s && strstr(s, "#MARK") != NULL);
    free(s);
    s = read_file(ipath);
    CHECK(s && strstr(s, "index = 1\n") != NULL && strstr(s, "count = 3\n") != NULL);
    free(s);

    // A queue edit rewrites it.
    player_cmd_enqueue(p, it, 1, false);
    player_run_once(p);
    CHECK_EQ_INT(player_queue_count(p), 4);
    CHECK_EQ_INT(player_save_state(p), CORE_OK);
    s = read_file(qpath);
    CHECK(s && strstr(s, "#MARK") == NULL);
    free(s);

    // A file deleted behind the player's back is written again without an edit.
    remove(qpath);
    CHECK_EQ_INT(player_save_state(p), CORE_OK);
    s = read_file(qpath);
    CHECK(s != NULL);
    free(s);
    player_destroy(p);

    p = new_player(NULL, dir, NULL, false);
    CHECK_EQ_INT(player_restore_state(p, false), CORE_OK);
    player_run_once(p);
    CHECK_EQ_INT(status_of(p).queue_index, 1);
    CHECK_EQ_INT(status_of(p).queue_length, 4);
    queue_item_t qi;
    CHECK_EQ_INT(player_queue_get(p, 3, &qi), CORE_OK);
    CHECK_STR(qi.path, g_files[0]);
    player_destroy(p);
}

TEST(large_queue) {
    const char *dir = TMP "/state3";
    player_t *p = new_player(NULL, dir, NULL, false);
    uint32_t n = PLAYER_QUEUE_MAX;
    queue_item_t *it = calloc(n, sizeof *it);
    for (uint32_t i = 0; i < n; i++) {
        it[i].track_id = LIB_ID_NONE;
        snprintf(it[i].path, sizeof it[i].path, "/sdcard/Музыка/Исполнитель %u/Альбом/%05u трек.flac", i / 12, i);
    }
    player_test_set_seed(p, 5);
    player_cmd_play_items(p, it, n, 12345, true);
    player_cmd_pause(p);
    player_run_once(p);
    CHECK_EQ_INT(player_queue_count(p), n);
    CHECK_EQ_INT(player_save_state(p), CORE_OK);
    player_t *r = new_player(NULL, dir, NULL, false);
    CHECK_EQ_INT(player_restore_state(r, false), CORE_OK);
    player_cmd_stop(r);
    player_run_once(r);
    CHECK_EQ_INT(player_queue_count(r), n);
    int bad = 0;
    for (uint32_t i = 0; i < n; i += 97) bad += !same_item(p, r, i);
    CHECK_EQ_INT(bad, 0);
    player_cmd_set_shuffle(r, false);
    player_run_once(r);
    queue_item_t q;
    player_queue_get(r, 777, &q);
    CHECK_STR(q.path, it[777].path);
    free(it);
    player_destroy(p);
    player_destroy(r);
}

TEST(library_ids_roundtrip) {
    mkdir_(TMP "/lib");
    mkdir_(TMP "/lib/music");
    static const char *titles[3] = {"Alpha", "Beta", "Gamma"};
    char paths[3][96];
    for (int i = 0; i < 3; i++) {
        snprintf(paths[i], sizeof paths[i], TMP "/lib/music/%d.wav", i + 1);
        char track[8];
        snprintf(track, sizeof track, "%d", i + 1);
        wavgen_tags_t tags = {titles[i], "Band", "Disc", track};
        wavgen_write(paths[i], RATE, 1, 16, 4000, tone_gen, NULL, &tags);
    }
    library_config_t lc = {TMP "/lib/music", TMP "/lib/db", false, NULL, NULL};
    library_t *lib = library_open(&lc);
    CHECK(lib != NULL);
    if (!lib) return;
    CHECK_EQ_INT(library_scan(lib, true, NULL, NULL), CORE_OK);
    CHECK_EQ_INT(library_track_count(lib), 3);
    lib_id_t ids[3];
    CHECK_EQ_INT(library_view_track_ids(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, ids, 3), 3);

    const char *dir = TMP "/state4";
    mem_sink_t m;
    mem_sink_init(&m, "mem", SINK_CAP_BITPERFECT);
    player_t *p = new_player(&m, dir, lib, false);
    player_cmd_play_tracks(p, ids, 3, 1, false);
    // A path entry of an indexed file becomes an id entry after restore.
    queue_item_t extra;
    memset(&extra, 0, sizeof extra);
    extra.track_id = LIB_ID_NONE;
    core_strlcpy(extra.path, paths[0], sizeof extra.path);
    player_cmd_enqueue(p, &extra, 1, false);
    player_run_once(p);
    player_status_t st = status_of(p);
    CHECK_EQ_INT(st.track_id, ids[1]);
    CHECK_STR(st.title, "Beta");
    CHECK_STR(st.artist, "Band");
    queue_item_t q;
    CHECK_EQ_INT(player_queue_get(p, 1, &q), CORE_OK);
    CHECK_EQ_INT(q.track_id, ids[1]);
    CHECK(q.path[0] != 0);  // filled in from the library
    player_cmd_pause(p);
    player_run_once(p);
    CHECK_EQ_INT(player_save_state(p), CORE_OK);
    player_destroy(p);

    p = new_player(&m, dir, lib, false);
    CHECK_EQ_INT(player_restore_state(p, false), CORE_OK);
    player_run_once(p);
    CHECK_EQ_INT(player_queue_count(p), 4);
    for (uint32_t i = 0; i < 3; i++) {
        CHECK_EQ_INT(player_queue_get(p, i, &q), CORE_OK);
        CHECK_EQ_INT(q.track_id, ids[i]);
    }
    CHECK_EQ_INT(player_queue_get(p, 3, &q), CORE_OK);
    CHECK_EQ_INT(q.track_id, library_find_path(lib, paths[0]));
    CHECK(q.track_id != LIB_ID_NONE);
    st = status_of(p);
    CHECK_STR(st.title, "Beta");
    CHECK_EQ_INT(st.queue_index, 1);
    player_destroy(p);

    // Without the index the same state plays by path.
    p = new_player(&m, dir, NULL, false);
    CHECK_EQ_INT(player_restore_state(p, false), CORE_OK);
    player_run_once(p);
    CHECK_EQ_INT(player_queue_get(p, 1, &q), CORE_OK);
    CHECK_EQ_INT(q.track_id, LIB_ID_NONE);
    CHECK(strstr(q.path, "2.wav") != NULL);
    st = status_of(p);
    CHECK_STR(st.title, "Beta");  // tags read from the file
    player_destroy(p);
    library_close(lib);
    mem_sink_free(&m);
}

// A rescan renumbers the library ids (scan order): files added in a folder that sorts first
// move every id after them. The queue, the status, a save and "next" follow the files.
TEST(queue_follows_rescan) {
    mkdir_(TMP "/lib2");
    mkdir_(TMP "/lib2/music");
    mkdir_(TMP "/lib2/music/b");
    for (int v = 0; v < 2; v++) {  // albums "added" by an earlier run
        char f[96];
        for (int k = 0; k < 2; k++) {
            snprintf(f, sizeof f, TMP "/lib2/music/a%d/%d.wav", v, k);
            remove(f);
        }
        snprintf(f, sizeof f, TMP "/lib2/music/a%d", v);
        rmdir_(f);
    }
    static const char *titles[3] = {"Alpha", "Beta", "Gamma"};
    char paths[3][96];
    for (int i = 0; i < 3; i++) {
        snprintf(paths[i], sizeof paths[i], TMP "/lib2/music/b/%d.wav", i + 1);
        char track[8];
        snprintf(track, sizeof track, "%d", i + 1);
        wavgen_tags_t tags = {titles[i], "Band", "Disc", track};
        wavgen_write(paths[i], RATE, 1, 16, 4000, tone_gen, NULL, &tags);
    }
    library_config_t lc = {TMP "/lib2/music", TMP "/lib2/db", false, NULL, NULL};
    library_t *lib = library_open(&lc);
    CHECK(lib != NULL);
    if (!lib) return;
    CHECK_EQ_INT(library_scan(lib, true, NULL, NULL), CORE_OK);
    // 1500 entries: the translation takes several loop iterations (512 per slice).
    enum { N = 1500 };
    static lib_id_t many[N];

    for (int variant = 0; variant < 2; variant++) {
        lib_id_t ids[3];
        for (int i = 0; i < 3; i++) ids[i] = library_find_path(lib, paths[i]);
        for (int i = 0; i < N; i++) many[i] = ids[i % 3];
        const char *dir = variant ? TMP "/state6" : TMP "/state5";
        mem_sink_t m;
        mem_sink_init(&m, "mem", SINK_CAP_BITPERFECT);
        m.discard = true;
        player_t *p = new_player(&m, dir, lib, false);
        player_cmd_play_tracks(p, many, N, 0, false);
        player_cmd_pause(p);  // the sink takes everything at once: stay on the first entry
        player_run_once(p);
        player_status_t st = status_of(p);
        CHECK_STR(st.title, "Alpha");
        // New album in a folder scanned before "b".
        char np[2][96];
        for (int k = 0; k < 2; k++) {
            snprintf(np[k], sizeof np[k], TMP "/lib2/music/a%d/%d.wav", variant, k);
            char d[96];
            snprintf(d, sizeof d, TMP "/lib2/music/a%d", variant);
            mkdir_(d);
            wavgen_tags_t tags = {k ? "New B" : "New A", "Other", "New", "1"};
            wavgen_write(np[k], RATE, 1, 16, 4000, tone_gen, NULL, &tags);
        }
        CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_OK);
        CHECK(library_find_path(lib, paths[0]) != ids[0]);  // the ids really moved
        if (variant == 1) {
            // Saved before the audio loop translated anything: the file still names the files.
            CHECK_EQ_INT(player_save_state(p), CORE_OK);
            char *q = read_file(TMP "/state6/queue.m3u8");
            CHECK(q && strstr(q, "b/1.wav") && strstr(q, "b/2.wav") && !strstr(q, "/a1/"));
            free(q);
            // An entry added while the translation runs.
            run_n(p, 1);
            queue_item_t add;
            memset(&add, 0, sizeof add);
            add.track_id = library_find_path(lib, paths[2]);
            player_cmd_enqueue(p, &add, 1, true);
        }
        run_n(p, 8);
        const uint32_t count = player_queue_count(p);
        CHECK_EQ_INT(count, N + (uint32_t)variant);
        int bad = 0;
        for (uint32_t i = 0; i < count; i++) {
            queue_item_t q;
            const char *want = variant && i == 1 ? paths[2] : paths[(i - (variant && i > 1 ? 1 : 0)) % 3];
            if (player_queue_get(p, i, &q) != CORE_OK || strcmp(q.path, want) != 0 ||
                q.track_id != library_find_path(lib, want)) {
                if (!bad) {
                    printf("  entry %u: %s (id %u), want %s\n", (unsigned)i, q.path, (unsigned)q.track_id, want);
                }
                bad++;
            }
        }
        CHECK_EQ_INT(bad, 0);
        st = status_of(p);
        CHECK_STR(st.title, "Alpha");
        CHECK_EQ_INT(st.track_id, library_find_path(lib, paths[0]));
        player_cmd_next(p);
        run_n(p, 2);
        st = status_of(p);
        CHECK_STR(st.title, variant ? "Gamma" : "Beta");
        CHECK_EQ_INT(st.track_id, library_find_path(lib, st.path));
        // Saved and restored after the rescan: the same files.
        player_cmd_pause(p);
        player_run_once(p);
        CHECK_EQ_INT(player_save_state(p), CORE_OK);
        player_destroy(p);
        p = new_player(&m, dir, lib, false);
        CHECK_EQ_INT(player_restore_state(p, false), CORE_OK);
        player_run_once(p);
        CHECK_EQ_INT(player_queue_count(p), count);
        queue_item_t q;
        CHECK_EQ_INT(player_queue_get(p, 0, &q), CORE_OK);
        CHECK_STR(q.path, paths[0]);
        st = status_of(p);
        CHECK_STR(st.title, variant ? "Gamma" : "Beta");
        player_destroy(p);
        mem_sink_free(&m);
    }
    library_close(lib);
}

TEST(scrobble_log) {
    core_set_clock(fake_clock);
    const char *dir = TMP "/scrobble";
    mkdir_(dir);
    rm(dir, ".scrobbler.log");
    wavgen_tags_t tags = {"Song", "Band\tX", "Disc", "3"};
    CHECK(wavgen_write(TMP "/song.wav", RATE, 1, 16, 8000, tone_gen, NULL, &tags));
    mem_sink_t m;
    mem_sink_init(&m, "mem", SINK_CAP_BITPERFECT);
    m.discard = true;
    player_t *p = new_player(&m, dir, NULL, true);
    player_test_set_wall_clock(p, wall_clock);
    g_wall = 1700000000;
    const char *song = TMP "/song.wav";
    queue_item_t it[2];
    memset(it, 0, sizeof it);
    it[0].track_id = it[1].track_id = LIB_ID_NONE;
    core_strlcpy(it[0].path, song, sizeof it[0].path);
    core_strlcpy(it[1].path, g_files[0], sizeof it[1].path);
    player_cmd_play_items(p, it, 1, 0, false);
    run_to_stop(p, 1000);
    char *log = read_file(TMP "/scrobble/.scrobbler.log");
    CHECK(log != NULL);
    CHECK(log && strncmp(log, "#AUDIOSCROBBLER/1.1\n#TZ/UTC\n#CLIENT/esp32-audio-player ", 55) == 0);
    CHECK(log && strstr(log, "\nBand X\tDisc\tSong\t3\t1\tL\t1700000000\t\n") != NULL);
    free(log);
    // Skipped after 0.6 s of a 5 s track: rated S; the header is written once.
    g_wall = 1700000500;
    player_cmd_play_items(p, &it[1], 1, 0, false);
    run_n(p, 5);
    player_cmd_stop(p);
    player_run_once(p);
    log = read_file(TMP "/scrobble/.scrobbler.log");
    CHECK(log && strstr(log, "\tf0\t\t5\tS\t1700000500\t\n") != NULL);
    int headers = 0;
    for (const char *s = log; s && (s = strstr(s, "#AUDIOSCROBBLER")) != NULL; s++) headers++;
    CHECK_EQ_INT(headers, 1);
    free(log);
    player_destroy(p);

    // Clock not set: kept in RAM, written with the start time once the clock is valid.
    rm(dir, ".scrobbler.log");
    p = new_player(&m, dir, NULL, true);
    player_test_set_wall_clock(p, wall_clock);
    g_wall = 0;
    g_now = 50000;
    player_cmd_play_items(p, it, 1, 0, false);
    run_to_stop(p, 1000);
    FILE *f = fopen(TMP "/scrobble/.scrobbler.log", "rb");
    CHECK(f == NULL);
    if (f) fclose(f);
    g_now += 7000;
    g_wall = 1700001000;
    run_n(p, 2);
    log = read_file(TMP "/scrobble/.scrobbler.log");
    CHECK(log && strstr(log, "\tSong\t3\t1\tL\t1700000993\t\n") != NULL);
    free(log);
    player_destroy(p);

    // Scrobbling off: no file.
    rm(dir, ".scrobbler.log");
    p = new_player(&m, dir, NULL, false);
    player_cmd_play_items(p, it, 1, 0, false);
    run_to_stop(p, 1000);
    f = fopen(TMP "/scrobble/.scrobbler.log", "rb");
    CHECK(f == NULL);
    if (f) fclose(f);
    player_destroy(p);
    mem_sink_free(&m);
    core_set_clock(NULL);
}

TEST_MAIN(make_files(); RUN(save_restore_roundtrip) RUN(missing_and_recovered_state) RUN(unchanged_queue_not_rewritten)
              RUN(large_queue)
              RUN(library_ids_roundtrip) RUN(queue_follows_rescan) RUN(scrobble_log))
