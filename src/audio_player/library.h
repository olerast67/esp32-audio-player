// SPDX-License-Identifier: Apache-2.0
/// \file
/// Music library index stored on the SD card (no LevelDB, no SQLite).
///
/// Files in <db_dir> (for example "/sdcard/.music/db"), named after the snapshot generation
/// ("tracks-0a1b2c3d.bin"):
///   tracks.bin   fixed-size records (path/title/artist/... as offsets into strings.bin)
///   strings.bin  UTF-8 string pool
///   *.idx        sorted uint32 arrays for each view (artists, albums, tracks, genres...)
///   files.bin    path -> (size, mtime) for incremental rescans
/// A scan writes the files of a new generation next to the loaded one and swaps it in
/// atomically, so readers keep working on the old snapshot while scanning and never wait
/// for card writes at the swap. After a crash the last complete generation is used. All
/// queries are thread-safe (core locks).
///
/// Memory: index arrays live in RAM (4 bytes per entry and view), records and
/// strings are read from the card through a small block cache.
#pragma once

#include "audio_player/tags.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t lib_id_t;
#define LIB_ID_NONE 0xFFFFFFFFu

typedef enum {
    LIB_VIEW_ARTISTS = 0,     ///< parent: none. Album artists (fallback: artist)
    LIB_VIEW_ALBUMS,          ///< parent: none. All albums sorted by title
    LIB_VIEW_ARTIST_ALBUMS,   ///< parent: artist id. Albums by year, then title
    LIB_VIEW_ALBUM_TRACKS,    ///< parent: album id. Disc, track number, title
    LIB_VIEW_ARTIST_TRACKS,   ///< parent: artist id. All tracks of the artist
    LIB_VIEW_TRACKS,          ///< parent: none. All tracks sorted by title
    LIB_VIEW_GENRES,          ///< parent: none
    LIB_VIEW_GENRE_ALBUMS,    ///< parent: genre id
    LIB_VIEW_RECENT,          ///< parent: none. Newest files first (by mtime)
    LIB_VIEW_AUDIOBOOKS,      ///< parent: none. Albums flagged as audiobooks
    LIB_VIEW_COUNT
} lib_view_t;

typedef struct {
    lib_id_t id;              ///< id of the artist/album/genre/track this row stands for
    char title[TAG_TEXT_MAX];
    char subtitle[TAG_TEXT_MAX];  ///< artist for albums; the application formats `count` ("12 albums")
    uint32_t count;           ///< children: albums of an artist, tracks of an album...
    uint32_t duration_ms;     ///< tracks: duration; albums: total
    uint16_t year;
    uint32_t sort_letter;     ///< collate_index_letter() of the sort key
} lib_item_t;

typedef struct {
    lib_id_t id;
    char path[CORE_PATH_MAX];
    track_tags_t tags;
    lib_id_t album_id, artist_id, genre_id;
    uint32_t file_size;
    uint32_t mtime;
} lib_track_t;

typedef struct {
    uint32_t files_seen;
    uint32_t files_total;     ///< 0 while still counting
    uint32_t tracks_added, tracks_updated, tracks_removed;
    const char *current_path; ///< valid only during the callback
} lib_scan_progress_t;

/// Return false to cancel the scan. Called from the scanning thread: after every file, once
/// per folder while counting (files_seen = files_total = 0), at most every ~5 ms while the
/// index is sorted, and once at the end (current_path ""). current_path is never NULL.
typedef bool (*lib_scan_cb_t)(const lib_scan_progress_t *p, void *user);

/// Scanner pacing, called from the scanning thread: between files and folders (the platform
/// may block here while the audio buffer is low, so indexing never causes dropouts), and at
/// least every ~5 ms of pure CPU work (the sorts that finish a scan of a large library). A
/// platform whose scanning task runs above its idle task should let it run here (e.g.
/// sleep one tick when the last sleep is a few ms ago), or the task watchdog fires.
typedef void (*lib_throttle_cb_t)(void *user);

typedef struct library library_t;

typedef struct {
    const char *music_root;   ///< e.g. "/sdcard" (scanned recursively, hidden dirs skipped)
    const char *db_dir;       ///< e.g. "/sdcard/.music/db"
    bool ignore_articles;     ///< collation option
    lib_throttle_cb_t throttle;
    void *throttle_user;
} library_config_t;

library_t *library_open(const library_config_t *cfg);  ///< loads an existing index if present
void library_close(library_t *lib);

/// Incremental scan (new/changed/removed files). Blocking; run it in a worker thread.
int library_scan(library_t *lib, bool full_rebuild, lib_scan_cb_t cb, void *user);
bool library_is_scanning(const library_t *lib);

uint32_t library_track_count(const library_t *lib);
/// Number of rows in a view (parent = LIB_ID_NONE when the view has no parent).
uint32_t library_view_count(library_t *lib, lib_view_t view, lib_id_t parent);
int library_view_item(library_t *lib, lib_view_t view, lib_id_t parent, uint32_t index, lib_item_t *out);
/// Index of the first row whose sort letter is >= letter (for jump-to-letter).
uint32_t library_view_find_letter(library_t *lib, lib_view_t view, lib_id_t parent, uint32_t letter);
/// Track ids of a view in display order (for "play all" / queue building).
/// Writes up to max ids, returns the total count.
uint32_t library_view_track_ids(library_t *lib, lib_view_t view, lib_id_t parent, lib_id_t *out, uint32_t max);

int library_get_track(library_t *lib, lib_id_t track_id, lib_track_t *out);
lib_id_t library_find_path(library_t *lib, const char *path);  ///< LIB_ID_NONE if not indexed

/// Track ids belong to one snapshot of the index: a completed scan renumbers them. The
/// generation names the snapshot (0 = none); reading it takes no lock, so it is cheap to
/// poll. library_translate_ids() brings ids of snapshot *gen into the current one, in
/// place (LIB_ID_NONE for files that are gone), and sets *gen to the current generation.
/// It works for ids of the current or the previous snapshot; for older ones it returns
/// false and changes nothing, so holders of ids (the play queue) translate after every scan.
uint32_t library_generation(const library_t *lib);
bool library_translate_ids(library_t *lib, uint32_t *gen, lib_id_t *ids, uint32_t n);

/// Audiobook / long-file positions and simple bookmarks (stored in db_dir/positions.bin).
int library_save_position(library_t *lib, const char *path, uint32_t position_ms);
uint32_t library_load_position(library_t *lib, const char *path);  ///< 0 if none

#ifdef __cplusplus
}
#endif
