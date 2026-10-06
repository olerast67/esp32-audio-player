// SPDX-License-Identifier: Apache-2.0
// Library index: scan of a generated tree (~190 tagged files), views, sort order,
// letter jumps, incremental rescan, id translation, cancel (also while counting and while
// sorting), pacing, retry of unreadable files, crash safety, positions, lock discipline.
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <sys/utime.h>
#define rmdir_ _rmdir
static void set_mtime(const char *p, time_t t) {
    struct _utimbuf u = {t, t};
    _utime(p, &u);
}
#else
#include <unistd.h>
#include <utime.h>
#define rmdir_ rmdir
static void set_mtime(const char *p, time_t t) {
    struct utimbuf u = {t, t};
    utime(p, &u);
}
#endif

#include "audio_player/collate.h"
#include "audio_player/library.h"
#include "library/lib_db.h"
#include "test.h"

#define BASE "tmp_library/idx"
#define ROOT BASE "/music"
#define DB ROOT "/.player/db"

// ---------------------------------------------------------------- helpers ----
static void rm_rf(const char *path) {
    lfs_stat_t st;
    if (lfs_stat(path, &st) != CORE_OK) return;
    if (!st.is_dir) {
        remove(path);
        return;
    }
    char (*names)[CORE_PATH_MAX] = malloc(4096 * CORE_PATH_MAX);
    int n = 0;
    lfs_dir_t *d = lfs_opendir(path);
    lfs_dirent_t e;
    while (d && n < 4096 && lfs_readdir(d, &e)) core_strlcpy(names[n++], e.name, CORE_PATH_MAX);
    lfs_closedir(d);
    char child[CORE_PATH_MAX * 2];
    for (int i = 0; i < n; i++) {
        snprintf(child, sizeof child, "%s/%s", path, names[i]);
        rm_rf(child);
    }
    free(names);
    rmdir_(path);
}

static void write_file(const char *path, const void *data, size_t len) {
    char dir[CORE_PATH_MAX];
    core_path_dirname(path, dir, sizeof dir);
    lfs_mkdirs(dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("  cannot write %s\n", path);
        return;
    }
    if (len) fwrite(data, 1, len, f);
    fclose(f);
}

static bool copy_file(const char *from, const char *to) {
    FILE *f = fopen(from, "rb");
    if (!f) return false;
    FILE *t = fopen(to, "wb");
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, t);
    fclose(f);
    fclose(t);
    return true;
}

static const char *const k_db_names[] = {"tracks.bin", "strings.bin", "files.bin",   "entities.bin",
                                         "tracks.idx", "recent.idx",  "albums.idx",  "albtrk.idx",
                                         "galbums.idx", "books.idx",  "info.bin"};

// Index files carry their generation: "tracks.bin" is "<db>/tracks-0a1b2c3d.bin". Writes the
// path of the first match to out (may be NULL) and returns the number of matches.
static int db_file(const char *db, const char *name, char *out, size_t cap) {
    const char *dot = strchr(name, '.');
    const size_t bl = (size_t)(dot - name);
    int n = 0;
    lfs_dir_t *d = lfs_opendir(db);
    lfs_dirent_t e;
    while (d && lfs_readdir(d, &e)) {
        if (strlen(e.name) == bl + 9 + strlen(dot) && strncmp(e.name, name, bl) == 0 && e.name[bl] == '-' &&
            strcmp(e.name + bl + 9, dot) == 0) {
            if (n++ == 0 && out) snprintf(out, cap, "%s/%s", db, e.name);
        }
    }
    lfs_closedir(d);
    return n;
}

// Path of the only file of a kind ("" when there is none or several).
static const char *db_path(const char *name) {
    static char out[4][CORE_PATH_MAX];
    static int slot;
    char *o = out[slot++ % 4];
    if (db_file(DB, name, o, CORE_PATH_MAX) != 1) o[0] = 0;
    return o;
}

// Index files in db (all kinds, all generations).
static int db_count(const char *db) {
    int n = 0;
    for (size_t i = 0; i < sizeof k_db_names / sizeof *k_db_names; i++) n += db_file(db, k_db_names[i], NULL, 0);
    return n;
}

static void flip_byte(const char *path, long off) {
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    fseek(f, off, SEEK_SET);
    int c = fgetc(f);
    fseek(f, off, SEEK_SET);
    fputc(c ^ 0x5A, f);
    fclose(f);
}

typedef struct {
    const char *title, *artist, *album, *album_artist, *genre;
    int year, track, disc;
} song_t;

static uint8_t g_buf[1 << 16];

static void put_frame(size_t *n, const char *id, const char *text) {
    size_t len = strlen(text) + 1;
    memcpy(g_buf + *n, id, 4);
    g_buf[*n + 4] = (uint8_t)((len >> 21) & 0x7F);
    g_buf[*n + 5] = (uint8_t)((len >> 14) & 0x7F);
    g_buf[*n + 6] = (uint8_t)((len >> 7) & 0x7F);
    g_buf[*n + 7] = (uint8_t)(len & 0x7F);
    g_buf[*n + 8] = g_buf[*n + 9] = 0;
    g_buf[*n + 10] = 3;  // UTF-8
    memcpy(g_buf + *n + 11, text, len - 1);
    *n += 10 + len;
}

// MP3 with an ID3v2.4 tag (s may be NULL: no tag) and three 128 kbps frames (78 ms).
static void make_mp3(const char *path, const song_t *s) {
    size_t n = 0;
    if (s) {
        memcpy(g_buf, "ID3\x04\x00\x00\x00\x00\x00\x00", 10);
        n = 10;
        char num[16];
        if (s->title) put_frame(&n, "TIT2", s->title);
        if (s->artist) put_frame(&n, "TPE1", s->artist);
        if (s->album) put_frame(&n, "TALB", s->album);
        if (s->album_artist) put_frame(&n, "TPE2", s->album_artist);
        if (s->genre) put_frame(&n, "TCON", s->genre);
        if (s->year) {
            snprintf(num, sizeof num, "%d", s->year);
            put_frame(&n, "TDRC", num);
        }
        if (s->track) {
            snprintf(num, sizeof num, "%d", s->track);
            put_frame(&n, "TRCK", num);
        }
        if (s->disc) {
            snprintf(num, sizeof num, "%d/2", s->disc);
            put_frame(&n, "TPOS", num);
        }
        uint32_t size = (uint32_t)(n - 10);
        g_buf[6] = (uint8_t)((size >> 21) & 0x7F);
        g_buf[7] = (uint8_t)((size >> 14) & 0x7F);
        g_buf[8] = (uint8_t)((size >> 7) & 0x7F);
        g_buf[9] = (uint8_t)(size & 0x7F);
    }
    for (int f = 0; f < 3; f++) {
        memset(g_buf + n, 0, 417);
        memcpy(g_buf + n, "\xFF\xFB\x90\x00", 4);
        n += 417;
    }
    write_file(path, g_buf, n);
}

static int g_files, g_books;
static time_t g_time = 1600000000;

static void song(const char *rel, const char *title, const char *artist, const char *album, const char *aa,
                 const char *genre, int year, int track, int disc) {
    char path[CORE_PATH_MAX];
    snprintf(path, sizeof path, ROOT "/%s", rel);
    song_t s = {title, artist, album, aa, genre, year, track, disc};
    make_mp3(path, &s);
    set_mtime(path, g_time++);
    g_files++;
}

static const char *const k_latin[20] = {"ABBA",        "Björk",   "Cars",       "Depeche Mode", "Eagles",
                                        "Franz Ferdinand", "Genesis", "Hurts",   "Interpol",     "Jamiroquai",
                                        "Kraftwerk",   "Linkin Park", "Muse",   "Nirvana",      "Oasis",
                                        "Queen",       "Radiohead", "Sting",    "U2",           "Zaz"};
static const char *const k_cyr[7] = {"Ария", "Би-2", "ДДТ", "Земфира", "Мумий Тролль", "Сплин", "Чайф"};
static const char *const k_words[] = {"Alpha",  "Bravo", "Charlie", "Delta", "Echo",  "Весна", "Дождь", "Звезда",
                                      "Кукушка", "Мама",  "Небо",    "Осень", "Птица", "Река",  "Солнце", "Ягода"};
static const char *const k_genres[3] = {"Pop", "Rock", "Jazz"};

static void build_tree(void) {
    char rel[CORE_PATH_MAX], title[64], album[64];
    for (int i = 0; i < 20; i++) {
        snprintf(album, sizeof album, "Album of %s", k_latin[i]);
        for (int k = 1; k <= 5; k++) {
            snprintf(rel, sizeof rel, "lat/%02d/t%d.mp3", i, k);
            snprintf(title, sizeof title, "%s %d", k_words[(i * 5 + k) % 16], k);
            song(rel, title, k_latin[i], album, NULL, k_genres[i % 3], 2000 + i, k, 0);
        }
    }
    for (int i = 0; i < 7; i++) {
        snprintf(album, sizeof album, "Альбом %d", i + 1);
        for (int k = 1; k <= 4; k++) {
            snprintf(rel, sizeof rel, "cyr/%02d/t%d.mp3", i, k);
            snprintf(title, sizeof title, "%s %d", k_words[5 + (i + k) % 11], k);
            song(rel, title, k_cyr[i], album, NULL, "Рок", 1990 + i, k, 0);
        }
    }
    // Pink Floyd: files named against the track order, a two-disc album, a case variant.
    const char *dsotm[] = {"The Great Gig in the Sky", "Time", "On the Run", "Breathe", "Speak to Me"};
    for (int k = 0; k < 5; k++) {
        snprintf(rel, sizeof rel, "pf/dsotm/%c.mp3", 'a' + k);
        song(rel, dsotm[k], "Pink Floyd", "The Dark Side of the Moon", NULL, "Rock", 1973, 5 - k, 0);
    }
    for (int k = 0; k < 6; k++) {
        snprintf(rel, sizeof rel, "pf/wall/%d.mp3", k);
        snprintf(title, sizeof title, "Wall d%d t%d", 2 - k / 3, 3 - k % 3);
        song(rel, title, "Pink Floyd", "The Wall", "Pink Floyd", "Rock", 1979, 3 - k % 3, 2 - k / 3);
    }
    for (int k = 1; k <= 4; k++) {
        snprintf(rel, sizeof rel, "pf/animals/%d.mp3", k);
        snprintf(title, sizeof title, "Animals %d", k);
        song(rel, title, "PINK FLOYD", "Animals", NULL, "Rock", 1977, k, 0);
    }
    for (int k = 1; k <= 5; k++) {
        snprintf(rel, sizeof rel, "beatles/abbey/%d.mp3", k);
        snprintf(title, sizeof title, "Abbey %d", k);
        song(rel, title, "The Beatles", "Abbey Road", NULL, "Rock", 1969, k, 0);
    }
    for (int k = 1; k <= 5; k++) {
        snprintf(rel, sizeof rel, "kino/krovi/%d.mp3", k);
        snprintf(title, sizeof title, "Кровь %d", k);
        song(rel, title, "Кино", "Группа крови", NULL, "Рок", 1988, k, 0);
    }
    for (int k = 1; k <= 5; k++) {
        snprintf(rel, sizeof rel, "kino/zvezda/%d.mp3", k);
        snprintf(title, sizeof title, "Звезда %d", k);
        song(rel, title, "Кино", "Звезда по имени Солнце", NULL, "Рок", 1989, k, 0);
    }
    for (int k = 1; k <= 3; k++) {
        snprintf(rel, sizeof rel, "yolka/a/%d.mp3", k);
        song(rel, "Прованс", "Ёлка", "Прованс", NULL, "Pop", 2011, k, 0);
        snprintf(rel, sizeof rel, "yolka/b/%d.mp3", k);
        song(rel, "Точки", "Елка", "Точки расставлены", NULL, "Pop", 2011, k, 0);
    }
    for (int k = 1; k <= 4; k++) {
        snprintf(rel, sizeof rel, "2pac/%d.mp3", k);
        snprintf(title, sizeof title, "Eyez %d", k);
        song(rel, title, "2Pac", "All Eyez on Me", NULL, "Hip-Hop", 1996, k, 0);
    }
    for (int k = 1; k <= 6; k++) {
        char artist[32];
        snprintf(rel, sizeof rel, "comp/%d.mp3", k);
        snprintf(artist, sizeof artist, "Singer %d", k);
        snprintf(title, sizeof title, "Hit %d", k);
        song(rel, title, artist, "Hits 2000", "Various Artists", "Pop", 2000, k, 0);
    }
    const char *untagged[] = {"untagged/01 Intro.mp3", "untagged/02 Song.mp3", "untagged/03 Outro.mp3"};
    for (int k = 0; k < 3; k++) {
        char path[CORE_PATH_MAX];
        snprintf(path, sizeof path, ROOT "/%s", untagged[k]);
        make_mp3(path, NULL);
        set_mtime(path, g_time++);
        g_files++;
    }
    for (int k = 1; k <= 10; k++) {
        snprintf(rel, sizeof rel, "Audiobooks/Master/ch%02d.mp3", k);
        snprintf(title, sizeof title, "Глава %d", k);
        song(rel, title, "Булгаков", "Мастер и Маргарита", NULL, NULL, 1967, k, 0);
        g_books++;
    }
    for (int k = 1; k <= 3; k++) {
        snprintf(rel, sizeof rel, "books2/Book A/%d.mp3", k);
        snprintf(title, sizeof title, "Chapter %d", k);
        song(rel, title, "Author A", "Book A", NULL, "Audiobook", 2020, k, 0);
        g_books++;
    }
    // Newest files: the Kino "Zvezda" album.
    for (int k = 1; k <= 5; k++) {
        snprintf(rel, sizeof rel, ROOT "/kino/zvezda/%d.mp3", k);
        set_mtime(rel, g_time + 100000 + k);
    }
    // Never indexed.
    song_t junk = {"Secret", "Hidden", NULL, NULL, NULL, 0, 0, 0};
    make_mp3(ROOT "/.hidden/secret.mp3", &junk);
    make_mp3(ROOT "/System Volume Information/x.mp3", &junk);
    make_mp3(DB "/../stray.mp3", &junk);  // inside the hidden .player folder
    write_file(ROOT "/misc/cover.jpg", "\xFF\xD8\xFF", 3);
    write_file(ROOT "/misc/notes.txt", "hello", 5);
}

// ------------------------------------------------------------ lock check ----
typedef struct {
    int depth;
} tlock_t;
static int g_lock_errors, g_lock_count;
static void *tl_create(void) { return calloc(1, sizeof(tlock_t)); }
static void tl_lock(void *m) {
    tlock_t *l = m;
    if (l->depth) g_lock_errors++;  // the library never locks recursively
    l->depth++;
    g_lock_count++;
}
static void tl_unlock(void *m) {
    tlock_t *l = m;
    if (l->depth != 1) g_lock_errors++;
    l->depth--;
}
static void tl_destroy(void *m) { free(m); }

// ---------------------------------------------------------------- state ----
static library_t *g_lib;
static int g_throttle;
static lib_scan_progress_t g_last;
static uint32_t g_view_during_scan = 0xFFFFFFFF;
static int g_cancel_after;
// Pacing of the finishing phase: throttle calls after the last file, and cancels there.
static bool g_walk_done;
static int g_throttle_after_walk, g_polls_counting, g_cancel_counting, g_cancel_finishing;
static bool g_files_in_ram = true;

static void throttle(void *user) {
    (void)user;
    g_throttle++;
    if (g_walk_done) g_throttle_after_walk++;
}

static bool on_progress(const lib_scan_progress_t *p, void *user) {
    library_t *lib = user;
    CHECK(p->current_path != NULL);
    if (!p->current_path) return true;
    if (p->files_total == 0) {
        // Pass 1 (counting): polled once per folder, nothing to report yet.
        CHECK_EQ_INT(p->files_seen, 0);
        g_polls_counting++;
        return !(g_cancel_counting && g_polls_counting >= g_cancel_counting);
    }
    if (p->current_path[0] && p->files_seen == p->files_total) g_walk_done = true;
    if (!p->current_path[0] && g_walk_done && g_cancel_finishing) return false;
    if (p->current_path[0] && lib) {
        // Every file is looked up in RAM, without a files.bin handle (FAT has few).
        core_mutex_lock(lib->mutex);
        if (lib->snap.loaded && lib->snap.info.tracks && (!lib->snap.files_mem || lib->snap.f[DBF_FILES])) {
            g_files_in_ram = false;
        }
        core_mutex_unlock(lib->mutex);
    }
    g_last = *p;
    if (p->files_seen == 3 && lib) {
        // Queries keep working on the old snapshot while the scan runs.
        g_view_during_scan = library_view_count(lib, LIB_VIEW_TRACKS, LIB_ID_NONE);
        lib_item_t it;
        if (g_view_during_scan) CHECK_EQ_INT(library_view_item(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, 0, &it), CORE_OK);
        CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_EAGAIN);  // one scan at a time
        CHECK(library_is_scanning(lib));
    }
    if (g_cancel_after && (int)p->files_seen >= g_cancel_after) return false;
    return true;
}

static library_t *open_lib(const char *db, bool articles) {
    library_config_t cfg = {ROOT, db, articles, throttle, NULL};
    return library_open(&cfg);
}

static uint32_t find_row(library_t *lib, lib_view_t v, lib_id_t parent, const char *title) {
    uint32_t n = library_view_count(lib, v, parent);
    lib_item_t it;
    for (uint32_t i = 0; i < n; i++) {
        if (library_view_item(lib, v, parent, i, &it) == CORE_OK && strcmp(it.title, title) == 0) return i;
    }
    return UINT32_MAX;
}

static lib_id_t find_id(library_t *lib, lib_view_t v, lib_id_t parent, const char *title) {
    uint32_t r = find_row(lib, v, parent, title);
    lib_item_t it;
    if (r == UINT32_MAX || library_view_item(lib, v, parent, r, &it) != CORE_OK) return LIB_ID_NONE;
    return it.id;
}

// Rows are in collation order of their titles and jump letters never go backwards.
static void check_sorted(library_t *lib, lib_view_t v, lib_id_t parent, const collate_opts_t *o) {
    uint32_t n = library_view_count(lib, v, parent);
    lib_item_t prev, cur;
    for (uint32_t i = 0; i < n; i++) {
        CHECK_EQ_INT(library_view_item(lib, v, parent, i, &cur), CORE_OK);
        CHECK_EQ_INT(cur.sort_letter, collate_index_letter(cur.title, o));
        if (i) {
            int c = collate_cmp_primary(prev.title, cur.title, o);
            if (c > 0) {
                printf("  view %d rows %u/%u out of order: \"%s\" > \"%s\"\n", v, i - 1, i, prev.title,
                       cur.title);
            }
            CHECK(c <= 0);
            CHECK(collate_letter_rank(prev.sort_letter) <= collate_letter_rank(cur.sort_letter));
        }
        prev = cur;
    }
}

// library_view_find_letter() == first row whose letter is >= the letter.
static void check_letters(library_t *lib, lib_view_t v, lib_id_t parent) {
    static const uint32_t letters[] = {'#', 'A', 'B', 'E', 'K', 'P', 'T', 'Y', 'Z', 0x0410, 0x0415, 0x0401,
                                       0x0417, 0x041A, 0x041C, 0x0427, 0x042F, 0x03A9};
    uint32_t n = library_view_count(lib, v, parent);
    for (size_t k = 0; k < sizeof letters / sizeof *letters; k++) {
        uint32_t rank = collate_letter_rank(letters[k]);
        uint32_t expect = n;
        lib_item_t it;
        for (uint32_t i = 0; i < n; i++) {
            library_view_item(lib, v, parent, i, &it);
            if (collate_letter_rank(it.sort_letter) >= rank) {
                expect = i;
                break;
            }
        }
        uint32_t got = library_view_find_letter(lib, v, parent, letters[k]);
        if (got != expect) printf("  view %d letter U+%04X: got %u, want %u\n", v, (unsigned)letters[k], got, expect);
        CHECK_EQ_INT(got, expect);
    }
}

// ------------------------------------------------------------------ tests ----
TEST(setup) {
    static const core_lock_ops_t ops = {tl_create, tl_lock, tl_unlock, tl_destroy};
    core_set_lock_ops(&ops);
    rm_rf("tmp_library/idx");
    build_tree();
    CHECK_EQ_INT(g_files, 190);
    CHECK(library_open(NULL) == NULL);
}

TEST(first_scan) {
    g_lib = open_lib(DB, false);
    CHECK(g_lib != NULL);
    if (!g_lib) return;
    CHECK_EQ_INT(library_track_count(g_lib), 0);  // nothing indexed yet
    CHECK_EQ_INT(library_view_count(g_lib, LIB_VIEW_ARTISTS, LIB_ID_NONE), 0);
    g_throttle = 0;
    CHECK_EQ_INT(library_scan(g_lib, false, on_progress, g_lib), CORE_OK);
    CHECK(!library_is_scanning(g_lib));
    CHECK_EQ_INT(g_view_during_scan, 0);  // the old (empty) snapshot served queries during the scan
    CHECK(g_throttle > g_files);  // once per file, plus once per folder while counting
    CHECK_EQ_INT(g_last.files_total, g_files);
    CHECK_EQ_INT(g_last.files_seen, g_files);
    CHECK_EQ_INT(g_last.tracks_added, g_files);
    CHECK_EQ_INT(g_last.tracks_updated, 0);
    CHECK_EQ_INT(g_last.tracks_removed, 0);
    CHECK_EQ_INT(library_track_count(g_lib), g_files);
    // One generation: every kind once, named after the snapshot.
    CHECK_EQ_INT(db_count(DB), 11);
    CHECK(db_path("tracks.bin")[0] && db_path("info.bin")[0] && db_path("albtrk.idx")[0]);
    char name[64];
    snprintf(name, sizeof name, DB "/info-%08lx.bin", (unsigned long)library_generation(g_lib));
    CHECK_STR(db_path("info.bin"), name);
    CHECK(library_generation(g_lib) != 0);
}

TEST(views) {
    library_t *lib = g_lib;
    if (!lib) return;
    const int music = g_files - g_books;
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_TRACKS, LIB_ID_NONE), music);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_RECENT, LIB_ID_NONE), music);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE), 34);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE), 38);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_GENRES, LIB_ID_NONE), 6);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_AUDIOBOOKS, LIB_ID_NONE), 2);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_ALBUM_TRACKS, 999999), 0);
    CHECK_EQ_INT(library_view_count(lib, (lib_view_t)77, LIB_ID_NONE), 0);
    lib_item_t it;
    CHECK_EQ_INT(library_view_item(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, 34, &it), CORE_ENOTFOUND);

    // Artists: unknown first, digits, Latin, Cyrillic; case and ё variants merged.
    static const char *const artists[] = {
        "",         "2Pac",         "ABBA",  "Björk",   "Cars",     "Depeche Mode", "Eagles", "Franz Ferdinand",
        "Genesis",  "Hurts",        "Interpol", "Jamiroquai", "Kraftwerk", "Linkin Park", "Muse", "Nirvana",
        "Oasis",    "Pink Floyd",   "Queen", "Radiohead", "Sting",    "The Beatles",  "U2",     "Various Artists",
        "Zaz",      "Ария",         "Би-2",  "ДДТ",     "Ёлка",     "Земфира",      "Кино",   "Мумий Тролль",
        "Сплин",    "Чайф"};
    for (uint32_t i = 0; i < 34; i++) {
        CHECK_EQ_INT(library_view_item(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, i, &it), CORE_OK);
        CHECK_STR(it.title, artists[i]);
    }
    CHECK_EQ_INT(find_row(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, "Булгаков"), UINT32_MAX);  // audiobooks only
    check_sorted(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, NULL);
    check_sorted(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE, NULL);
    check_sorted(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, NULL);
    check_sorted(lib, LIB_VIEW_GENRES, LIB_ID_NONE, NULL);
    check_sorted(lib, LIB_VIEW_AUDIOBOOKS, LIB_ID_NONE, NULL);

    // Pink Floyd: three albums by year, 15 tracks, merged with "PINK FLOYD".
    lib_id_t pf = find_id(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, "Pink Floyd");
    CHECK(pf != LIB_ID_NONE);
    uint32_t pf_row = find_row(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, "Pink Floyd");
    library_view_item(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, pf_row, &it);
    CHECK_EQ_INT(it.count, 3);
    CHECK_EQ_INT(it.duration_ms, 15 * 78);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_ARTIST_ALBUMS, pf), 3);
    const char *pf_albums[] = {"The Dark Side of the Moon", "Animals", "The Wall"};
    const int pf_years[] = {1973, 1977, 1979};
    for (uint32_t i = 0; i < 3; i++) {
        library_view_item(lib, LIB_VIEW_ARTIST_ALBUMS, pf, i, &it);
        CHECK_STR(it.title, pf_albums[i]);
        CHECK_EQ_INT(it.year, pf_years[i]);
        CHECK_STR(it.subtitle, "Pink Floyd");
    }
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_ARTIST_TRACKS, pf), 15);
    lib_id_t dsotm = find_id(lib, LIB_VIEW_ARTIST_ALBUMS, pf, "The Dark Side of the Moon");
    library_view_item(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE,
                      find_row(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE, "The Dark Side of the Moon"), &it);
    CHECK_EQ_INT(it.id, dsotm);
    CHECK_EQ_INT(it.count, 5);
    CHECK_EQ_INT(it.duration_ms, 5 * 78);
    CHECK_EQ_INT(it.year, 1973);
    const char *order[] = {"Speak to Me", "Breathe", "On the Run", "Time", "The Great Gig in the Sky"};
    lib_id_t ids[32];
    CHECK_EQ_INT(library_view_track_ids(lib, LIB_VIEW_ALBUM_TRACKS, dsotm, ids, 32), 5);
    for (uint32_t i = 0; i < 5; i++) {
        library_view_item(lib, LIB_VIEW_ALBUM_TRACKS, dsotm, i, &it);
        CHECK_STR(it.title, order[i]);
        CHECK_EQ_INT(it.id, ids[i]);
        CHECK_STR(it.subtitle, "Pink Floyd");
        CHECK_EQ_INT(it.duration_ms, 78);
    }
    library_view_item(lib, LIB_VIEW_ARTIST_TRACKS, pf, 0, &it);
    CHECK_STR(it.title, "Speak to Me");
    // Not sorted by title: first row at or past the letter in track order.
    CHECK_EQ_INT(library_view_find_letter(lib, LIB_VIEW_ALBUM_TRACKS, dsotm, 'T'), 3);
    lib_id_t wall = find_id(lib, LIB_VIEW_ARTIST_ALBUMS, pf, "The Wall");
    const char *wall_order[] = {"Wall d1 t1", "Wall d1 t2", "Wall d1 t3", "Wall d2 t1", "Wall d2 t2", "Wall d2 t3"};
    for (uint32_t i = 0; i < 6; i++) {
        library_view_item(lib, LIB_VIEW_ALBUM_TRACKS, wall, i, &it);
        CHECK_STR(it.title, wall_order[i]);
    }

    // Track details and path lookup.
    lib_track_t t;
    CHECK_EQ_INT(library_get_track(lib, ids[0], &t), CORE_OK);
    CHECK_STR(t.path, ROOT "/pf/dsotm/e.mp3");
    CHECK_STR(t.tags.title, "Speak to Me");
    CHECK_STR(t.tags.artist, "Pink Floyd");
    CHECK_STR(t.tags.album, "The Dark Side of the Moon");
    CHECK_STR(t.tags.album_artist, "Pink Floyd");
    CHECK_STR(t.tags.genre, "Rock");
    CHECK_EQ_INT(t.tags.year, 1973);
    CHECK_EQ_INT(t.tags.track_no, 1);
    CHECK_EQ_INT(t.tags.codec, CODEC_MP3);
    CHECK_EQ_INT(t.tags.duration_ms, 78);
    CHECK_EQ_INT(t.tags.sample_rate, 44100);
    CHECK_EQ_INT(t.album_id, dsotm);
    CHECK_EQ_INT(t.artist_id, pf);
    CHECK(t.file_size > 1000);
    CHECK(t.mtime > 0);
    CHECK(isnan(t.tags.rg_track_gain_db));
    CHECK_EQ_INT(library_find_path(lib, ROOT "/pf/dsotm/e.mp3"), ids[0]);
    CHECK_EQ_INT(library_find_path(lib, ROOT "/pf/dsotm/none.mp3"), LIB_ID_NONE);
    CHECK_EQ_INT(library_find_path(lib, ROOT "/.hidden/secret.mp3"), LIB_ID_NONE);
    CHECK_EQ_INT(library_get_track(lib, 100000, &t), CORE_ENOTFOUND);
    // Every indexed path resolves to its own id.
    int bad = 0;
    for (uint32_t id = 0; id < library_track_count(lib); id++) {
        if (library_get_track(lib, id, &t) != CORE_OK || library_find_path(lib, t.path) != id) bad++;
    }
    CHECK_EQ_INT(bad, 0);

    // Unknown artist / album: file names as titles.
    library_view_item(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, 0, &it);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_ARTIST_ALBUMS, it.id), 1);
    lib_item_t alb;
    library_view_item(lib, LIB_VIEW_ARTIST_ALBUMS, it.id, 0, &alb);
    CHECK_STR(alb.title, "");
    CHECK_EQ_INT(alb.count, 3);
    library_view_item(lib, LIB_VIEW_ALBUM_TRACKS, alb.id, 0, &it);
    CHECK_STR(it.title, "01 Intro");
    CHECK_STR(it.subtitle, "");

    // Merged "Ёлка" / "Елка", compilation with per-track artists.
    lib_id_t yolka = find_id(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, "Ёлка");
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_ARTIST_ALBUMS, yolka), 2);
    lib_id_t hits = find_id(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE, "Hits 2000");
    library_view_item(lib, LIB_VIEW_ALBUM_TRACKS, hits, 2, &it);
    CHECK_STR(it.subtitle, "Singer 3");

    // Genres.
    const char *genres[] = {"", "Hip-Hop", "Jazz", "Pop", "Rock", "Рок"};
    for (uint32_t i = 0; i < 6; i++) {
        library_view_item(lib, LIB_VIEW_GENRES, LIB_ID_NONE, i, &it);
        CHECK_STR(it.title, genres[i]);
    }
    lib_id_t rock = find_id(lib, LIB_VIEW_GENRES, LIB_ID_NONE, "Rock");
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_GENRE_ALBUMS, rock), 11);
    check_sorted(lib, LIB_VIEW_GENRE_ALBUMS, rock, NULL);
    CHECK_EQ_INT(library_view_track_ids(lib, LIB_VIEW_GENRE_ALBUMS, rock, NULL, 0), 7 * 5 + 15 + 5);
    CHECK_EQ_INT(library_view_track_ids(lib, LIB_VIEW_GENRES, LIB_ID_NONE, NULL, 0), music);
    CHECK_EQ_INT(library_view_track_ids(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, NULL, 0), music);
    CHECK_EQ_INT(library_view_track_ids(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE, NULL, 0), music);
    CHECK_EQ_INT(library_view_track_ids(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, NULL, 0), music);
    CHECK_EQ_INT(library_view_track_ids(lib, LIB_VIEW_ARTIST_ALBUMS, pf, NULL, 0), 15);
    CHECK_EQ_INT(library_view_track_ids(lib, LIB_VIEW_AUDIOBOOKS, LIB_ID_NONE, NULL, 0), g_books);
    CHECK_EQ_INT(find_row(lib, LIB_VIEW_GENRES, LIB_ID_NONE, "Audiobook"), UINT32_MAX);

    // Audiobooks: by genre and by folder name; not in the music views.
    library_view_item(lib, LIB_VIEW_AUDIOBOOKS, LIB_ID_NONE, 0, &it);
    CHECK_STR(it.title, "Book A");
    CHECK_STR(it.subtitle, "Author A");
    library_view_item(lib, LIB_VIEW_AUDIOBOOKS, LIB_ID_NONE, 1, &it);
    CHECK_STR(it.title, "Мастер и Маргарита");
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_ALBUM_TRACKS, it.id), 10);
    CHECK_EQ_INT(find_row(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, "Глава 1"), UINT32_MAX);
    CHECK_EQ_INT(find_row(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE, "Book A"), UINT32_MAX);
    lib_item_t ch;
    library_view_item(lib, LIB_VIEW_ALBUM_TRACKS, it.id, 0, &ch);
    CHECK_EQ_INT(library_get_track(lib, ch.id, &t), CORE_OK);
    CHECK(t.tags.is_audiobook_hint);

    // Recent: newest files first.
    for (uint32_t i = 0; i < 5; i++) {
        library_view_item(lib, LIB_VIEW_RECENT, LIB_ID_NONE, i, &it);
        library_get_track(lib, it.id, &t);
        CHECK(strstr(t.path, "kino/zvezda") != NULL);
    }
    uint32_t prev_mtime = UINT32_MAX;
    for (uint32_t i = 0; i < (uint32_t)music; i++) {
        library_view_item(lib, LIB_VIEW_RECENT, LIB_ID_NONE, i, &it);
        library_get_track(lib, it.id, &t);
        CHECK(t.mtime <= prev_mtime);
        prev_mtime = t.mtime;
    }

    // Letter jumps.
    library_view_item(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE,
                      library_view_find_letter(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, 'K'), &it);
    CHECK_STR(it.title, "Kraftwerk");
    library_view_item(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE,
                      library_view_find_letter(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, 0x0401), &it);  // Ё
    CHECK_STR(it.title, "Ёлка");
    CHECK_EQ_INT(library_view_find_letter(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, 0x042F), 34);  // Я: past the end
    check_letters(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE);
    check_letters(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE);
    check_letters(lib, LIB_VIEW_TRACKS, LIB_ID_NONE);
    check_letters(lib, LIB_VIEW_GENRES, LIB_ID_NONE);
    check_letters(lib, LIB_VIEW_GENRE_ALBUMS, rock);
    check_letters(lib, LIB_VIEW_AUDIOBOOKS, LIB_ID_NONE);
    check_letters(lib, LIB_VIEW_ARTIST_ALBUMS, pf);
    check_letters(lib, LIB_VIEW_ALBUM_TRACKS, dsotm);
}

TEST(articles_option) {
    library_t *lib = open_lib(BASE "/db_articles", true);
    CHECK(lib != NULL);
    if (!lib) return;
    CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_OK);
    collate_opts_t o = {true};
    lib_item_t it;
    library_view_item(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, 3, &it);
    CHECK_STR(it.title, "The Beatles");
    CHECK_EQ_INT(it.sort_letter, 'B');
    check_sorted(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, &o);
    check_sorted(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE, &o);
    check_sorted(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, &o);
    check_letters(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE);
    check_letters(lib, LIB_VIEW_TRACKS, LIB_ID_NONE);
    // "The Dark Side of the Moon" files under D.
    uint32_t d_row = library_view_find_letter(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE, 'D');
    library_view_item(lib, LIB_VIEW_ALBUMS, LIB_ID_NONE, d_row, &it);
    CHECK_STR(it.title, "The Dark Side of the Moon");
    library_close(lib);

    // Option changed in the settings: until the next scan, queries follow the order the
    // index was built with, so letters and jumps stay consistent.
    lib = open_lib(BASE "/db_articles", false);
    library_view_item(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, 3, &it);
    CHECK_STR(it.title, "The Beatles");
    CHECK_EQ_INT(it.sort_letter, 'B');
    check_letters(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE);
    CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_OK);
    library_view_item(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, find_row(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, "The Beatles"),
                      &it);
    CHECK_EQ_INT(it.sort_letter, 'T');
    check_sorted(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, NULL);
    check_letters(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE);
    library_close(lib);
}

// Path of every track id of the current snapshot.
static char (*all_paths(library_t *lib, uint32_t *n))[CORE_PATH_MAX] {
    *n = library_track_count(lib);
    char (*p)[CORE_PATH_MAX] = calloc(*n + 1, CORE_PATH_MAX);
    lib_track_t *t = malloc(sizeof *t);
    for (uint32_t i = 0; p && t && i < *n; i++) {
        if (library_get_track(lib, i, t) == CORE_OK) core_strlcpy(p[i], t->path, CORE_PATH_MAX);
    }
    free(t);
    return p;
}

TEST(incremental_rescan) {
    library_t *lib = g_lib;
    if (!lib) return;
    const uint32_t gen0 = library_generation(lib);
    uint32_t n0 = 0;
    char (*paths0)[CORE_PATH_MAX] = all_paths(lib, &n0);
    g_files_in_ram = true;
    // Modify one file (longer tag), delete one, add one.
    song_t s = {"Time (Remastered 2011)", "Pink Floyd", "The Dark Side of the Moon", NULL, "Rock", 1973, 4, 0};
    make_mp3(ROOT "/pf/dsotm/b.mp3", &s);
    remove(ROOT "/lat/00/t1.mp3");
    song_t n = {"Brand New", "Newcomer", "Debut", NULL, "Pop", 2024, 1, 0};
    make_mp3(ROOT "/new/debut/01.mp3", &n);
    memset(&g_last, 0, sizeof g_last);
    g_view_during_scan = 0xFFFFFFFF;
    CHECK_EQ_INT(library_scan(lib, false, on_progress, lib), CORE_OK);
    CHECK_EQ_INT(g_view_during_scan, g_files - g_books);  // old snapshot during the scan
    CHECK_EQ_INT(g_last.tracks_added, 1);
    CHECK_EQ_INT(g_last.tracks_updated, 1);
    CHECK_EQ_INT(g_last.tracks_removed, 1);
    CHECK_EQ_INT(g_last.files_seen, g_files);
    CHECK_EQ_INT(library_track_count(lib), g_files);
    CHECK(find_row(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, "Time (Remastered 2011)") != UINT32_MAX);
    CHECK_EQ_INT(find_row(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, "Time"), UINT32_MAX);
    CHECK(find_row(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, "Newcomer") != UINT32_MAX);
    CHECK_EQ_INT(library_find_path(lib, ROOT "/lat/00/t1.mp3"), LIB_ID_NONE);
    CHECK(library_find_path(lib, ROOT "/new/debut/01.mp3") != LIB_ID_NONE);
    CHECK(g_files_in_ram);
    CHECK_EQ_INT(db_count(DB), 11);  // the old generation is gone

    // Ids of the previous snapshot follow their files (the queue keeps ids across scans):
    // "lat/00/t1" is gone, "new/..." sorts before "pf/..." so later ids moved.
    const uint32_t gen1 = library_generation(lib);
    CHECK(gen1 != gen0);
    lib_id_t *ids = malloc((n0 + 1) * sizeof *ids);
    for (uint32_t i = 0; i < n0; i++) ids[i] = i;
    uint32_t g = gen0;
    CHECK(library_translate_ids(lib, &g, ids, n0));
    CHECK_EQ_INT(g, gen1);
    int moved = 0, lost = 0, wrong = 0;
    lib_track_t *tr = malloc(sizeof *tr);
    for (uint32_t i = 0; i < n0; i++) {
        if (ids[i] == LIB_ID_NONE) {
            lost++;
            CHECK_STR(paths0[i], ROOT "/lat/00/t1.mp3");
            continue;
        }
        moved += ids[i] != i;
        if (library_get_track(lib, ids[i], tr) != CORE_OK || strcmp(tr->path, paths0[i]) != 0) wrong++;
    }
    CHECK_EQ_INT(lost, 1);
    CHECK_EQ_INT(wrong, 0);
    CHECK(moved > 0);
    g = gen1;  // current ids stay as they are
    lib_id_t one = 5;
    CHECK(library_translate_ids(lib, &g, &one, 1));
    CHECK_EQ_INT(one, 5);
    CHECK_EQ_INT(g, gen1);
    free(paths0);
    lib_id_t abba = find_id(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE, "ABBA");
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_ARTIST_TRACKS, abba), 4);
    check_sorted(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, NULL);

    // No changes: everything is reused.
    memset(&g_last, 0, sizeof g_last);
    uint32_t t0 = core_now_ms();
    CHECK_EQ_INT(library_scan(lib, false, on_progress, lib), CORE_OK);
    printf("  rescan without changes: %u ms for %d files\n", (unsigned)(core_now_ms() - t0), g_files);
    CHECK_EQ_INT(g_last.tracks_added + g_last.tracks_updated + g_last.tracks_removed, 0);
    CHECK_EQ_INT(library_track_count(lib), g_files);
    // Two snapshots later, ids of gen0 cannot be translated any more: nothing changes.
    g = gen0;
    one = 7;
    CHECK(!library_translate_ids(lib, &g, &one, 1));
    CHECK_EQ_INT(g, gen0);
    CHECK_EQ_INT(one, 7);
    free(ids);
    free(tr);

    // Full rebuild parses everything again.
    memset(&g_last, 0, sizeof g_last);
    CHECK_EQ_INT(library_scan(lib, true, on_progress, lib), CORE_OK);
    CHECK_EQ_INT(g_last.tracks_added, 0);
    CHECK_EQ_INT(g_last.tracks_updated, g_files);
    CHECK_EQ_INT(library_track_count(lib), g_files);

    // Cancel: the old snapshot stays, the files of the unfinished one are deleted.
    remove(ROOT "/new/debut/01.mp3");
    const uint32_t gen_before = library_generation(lib);
    g_cancel_after = 10;
    CHECK_EQ_INT(library_scan(lib, false, on_progress, lib), CORE_EAGAIN);
    g_cancel_after = 0;
    CHECK_EQ_INT(library_track_count(lib), g_files);
    CHECK(library_find_path(lib, ROOT "/new/debut/01.mp3") != LIB_ID_NONE);
    CHECK_EQ_INT(db_count(DB), 11);
    CHECK_EQ_INT(library_generation(lib), gen_before);

    // Cancel while counting (pass 1 polls once per folder): stops before any file is read.
    g_polls_counting = 0;
    g_cancel_counting = 3;
    memset(&g_last, 0, sizeof g_last);
    CHECK_EQ_INT(library_scan(lib, false, on_progress, lib), CORE_EAGAIN);
    g_cancel_counting = 0;
    CHECK_EQ_INT(g_polls_counting, 3);
    CHECK_EQ_INT(g_last.files_seen, 0);
    CHECK_EQ_INT(db_count(DB), 11);

    // Pacing of the finishing phase (sorts, views): the throttle runs after the last file
    // too (at least once per stage), and a cancel there keeps the old snapshot.
    g_walk_done = false;
    g_throttle_after_walk = 0;
    CHECK_EQ_INT(library_scan(lib, true, on_progress, lib), CORE_OK);
    CHECK(g_walk_done);
    CHECK(g_throttle_after_walk >= 5);
    g_walk_done = false;
    g_cancel_finishing = 1;
    CHECK_EQ_INT(library_scan(lib, true, on_progress, lib), CORE_EAGAIN);
    g_cancel_finishing = 0;
    g_walk_done = false;
    CHECK_EQ_INT(library_track_count(lib), g_files - 1);
    CHECK(library_find_path(lib, ROOT "/new/debut/01.mp3") == LIB_ID_NONE);
    CHECK_EQ_INT(db_count(DB), 11);
    make_mp3(ROOT "/new/debut/01.mp3", &n);
    CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_OK);
    CHECK_EQ_INT(library_track_count(lib), g_files);
}

// Holds a file so it cannot be opened (another program has it locked, or no permission):
// its tags are not readable for now, which is not the same as a file without tags.
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
static int lock_file(const char *path) {
    int fd = -1;
    _sopen_s(&fd, path, _O_RDONLY | _O_BINARY, _SH_DENYRW, _S_IREAD);
    return fd;
}
static void unlock_file(const char *path, int fd) {
    (void)path;
    if (fd >= 0) _close(fd);
}
#else
#include <sys/stat.h>
static int lock_file(const char *path) { return chmod(path, 0) == 0 ? 0 : -1; }
static void unlock_file(const char *path, int fd) {
    if (fd >= 0) chmod(path, 0644);
}
#endif

TEST(unreadable_file_is_retried) {
    library_t *lib = g_lib;
    if (!lib) return;
    const char *path = ROOT "/pf/dsotm/c.mp3";  // "On the Run"
    song_t s = {"On the Run (2023)", "Pink Floyd", "The Dark Side of the Moon", NULL, "Rock", 1973, 3, 0};
    make_mp3(path, &s);  // changed: the next scan must read it
    int fd = lock_file(path);
    CHECK(fd >= 0);
    FILE *probe = fopen(path, "rb");
    const bool locked = probe == NULL;
    if (probe) fclose(probe);  // running as root: the file stays readable, nothing to test
    memset(&g_last, 0, sizeof g_last);
    CHECK_EQ_INT(library_scan(lib, false, on_progress, lib), CORE_OK);
    lib_track_t *t = malloc(sizeof *t);
    lib_id_t id = library_find_path(lib, path);
    CHECK(id != LIB_ID_NONE);
    if (locked && t && library_get_track(lib, id, t) == CORE_OK) {
        // The old record's tags, not an empty "c" record that would stick.
        CHECK_STR(t->tags.title, "On the Run");
        CHECK_STR(t->tags.artist, "Pink Floyd");
    }
    unlock_file(path, fd);
    // Size and mtime are the same as at the failed read: the record is parsed again anyway.
    memset(&g_last, 0, sizeof g_last);
    CHECK_EQ_INT(library_scan(lib, false, on_progress, lib), CORE_OK);
    CHECK_EQ_INT(g_last.tracks_updated, locked ? 1 : 0);
    id = library_find_path(lib, path);
    if (t && library_get_track(lib, id, t) == CORE_OK) CHECK_STR(t->tags.title, "On the Run (2023)");
    // And then it is reused like any other file.
    memset(&g_last, 0, sizeof g_last);
    CHECK_EQ_INT(library_scan(lib, false, on_progress, lib), CORE_OK);
    CHECK_EQ_INT(g_last.tracks_updated, 0);
    free(t);
}

TEST(crash_safety) {
    library_close(g_lib);
    g_lib = NULL;
    const uint32_t total = (uint32_t)g_files;

    // 1. Damaged small file: the index loads empty and the next scan rebuilds it.
    flip_byte(db_path("entities.bin"), 200);
    library_t *lib = open_lib(DB, false);
    CHECK(lib != NULL);
    CHECK_EQ_INT(library_track_count(lib), 0);
    CHECK_EQ_INT(library_generation(lib), 0);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_ARTISTS, LIB_ID_NONE), 0);
    memset(&g_last, 0, sizeof g_last);
    CHECK_EQ_INT(library_scan(lib, false, on_progress, NULL), CORE_OK);
    CHECK_EQ_INT(g_last.tracks_added, total);
    CHECK_EQ_INT(library_track_count(lib), total);
    CHECK_EQ_INT(db_count(DB), 11);  // the damaged generation was deleted
    library_close(lib);

    // 2. Damaged block of tracks.bin: detected when read; the next scan re-reads the files.
    flip_byte(db_path("tracks.bin"), 32 + 84 * 3 + 10);
    lib = open_lib(DB, false);
    CHECK_EQ_INT(library_track_count(lib), total);
    lib_track_t t;
    CHECK_EQ_INT(library_get_track(lib, 3, &t), CORE_ECORRUPT);
    memset(&g_last, 0, sizeof g_last);
    CHECK_EQ_INT(library_scan(lib, false, on_progress, NULL), CORE_OK);
    // Nothing is reused from the damaged snapshot: every file is parsed again (records in
    // the bad block cannot even be found, so they count as added).
    CHECK_EQ_INT(g_last.tracks_added + g_last.tracks_updated, total);
    CHECK(g_last.tracks_added > 0 && g_last.tracks_updated > 0);
    CHECK_EQ_INT(library_get_track(lib, 3, &t), CORE_OK);
    library_close(lib);

    // 3. Truncated strings.bin.
    FILE *f = fopen(db_path("strings.bin"), "r+b");
    char head[64];
    size_t hn = f ? fread(head, 1, sizeof head, f) : 0;
    if (f) fclose(f);
    f = fopen(db_path("strings.bin"), "wb");
    if (f) {
        fwrite(head, 1, hn, f);
        fclose(f);
    }
    lib = open_lib(DB, false);
    CHECK_EQ_INT(library_track_count(lib), 0);
    CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_OK);
    CHECK_EQ_INT(library_track_count(lib), total);

    // 4. Power loss after a commit, before the old files were deleted: two complete
    //    generations. The one with the higher commit sequence is the index.
    static char saved[11][CORE_PATH_MAX], back[11][CORE_PATH_MAX];
    const uint32_t gen_a = library_generation(lib);
    for (int i = 0; i < 11; i++) {
        core_strlcpy(saved[i], db_path(k_db_names[i]), CORE_PATH_MAX);
        snprintf(back[i], CORE_PATH_MAX, BASE "/saved_%s", k_db_names[i]);
        CHECK(copy_file(saved[i], back[i]));
    }
    remove(ROOT "/comp/6.mp3");
    CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_OK);  // generation b without comp/6
    const uint32_t gen_b = library_generation(lib);
    CHECK(gen_b != gen_a);
    library_close(lib);
    for (int i = 0; i < 11; i++) CHECK(copy_file(back[i], saved[i]));  // a is back next to b
    CHECK_EQ_INT(db_count(DB), 22);
    lib = open_lib(DB, false);
    CHECK_EQ_INT(library_generation(lib), gen_b);
    CHECK_EQ_INT(library_track_count(lib), total - 1);
    CHECK_EQ_INT(db_count(DB), 11);
    library_close(lib);

    // 5. The newest commit record is damaged (cut while written): the previous complete
    //    generation is used. Leftovers of an unfinished build and of the old fixed-name
    //    layout are deleted.
    for (int i = 0; i < 11; i++) CHECK(copy_file(back[i], saved[i]));
    char info_b[CORE_PATH_MAX];
    snprintf(info_b, sizeof info_b, DB "/info-%08lx.bin", (unsigned long)gen_b);
    f = fopen(info_b, "r+b");
    if (f) {
        fseek(f, 40, SEEK_SET);
        fputc('X', f);
        fclose(f);
    }
    write_file(DB "/tracks-0badf00d.bin", "garbage", 7);
    write_file(DB "/tracks.bin", "old layout", 10);
    write_file(DB "/tmp/info.bin", "old layout", 10);
    lib = open_lib(DB, false);
    CHECK_EQ_INT(library_generation(lib), gen_a);
    CHECK_EQ_INT(library_track_count(lib), total);
    CHECK(library_find_path(lib, ROOT "/pf/dsotm/e.mp3") != LIB_ID_NONE);
    CHECK_EQ_INT(db_count(DB), 11);
    CHECK(!lfs_exists(DB "/tracks.bin") && !lfs_exists(DB "/tmp/info.bin") && !lfs_exists(DB "/tmp"));
    // The library follows the card again (comp/6 is gone), then the file comes back.
    CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_OK);
    CHECK_EQ_INT(library_track_count(lib), total - 1);
    song_t c6 = {"Hit 6", "Singer 6", "Hits 2000", "Various Artists", "Pop", 2000, 6, 0};
    make_mp3(ROOT "/comp/6.mp3", &c6);
    CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_OK);
    CHECK_EQ_INT(library_track_count(lib), total);
    library_close(lib);
    for (int i = 0; i < 11; i++) remove(back[i]);

    // 6. Files from different generations never mix.
    lib = open_lib(BASE "/db_articles", true);
    library_close(lib);
    char other[CORE_PATH_MAX];
    CHECK_EQ_INT(db_file(BASE "/db_articles", "recent.idx", other, sizeof other), 1);
    CHECK(copy_file(other, db_path("recent.idx")));
    lib = open_lib(DB, false);
    CHECK_EQ_INT(library_track_count(lib), 0);
    CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_OK);
    CHECK_EQ_INT(library_track_count(lib), total);
    g_lib = lib;
}

TEST(positions) {
    library_t *lib = g_lib;
    if (!lib) return;
    CHECK_EQ_INT(library_load_position(lib, "/sdcard/Books/ch01.mp3"), 0);
    CHECK_EQ_INT(library_save_position(lib, "/sdcard/Books/ch01.mp3", 12345), CORE_OK);
    CHECK_EQ_INT(library_load_position(lib, "/sdcard/Books/ch01.mp3"), 12345);
    CHECK_EQ_INT(library_save_position(lib, "/sdcard/Books/ch01.mp3", 20000), CORE_OK);
    CHECK_EQ_INT(library_save_position(lib, "/sdcard/Books/ch02.mp3", 777), CORE_OK);
    CHECK_EQ_INT(library_save_position(lib, "", 1), CORE_EINVAL);
    library_close(lib);
    lib = open_lib(DB, false);
    CHECK_EQ_INT(library_load_position(lib, "/sdcard/Books/ch01.mp3"), 20000);
    CHECK_EQ_INT(library_load_position(lib, "/sdcard/Books/ch02.mp3"), 777);
    CHECK_EQ_INT(library_save_position(lib, "/sdcard/Books/ch02.mp3", 0), CORE_OK);  // forget
    CHECK_EQ_INT(library_load_position(lib, "/sdcard/Books/ch02.mp3"), 0);
    // Capacity: the least recently saved positions are dropped.
    char p[64];
    for (int i = 0; i < 1030; i++) {
        snprintf(p, sizeof p, "/sdcard/many/%d.mp3", i);
        library_save_position(lib, p, 1000u + (uint32_t)i);
    }
    CHECK_EQ_INT(library_load_position(lib, "/sdcard/Books/ch01.mp3"), 0);
    CHECK_EQ_INT(library_load_position(lib, "/sdcard/many/0.mp3"), 0);
    CHECK_EQ_INT(library_load_position(lib, "/sdcard/many/1029.mp3"), 2029);
    CHECK_EQ_INT(library_load_position(lib, "/sdcard/many/10.mp3"), 1010);
    library_close(lib);
    // Crash between "remove" and "rename" of the atomic save: the .tmp file is used.
    CHECK_EQ_INT(lfs_replace(DB "/positions.bin", DB "/positions.bin.tmp"), CORE_OK);
    lib = open_lib(DB, false);
    CHECK_EQ_INT(library_load_position(lib, "/sdcard/many/1029.mp3"), 2029);
    // Corrupt file: positions start empty, nothing crashes.
    library_close(lib);
    write_file(DB "/positions.bin", "PLDB garbage", 12);
    remove(DB "/positions.bin.tmp");
    lib = open_lib(DB, false);
    CHECK_EQ_INT(library_load_position(lib, "/sdcard/many/1029.mp3"), 0);
    g_lib = lib;
}

TEST(teardown) {
    library_close(g_lib);
    g_lib = NULL;
    CHECK_EQ_INT(g_lock_errors, 0);
    CHECK(g_lock_count > 1000);
    core_set_lock_ops(NULL);
    // Empty music folder: a valid empty index.
    lfs_mkdirs(BASE "/empty");
    library_config_t cfg = {BASE "/empty", BASE "/empty_db", false, NULL, NULL};
    library_t *lib = library_open(&cfg);
    CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_OK);
    CHECK_EQ_INT(library_track_count(lib), 0);
    CHECK_EQ_INT(library_view_count(lib, LIB_VIEW_TRACKS, LIB_ID_NONE), 0);
    CHECK_EQ_INT(library_view_find_letter(lib, LIB_VIEW_TRACKS, LIB_ID_NONE, 'A'), 0);
    library_close(lib);
    lib = library_open(&cfg);
    CHECK_EQ_INT(library_track_count(lib), 0);
    library_close(lib);
    // Missing music folder (card not mounted): the scan fails, the old index stays.
    library_config_t bad = {BASE "/no_such_dir", DB, false, NULL, NULL};
    lib = library_open(&bad);
    CHECK_EQ_INT(library_track_count(lib), g_files);
    CHECK_EQ_INT(library_scan(lib, false, NULL, NULL), CORE_EIO);
    CHECK_EQ_INT(library_track_count(lib), g_files);
    CHECK_EQ_INT(db_count(DB), 11);
    library_close(lib);
}

TEST_MAIN(RUN(setup) RUN(first_scan) RUN(views) RUN(articles_option) RUN(incremental_rescan)
              RUN(unreadable_file_is_retried) RUN(crash_safety) RUN(positions) RUN(teardown))
