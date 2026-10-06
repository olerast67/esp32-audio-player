// SPDX-License-Identifier: Apache-2.0
// Playback engine through a memory sink:
// - every lossless test vector reaches the sink bit-exact with the DSP bypassed (CRC of
//   vectors.txt), mono is upmixed to identical channels, status reports bit-perfect
// - gapless: the FLAC pair plays as the continuous reference, the MP3 pair keeps its length
// - CUE virtual tracks: one decoder, continuous output, titles from the sheet, one track alone
// - resampling to a 44.1 kHz / 16-bit (Bluetooth-like) sink keeps the tone frequency
// - DoP passes untouched to a DoP sink and is refused (error, skip) by others
// - seek accuracy (absolute, relative, inside a CUE track), pause/resume without loss,
//   partial and refused writes never drop samples, sink change continues at the audible frame
// - volume: limit, steps, software vs hardware volume; sleep timer fade and pause
// - unplayable entries are skipped and playback stops after 10 in a row
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define mkdir_(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mkdir_(p) mkdir(p, 0775)
#endif

#include "support/player_sinks.h"
#include "audio_player/events.h"
#include "audio_player/player.h"
#include "audio_player/playlist.h"
#include "player/player_priv.h"
#include "test.h"

#define TMP "tmp_player_engine"

// ---------------------------------------------------------------------------- helpers ----
static core_event_t g_ev[8192];
static int g_nev;
static player_t *g_evp;
static char g_titles[64][TAG_TEXT_MAX];
static int g_ntitles;

static void ev_sink(const core_event_t *ev, void *user) {
    (void)user;
    if (g_nev < (int)CORE_ARRAY_SIZE(g_ev)) g_ev[g_nev++] = *ev;
    if (ev->type == EV_TRACK_CHANGED && g_evp && g_ntitles < 64) {
        player_status_t st;
        player_get_status(g_evp, &st);
        core_strlcpy(g_titles[g_ntitles++], st.title, TAG_TEXT_MAX);
    }
}

static void ev_reset(void) {
    g_nev = 0;
    g_ntitles = 0;
}

static int ev_count(core_event_type_t t) {
    int n = 0;
    for (int i = 0; i < g_nev; i++) n += g_ev[i].type == t;
    return n;
}

static const core_event_t *ev_last(core_event_type_t t) {
    for (int i = g_nev - 1; i >= 0; i--) {
        if (g_ev[i].type == t) return &g_ev[i];
    }
    return NULL;
}

static uint32_t g_now;
static uint32_t fake_clock(void) { return g_now; }

static const char *data_path(const char *name) {
    static char buf[8][512];
    static int slot;
    slot = (slot + 1) % 8;
    snprintf(buf[slot], sizeof buf[slot], "%s/%s", TEST_DATA_DIR, name);
    return buf[slot];
}

static player_t *new_player(mem_sink_t *m, float vol) {
    player_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.sink = m ? &m->sink : NULL;
    cfg.volume_default_db = vol;
    cfg.volume_max_db = 0.0f;
    cfg.volume_step_db = 1.0f;
    player_t *p = player_create(&cfg);
    g_evp = p;
    ev_reset();
    return p;
}

static void free_player(player_t *p) {
    if (g_evp == p) g_evp = NULL;
    player_destroy(p);
}

static void play_paths(player_t *p, const char *const *paths, uint32_t n, uint32_t start) {
    queue_item_t *it = calloc(n, sizeof *it);
    for (uint32_t i = 0; i < n; i++) {
        it[i].track_id = LIB_ID_NONE;
        core_strlcpy(it[i].path, paths[i], sizeof it[i].path);
    }
    player_cmd_play_items(p, it, n, start, false);
    free(it);
}

static void play_one(player_t *p, const char *path) { play_paths(p, &path, 1, 0); }

static player_status_t status_of(player_t *p) {
    player_status_t st;
    player_get_status(p, &st);
    return st;
}

static int run_to_stop(player_t *p, int max_iter) {
    int i;
    for (i = 0; i < max_iter; i++) {
        player_run_once(p);
        if (status_of(p).state == PLAYER_STOPPED) break;
    }
    return i;
}

static void run_n(player_t *p, int n) {
    for (int i = 0; i < n; i++) player_run_once(p);
}

static int32_t sine16(uint64_t i, unsigned ch, void *user) {
    double f = user ? *(double *)user : 1000.0;
    (void)ch;
    return (int32_t)lrint(16384.0 * sin(2.0 * 3.14159265358979 * f * (double)i / 8000.0));
}

static int32_t ramp16(uint64_t i, unsigned ch, void *user) {
    (void)user;
    return (int32_t)((i * 7 + ch * 3) % 60000) - 30000;
}

static bool copy_file(const char *from, const char *to) {
    FILE *f = fopen(from, "rb");
    if (!f) return false;
    FILE *t = fopen(to, "wb");
    if (!t) {
        fclose(f);
        return false;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, t);
    fclose(f);
    fclose(t);
    return true;
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static double peak_db(const int32_t *pcm, uint64_t samples) {
    double pk = 0;
    for (uint64_t i = 0; i < samples; i++) {
        double v = fabs((double)pcm[i]);
        if (v > pk) pk = v;
    }
    return 20.0 * log10(pk / 2147483648.0 + 1e-12);
}

// Frequency of channel c from the zero crossings between frames skip and frames - skip.
static double tone_freq(const int32_t *pcm, uint64_t frames, unsigned ch, unsigned c, uint32_t rate, uint64_t skip) {
    double first = -1, last = -1;
    int count = 0;
    for (uint64_t i = skip + 1; i + skip < frames; i++) {
        double a = pcm[(i - 1) * ch + c], b = pcm[i * ch + c];
        if ((a < 0 && b >= 0) || (a >= 0 && b < 0)) {
            double t = (double)(i - 1) + a / (a - b);
            if (first < 0) first = t;
            last = t;
            count++;
        }
    }
    if (count < 3) return 0;
    return (count - 1) / 2.0 / ((last - first) / rate);
}

typedef struct {
    char name[64];
    unsigned rate, channels, bits, dop;
    unsigned long long frames;
    unsigned crc;
    bool has_crc;
} vector_t;

static vector_t g_vec[64];
static int g_nvec;

static void load_manifest(void) {
    FILE *f = fopen(data_path("vectors.txt"), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f) && g_nvec < (int)CORE_ARRAY_SIZE(g_vec)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        vector_t *v = &g_vec[g_nvec];
        char codec[16], crc[16], ref[64];
        if (sscanf(line, "%63s %15s %u %u %u %u %llu %15s %63s", v->name, codec, &v->rate, &v->channels, &v->bits,
                   &v->dop, &v->frames, crc, ref) != 9) {
            continue;
        }
        v->has_crc = strcmp(crc, "-") != 0;
        v->crc = v->has_crc ? (unsigned)strtoul(crc, NULL, 16) : 0;
        g_nvec++;
    }
    fclose(f);
}

static uint32_t crc_channel(const int32_t *pcm, uint64_t frames, unsigned ch, unsigned c) {
    uint32_t crc = 0;
    for (uint64_t i = 0; i < frames; i++) crc = crc32_update(crc, &pcm[i * ch + c], 4);
    return crc;
}

#define PCM_CAPS (SINK_CAP_BITPERFECT | SINK_CAP_RATE_SWITCH)

// ------------------------------------------------------------------------------ tests ----
TEST(passthrough_vectors) {
    load_manifest();
    CHECK(g_nvec > 20);
    int tested = 0;
    for (int k = 0; k < g_nvec; k++) {
        const vector_t *v = &g_vec[k];
        if (!v->has_crc || v->dop) continue;
        mem_sink_t m;
        mem_sink_init(&m, "mem", PCM_CAPS);
        player_t *p = new_player(&m, 0.0f);
        play_one(p, data_path(v->name));
        player_run_once(p);
        player_status_t st = status_of(p);
        CHECK(st.state == PLAYER_PLAYING);
        CHECK(st.bitperfect);
        CHECK_EQ_INT(st.dsp_flags, 0);
        CHECK_EQ_INT(st.src_fmt.sample_rate, v->rate);
        CHECK_EQ_INT(st.out_fmt.sample_rate, v->rate);
        CHECK_EQ_INT(st.out_fmt.channels, 2);
        run_to_stop(p, 100000);
        CHECK_EQ_INT(m.frames, v->frames);
        uint32_t crc;
        if (v->channels == 2) {
            crc = crc32_update(0, m.pcm, (size_t)m.frames * 2 * 4);
        } else {
            crc = crc_channel(m.pcm, m.frames, 2, 0);
            CHECK_EQ_INT(crc_channel(m.pcm, m.frames, 2, 1), crc);  // upmix: identical channels
        }
        if (crc != v->crc) printf("  %s: crc %08x, expected %08x\n", v->name, crc, v->crc);
        CHECK_EQ_INT(crc, v->crc);
        CHECK_EQ_INT(ev_count(EV_PLAYBACK_ERROR), 0);
        free_player(p);
        mem_sink_free(&m);
        tested++;
    }
    CHECK(tested >= 15);
}

TEST(gapless_flac_pair) {
    uint64_t nref = 0;
    int32_t *ref = decode_all(data_path("gap_ref.wav"), &nref, NULL);
    CHECK(ref && nref == 44100);
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    player_t *p = new_player(&m, 0.0f);
    const char *files[] = {data_path("gap_a.flac"), data_path("gap_b.flac")};
    play_paths(p, files, 2, 0);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(m.frames, 44100);
    uint64_t bad = 0;
    for (uint64_t i = 0; ref && i < m.frames && i < nref; i++) {
        bad += m.pcm[2 * i] != ref[i] || m.pcm[2 * i + 1] != ref[i];
    }
    CHECK_EQ_INT(bad, 0);
    CHECK_EQ_INT(m.opens, 1);    // same format: no reopen
    CHECK_EQ_INT(m.flushes, 0);  // natural transitions never flush
    CHECK_EQ_INT(ev_count(EV_TRACK_CHANGED), 2);
    CHECK_EQ_INT(ev_count(EV_PLAYBACK_ERROR), 0);
    free(ref);
    free_player(p);
    mem_sink_free(&m);
}

TEST(gapless_mp3_pair_length) {
    uint64_t nref = 0;
    int32_t *ref = decode_all(data_path("gap_ref.wav"), &nref, NULL);
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    player_t *p = new_player(&m, 0.0f);
    const char *files[] = {data_path("gap_a.mp3"), data_path("gap_b.mp3")};
    play_paths(p, files, 2, 0);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(m.frames, 22383 + 21717);
    // Aligned with the continuous signal (no gap, no overlap at the join).
    double sig = 0, err = 0;
    for (uint64_t i = 0; ref && i < m.frames && i < nref; i++) {
        double r = ref[i], d = (double)m.pcm[2 * i] - r;
        sig += r * r;
        err += d * d;
    }
    double snr = 10.0 * log10(sig / (err + 1.0));
    if (snr <= 25.0) printf("  mp3 pair snr %.1f dB\n", snr);
    CHECK(snr > 25.0);
    free(ref);
    free_player(p);
    mem_sink_free(&m);
}

TEST(cue_split) {
    mkdir_(TMP);
    mkdir_(TMP "/cue");
    CHECK(copy_file(data_path("wav_s16_44k.wav"), TMP "/cue/image.wav"));
    write_text(TMP "/cue/image.cue", "PERFORMER \"Band\"\nTITLE \"Album\"\nFILE \"image.wav\" WAVE\n"
                                     "  TRACK 01 AUDIO\n    TITLE \"One\"\n    INDEX 01 00:00:00\n"
                                     "  TRACK 02 AUDIO\n    TITLE \"Two\"\n    PERFORMER \"Guest\"\n"
                                     "    INDEX 01 00:00:25\n"
                                     "  TRACK 03 AUDIO\n    TITLE \"Three\"\n    INDEX 01 00:00:50\n");
    playlist_t *pl = playlist_load_cue(TMP "/cue/image.cue");
    CHECK(pl && playlist_count(pl) == 3);
    if (!pl) return;
    queue_item_t items[3];
    memset(items, 0, sizeof items);
    for (uint32_t i = 0; i < 3; i++) {
        const playlist_entry_t *e = playlist_get(pl, i);
        items[i].track_id = LIB_ID_NONE;
        core_strlcpy(items[i].path, e->path, sizeof items[i].path);
        items[i].start_ms = e->start_ms;
        items[i].end_ms = e->end_ms;
    }
    playlist_free(pl);
    CHECK_EQ_INT(items[1].start_ms, 333);
    CHECK_EQ_INT(items[1].end_ms, 666);
    uint64_t nref = 0;
    int32_t *ref = decode_all(data_path("wav_s16_44k.wav"), &nref, NULL);

    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    player_t *p = new_player(&m, 0.0f);
    player_cmd_play_items(p, items, 3, 0, false);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(m.frames, 44100);
    CHECK(ref && m.frames == nref && memcmp(m.pcm, ref, (size_t)nref * 8) == 0);
    CHECK_EQ_INT(m.opens, 1);
    CHECK_EQ_INT(ev_count(EV_TRACK_CHANGED), 3);
    CHECK_STR(g_titles[0], "One");
    CHECK_STR(g_titles[1], "Two");
    CHECK_STR(g_titles[2], "Three");
    free_player(p);
    mem_sink_free(&m);

    // Track 2 alone: frames [333 ms, 666 ms), metadata from the sheet.
    mem_sink_init(&m, "mem", PCM_CAPS);
    p = new_player(&m, 0.0f);
    player_cmd_play_items(p, &items[1], 1, 0, false);
    player_run_once(p);
    player_status_t st = status_of(p);
    CHECK_STR(st.title, "Two");
    CHECK_STR(st.artist, "Guest");
    CHECK_EQ_INT(st.duration_ms, 333);
    CHECK_EQ_INT(st.track_no, 2);
    run_to_stop(p, 10000);
    uint64_t a = 14685, b = 29370;
    CHECK_EQ_INT(m.frames, b - a);
    CHECK(ref && memcmp(m.pcm, ref + a * 2, (size_t)(b - a) * 8) == 0);
    free_player(p);
    mem_sink_free(&m);

    // Seek inside a CUE track is relative to its start.
    mem_sink_init(&m, "mem", PCM_CAPS);
    p = new_player(&m, 0.0f);
    player_cmd_play_items(p, &items[1], 1, 0, false);
    player_run_once(p);
    player_cmd_seek_ms(p, 100);
    player_run_once(p);
    CHECK_EQ_INT(status_of(p).position_ms, 100 + (1024 * 1000 / 44100));
    uint64_t before = 1024;
    run_to_stop(p, 10000);
    uint64_t at = a + 4410;
    CHECK_EQ_INT(m.frames - before, b - at);
    CHECK(ref && memcmp(m.pcm + before * 2, ref + at * 2, (size_t)(b - at) * 8) == 0);
    free(ref);
    free_player(p);
    mem_sink_free(&m);
}

TEST(resample_to_bluetooth_rate) {
    static const struct {
        const char *name;
        uint32_t in_frames, in_rate;
    } cases[] = {{"wav_s24_96k_ext.wav", 48000, 96000}, {"wav_s32_48k_rf64.wav", 12000, 48000}};
    for (size_t k = 0; k < CORE_ARRAY_SIZE(cases); k++) {
        mem_sink_t m;
        mem_sink_init(&m, "bt", 0);
        m.fixed_rate = 44100;
        m.fixed_bits = 16;
        player_t *p = new_player(&m, 0.0f);
        play_one(p, data_path(cases[k].name));
        player_run_once(p);
        player_status_t st = status_of(p);
        CHECK_EQ_INT(st.out_fmt.sample_rate, 44100);
        CHECK_EQ_INT(st.out_fmt.bits, 16);
        CHECK_EQ_INT(st.src_fmt.sample_rate, cases[k].in_rate);
        CHECK(st.dsp_flags & DSP_FLAG_RESAMPLE);
        CHECK(!st.bitperfect);
        run_to_stop(p, 10000);
        // Plus the resampler tail: silence pushed through at the end of the queue.
        uint64_t expect = (uint64_t)(cases[k].in_frames + PLAYER_TAIL_FRAMES) * 44100 / cases[k].in_rate;
        if (m.frames > expect + 2 || m.frames + 2 < expect) {
            printf("  %s: %llu frames, expected %llu\n", cases[k].name, (unsigned long long)m.frames,
                   (unsigned long long)expect);
        }
        CHECK(m.frames <= expect + 2 && m.frames + 2 >= expect);
        double f = tone_freq(m.pcm, m.frames, 2, 0, 44100, 2000);
        if (fabs(f - 997.0) > 1.0) printf("  %s: %.2f Hz\n", cases[k].name, f);
        CHECK_NEAR(f, 997.0, 1.0);
        CHECK_NEAR(peak_db(m.pcm + 4000, (m.frames - 6000) * 2), -6.02, 0.3);  // amplitude 0.5 kept
        free_player(p);
        mem_sink_free(&m);
    }
}

TEST(mono_upmix) {
    uint64_t nref = 0;
    int32_t *ref = decode_all(data_path("wav_s16_22k_mono.wav"), &nref, NULL);
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    player_t *p = new_player(&m, 0.0f);
    play_one(p, data_path("wav_s16_22k_mono.wav"));
    player_run_once(p);
    player_status_t st = status_of(p);
    CHECK_EQ_INT(st.src_fmt.channels, 1);
    CHECK_EQ_INT(st.out_fmt.channels, 2);
    CHECK(st.bitperfect);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(m.frames, nref);
    uint64_t bad = 0;
    for (uint64_t i = 0; ref && i < m.frames; i++) bad += m.pcm[2 * i] != ref[i] || m.pcm[2 * i + 1] != ref[i];
    CHECK_EQ_INT(bad, 0);
    free(ref);
    free_player(p);
    mem_sink_free(&m);
}

TEST(dop_negotiated_and_refused) {
    mem_sink_t m;
    mem_sink_init(&m, "dac", PCM_CAPS | SINK_CAP_DOP);
    m.accept_dop = true;
    player_t *p = new_player(&m, -20.0f);  // volume must not touch DoP
    play_one(p, data_path("dsd64_1k.dsf"));
    player_run_once(p);
    player_status_t st = status_of(p);
    CHECK(st.out_fmt.dop);
    CHECK(st.bitperfect);
    CHECK_EQ_INT(st.dsp_flags, 0);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(m.frames, 44100);
    CHECK_EQ_INT(crc32_update(0, m.pcm, (size_t)m.frames * 8), 0x967dccfdu);
    free_player(p);
    mem_sink_free(&m);

    // A sink without DoP: EV_PLAYBACK_ERROR (unsupported) and the next entry plays.
    mem_sink_init(&m, "pcm", PCM_CAPS);
    p = new_player(&m, 0.0f);
    const char *files[] = {data_path("dsd64_1k.dsf"), data_path("wav_s16_44k.wav")};
    play_paths(p, files, 2, 0);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(ev_count(EV_PLAYBACK_ERROR), 1);
    const core_event_t *e = ev_last(EV_PLAYBACK_ERROR);
    CHECK(e && e->a == CORE_EUNSUPPORTED);
    CHECK(e && strcmp(e->text, "dsd64_1k.dsf") == 0);
    CHECK_EQ_INT(m.frames, 44100);
    free_player(p);
    mem_sink_free(&m);
}

TEST(seek_accuracy) {
    uint64_t nref = 0;
    int32_t *ref = decode_all(data_path("wav_s16_44k.wav"), &nref, NULL);
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    player_t *p = new_player(&m, 0.0f);
    play_one(p, data_path("wav_s16_44k.wav"));
    run_n(p, 3);
    CHECK_EQ_INT(m.frames, 3072);
    CHECK_EQ_INT(status_of(p).position_ms, 3072 * 1000 / 44100);
    ev_reset();
    player_cmd_seek_ms(p, 500);
    player_run_once(p);
    const core_event_t *e = NULL;
    for (int i = 0; i < g_nev; i++) {
        if (g_ev[i].type == EV_POSITION) {
            e = &g_ev[i];
            break;
        }
    }
    CHECK(e && e->a == 500);
    CHECK_EQ_INT(m.flushes, 1);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(m.frames - 3072, 44100 - 22050);
    CHECK(ref && memcmp(m.pcm + 3072 * 2, ref + 22050 * 2, (size_t)(44100 - 22050) * 8) == 0);
    free_player(p);
    mem_sink_free(&m);

    // Relative seek from the audible position; merged when repeated.
    mem_sink_init(&m, "mem", PCM_CAPS);
    p = new_player(&m, 0.0f);
    play_one(p, data_path("wav_s16_44k.wav"));
    run_n(p, 3);  // 69 ms
    player_cmd_seek_relative_ms(p, 100);
    player_cmd_seek_relative_ms(p, 150);
    player_run_once(p);
    uint64_t at = audio_ms_to_frames(69 + 250, 44100);
    CHECK_EQ_INT(m.flushes, 1);
    CHECK(ref && memcmp(m.pcm + 3072 * 2, ref + at * 2, 1024 * 8) == 0);
    // Seeking back before the start clamps to 0.
    player_cmd_seek_relative_ms(p, -100000);
    player_run_once(p);
    CHECK(ref && memcmp(m.pcm + 4096 * 2, ref, 1024 * 8) == 0);
    // Past the end: the entry ends, the queue stops.
    player_cmd_seek_ms(p, 5000);
    run_to_stop(p, 100);
    CHECK(status_of(p).state == PLAYER_STOPPED);
    free(ref);
    free_player(p);
    mem_sink_free(&m);
}

TEST(pause_resume_partial_writes) {
    uint64_t nref = 0;
    int32_t *ref = decode_all(data_path("wav_s16_44k.wav"), &nref, NULL);
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    m.max_write = 300;  // short writes: the rest stays pending
    player_t *p = new_player(&m, 0.0f);
    play_one(p, data_path("wav_s16_44k.wav"));
    run_n(p, 5);
    uint64_t f = m.frames;
    player_cmd_pause(p);
    for (int i = 0; i < 5; i++) CHECK_EQ_INT(player_run_once(p), 0);
    CHECK_EQ_INT(m.frames, f);
    CHECK(m.paused);
    CHECK(status_of(p).state == PLAYER_PAUSED);
    CHECK(player_wait_hint_ms(p) > 0);
    m.refuse_writes = 4;  // sink full for a while
    player_cmd_toggle_pause(p);
    player_run_once(p);
    CHECK(!m.paused);
    CHECK(status_of(p).state == PLAYER_PLAYING);
    run_to_stop(p, 100000);
    CHECK_EQ_INT(m.frames, 44100);
    CHECK(ref && memcmp(m.pcm, ref, (size_t)nref * 8) == 0);
    // Resume from STOPPED starts the current entry again (the queue rewinds at its end).
    ev_reset();
    player_cmd_resume(p);
    player_run_once(p);
    CHECK(status_of(p).state == PLAYER_PLAYING);
    CHECK_EQ_INT(ev_count(EV_TRACK_CHANGED), 1);
    free(ref);
    free_player(p);
    mem_sink_free(&m);
}

TEST(sink_change_keeps_position) {
    uint64_t nref = 0;
    int32_t *ref = decode_all(data_path("wav_s16_44k.wav"), &nref, NULL);
    // A plays 5 chunks; 1000 frames of them are still in its buffer when B takes over.
    mem_sink_t a, b;
    mem_sink_init(&a, "wired", PCM_CAPS);
    mem_sink_init(&b, "bluetooth", PCM_CAPS);
    a.level = 1000;
    player_t *p = new_player(&a, 0.0f);
    play_one(p, data_path("wav_s16_44k.wav"));
    run_n(p, 5);
    CHECK_EQ_INT(a.frames, 5120);
    CHECK_EQ_INT(status_of(p).position_ms, (5120 - 1000) * 1000 / 44100);
    player_cmd_set_sink(p, &b.sink);
    player_run_once(p);
    CHECK_EQ_INT(a.closes, 1);
    player_status_t st = status_of(p);
    CHECK_STR(st.sink_name, "bluetooth");
    run_to_stop(p, 10000);
    uint64_t at = 5120 - 1000;
    CHECK_EQ_INT(b.frames, 44100 - at);
    CHECK(ref && memcmp(b.pcm, ref + at * 2, (size_t)(44100 - at) * 8) == 0);
    free_player(p);
    mem_sink_free(&a);
    mem_sink_free(&b);

    // NULL sink pauses; a new sink resumes where it stopped.
    mem_sink_init(&a, "wired", PCM_CAPS);
    mem_sink_init(&b, "bluetooth", PCM_CAPS);
    p = new_player(&a, 0.0f);
    play_one(p, data_path("wav_s16_44k.wav"));
    run_n(p, 2);
    player_cmd_set_sink(p, NULL);
    run_n(p, 3);
    CHECK(status_of(p).state == PLAYER_PAUSED);
    player_cmd_set_sink(p, &b.sink);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(a.frames + b.frames, 44100);
    CHECK(ref && memcmp(b.pcm, ref + 2048 * 2, (size_t)b.frames * 8) == 0);
    free(ref);
    free_player(p);
    mem_sink_free(&a);
    mem_sink_free(&b);
}

TEST(volume_limit_and_paths) {
    mem_sink_t m;
    mem_sink_init(&m, "pcm5102a", PCM_CAPS);  // no hardware volume
    player_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.sink = &m.sink;
    cfg.volume_default_db = -3.0f;
    cfg.volume_max_db = -6.0f;
    cfg.volume_step_db = 2.0f;
    player_t *p = player_create(&cfg);
    ev_reset();
    CHECK_NEAR(status_of(p).volume_db, -6.0, 1e-6);  // default clamped to the limit
    player_cmd_set_volume_db(p, 0.0f);
    player_run_once(p);
    CHECK_NEAR(status_of(p).volume_db, -6.0, 1e-6);
    const core_event_t *e = ev_last(EV_VOLUME);
    CHECK(e && fabs(e->f + 6.0f) < 1e-6);
    player_cmd_volume_step(p, 3);
    player_run_once(p);
    CHECK_NEAR(status_of(p).volume_db, -6.0, 1e-6);
    player_cmd_volume_step(p, -1);
    player_cmd_volume_step(p, -1);  // merged: one change of -4 dB
    ev_reset();
    player_run_once(p);
    CHECK_NEAR(status_of(p).volume_db, -10.0, 1e-6);
    CHECK_EQ_INT(ev_count(EV_VOLUME), 1);
    player_cmd_set_volume_limit(p, -12.0f);
    player_run_once(p);
    CHECK_NEAR(status_of(p).volume_db, -12.0, 1e-6);
    CHECK_NEAR(status_of(p).volume_max_db, -12.0, 1e-6);
    player_cmd_volume_step(p, -1000);
    player_run_once(p);
    CHECK_NEAR(status_of(p).volume_db, PLAYER_VOLUME_MIN_DB, 1e-6);
    player_cmd_set_volume_db(p, -12.0f);
    // Software volume: -12 dB on the samples.
    play_one(p, data_path("wav_s16_44k.wav"));
    run_n(p, 2);
    player_status_t st = status_of(p);
    CHECK(!st.bitperfect);
    CHECK(st.dsp_flags & DSP_FLAG_SW_VOLUME);
    run_to_stop(p, 10000);
    CHECK_NEAR(peak_db(m.pcm + 4000 * 2, 20000 * 2), -6.02 - 12.0, 0.2);
    player_destroy(p);
    mem_sink_free(&m);

    // Hardware volume: samples untouched, the sink gets the level.
    mem_sink_init(&m, "cs43131", PCM_CAPS | SINK_CAP_HW_VOLUME);
    cfg.sink = &m.sink;
    cfg.volume_default_db = -20.0f;
    cfg.volume_max_db = 0.0f;
    p = player_create(&cfg);
    play_one(p, data_path("wav_s16_44k.wav"));
    run_n(p, 2);
    st = status_of(p);
    CHECK(st.bitperfect);
    CHECK_EQ_INT(st.dsp_flags, 0);
    CHECK_NEAR(m.volume_db, -20.0, 1e-6);
    player_cmd_set_volume_db(p, -30.0f);
    player_run_once(p);
    CHECK_NEAR(m.volume_db, -30.0, 1e-6);
    run_to_stop(p, 10000);
    uint64_t nref = 0;
    int32_t *ref = decode_all(data_path("wav_s16_44k.wav"), &nref, NULL);
    CHECK(ref && m.frames == nref && memcmp(m.pcm, ref, (size_t)nref * 8) == 0);
    free(ref);
    player_destroy(p);
    mem_sink_free(&m);
}

TEST(dsp_eq_flags) {
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    player_t *p = new_player(&m, 0.0f);
    dsp_config_t d;
    dsp_config_defaults(&d);
    d.eq_enabled = true;
    d.eq.band_count = 1;
    d.eq.bands[0] = (peq_band_t){PEQ_PEAK, 1000.0f, -6.0f, 1.0f, true};
    player_cmd_set_dsp(p, &d);
    play_one(p, data_path("wav_s16_44k.wav"));
    run_n(p, 2);
    player_status_t st = status_of(p);
    CHECK(st.dsp_flags & DSP_FLAG_EQ);
    CHECK(st.dsp_flags & DSP_FLAG_DITHER);  // 16-bit sink
    CHECK(!st.bitperfect);
    // EQ off again: back to bit-perfect.
    d.eq_enabled = false;
    player_cmd_set_dsp(p, &d);
    run_n(p, 3);
    st = status_of(p);
    CHECK(st.bitperfect);
    CHECK_EQ_INT(st.dsp_flags, 0);
    free_player(p);
    mem_sink_free(&m);
}

// Counts samples with bits set below the given resolution (bits = 16: any of the low 16).
static uint64_t below_bits(const int32_t *pcm, uint64_t samples, unsigned bits) {
    const uint32_t mask = (uint32_t)((1ull << (32 - bits)) - 1);
    uint64_t n = 0;
    for (uint64_t i = 0; i < samples; i++) n += ((uint32_t)pcm[i] & mask) != 0;
    return n;
}

TEST(dither_resolution_follows_sink) {
    // 1. Bluetooth-like sink that dithers itself: the DSP delivers Q31 without its own dither.
    // 2. The same sink without SINK_CAP_DITHERS: the DSP reduces to the negotiated 16 bits.
    for (int k = 0; k < 2; k++) {
        mem_sink_t m;
        mem_sink_init(&m, "bluetooth", k == 0 ? SINK_CAP_DITHERS : 0);
        m.fixed_rate = 44100;
        m.fixed_bits = 16;
        player_t *p = new_player(&m, -6.0f);
        play_one(p, data_path("wav_s16_44k.wav"));
        run_n(p, 4);
        player_status_t st = status_of(p);
        CHECK(st.dsp_flags & DSP_FLAG_SW_VOLUME);
        CHECK(!st.bitperfect);
        if (k == 0) {
            CHECK(!(st.dsp_flags & DSP_FLAG_DITHER));
            CHECK(below_bits(m.pcm, m.frames * 2, 16) > m.frames);  // finer than 16 bits
        } else {
            CHECK(st.dsp_flags & DSP_FLAG_DITHER);
            CHECK_EQ_INT(below_bits(m.pcm, m.frames * 2, 16), 0);
        }
        free_player(p);
        mem_sink_free(&m);
    }
    // 3. Bit-perfect (32-bit slot) sink, 16-bit source at -30 dB: dithered to 24 bits, not to 16.
    mem_sink_t m;
    mem_sink_init(&m, "wired", PCM_CAPS);
    player_t *p = new_player(&m, -30.0f);
    play_one(p, data_path("wav_s16_44k.wav"));
    run_n(p, 4);
    player_status_t st = status_of(p);
    CHECK_EQ_INT(st.out_fmt.bits, 16);  // negotiated as the source
    CHECK(st.dsp_flags & DSP_FLAG_DITHER);
    CHECK_EQ_INT(below_bits(m.pcm, m.frames * 2, 24), 0);
    CHECK(below_bits(m.pcm, m.frames * 2, 16) > m.frames);
    free_player(p);
    mem_sink_free(&m);
    // 4. A sink without SINK_CAP_BITPERFECT is never reported bit-perfect, even with a
    //    matching format, the DSP bypassed and full volume.
    for (int k = 0; k < 2; k++) {
        mem_sink_init(&m, "bluetooth", k == 0 ? 0 : SINK_CAP_DITHERS);
        m.fixed_rate = 44100;
        m.fixed_bits = 16;
        p = new_player(&m, 0.0f);
        play_one(p, data_path("wav_s16_44k.wav"));
        run_n(p, 2);
        st = status_of(p);
        CHECK_EQ_INT(st.dsp_flags, 0);
        CHECK(!st.bitperfect);
        free_player(p);
        mem_sink_free(&m);
    }
}

TEST(volume_to_unity_keeps_every_frame) {
    // Software volume -1 dB -> 0 dB mid-track: the DSP ramps to unity and goes to bypass.
    // The 1 ms it delayed while active is played out, so the output is the whole file after
    // the initial delay, and bit-exact from the switch on.
    uint64_t nref = 0;
    int32_t *ref = decode_all(data_path("wav_s16_44k.wav"), &nref, NULL);
    const uint64_t D = 43;  // limiter lookahead at 44.1 kHz (44 frames) minus one
    mem_sink_t m;
    mem_sink_init(&m, "pcm5102a", PCM_CAPS);
    player_t *p = new_player(&m, -1.0f);
    play_one(p, data_path("wav_s16_44k.wav"));
    run_n(p, 5);
    CHECK(!status_of(p).bitperfect);
    player_cmd_set_volume_db(p, 0.0f);
    run_n(p, 3);
    CHECK(status_of(p).bitperfect);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(m.frames, nref + D);
    uint64_t bad = 0, inexact = 0;
    for (uint64_t i = 0; ref && i < nref && D + i < m.frames; i++) {
        for (unsigned c = 0; c < 2; c++) {
            const double v = m.pcm[(D + i) * 2 + c], r = ref[i * 2 + c];
            if (i >= 8 * 1024) {
                inexact += v != r;
            } else {
                const double g = pow(10, -1.0 / 20);
                bad += v < CORE_MIN(g * r, r) - 70000 || v > CORE_MAX(g * r, r) + 70000;  // 16-bit dither
            }
        }
    }
    CHECK_EQ_INT(bad, 0);
    CHECK_EQ_INT(inexact, 0);
    free(ref);
    free_player(p);
    mem_sink_free(&m);
}

static void put_le(uint8_t *p, uint64_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// Stereo DSD64 DSF with `frames` DoP frames (16 DSD bits per channel each).
static bool write_dsf(const char *path, uint32_t frames) {
    const uint32_t block = 4096, ch = 2;
    const uint32_t groups = (frames * 2u + block - 1) / block;
    const uint64_t data = (uint64_t)groups * block * ch;
    uint8_t h[92];
    memset(h, 0, sizeof h);
    memcpy(h, "DSD ", 4);
    put_le(h + 4, 28, 8);
    put_le(h + 12, sizeof h + data, 8);
    memcpy(h + 28, "fmt ", 4);
    put_le(h + 32, 52, 8);
    put_le(h + 40, 1, 4);        // format version
    put_le(h + 48, 2, 4);        // channel type: stereo
    put_le(h + 52, ch, 4);
    put_le(h + 56, 2822400, 4);  // DSD64
    put_le(h + 60, 1, 4);        // 1 bit per sample, LSB first
    put_le(h + 64, (uint64_t)frames * 16, 8);
    put_le(h + 72, block, 4);
    memcpy(h + 80, "data", 4);
    put_le(h + 84, 12 + data, 8);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(h, 1, sizeof h, f);
    for (uint64_t i = 0; i < data; i++) fputc(0x69, f);  // DSD idle pattern
    return fclose(f) == 0;
}

TEST(dop_markers_alternate_across_entries) {
    // Two DSF files with an odd number of DoP frames each: every file starts with 0x05, so
    // without a phase fix the join would repeat a marker.
    mkdir_(TMP);
    CHECK(write_dsf(TMP "/odd_a.dsf", 4097));
    CHECK(write_dsf(TMP "/odd_b.dsf", 4097));
    mem_sink_t m;
    mem_sink_init(&m, "dac", PCM_CAPS | SINK_CAP_DOP);
    m.accept_dop = true;
    player_t *p = new_player(&m, 0.0f);
    const char *files[] = {TMP "/odd_a.dsf", TMP "/odd_b.dsf", TMP "/odd_a.dsf"};
    play_paths(p, files, 3, 0);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(m.frames, 3 * 4097);
    CHECK_EQ_INT(m.opens, 1);
    uint64_t bad = 0;
    for (uint64_t i = 0; i < m.frames; i++) {
        for (unsigned c = 0; c < 2; c++) {
            uint8_t mk = (uint8_t)((uint32_t)m.pcm[i * 2 + c] >> 24);
            uint8_t prev = i ? (uint8_t)((uint32_t)m.pcm[(i - 1) * 2 + c] >> 24) : 0;
            bad += (mk != 0x05 && mk != 0xFA) || mk == prev;
        }
    }
    CHECK_EQ_INT(bad, 0);
    free_player(p);
    mem_sink_free(&m);
}

TEST(end_of_queue_drain_and_new_commands) {
    core_set_clock(fake_clock);
    g_now = 5000;
    // The sink reports queued audio, so the end of the queue waits for it to play out.
    mem_sink_t m, b;
    mem_sink_init(&m, "wired", PCM_CAPS);
    m.level = 3000;
    player_t *p = new_player(&m, 0.0f);
    play_one(p, data_path("wav_s16_44k.wav"));
    for (int i = 0; i < 100 && p->drain == DRAIN_NONE; i++) player_run_once(p);
    CHECK(p->drain == DRAIN_STOP);
    CHECK(status_of(p).state == PLAYER_PLAYING);
    // 1. A new album in that window plays; the old stop does not end it.
    uint64_t before = m.frames;
    const char *two[] = {data_path("wav_s16_22k_mono.wav"), data_path("wav_s16_44k.wav")};
    play_paths(p, two, 2, 0);
    for (int i = 0; i < 400 && status_of(p).state == PLAYER_PLAYING; i++) {
        g_now += 200;
        player_run_once(p);
    }
    CHECK_EQ_INT(m.frames - before, 11025 + 44100);
    CHECK(status_of(p).state == PLAYER_STOPPED);
    // 2. The output changes while the tail plays out: the stop completes, nothing replays.
    play_one(p, data_path("wav_s16_44k.wav"));
    for (int i = 0; i < 100 && p->drain == DRAIN_NONE; i++) player_run_once(p);
    CHECK(p->drain == DRAIN_STOP);
    mem_sink_init(&b, "bluetooth", PCM_CAPS);
    player_cmd_set_sink(p, &b.sink);
    run_n(p, 20);
    CHECK(status_of(p).state == PLAYER_STOPPED);
    CHECK_EQ_INT(b.frames, 0);
    free_player(p);
    mem_sink_free(&m);
    mem_sink_free(&b);
    core_set_clock(NULL);
}

TEST(jump_drops_stale_prefetch) {
    mkdir_(TMP);
    double f = 440.0;
    CHECK(wavgen_write(TMP "/j.wav", 8000, 1, 16, 40000, sine16, &f, NULL));  // 5 s
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    m.discard = true;
    player_t *p = new_player(&m, 0.0f);
    const char *files[] = {TMP "/j.wav", TMP "/j.wav", TMP "/j.wav", TMP "/j.wav"};
    play_paths(p, files, 4, 0);
    for (int i = 0; i < 100 && !p->next.open; i++) player_run_once(p);
    CHECK(p->next.open && p->next.qpos == 1);  // prefetched in the last 2 s of entry 0
    player_cmd_queue_jump(p, 2);
    player_run_once(p);
    CHECK_EQ_INT(status_of(p).queue_index, 2);
    CHECK(!p->next.open || p->next.qpos != 1);
    // Entry 2 gets its own prefetch of entry 3 (gapless kept).
    bool prefetched = false;
    for (int i = 0; i < 100 && status_of(p).queue_index == 2; i++) {
        player_run_once(p);
        if (p->next.open && p->next.qpos == 3) prefetched = true;
    }
    CHECK(prefetched);
    free_player(p);
    mem_sink_free(&m);
}

TEST(pause_without_output_stays_paused) {
    mem_sink_t a, b;
    mem_sink_init(&a, "wired", PCM_CAPS);
    mem_sink_init(&b, "wired", PCM_CAPS);
    player_t *p = new_player(&a, 0.0f);
    play_one(p, data_path("wav_s16_44k.wav"));
    run_n(p, 2);
    // Mode switch: no output, playback waits for the next one.
    player_cmd_set_sink(p, NULL);
    run_n(p, 2);
    CHECK(status_of(p).state == PLAYER_PAUSED);
    CHECK(p->resume_on_sink);
    // An explicit pause in that window cancels the automatic resume.
    player_cmd_pause(p);
    player_cmd_set_sink(p, &b.sink);
    run_n(p, 2);
    CHECK(status_of(p).state == PLAYER_PAUSED);
    CHECK_EQ_INT(b.frames, 0);
    // Toggle without output: armed, disarmed, armed again; the next output then plays.
    player_cmd_set_sink(p, NULL);
    player_cmd_toggle_pause(p);
    run_n(p, 1);
    CHECK(p->resume_on_sink);
    player_cmd_toggle_pause(p);
    run_n(p, 1);
    CHECK(!p->resume_on_sink);
    player_cmd_toggle_pause(p);
    player_cmd_set_sink(p, &a.sink);
    run_n(p, 2);
    CHECK(status_of(p).state == PLAYER_PLAYING);
    free_player(p);
    mem_sink_free(&a);
    mem_sink_free(&b);
}

TEST(commands_follow_audible_entry) {
    // 2000 frames sit in the sink buffer: after the gapless switch A stays audible (and on
    // the screen) for a moment while B is already decoding.
    for (int k = 0; k < 3; k++) {
        mem_sink_t m;
        mem_sink_init(&m, "bt", PCM_CAPS);
        m.level = 2000;
        player_t *p = new_player(&m, 0.0f);
        const char *files[] = {data_path("gap_a.flac"), data_path("gap_b.flac"), data_path("wav_s16_44k.wav")};
        play_paths(p, files, 3, 0);
        for (int i = 0; i < 100 && !p->sw_pending; i++) player_run_once(p);
        CHECK(p->sw_pending);
        player_status_t st0 = status_of(p);
        CHECK_STR(st0.title, "gap_a");
        const uint32_t shown = st0.position_ms;
        if (k == 0) {
            // NEXT while A is shown: B (not the entry after it).
            player_cmd_next(p);
            player_run_once(p);
            player_status_t st = status_of(p);
            CHECK_EQ_INT(st.queue_index, 1);
            CHECK_STR(st.title, "gap_b");
            CHECK(st.position_ms < 50);
        } else if (k == 1) {
            // A relative seek back counts from A's position and lands in A.
            player_cmd_seek_relative_ms(p, -200);
            player_run_once(p);
            player_status_t st = status_of(p);
            CHECK_EQ_INT(st.queue_index, 0);
            CHECK_STR(st.title, "gap_a");
            CHECK_NEAR(st.position_ms, shown - 200.0, 40);
        } else {
            // A relative seek past A's end continues into B.
            player_cmd_seek_relative_ms(p, 200);
            player_run_once(p);
            player_status_t st = status_of(p);
            CHECK_EQ_INT(st.queue_index, 1);
            CHECK_NEAR(st.position_ms, shown + 200.0 - 507.0, 40);
        }
        free_player(p);
        mem_sink_free(&m);
    }
}

TEST(sleep_timer_fade_and_pause) {
    core_set_clock(fake_clock);
    g_now = 1000;
    mkdir_(TMP);
    double f = 1000.0;
    CHECK(wavgen_write(TMP "/tone.wav", 8000, 1, 16, 16000, sine16, &f, NULL));
    for (int hw = 0; hw < 2; hw++) {
        mem_sink_t m;
        mem_sink_init(&m, "mem", PCM_CAPS | (hw ? SINK_CAP_HW_VOLUME : 0));
        player_t *p = new_player(&m, 0.0f);
        player_cmd_set_repeat(p, REPEAT_ONE);
        play_one(p, TMP "/tone.wav");
        player_cmd_set_sleep_timer(p, 1);
        uint32_t t0 = g_now;
        player_run_once(p);
        CHECK_EQ_INT(status_of(p).sleep_left_s, 60);
        const core_event_t *e = ev_last(EV_SLEEP_TIMER);
        CHECK(e && e->a == 60);
        double level_mid = 0, level_start = 0;
        for (int i = 0; i < 700 && status_of(p).state == PLAYER_PLAYING; i++) {
            g_now += 100;
            mem_sink_reset_recording(&m);
            player_run_once(p);
            uint32_t left = 60000 - (g_now - t0);
            if (!hw && m.frames && left == 40000) level_start = peak_db(m.pcm, m.frames * 2);
            if (!hw && m.frames && left == 15000) level_mid = peak_db(m.pcm, m.frames * 2);
            if (hw && left == 15000) level_mid = m.volume_db;
        }
        player_status_t st = status_of(p);
        CHECK(st.state == PLAYER_PAUSED);
        CHECK_EQ_INT(st.sleep_left_s, 0);
        e = ev_last(EV_SLEEP_TIMER);
        CHECK(e && e->a == 0);
        CHECK(ev_count(EV_SLEEP_TIMER) >= 5);
        CHECK_NEAR(st.volume_db, 0.0, 1e-6);  // restored for the next play
        if (hw) {
            CHECK_NEAR(level_mid, -30.0, 1.0);
            CHECK_NEAR(m.volume_db, 0.0, 1e-6);
        } else {
            CHECK_NEAR(level_start, -6.02, 0.3);
            CHECK_NEAR(level_mid, -6.02 - 30.0, 1.0);
            // The gain ramps back to unity with the first samples after resume.
            player_cmd_resume(p);
            run_n(p, 3);
            CHECK(status_of(p).bitperfect);
            CHECK_NEAR(peak_db(m.pcm + (m.frames - 1024) * 2, 2048), -6.02, 0.3);
        }
        free_player(p);
        mem_sink_free(&m);
    }
    // Cancel: EV_SLEEP_TIMER with -1, no pause.
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    player_t *p = new_player(&m, 0.0f);
    player_cmd_set_repeat(p, REPEAT_ONE);
    play_one(p, TMP "/tone.wav");
    player_cmd_set_sleep_timer(p, 1);
    run_n(p, 2);
    player_cmd_set_sleep_timer(p, 0);
    player_run_once(p);
    const core_event_t *e = ev_last(EV_SLEEP_TIMER);
    CHECK(e && e->a == -1);
    g_now += 120000;
    run_n(p, 3);
    CHECK(status_of(p).state == PLAYER_PLAYING);
    free_player(p);
    mem_sink_free(&m);
    core_set_clock(NULL);
}

TEST(error_skipping) {
    mkdir_(TMP);
    FILE *f = fopen(TMP "/corrupt.flac", "wb");
    for (int i = 0; f && i < 5000; i++) fputc((i * 7919) & 0xFF, f);
    if (f) fclose(f);
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    player_t *p = new_player(&m, 0.0f);
    const char *files[] = {TMP "/missing.flac", TMP "/corrupt.flac", data_path("wav_s16_44k.wav")};
    play_paths(p, files, 3, 0);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(ev_count(EV_PLAYBACK_ERROR), 2);
    CHECK(g_ev[0].type != EV_PLAYBACK_ERROR || g_ev[0].a < 0);
    const core_event_t *e = NULL;
    for (int i = 0; i < g_nev; i++) {
        if (g_ev[i].type == EV_PLAYBACK_ERROR) {
            e = &g_ev[i];
            break;
        }
    }
    CHECK(e && e->a == CORE_ENOTFOUND && strcmp(e->text, "missing.flac") == 0);
    CHECK_EQ_INT(m.frames, 44100);
    free_player(p);
    mem_sink_free(&m);

    // Twelve unplayable entries: stop after ten.
    mem_sink_init(&m, "mem", PCM_CAPS);
    p = new_player(&m, 0.0f);
    char names[12][64];
    const char *list[12];
    for (int i = 0; i < 12; i++) {
        snprintf(names[i], sizeof names[i], TMP "/nothing_%d.mp3", i);
        list[i] = names[i];
    }
    play_paths(p, list, 12, 0);
    run_to_stop(p, 100);
    CHECK_EQ_INT(ev_count(EV_PLAYBACK_ERROR), PLAYER_MAX_FAILURES);
    CHECK(status_of(p).state == PLAYER_STOPPED);
    CHECK_EQ_INT(m.frames, 0);
    // Repeat all over a queue that cannot play: stops after one round.
    ev_reset();
    player_cmd_set_repeat(p, REPEAT_ALL);
    play_paths(p, list, 3, 0);
    run_to_stop(p, 100);
    CHECK_EQ_INT(ev_count(EV_PLAYBACK_ERROR), 3);
    free_player(p);
    mem_sink_free(&m);
}

TEST(play_folder) {
    mkdir_(TMP);
    mkdir_(TMP "/folder");
    mkdir_(TMP "/folder/sub");
    mkdir_(TMP "/empty_folder");
    CHECK(copy_file(data_path("gap_b.flac"), TMP "/folder/02 b.flac"));
    CHECK(copy_file(data_path("gap_a.flac"), TMP "/folder/01 a.flac"));
    CHECK(copy_file(data_path("gap_a.flac"), TMP "/folder/sub/skipped.flac"));  // not recursive
    write_text(TMP "/folder/notes.txt", "not audio");
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    player_t *p = new_player(&m, 0.0f);
    CHECK_EQ_INT(player_cmd_play_folder(p, TMP "/folder", 0, false), 2);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(m.frames, 22383 + 21717);  // a then b, in browser order
    CHECK_EQ_INT(player_cmd_play_folder(p, TMP "/empty_folder", 0, false), CORE_ENOTFOUND);
    CHECK_EQ_INT(player_cmd_play_folder(p, TMP "/no_such_folder", 0, false), CORE_ENOTFOUND);
    CHECK_EQ_INT(player_cmd_play_folder(p, "", 0, false), CORE_EINVAL);
    CHECK_EQ_INT(player_cmd_play_folder(NULL, TMP "/folder", 0, false), CORE_EINVAL);
    free_player(p);
    mem_sink_free(&m);
}

TEST(metadata_and_title_fallback) {
    mkdir_(TMP);
    wavgen_tags_t tags = {"Song", "Artist", "Record", "7"};
    CHECK(wavgen_write(TMP "/tagged.wav", 8000, 2, 16, 4000, ramp16, NULL, &tags));
    CHECK(wavgen_write(TMP "/Plain Name.v2.wav", 8000, 2, 24, 4000, ramp16, NULL, NULL));
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    player_t *p = new_player(&m, 0.0f);
    const char *files[] = {TMP "/tagged.wav", TMP "/Plain Name.v2.wav"};
    play_paths(p, files, 2, 0);
    player_run_once(p);
    player_status_t st = status_of(p);
    CHECK_STR(st.title, "Song");
    CHECK_STR(st.artist, "Artist");
    CHECK_STR(st.album, "Record");
    CHECK_EQ_INT(st.track_no, 7);
    CHECK_EQ_INT(st.duration_ms, 500);
    CHECK_EQ_INT(st.codec, CODEC_WAV);
    CHECK_EQ_INT(st.queue_length, 2);
    player_cmd_next(p);
    player_run_once(p);
    st = status_of(p);
    CHECK_STR(st.title, "Plain Name.v2");
    CHECK_EQ_INT(st.src_fmt.bits, 24);
    CHECK_EQ_INT(st.queue_index, 1);
    CHECK_EQ_INT(m.opens, 2);  // 16 -> 24 bit: the sink is reconfigured
    free_player(p);
    mem_sink_free(&m);
}

TEST(format_change_reopens_after_drain) {
    // 44.1 kHz then 22.05 kHz: the sink is reopened once the first file played out.
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    player_t *p = new_player(&m, 0.0f);
    const char *files[] = {data_path("wav_s16_44k.wav"), data_path("wav_s16_22k_mono.wav")};
    play_paths(p, files, 2, 0);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(m.opens, 2);
    CHECK_EQ_INT(m.open_fmts[1].sample_rate, 22050);
    CHECK_EQ_INT(m.open_at[1], 44100);  // nothing lost, nothing added (DSP bypassed)
    CHECK_EQ_INT(m.frames, 44100 + 11025);
    CHECK_EQ_INT(ev_count(EV_TRACK_CHANGED), 2);
    free_player(p);
    mem_sink_free(&m);
}

TEST(status_follows_audible_track) {
    // 2000 frames sit in the sink buffer: the switch to B shows once A's tail has played.
    mem_sink_t m;
    mem_sink_init(&m, "bt", PCM_CAPS);
    m.level = 2000;
    player_t *p = new_player(&m, 0.0f);
    const char *files[] = {data_path("gap_a.flac"), data_path("gap_b.flac")};
    play_paths(p, files, 2, 0);
    bool switched_early = false;
    uint64_t switch_at = 0;
    player_run_once(p);
    for (int i = 0; i < 200 && status_of(p).state == PLAYER_PLAYING; i++) {
        int before = ev_count(EV_TRACK_CHANGED);
        player_run_once(p);
        if (ev_count(EV_TRACK_CHANGED) == 2 && before == 1) {
            switch_at = m.frames;
            player_status_t st = status_of(p);
            CHECK_STR(st.title, "gap_b");
            CHECK_EQ_INT(st.queue_index, 1);
            CHECK(st.position_ms < 50);
        }
        if (ev_count(EV_TRACK_CHANGED) < 2 && m.frames > 22383 + 2000) switched_early = true;
        if (ev_count(EV_TRACK_CHANGED) < 2 && m.frames > 22383) {
            player_status_t st = status_of(p);
            CHECK_STR(st.title, "gap_a");  // still audible
            CHECK_EQ_INT(st.queue_index, 0);
        }
    }
    CHECK(!switched_early);
    if (!(switch_at >= 22383 + 2000 && switch_at < 22383 + 2000 + 1024)) {
        printf("  switch at %llu frames\n", (unsigned long long)switch_at);
    }
    CHECK(switch_at >= 22383 + 2000 && switch_at < 22383 + 2000 + 1024);
    free_player(p);
    mem_sink_free(&m);
}

TEST(underrun_counter) {
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    player_t *p = new_player(&m, 0.0f);
    play_one(p, data_path("wav_s16_44k.wav"));
    m.level = 3000;
    run_n(p, 3);
    CHECK_EQ_INT(status_of(p).underruns, 0);
    m.level = 0;  // the output ran dry between two writes
    player_run_once(p);
    m.level = 3000;
    run_n(p, 2);
    CHECK_EQ_INT(status_of(p).underruns, 1);
    player_cmd_pause(p);
    player_run_once(p);
    player_cmd_resume(p);
    run_n(p, 2);
    CHECK_EQ_INT(status_of(p).underruns, 1);  // a pause is not an underrun
    free_player(p);
    mem_sink_free(&m);
}

TEST(prefetch_timing) {
    mkdir_(TMP);
    double f = 440.0;
    CHECK(wavgen_write(TMP "/ten.wav", 8000, 1, 16, 80000, sine16, &f, NULL));
    mem_sink_t m;
    mem_sink_init(&m, "mem", PCM_CAPS);
    m.discard = true;
    player_t *p = new_player(&m, 0.0f);
    const char *files[] = {TMP "/ten.wav", TMP "/ten.wav"};
    play_paths(p, files, 2, 0);
    bool early = false;
    uint64_t opened_at = 0;
    for (int i = 0; i < 100 && !opened_at; i++) {
        player_run_once(p);
        if (p->next.open) opened_at = m.frames;
        if (p->next.open && m.frames < 80000 - 16000) early = true;
    }
    CHECK(!early);
    CHECK(opened_at >= 80000 - 16000 && opened_at <= 80000 - 16000 + 2048);  // ~2 s before the end
    // Gapless off: nothing is opened ahead.
    free_player(p);
    p = new_player(&m, 0.0f);
    player_cmd_set_gapless(p, false);
    play_paths(p, files, 2, 0);
    bool any = false;
    for (int i = 0; i < 90; i++) {
        player_run_once(p);
        any = any || p->next.open;
    }
    CHECK(!any);
    CHECK_EQ_INT(ev_count(EV_TRACK_CHANGED), 2);
    CHECK_EQ_INT(status_of(p).underruns, 0);  // a sink that never reports a level
    free_player(p);
    mem_sink_free(&m);
}

TEST(resampled_gapless_keeps_output) {
    // Two 48 kHz files into a 44.1 kHz output: one open, continuous resampled stream.
    mem_sink_t m;
    mem_sink_init(&m, "bt", 0);
    m.fixed_rate = 44100;
    m.fixed_bits = 16;
    player_t *p = new_player(&m, 0.0f);
    const char *files[] = {data_path("wav_s32_48k_rf64.wav"), data_path("wav_s32_48k_rf64.wav"),
                           data_path("wav_s16_44k.wav")};
    play_paths(p, files, 3, 0);
    run_to_stop(p, 10000);
    CHECK_EQ_INT(m.opens, 1);
    CHECK_EQ_INT(ev_count(EV_TRACK_CHANGED), 3);
    uint64_t expect = (uint64_t)24000 * 44100 / 48000 + 44100;
    CHECK(m.frames + 64 >= expect && m.frames <= expect + 64);
    free_player(p);
    mem_sink_free(&m);
}

TEST(replaygain_album_context) {
    mkdir_(TMP);
    mkdir_(TMP "/album");
    mkdir_(TMP "/other");
    double f = 440.0;
    const char *paths[] = {TMP "/album/1.wav", TMP "/album/2.wav", TMP "/other/3.wav"};
    for (int i = 0; i < 3; i++) CHECK(wavgen_write(paths[i], 8000, 1, 16, 800, sine16, &f, NULL));
    player_t *p = new_player(NULL, 0.0f);
    dsp_config_t d;
    dsp_config_defaults(&d);
    d.rg_mode = RG_AUTO;
    player_cmd_set_dsp(p, &d);
    play_paths(p, paths, 3, 0);
    player_run_once(p);
    pmeta_t mt;
    memset(&mt, 0, sizeof mt);
    mt.track_id = mt.album_id = LIB_ID_NONE;
    core_strlcpy(mt.path, paths[0], sizeof mt.path);
    CHECK(player_meta_album_ctx(p, 0, &mt));   // next entry in the same folder
    core_strlcpy(mt.path, paths[2], sizeof mt.path);
    CHECK(!player_meta_album_ctx(p, 2, &mt));  // alone in its folder
    player_cmd_set_shuffle(p, true);
    player_run_once(p);
    core_strlcpy(mt.path, paths[0], sizeof mt.path);
    CHECK(!player_meta_album_ctx(p, 0, &mt));  // shuffled: track gain
    d.rg_mode = RG_ALBUM;
    player_cmd_set_dsp(p, &d);
    player_run_once(p);
    CHECK(player_meta_album_ctx(p, 0, &mt));
    free_player(p);
}

TEST_MAIN(core_events_set_sink(ev_sink, NULL);
          RUN(passthrough_vectors) RUN(gapless_flac_pair) RUN(gapless_mp3_pair_length) RUN(cue_split)
              RUN(resample_to_bluetooth_rate) RUN(mono_upmix) RUN(dop_negotiated_and_refused) RUN(seek_accuracy)
                  RUN(pause_resume_partial_writes) RUN(sink_change_keeps_position) RUN(volume_limit_and_paths)
                      RUN(dsp_eq_flags) RUN(dither_resolution_follows_sink) RUN(volume_to_unity_keeps_every_frame)
                          RUN(dop_markers_alternate_across_entries) RUN(end_of_queue_drain_and_new_commands)
                              RUN(jump_drops_stale_prefetch) RUN(pause_without_output_stays_paused)
                                  RUN(commands_follow_audible_entry) RUN(sleep_timer_fade_and_pause)
                          RUN(error_skipping) RUN(play_folder)
                          RUN(metadata_and_title_fallback) RUN(format_change_reopens_after_drain)
                              RUN(status_follows_audible_track) RUN(underrun_counter) RUN(prefetch_timing)
                                  RUN(resampled_gapless_keeps_output) RUN(replaygain_album_context))
