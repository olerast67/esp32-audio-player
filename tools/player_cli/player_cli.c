// SPDX-License-Identifier: Apache-2.0
// player_cli: plays files through the real player engine into a WAV file, for listening
// tests and for checking the signal path by hand.
//
//   player_cli <in-file|folder|playlist> <out.wav> [--rate N] [--eq autoeq.txt]
//              [--rg track|album] [--crossfeed] [--volume dB] [--gapless-folder]
//
// Input: an audio file, a folder (its audio files in display order), an .m3u/.m3u8 playlist
// or a .cue sheet. Output: 24-bit PCM at the rate of the first track (later tracks are
// resampled to it, as a fixed-rate output would do), or with --rate N a 16-bit file at N Hz
// with TPDF dither, like the Bluetooth output. DoP (DSF/DFF) goes into a 24-bit DoP WAV when
// it is the first track and --rate is not given. --gapless-folder opens each next file ahead
// (gapless mode, the device default); without it gapless mode is off.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "audio_player/events.h"
#include "audio_player/fsbrowse.h"
#include "audio_player/player.h"
#include "audio_player/playlist.h"
#include "player_sinks.h"

#define MAX_ITEMS PLAYER_QUEUE_MAX

typedef struct {
    const char *in, *out, *eq;
    uint32_t rate;
    rg_mode_t rg;
    bool crossfeed, gapless;
    float volume_db;
} cli_opts_t;

static player_t *g_player;
static int g_errors;

static void usage(void) {
    fprintf(stderr,
            "usage: player_cli <in-file|folder|playlist> <out.wav> [--rate N] [--eq autoeq.txt]\n"
            "                  [--rg track|album] [--crossfeed] [--volume dB] [--gapless-folder]\n"
            "  --rate N          16-bit output at N Hz with dither (Bluetooth-like); default 24-bit at the\n"
            "                    first track's rate\n"
            "  --eq FILE         AutoEQ ParametricEQ.txt profile\n"
            "  --rg MODE         ReplayGain: track or album\n"
            "  --crossfeed       headphone crossfeed (700 Hz, 4.5 dB)\n"
            "  --volume dB       software volume, <= 0 (default 0)\n"
            "  --gapless-folder  gapless mode: open the next file ahead\n");
}

static void print_flags(uint32_t f) {
    static const char *const names[] = {"RG", "EQ", "crossfeed", "volume", "limiter", "dither", "resample"};
    bool any = false;
    for (unsigned i = 0; i < CORE_ARRAY_SIZE(names); i++) {
        if (f & (1u << i)) {
            printf("%s%s", any ? "+" : "", names[i]);
            any = true;
        }
    }
    if (!any) printf("none");
}

static void on_event(const core_event_t *ev, void *user) {
    (void)user;
    player_status_t st;
    switch (ev->type) {
    case EV_TRACK_CHANGED:
        player_get_status(g_player, &st);
        printf("[%u/%u] %s%s%s  %s %u Hz %u bit %s", (unsigned)st.queue_index + 1, (unsigned)st.queue_length,
               st.artist, st.artist[0] ? " - " : "", st.title, codec_name(st.codec),
               (unsigned)st.src_fmt.sample_rate, (unsigned)st.src_fmt.bits,
               st.src_fmt.channels == 1 ? "mono" : "stereo");
        if (st.duration_ms) printf(", %u:%02u", (unsigned)(st.duration_ms / 60000), (unsigned)(st.duration_ms / 1000 % 60));
        printf("\n");
        break;
    case EV_PLAYBACK_ERROR:
        g_errors++;
        fprintf(stderr, "error: %s: %s\n", ev->text, core_err_name(ev->a));
        break;
    default:
        break;
    }
}

static bool is_dir(const char *path) {
    struct stat s;
    return stat(path, &s) == 0 && (s.st_mode & S_IFMT) == S_IFDIR;
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(65537);
    size_t n = buf ? fread(buf, 1, 65536, f) : 0;
    fclose(f);
    if (buf) buf[n] = 0;
    return buf;
}

// Normalises a path to forward slashes (the core expects '/').
static void slashes(char *p) {
    for (; *p; p++) {
        if (*p == '\\') *p = '/';
    }
}

static uint32_t items_from_playlist(playlist_t *pl, queue_item_t *items, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; pl && i < playlist_count(pl) && n < max; i++) {
        const playlist_entry_t *e = playlist_get(pl, i);
        if (!e) continue;
        items[n].track_id = LIB_ID_NONE;
        core_strlcpy(items[n].path, e->path, sizeof items[n].path);
        items[n].start_ms = e->start_ms;
        items[n].end_ms = e->end_ms;
        n++;
    }
    return n;
}

static uint32_t build_items(const char *in, queue_item_t *items, uint32_t max) {
    if (is_dir(in)) {
        fs_listing_t *l = fs_list_dir(in, max);
        if (!l) return 0;
        char (*paths)[CORE_PATH_MAX] = malloc((size_t)max * CORE_PATH_MAX);
        uint32_t n = paths ? fs_listing_audio_paths(l, in, paths, max) : 0;
        if (n > max) n = max;
        for (uint32_t i = 0; i < n; i++) {
            items[i].track_id = LIB_ID_NONE;
            core_strlcpy(items[i].path, paths[i], sizeof items[i].path);
        }
        free(paths);
        fs_listing_free(l);
        return n;
    }
    if (core_str_ends_with_ci(in, ".cue")) {
        playlist_t *pl = playlist_load_cue(in);
        uint32_t n = items_from_playlist(pl, items, max);
        playlist_free(pl);
        return n;
    }
    if (core_str_ends_with_ci(in, ".m3u") || core_str_ends_with_ci(in, ".m3u8")) {
        playlist_t *pl = playlist_load_m3u(in);
        uint32_t n = items_from_playlist(pl, items, max);
        playlist_free(pl);
        return n;
    }
    items[0].track_id = LIB_ID_NONE;
    core_strlcpy(items[0].path, in, sizeof items[0].path);
    return 1;
}

static int parse_args(int argc, char **argv, cli_opts_t *o) {
    memset(o, 0, sizeof *o);
    o->rg = RG_OFF;
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has_val = i + 1 < argc;
        if (!strcmp(a, "--rate") && has_val) {
            long r = strtol(argv[++i], NULL, 10);
            if (r < 8000 || r > 384000) return -1;
            o->rate = (uint32_t)r;
        } else if (!strcmp(a, "--eq") && has_val) {
            o->eq = argv[++i];
        } else if (!strcmp(a, "--rg") && has_val) {
            const char *m = argv[++i];
            if (!strcmp(m, "track")) {
                o->rg = RG_TRACK;
            } else if (!strcmp(m, "album")) {
                o->rg = RG_ALBUM;
            } else if (!strcmp(m, "auto")) {
                o->rg = RG_AUTO;
            } else {
                return -1;
            }
        } else if (!strcmp(a, "--crossfeed")) {
            o->crossfeed = true;
        } else if (!strcmp(a, "--volume") && has_val) {
            o->volume_db = strtof(argv[++i], NULL);
            if (o->volume_db > 0.0f) o->volume_db = 0.0f;
        } else if (!strcmp(a, "--gapless-folder")) {
            o->gapless = true;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            return -1;
        } else if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "unknown option %s\n", a);
            return -1;
        } else if (pos == 0) {
            o->in = a;
            pos++;
        } else if (pos == 1) {
            o->out = a;
            pos++;
        } else {
            return -1;
        }
    }
    return (o->in && o->out) ? 0 : -1;
}

int main(int argc, char **argv) {
    cli_opts_t o;
    if (parse_args(argc, argv, &o) != 0) {
        usage();
        return 2;
    }
    char in[CORE_PATH_MAX];
    core_strlcpy(in, o.in, sizeof in);
    slashes(in);
    size_t il = strlen(in);
    while (il > 1 && in[il - 1] == '/') in[--il] = 0;

    dsp_config_t dsp;
    dsp_config_defaults(&dsp);
    dsp.rg_mode = o.rg;
    dsp.crossfeed_enabled = o.crossfeed;
    if (o.eq) {
        char *text = read_text(o.eq);
        if (!text || eq_preset_parse_autoeq(text, &dsp.eq) != CORE_OK) {
            fprintf(stderr, "cannot read the AutoEQ profile %s\n", o.eq);
            free(text);
            return 1;
        }
        free(text);
        dsp.eq_enabled = true;
    }

    queue_item_t *items = calloc(MAX_ITEMS, sizeof *items);
    uint32_t n = items ? build_items(in, items, MAX_ITEMS) : 0;
    if (n == 0) {
        fprintf(stderr, "nothing to play in %s\n", in);
        free(items);
        return 1;
    }

    wav_sink_t wav;
    if (wav_sink_open(&wav, o.out, o.rate, o.rate ? 16 : 24) != CORE_OK) {
        fprintf(stderr, "cannot create %s\n", o.out);
        free(items);
        return 1;
    }
    player_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.sink = &wav.sink;
    cfg.volume_default_db = o.volume_db;
    cfg.volume_max_db = 0.0f;
    cfg.volume_step_db = 1.0f;
    g_player = player_create(&cfg);
    if (!g_player) {
        fprintf(stderr, "out of memory\n");
        wav_sink_finish(&wav);
        free(items);
        return 1;
    }
    core_events_set_sink(on_event, NULL);
    player_cmd_set_dsp(g_player, &dsp);
    player_cmd_set_gapless(g_player, o.gapless);
    player_cmd_play_items(g_player, items, n, 0, false);
    free(items);

    bool started = false, shown = false;
    player_status_t st;
    for (uint64_t iter = 0;; iter++) {
        player_run_once(g_player);
        player_get_status(g_player, &st);
        if (st.state == PLAYER_PLAYING) started = true;
        if (st.state == PLAYER_PLAYING && !shown && st.out_fmt.sample_rate) {
            printf("output: %s, %u Hz %u bit%s, processing: ", st.sink_detail, (unsigned)st.out_fmt.sample_rate,
                   (unsigned)st.out_fmt.bits, st.out_fmt.dop ? " DoP" : "");
            print_flags(st.dsp_flags);
            printf("%s\n", st.bitperfect ? ", bit-perfect" : "");
            shown = true;
        }
        if (st.state != PLAYER_PLAYING && (started || iter > 1000)) break;
    }
    player_destroy(g_player);
    core_events_set_sink(NULL, NULL);
    uint64_t frames = wav.frames;
    uint32_t rate = wav.rate ? wav.rate : 44100u;
    int r = wav_sink_finish(&wav);
    printf("%s: %llu frames, %.2f s, %u Hz, %u bit%s\n", o.out, (unsigned long long)frames, (double)frames / rate,
           (unsigned)rate, (unsigned)wav.out_bits, wav.dop ? " (DoP)" : "");
    if (r != CORE_OK) {
        fprintf(stderr, "writing %s failed: %s\n", o.out, core_err_name(r));
        return 1;
    }
    return (frames == 0 || g_errors) ? 1 : 0;
}
