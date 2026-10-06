// SPDX-License-Identifier: Apache-2.0
// Folder browsing (fsbrowse) and playlists (M3U/M3U8 read and write, CUE sheets).
// Files are created under the build directory (tmp_library/browse).
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define rmdir_ _rmdir
#define getcwd_ _getcwd
#else
#include <unistd.h>
#define rmdir_ rmdir
#define getcwd_ getcwd
#endif

#include "audio_player/fsbrowse.h"
#include "audio_player/playlist.h"
#include "library/lib_priv.h"
#include "test.h"

#define ROOT "tmp_library/browse"

static void rm_rf(const char *path) {
    lfs_stat_t st;
    if (lfs_stat(path, &st) != CORE_OK) return;
    if (!st.is_dir) {
        remove(path);
        return;
    }
    char (*names)[CORE_PATH_MAX] = malloc(2048 * CORE_PATH_MAX);
    int n = 0;
    lfs_dir_t *d = lfs_opendir(path);
    lfs_dirent_t e;
    while (d && n < 2048 && lfs_readdir(d, &e)) core_strlcpy(names[n++], e.name, CORE_PATH_MAX);
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

static void write_text(const char *path, const char *text) { write_file(path, text, strlen(text)); }

static size_t read_text(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

// ------------------------------------------------------------ fsbrowse ----
TEST(list_and_sort) {
    const char *dir = ROOT "/dir";
    const char *dirs[] = {"a", "B", "c10", "c9", ".hidden", "System Volume Information", "$RECYCLE.BIN"};
    char p[CORE_PATH_MAX];
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, dirs[i]);
        lfs_mkdirs(p);
    }
    const char *files[] = {"10.mp3", "2.mp3",    "1.flac",     "cover.jpg", "notes.txt", "list.m3u",
                           "list2.M3U8", "album.cue", "Track.WAV", ".hidden.mp3", "noext"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, files[i]);
        char data[64];
        memset(data, 'x', sizeof data);
        write_file(p, data, 10 + i);
    }
    fs_listing_t *l = fs_list_dir(dir, 0);
    CHECK(l != NULL);
    if (!l) return;
    const char *expect[] = {"a", "B", "c9", "c10", "1.flac", "2.mp3", "10.mp3", "album.cue", "list.m3u", "list2.M3U8",
                            "Track.WAV"};
    const fs_entry_type_t types[] = {FS_ENTRY_DIR,   FS_ENTRY_DIR,      FS_ENTRY_DIR,      FS_ENTRY_DIR,
                                     FS_ENTRY_AUDIO, FS_ENTRY_AUDIO,    FS_ENTRY_AUDIO,    FS_ENTRY_CUE,
                                     FS_ENTRY_PLAYLIST, FS_ENTRY_PLAYLIST, FS_ENTRY_AUDIO};
    CHECK_EQ_INT(fs_listing_count(l), 11);
    for (uint32_t i = 0; i < fs_listing_count(l) && i < 11; i++) {
        const fs_entry_t *e = fs_listing_get(l, i);
        CHECK_STR(e->name, expect[i]);
        CHECK_EQ_INT(e->type, types[i]);
    }
    CHECK_EQ_INT(fs_listing_get(l, 4)->size, 10 + 2);  // "1.flac" was files[2]
    CHECK(fs_listing_get(l, 11) == NULL);
    CHECK_EQ_INT(fs_listing_find_letter(l, 'C'), 2);
    CHECK_EQ_INT(fs_listing_find_letter(l, 'b'), 1);
    CHECK_EQ_INT(fs_listing_find_letter(l, '#'), 4);   // exact letter among files wins
    CHECK_EQ_INT(fs_listing_find_letter(l, 'L'), 8);
    CHECK_EQ_INT(fs_listing_find_letter(l, 'T'), 10);
    CHECK_EQ_INT(fs_listing_find_letter(l, 'Z'), 11);  // past the end
    CHECK_EQ_INT(fs_listing_find_letter(l, 0x0416), 11);
    char paths[8][CORE_PATH_MAX];
    CHECK_EQ_INT(fs_listing_audio_paths(l, dir, paths, 8), 4);
    CHECK_STR(paths[0], ROOT "/dir/1.flac");
    CHECK_STR(paths[1], ROOT "/dir/2.mp3");
    CHECK_STR(paths[2], ROOT "/dir/10.mp3");
    CHECK_STR(paths[3], ROOT "/dir/Track.WAV");
    CHECK_EQ_INT(fs_listing_audio_paths(l, dir, paths, 2), 2);
    CHECK(!fs_listing_truncated(l));
    fs_listing_free(l);

    // RAM limit.
    l = fs_list_dir(dir, 3);
    CHECK(l != NULL);
    CHECK_EQ_INT(fs_listing_count(l), 3);
    CHECK(fs_listing_truncated(l));
    fs_listing_free(l);
    l = fs_list_dir(dir, 11);  // exactly as many as the folder has: nothing left out
    CHECK_EQ_INT(fs_listing_count(l), 11);
    CHECK(!fs_listing_truncated(l));
    fs_listing_free(l);
    l = fs_list_dir(dir, 10);
    CHECK_EQ_INT(fs_listing_count(l), 10);
    CHECK(fs_listing_truncated(l));
    fs_listing_free(l);
    CHECK(fs_list_dir(ROOT "/missing", 0) == NULL);
    fs_listing_free(NULL);
    CHECK_EQ_INT(fs_listing_count(NULL), 0);
    CHECK(!fs_listing_truncated(NULL));
}

// The entry array grows 64, 128, ...: an allocator that refuses more than 64 entries
// cuts a 100-file folder short, and the listing says so.
static size_t s_grow_limit;
static void *grow_limited_alloc(size_t n) { return malloc(n ? n : 1); }
static void *grow_limited_realloc(void *p, size_t n) { return n > s_grow_limit ? NULL : realloc(p, n ? n : 1); }
static void grow_limited_free(void *p) { free(p); }
static const core_allocator_t k_grow_limited = {grow_limited_alloc, grow_limited_alloc, grow_limited_realloc,
                                                grow_limited_free};

TEST(listing_truncated_by_memory) {
    const char *dir = ROOT "/big";
    char p[CORE_PATH_MAX];
    for (int i = 0; i < 100; i++) {
        snprintf(p, sizeof p, "%s/%03d.flac", dir, i);
        write_file(p, "x", 1);
    }
    fs_listing_t *l = fs_list_dir(dir, 0);
    CHECK_EQ_INT(fs_listing_count(l), 100);
    CHECK(!fs_listing_truncated(l));
    fs_listing_free(l);
    s_grow_limit = 64 * sizeof(fs_entry_t);
    core_set_allocator(&k_grow_limited);
    l = fs_list_dir(dir, 0);
    core_set_allocator(NULL);
    CHECK(l != NULL);
    CHECK_EQ_INT(fs_listing_count(l), 64);
    CHECK(fs_listing_truncated(l));
    // What it holds is still sorted and usable.
    for (uint32_t i = 1; l && i < fs_listing_count(l); i++)
        CHECK(strcmp(fs_listing_get(l, i - 1)->name, fs_listing_get(l, i)->name) < 0);
    fs_listing_free(l);
}

// ----------------------------------------------------------------- M3U ----
TEST(m3u_read) {
    write_text(ROOT "/pl/list.m3u",
               "\xEF\xBB\xBF#EXTM3U\r\n"
               "#EXTINF:123,Кино - Группа крови\r\n"
               "..\\music\\Кино\\01.mp3\r\n"
               "\r\n"
               "sub/02.flac\r\n"
               "# a comment\r\n"
               "#EXTINF:-1 tvg-name=\"a,b\",Title Only\r\n"
               "./03.mp3\r\n"
               "http://radio.example/stream\r\n"
               "file:///C:/My%20Music/a.mp3\r\n"
               "C:\\Music\\x.mp3\r\n"
               "/abs/path.mp3\n"
               "../../../../../../../../../outside.mp3");
    playlist_t *p = playlist_load_m3u(ROOT "/pl/list.m3u");
    CHECK(p != NULL);
    if (!p) return;
    CHECK_EQ_INT(playlist_count(p), 7);
    const playlist_entry_t *e0 = playlist_get(p, 0);
    const playlist_entry_t *e1 = playlist_get(p, 1);
    CHECK_STR(e0->path, ROOT "/music/Кино/01.mp3");
    CHECK_STR(e0->title, "Группа крови");
    CHECK_STR(e0->artist, "Кино");
    CHECK_EQ_INT(e0->start_ms, 0);
    CHECK_EQ_INT(e0->end_ms, 0);
    CHECK_STR(e1->path, ROOT "/pl/sub/02.flac");  // both pointers stay valid
    CHECK_STR(e1->title, "");
    CHECK_STR(playlist_get(p, 2)->path, ROOT "/pl/03.mp3");
    CHECK_STR(playlist_get(p, 2)->title, "Title Only");
    CHECK_STR(playlist_get(p, 2)->artist, "");
    CHECK_STR(playlist_get(p, 3)->path, "C:/My Music/a.mp3");
    CHECK_STR(playlist_get(p, 4)->path, "C:/Music/x.mp3");
    CHECK_STR(playlist_get(p, 5)->path, "/abs/path.mp3");
    CHECK_STR(playlist_get(p, 6)->path, "../../../../../../outside.mp3");  // 9 ups from a 3-level folder
    CHECK(playlist_get(p, 7) == NULL);
    playlist_free(p);

    // CP1251 without BOM (playlist from an old Windows player).
    write_text(ROOT "/pl/old.m3u", "#EXTINF:1,\xCA\xE8\xED\xEE - \xC2\xEE\xE9\xED\xE0\n\xC2\xEE\xE9\xED\xE0.mp3\n");
    p = playlist_load_m3u(ROOT "/pl/old.m3u");
    CHECK(p != NULL);
    CHECK_EQ_INT(playlist_count(p), 1);
    if (playlist_count(p) == 1) {
        CHECK_STR(playlist_get(p, 0)->path, ROOT "/pl/Война.mp3");
        CHECK_STR(playlist_get(p, 0)->title, "Война");
        CHECK_STR(playlist_get(p, 0)->artist, "Кино");
    }
    playlist_free(p);

    // UTF-16 LE with BOM (Windows "Unicode" text files).
    const char *u8 = "#EXTM3U\r\n#EXTINF:5,Тест - Песня\r\nпапка\\song.mp3\r\n";
    uint8_t u16[512];
    size_t un = 0;
    u16[un++] = 0xFF;
    u16[un++] = 0xFE;
    for (const char *c = u8; *c;) {
        uint32_t cp;
        c += utf8_decode(c, &cp);
        u16[un++] = (uint8_t)cp;
        u16[un++] = (uint8_t)(cp >> 8);
    }
    write_file(ROOT "/pl/utf16.m3u", u16, un);
    p = playlist_load_m3u(ROOT "/pl/utf16.m3u");
    CHECK(p != NULL);
    CHECK_EQ_INT(playlist_count(p), 1);
    if (playlist_count(p) == 1) {
        CHECK_STR(playlist_get(p, 0)->path, ROOT "/pl/папка/song.mp3");
        CHECK_STR(playlist_get(p, 0)->title, "Песня");
        CHECK_STR(playlist_get(p, 0)->artist, "Тест");
    }
    playlist_free(p);

    // Large playlist, read as a stream; a CP1251 line inside a UTF-8 file.
    FILE *f = fopen(ROOT "/pl/big.m3u8", "wb");
    for (int i = 0; i < 5000; i++) fprintf(f, "#EXTINF:1,Artist %d - Song %d\nmusic/track%04d.mp3\n", i, i, i);
    fputs("\xCA\xE8\xED\xEE.mp3\n", f);
    fclose(f);
    p = playlist_load_m3u(ROOT "/pl/big.m3u8");
    CHECK(p != NULL);
    CHECK_EQ_INT(playlist_count(p), 5001);
    if (playlist_count(p) == 5001) {
        CHECK_STR(playlist_get(p, 4321)->path, ROOT "/pl/music/track4321.mp3");
        CHECK_STR(playlist_get(p, 4321)->title, "Song 4321");
        CHECK_STR(playlist_get(p, 5000)->path, ROOT "/pl/Кино.mp3");
    }
    playlist_free(p);

    // Empty file, garbage, missing file.
    write_text(ROOT "/pl/empty.m3u", "");
    p = playlist_load_m3u(ROOT "/pl/empty.m3u");
    CHECK(p != NULL);
    CHECK_EQ_INT(playlist_count(p), 0);
    playlist_free(p);
    uint8_t junk[3000];
    uint32_t seed = 7;
    for (size_t i = 0; i < sizeof junk; i++) {
        seed = seed * 1103515245u + 12345u;
        junk[i] = (uint8_t)(seed >> 16);
    }
    write_file(ROOT "/pl/junk.m3u", junk, sizeof junk);
    p = playlist_load_m3u(ROOT "/pl/junk.m3u");
    CHECK(p != NULL);
    for (uint32_t i = 0; i < playlist_count(p); i++) {
        CHECK(text_is_valid_utf8((const uint8_t *)playlist_get(p, i)->path, 256));
    }
    playlist_free(p);
    CHECK(playlist_load_m3u(ROOT "/pl/none.m3u") == NULL);
}

TEST(m3u_device_paths) {
    // Absolute paths written on a PC are mapped onto the card that holds the playlist.
    char cwd[CORE_PATH_MAX];
    CHECK(getcwd_(cwd, sizeof cwd) != NULL);
    for (char *c = cwd; *c; c++) {
        if (*c == '\\') *c = '/';
    }
    const char *abs = cwd;
    if (abs[0] && abs[1] == ':') abs += 2;  // "D:/x" -> "/x" (same drive as the working directory)
    char mount[CORE_PATH_MAX], path[CORE_PATH_MAX * 2], expect[CORE_PATH_MAX * 2];
    const char *slash = strchr(abs + 1, '/');
    size_t ml = slash ? (size_t)(slash - abs) : strlen(abs);
    memcpy(mount, abs, ml);
    mount[ml] = 0;
    snprintf(path, sizeof path, "%s/" ROOT "/pl/dev.m3u", abs);
    char body[1024];
    snprintf(body, sizeof body, "/Music/a.mp3\nD:\\Music\\b.mp3\n%s/c.mp3\n", mount);
    write_text(ROOT "/pl/dev.m3u", body);
    playlist_t *p = playlist_load_m3u(path);
    CHECK(p != NULL);
    CHECK_EQ_INT(playlist_count(p), 3);
    if (playlist_count(p) == 3) {
        snprintf(expect, sizeof expect, "%s/Music/a.mp3", mount);
        CHECK_STR(playlist_get(p, 0)->path, expect);
        snprintf(expect, sizeof expect, "%s/Music/b.mp3", mount);
        CHECK_STR(playlist_get(p, 1)->path, expect);
        snprintf(expect, sizeof expect, "%s/c.mp3", mount);
        CHECK_STR(playlist_get(p, 2)->path, expect);
    }
    playlist_free(p);
}

TEST(m3u8_save) {
    playlist_t *p = playlist_new();
    playlist_entry_t e;
    memset(&e, 0, sizeof e);
    core_strlcpy(e.path, ROOT "/music/Artist/01 a.mp3", sizeof e.path);
    core_strlcpy(e.title, "Title A", sizeof e.title);
    core_strlcpy(e.artist, "Artist", sizeof e.artist);
    CHECK_EQ_INT(playlist_add(p, &e), CORE_OK);
    memset(&e, 0, sizeof e);
    core_strlcpy(e.path, ROOT "/pl/local.flac", sizeof e.path);
    CHECK_EQ_INT(playlist_add(p, &e), CORE_OK);
    core_strlcpy(e.path, ROOT "/music/Кино/cue.flac", sizeof e.path);
    core_strlcpy(e.title, "Война", sizeof e.title);
    e.start_ms = 60000;
    e.end_ms = 245000;
    CHECK_EQ_INT(playlist_add(p, &e), CORE_OK);
    memset(&e, 0, sizeof e);
    CHECK_EQ_INT(playlist_add(p, &e), CORE_EINVAL);  // empty path
    CHECK_EQ_INT(playlist_save_m3u8(p, ROOT "/pl/out.m3u8"), CORE_OK);
    CHECK(!lfs_exists(ROOT "/pl/out.m3u8.tmp"));
    char text[2048];
    read_text(ROOT "/pl/out.m3u8", text, sizeof text);
    CHECK_STR(text,
              "#EXTM3U\n"
              "#EXTINF:-1,Artist - Title A\n"
              "../music/Artist/01 a.mp3\n"
              "local.flac\n"
              "#EXTINF:185,Война\n"
              "#EXT-EMP-RANGE:60000,245000\n"
              "../music/Кино/cue.flac\n");
    // Round trip.
    playlist_t *q = playlist_load_m3u(ROOT "/pl/out.m3u8");
    CHECK(q != NULL);
    CHECK_EQ_INT(playlist_count(q), 3);
    for (uint32_t i = 0; i < playlist_count(q) && i < 3; i++) {
        CHECK_STR(playlist_get(q, i)->path, playlist_get(p, i)->path);
    }
    CHECK_STR(playlist_get(q, 0)->title, "Title A");
    CHECK_STR(playlist_get(q, 0)->artist, "Artist");
    // The CUE track keeps its range (else it would play the whole image).
    if (playlist_count(q) == 3) {
        CHECK_EQ_INT(playlist_get(q, 2)->start_ms, 60000);
        CHECK_EQ_INT(playlist_get(q, 2)->end_ms, 245000);
        CHECK_EQ_INT(playlist_get(q, 1)->start_ms, 0);
        CHECK_EQ_INT(playlist_get(q, 1)->end_ms, 0);
    }
    // Overwrite an existing playlist atomically.
    CHECK_EQ_INT(playlist_save_m3u8(q, ROOT "/pl/out.m3u8"), CORE_OK);
    playlist_free(q);
    q = playlist_load_m3u(ROOT "/pl/out.m3u8");
    CHECK_EQ_INT(playlist_count(q), 3);
    playlist_free(q);
    playlist_free(p);
    CHECK_EQ_INT(playlist_count(NULL), 0);
    CHECK(playlist_get(NULL, 0) == NULL);
    CHECK_EQ_INT(playlist_save_m3u8(NULL, "x"), CORE_EINVAL);
}

TEST(m3u8_interrupted_save) {
    // Power lost between "remove" and "rename": only favorites.m3u8.tmp is left. It is read,
    // and the next save (the "add to favourites" path: load, append, save) keeps its entries.
    playlist_t *p = playlist_new();
    playlist_entry_t e;
    memset(&e, 0, sizeof e);
    for (int i = 0; i < 3; i++) {
        snprintf(e.path, sizeof e.path, ROOT "/music/fav%d.mp3", i);
        CHECK_EQ_INT(playlist_add(p, &e), CORE_OK);
    }
    CHECK_EQ_INT(playlist_save_m3u8(p, ROOT "/pl/favorites.m3u8"), CORE_OK);
    playlist_free(p);
    CHECK_EQ_INT(lfs_replace(ROOT "/pl/favorites.m3u8", ROOT "/pl/favorites.m3u8.tmp"), CORE_OK);
    CHECK(!lfs_exists(ROOT "/pl/favorites.m3u8"));
    p = playlist_load_m3u(ROOT "/pl/favorites.m3u8");
    CHECK(p != NULL);
    if (!p) return;
    CHECK_EQ_INT(playlist_count(p), 3);
    snprintf(e.path, sizeof e.path, ROOT "/music/fav3.mp3");
    CHECK_EQ_INT(playlist_add(p, &e), CORE_OK);
    CHECK_EQ_INT(playlist_save_m3u8(p, ROOT "/pl/favorites.m3u8"), CORE_OK);
    playlist_free(p);
    CHECK(!lfs_exists(ROOT "/pl/favorites.m3u8.tmp"));
    p = playlist_load_m3u(ROOT "/pl/favorites.m3u8");
    CHECK_EQ_INT(playlist_count(p), 4);
    if (playlist_count(p) == 4) CHECK_STR(playlist_get(p, 0)->path, ROOT "/music/fav0.mp3");
    playlist_free(p);
    // Saving a new list over a playlist that exists only as .tmp: the .tmp is put in place
    // first, then replaced as a whole (never truncated while it is the only copy).
    CHECK_EQ_INT(lfs_replace(ROOT "/pl/favorites.m3u8", ROOT "/pl/favorites.m3u8.tmp"), CORE_OK);
    p = playlist_new();
    snprintf(e.path, sizeof e.path, ROOT "/music/only.mp3");
    CHECK_EQ_INT(playlist_add(p, &e), CORE_OK);
    CHECK_EQ_INT(playlist_save_m3u8(p, ROOT "/pl/favorites.m3u8"), CORE_OK);
    playlist_free(p);
    CHECK(!lfs_exists(ROOT "/pl/favorites.m3u8.tmp"));
    p = playlist_load_m3u(ROOT "/pl/favorites.m3u8");
    CHECK_EQ_INT(playlist_count(p), 1);
    playlist_free(p);
}

TEST(m3u8_large_utf8) {
    // A UTF-8 playlist without BOM over 64 KiB whose first 64 KiB end inside a Cyrillic
    // character must not be taken for CP1251 (every path would turn into mojibake), as the
    // player's own favourites once they grow past 64 KiB.
    bool cut_any = false;
    for (int shift = 0; shift < 4; shift++) {
        FILE *f = fopen(ROOT "/pl/large.m3u8", "wb");
        CHECK(f != NULL);
        if (!f) return;
        size_t n = 0;
        n += (size_t)fprintf(f, "%.*s", shift, "xxx");
        int count = 0;
        char line[128];
        bool cut = false;
        while (n < 70000) {
            int len = snprintf(line, sizeof line, "Музыка/Исполнитель %04d/Песня.mp3\n", count++);
            // Byte 65535 (the last of the sample) is the lead byte of a 2-byte character?
            if (n <= 65535 && n + (size_t)len > 65535) cut = ((uint8_t)line[65535 - n] & 0xE0) == 0xC0;
            fputs(line, f);
            n += (size_t)len;
        }
        fclose(f);
        cut_any |= cut;
        playlist_t *p = playlist_load_m3u(ROOT "/pl/large.m3u8");
        CHECK(p != NULL);
        if (!p) return;
        CHECK_EQ_INT(playlist_count(p), (uint32_t)count);
        int bad = 0;
        for (uint32_t i = 0; i < playlist_count(p); i++) bad += strstr(playlist_get(p, i)->path, "Исполнитель") == NULL;
        CHECK_EQ_INT(bad, 0);
        playlist_free(p);
    }
    CHECK(cut_any);  // at least one layout really cuts a character at the end of the sample
    // A CP1251 file stays CP1251.
    FILE *f = fopen(ROOT "/pl/large1251.m3u", "wb");
    for (int i = 0; i < 4000; i++) fprintf(f, "\xCC\xF3\xE7\xFB\xEA\xE0/%04d.mp3\n", i);  // "Музыка"
    fclose(f);
    playlist_t *p = playlist_load_m3u(ROOT "/pl/large1251.m3u");
    CHECK_EQ_INT(playlist_count(p), 4000);
    if (playlist_count(p) == 4000) CHECK(strstr(playlist_get(p, 3999)->path, "Музыка/3999.mp3") != NULL);
    playlist_free(p);
}

// ----------------------------------------------------------------- CUE ----
TEST(cue_cp1251) {
    char path[512];
    snprintf(path, sizeof path, "%s/library/cp1251.cue", TEST_DATA_DIR);
    playlist_t *p = playlist_load_cue(path);
    CHECK(p != NULL);
    if (!p) return;
    CHECK_EQ_INT(playlist_count(p), 3);
    if (playlist_count(p) != 3) {
        playlist_free(p);
        return;
    }
    char file[512];
    snprintf(file, sizeof file, "%s/library/Кино - Группа крови.wav", TEST_DATA_DIR);
    const playlist_entry_t *t1 = playlist_get(p, 0), *t2 = playlist_get(p, 1), *t3 = playlist_get(p, 2);
    CHECK_STR(t1->path, file);
    CHECK_STR(t1->title, "Группа крови");
    CHECK_STR(t1->artist, "Кино");
    CHECK_EQ_INT(t1->start_ms, 0);
    CHECK_EQ_INT(t1->end_ms, 286000);
    CHECK_STR(t2->title, "Закрой за мной дверь, я ухожу");
    CHECK_EQ_INT(t2->start_ms, 286000);  // INDEX 01, not the INDEX 00 pregap
    CHECK_EQ_INT(t2->end_ms, 539000 + 37 * 1000 / 75);
    CHECK_STR(t3->title, "Война");
    CHECK_STR(t3->artist, "Виктор Цой");
    CHECK_EQ_INT(t3->start_ms, 539000 + 37 * 1000 / 75);
    CHECK_EQ_INT(t3->end_ms, 0);
    playlist_free(p);
}

TEST(cue_files) {
    // FILE names a .wav that was converted to .flac; two FILE sections; data track.
    write_text(ROOT "/cue/CDImage.flac", "fLaC");
    write_text(ROOT "/cue/Disc2.ape", "MAC ");
    write_text(ROOT "/cue/sheet.cue",
               "REM COMMENT \"ExactAudioCopy\"\n"
               "PERFORMER \"Album Artist\"\n"
               "TITLE \"The Album\"\n"
               "FILE \"CDImage.wav\" WAVE\n"
               "  track 01 audio\n"
               "    title \"One\"\n"
               "    index 01 00:00:00\n"
               "  TRACK 02 AUDIO\n"
               "    TITLE \"Two\"\n"
               "    PERFORMER \"Guest\"\n"
               "    INDEX 00 03:00:00\n"
               "    INDEX 01 03:02:15\n"
               "  TRACK 03 MODE1/2352\n"
               "    INDEX 01 05:00:00\n"
               "FILE \"Disc2.wav\" WAVE\n"
               "  TRACK 04 AUDIO\n"
               "    TITLE \"Four\"\n"
               "    INDEX 01 00:00:32\n"
               "  TRACK 05 AUDIO\n"
               "    TITLE \"No index\"\n"
               "  TRACK 06 AUDIO\n"
               "    INDEX 01 99:99:99\n");
    playlist_t *p = playlist_load_cue(ROOT "/cue/sheet.cue");
    CHECK(p != NULL);
    if (!p) return;
    CHECK_EQ_INT(playlist_count(p), 3);
    if (playlist_count(p) == 3) {
        CHECK_STR(playlist_get(p, 0)->path, ROOT "/cue/CDImage.flac");
        CHECK_STR(playlist_get(p, 0)->title, "One");
        CHECK_STR(playlist_get(p, 0)->artist, "Album Artist");
        CHECK_EQ_INT(playlist_get(p, 0)->end_ms, 182200);
        CHECK_STR(playlist_get(p, 1)->artist, "Guest");
        CHECK_EQ_INT(playlist_get(p, 1)->start_ms, 182200);
        CHECK_EQ_INT(playlist_get(p, 1)->end_ms, 0);  // next track is in another file
        CHECK_STR(playlist_get(p, 2)->path, ROOT "/cue/Disc2.ape");
        CHECK_EQ_INT(playlist_get(p, 2)->start_ms, 32 * 1000 / 75);
    }
    playlist_free(p);
    CHECK(playlist_load_cue(ROOT "/cue/missing.cue") == NULL);
    write_text(ROOT "/cue/empty.cue", "garbage\nFILE\nTRACK\nINDEX 01\n");
    p = playlist_load_cue(ROOT "/cue/empty.cue");
    CHECK(p != NULL);
    CHECK_EQ_INT(playlist_count(p), 0);
    playlist_free(p);
}

static void run_all(void) {
    rm_rf("tmp_library/browse");
    lfs_mkdirs(ROOT);
}

TEST(setup) { run_all(); }

TEST_MAIN(RUN(setup) RUN(list_and_sort) RUN(listing_truncated_by_memory) RUN(m3u_read) RUN(m3u_device_paths)
              RUN(m3u8_save) RUN(m3u8_interrupted_save) RUN(m3u8_large_utf8) RUN(cue_cp1251) RUN(cue_files))
