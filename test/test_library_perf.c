// SPDX-License-Identifier: Apache-2.0
// Library timing and memory on 5 000 generated tracks (100 artists x 5 albums x 10
// tracks). Prints scan, load and query times and the RAM of the loaded index, measured
// with a counting allocator installed through core_set_allocator().
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define rmdir_ _rmdir
#else
#include <unistd.h>
#define rmdir_ rmdir
#endif

#include "audio_player/collate.h"
#include "audio_player/library.h"
#include "library/lib_priv.h"
#include "test.h"

#define BASE "tmp_library/perf"
#define ROOT BASE "/music"
#define DB BASE "/db"
#define N_ARTISTS 100
#define N_ALBUMS 5
#define N_TRACKS 10
#define N_TOTAL (N_ARTISTS * N_ALBUMS * N_TRACKS)

// ------------------------------------------------------ counting allocator ----
typedef struct {
    size_t size;
    size_t pad;
} ahdr_t;
static size_t g_cur, g_peak;

static void *t_alloc(size_t n) {
    ahdr_t *h = malloc(sizeof *h + n);
    if (!h) return NULL;
    h->size = n;
    g_cur += n;
    if (g_cur > g_peak) g_peak = g_cur;
    return h + 1;
}
static void *t_realloc(void *p, size_t n) {
    if (!p) return t_alloc(n);
    ahdr_t *h = (ahdr_t *)p - 1;
    size_t old = h->size;
    ahdr_t *nh = realloc(h, sizeof *nh + n);
    if (!nh) return NULL;
    nh->size = n;
    g_cur = g_cur - old + n;
    if (g_cur > g_peak) g_peak = g_cur;
    return nh + 1;
}
static void t_free(void *p) {
    if (!p) return;
    ahdr_t *h = (ahdr_t *)p - 1;
    g_cur -= h->size;
    free(h);
}

// ---------------------------------------------------------------- helpers ----
static void rm_rf(const char *path) {
    lfs_stat_t st;
    if (lfs_stat(path, &st) != CORE_OK) return;
    if (!st.is_dir) {
        remove(path);
        return;
    }
    char (*names)[CORE_PATH_MAX] = malloc(1024 * CORE_PATH_MAX);
    int n = 0;
    lfs_dir_t *d = lfs_opendir(path);
    lfs_dirent_t e;
    while (d && n < 1024 && lfs_readdir(d, &e)) core_strlcpy(names[n++], e.name, CORE_PATH_MAX);
    lfs_closedir(d);
    char child[CORE_PATH_MAX * 2];
    for (int i = 0; i < n; i++) {
        snprintf(child, sizeof child, "%s/%s", path, names[i]);
        rm_rf(child);
    }
    free(names);
    rmdir_(path);
}

static uint8_t g_buf[8192];

static void put_frame(size_t *n, const char *id, const char *text) {
    size_t len = strlen(text) + 1;
    memcpy(g_buf + *n, id, 4);
    g_buf[*n + 4] = 0;
    g_buf[*n + 5] = 0;
    g_buf[*n + 6] = (uint8_t)((len >> 7) & 0x7F);
    g_buf[*n + 7] = (uint8_t)(len & 0x7F);
    g_buf[*n + 8] = g_buf[*n + 9] = 0;
    g_buf[*n + 10] = 3;
    memcpy(g_buf + *n + 11, text, len - 1);
    *n += 10 + len;
}

static void make_track(const char *path, int a, int b, int t) {
    static const char *const words[] = {"Love", "Night", "Road", "Fire", "Rain", "Dream", "Звезда", "Ночь",
                                        "Дорога", "Солнце", "Ветер", "Море", "Heart", "Light", "Time", "Город"};
    char title[64], artist[64], album[64], num[16];
    snprintf(title, sizeof title, "%s %s %d", words[(a * 7 + b * 3 + t) % 16], words[(a + t * 5) % 16], t + 1);
    if (a % 2) snprintf(artist, sizeof artist, "Исполнитель %03d", a);
    else snprintf(artist, sizeof artist, "Artist %03d", a);
    snprintf(album, sizeof album, "Album %d of %s", b + 1, artist);
    size_t n = 10;
    memcpy(g_buf, "ID3\x04\x00\x00\x00\x00\x00\x00", 10);
    put_frame(&n, "TIT2", title);
    put_frame(&n, "TPE1", artist);
    put_frame(&n, "TALB", album);
    put_frame(&n, "TCON", (a % 3) ? "Rock" : "Pop");
    snprintf(num, sizeof num, "%d", 1970 + a % 50);
    put_frame(&n, "TDRC", num);
    snprintf(num, sizeof num, "%d/%d", t + 1, N_TRACKS);
    put_frame(&n, "TRCK", num);
    uint32_t size = (uint32_t)(n - 10);
    g_buf[8] = (uint8_t)((size >> 7) & 0x7F);
    g_buf[9] = (uint8_t)(size & 0x7F);
    memset(g_buf + n, 0, 417 * 2);
    memcpy(g_buf + n, "\xFF\xFB\x90\x00", 4);
    memcpy(g_buf + n + 417, "\xFF\xFB\x90\x00", 4);
    n += 417 * 2;
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(g_buf, 1, n, f);
        fclose(f);
    }
}

static library_t *open_lib(void) {
    library_config_t cfg = {ROOT, DB, true, NULL, NULL};
    return library_open(&cfg);
}

static double per_row_us(uint32_t ms, uint32_t rows) { return rows ? (double)ms * 1000.0 / rows : 0.0; }

TEST(perf_5000) {
    rm_rf(BASE);
    char path[CORE_PATH_MAX];
    uint32_t t0 = core_now_ms();
    for (int a = 0; a < N_ARTISTS; a++) {
        for (int b = 0; b < N_ALBUMS; b++) {
            snprintf(path, sizeof path, ROOT "/a%03d/b%d", a, b);
            lfs_mkdirs(path);
            for (int t = 0; t < N_TRACKS; t++) {
                snprintf(path, sizeof path, ROOT "/a%03d/b%d/t%02d.mp3", a, b, t);
                make_track(path, a, b, t);
            }
        }
    }
    printf("  generated %d files in %u ms\n", N_TOTAL, (unsigned)(core_now_ms() - t0));

    static const core_allocator_t alloc = {t_alloc, t_alloc, t_realloc, t_free};
    core_set_allocator(&alloc);
    size_t base = g_cur;

    library_t *lib = open_lib();
    CHECK(lib != NULL);
    if (!lib) return;
    size_t empty = g_cur - base;
    g_peak = g_cur;
    t0 = core_now_ms();
    CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_OK);
    uint32_t t_scan = core_now_ms() - t0;
    size_t scan_peak = g_peak - base;
    size_t snap = g_cur - base;
    CHECK_EQ_INT(library_track_count(lib), N_TOTAL);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE), N_ARTISTS);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE), N_ARTISTS * N_ALBUMS);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_TRACKS, LIB_ID_NONE), N_TOTAL);

    g_peak = g_cur;
    t0 = core_now_ms();
    CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_OK);
    uint32_t t_rescan = core_now_ms() - t0;
    size_t rescan_peak = g_peak - base;

    library_close(lib);
    t0 = core_now_ms();
    lib = open_lib();
    uint32_t t_open = core_now_ms() - t0;
    CHECK_EQ_INT(library_track_count(lib), N_TOTAL);

    // Queries.
    lib_item_t it, prev;
    t0 = core_now_ms();
    for (uint32_t i = 0; i < N_TOTAL; i++) {
        CHECK_EQ_INT(library_view_item(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, i, &it), CORE_OK);
        if (i) CHECK(collate_cmp_primary(prev.title, it.title, &(collate_opts_t){true}) <= 0);
        prev = it;
    }
    uint32_t t_tracks = core_now_ms() - t0;
    t0 = core_now_ms();
    for (uint32_t i = 0; i < N_TOTAL; i++) library_view_item(lib, LIB_VIEW_RECENT, LIB_ID_NONE, i, &it);
    uint32_t t_recent = core_now_ms() - t0;
    t0 = core_now_ms();
    for (int rep = 0; rep < 10; rep++) {
        for (uint32_t i = 0; i < N_ARTISTS * N_ALBUMS; i++) {
            library_view_item(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE, i, &it);
        }
    }
    uint32_t t_albums = core_now_ms() - t0;
    t0 = core_now_ms();
    uint32_t found = 0;
    for (int a = 0; a < N_ARTISTS; a++) {
        for (int b = 0; b < N_ALBUMS; b++) {
            for (int t = 0; t < N_TRACKS; t++) {
                snprintf(path, sizeof path, ROOT "/a%03d/b%d/t%02d.mp3", a, b, t);
                if (library_find_path(lib, path) != LIB_ID_NONE) found++;
            }
        }
    }
    uint32_t t_find = core_now_ms() - t0;
    CHECK_EQ_INT(found, N_TOTAL);
    t0 = core_now_ms();
    for (uint32_t l = 'A'; l <= 'Z'; l++) library_view_find_letter(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, l);
    for (uint32_t l = 0x0410; l <= 0x042F; l++) library_view_find_letter(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, l);
    for (uint32_t l = 'A'; l <= 'Z'; l++) library_view_find_letter(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE, l);
    uint32_t t_letters = core_now_ms() - t0;
    static lib_id_t ids[N_TOTAL];
    t0 = core_now_ms();
    CHECK_EQ_INT(library_view_track_ids(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, ids, N_TOTAL), N_TOTAL);
    uint32_t t_ids = core_now_ms() - t0;
    library_close(lib);
    core_set_allocator(NULL);

    const size_t cache = 32 * 4096;
    double per_track = (double)(snap - empty) / N_TOTAL;
    printf("  first scan      %6u ms  (%.2f ms per file)\n", (unsigned)t_scan, (double)t_scan / N_TOTAL);
    printf("  rescan, no change %4u ms\n", (unsigned)t_rescan);
    printf("  open (load)     %6u ms\n", (unsigned)t_open);
    printf("  TRACKS rows     %6u ms  (%.1f us per row, 5000 rows)\n", (unsigned)t_tracks,
           per_row_us(t_tracks, N_TOTAL));
    printf("  RECENT rows     %6u ms  (%.1f us per row)\n", (unsigned)t_recent, per_row_us(t_recent, N_TOTAL));
    printf("  ALBUMS rows     %6u ms  (%.1f us per row, 5000 rows)\n", (unsigned)t_albums, per_row_us(t_albums, 5000));
    printf("  find_path       %6u ms  (%.1f us per lookup)\n", (unsigned)t_find, per_row_us(t_find, N_TOTAL));
    printf("  letter jumps    %6u ms  (84 jumps)\n", (unsigned)t_letters);
    printf("  all track ids   %6u ms\n", (unsigned)t_ids);
    printf("  RAM: empty library %zu B (block cache %zu B), loaded index %zu B = %.1f B per track\n", empty,
           cache, snap,
           per_track);
    printf("  RAM: scan peak %zu B (first scan), %zu B (rescan with the old index loaded)\n", scan_peak, rescan_peak);
    double scan_per_track = (double)(rescan_peak - empty) / N_TOTAL;
    printf("  RAM: rescan needs %.1f B per track on top of the fixed %zu B (old index included)\n",
           scan_per_track, empty);
    printf("  RAM estimate for 30 000 tracks: index %.0f KiB, rescan peak %.0f KiB\n",
           (empty + per_track * 30000) / 1024.0, (empty + scan_per_track * 30000) / 1024.0);
    CHECK(empty + per_track * 30000 < 1.5 * 1024 * 1024);
}

TEST_MAIN(RUN(perf_5000))
