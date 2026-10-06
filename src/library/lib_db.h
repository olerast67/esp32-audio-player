// SPDX-License-Identifier: Apache-2.0
// On-card format of the library index and the in-RAM snapshot. Private to
// library.c (loading, queries) and lib_scan.c (building). The design is described
// at the top of library.c.
#pragma once

#include <stdio.h>

#include "audio_player/library.h"
#include "lib_priv.h"

#define LIB_MAGIC 0x42444C50u  // "PLDB"
#define LIB_VERSION 2          // 2: generation-tagged file names, commit sequence in info
#define LIB_HDR_SIZE 32
#define LIB_BLOCK 4096u        // cache block and CRC block (payload bytes)
#define LIB_CACHE_BLOCKS 32    // 128 KiB block cache
#define LIB_TITLE_KEY_MAX 16   // bytes of the title sort key kept while building
#define LIB_MAX_TRACKS 250000u
#define LIB_MAX_ENTITIES 250000u
#define LIB_POS_MAX 1024       // remembered playback positions

// File kinds (also the index in s_db_files[]).
typedef enum {
    DBF_TRACKS = 0,  // records (block CRCs in info)
    DBF_STRINGS,     // per-track string pool (block CRCs in info)
    DBF_FILES,       // (path hash, track id) sorted by hash (block CRCs in info)
    DBF_ENTITIES,    // artists, albums, genres, bid maps, names pool (CRC)
    DBF_V_TRACKS,    // TRACKS view ids + jump table (CRC)
    DBF_V_RECENT,    // RECENT view ids (CRC)
    DBF_V_ALBUMS,    // ALBUMS view ids (CRC)
    DBF_V_ALBTRK,    // all tracks in album order (CRC)
    DBF_V_GALBUMS,   // albums of each genre (CRC)
    DBF_V_BOOKS,     // AUDIOBOOKS view ids (CRC)
    DBF_INFO,        // counts, options, block CRC tables; written last (commit record)
    DBF_COUNT,
    DBF_POSITIONS = 100,
} db_file_t;

extern const char *const g_lib_db_names[DBF_COUNT];

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t generation;  // equal in all files of one snapshot
    uint32_t count;       // elements (meaning depends on the file)
    uint32_t payload;     // payload bytes after this header
    uint32_t crc;         // CRC-32 of the payload (0 for block-checked files)
    uint32_t reserved[2];
} db_hdr_t;
_Static_assert(sizeof(db_hdr_t) == LIB_HDR_SIZE, "header size");

// Track record in tracks.bin (fixed size, little-endian host layout: the index is
// rebuilt when the format version changes, never shared between architectures).
#define REC_F_BOOK_HINT 0x01  // tags or folder say "audiobook"
#define REC_F_RETRY 0x02      // tags were not readable (I/O error): the next scan parses the file again
typedef struct {
    uint32_t dir, name, title, artist, composer;  // strings.bin payload offsets (0 = "")
    uint32_t album;                               // album build id (-> album_map)
    uint32_t duration_ms, sample_rate, file_size, mtime;
    uint32_t cover_offset, cover_size;
    float rg_track_gain, rg_track_peak, rg_album_gain, rg_album_peak;
    uint16_t genre;                               // genre build id (-> genre_map)
    uint16_t year, track_no, track_total, disc_no, disc_total;
    uint8_t bits, channels, codec, cover_mime, flags, pad[3];
} db_rec_t;
_Static_assert(sizeof(db_rec_t) == 84, "record size");

typedef struct {
    uint32_t name;         // names pool offset
    uint32_t album_first;  // first album id; music albums of the artist are contiguous
    uint32_t album_count;
    uint32_t track_first;  // index into the album-ordered track array
    uint32_t track_count;
    uint32_t duration_ms;  // saturated
} db_artist_t;

#define ALB_F_BOOK 0x01
typedef struct {
    uint32_t title;        // names pool offset
    uint32_t artist;       // artist id
    uint32_t track_first;  // index into the album-ordered track array
    uint32_t track_count;
    uint32_t duration_ms;
    uint16_t year;
    uint8_t flags;
    uint8_t pad;
} db_album_t;

typedef struct {
    uint32_t name;
    uint32_t album_first;  // index into the genre-albums array
    uint32_t album_count;
} db_genre_t;

typedef struct {
    uint32_t rank;  // collate_letter_rank of the jump letter
    uint32_t row;   // first row with this letter
} db_jump_t;

typedef struct {
    uint32_t hash;
    uint32_t id;
} db_file_ent_t;

#define DB_FILES_PER_BLOCK (LIB_BLOCK / sizeof(db_file_ent_t))

typedef struct {
    uint32_t tracks, music_tracks;
    uint32_t artists, music_artists;
    uint32_t albums, music_albums;
    uint32_t genres, music_genres;
    uint32_t album_bids, genre_bids;  // sizes of the bid maps
    uint32_t names_bytes;
    uint32_t strings_bytes;
    uint32_t galbums;                 // genre-album pairs
    uint32_t jumps;                   // TRACKS view jump entries
    uint32_t flags;                   // bit 0: articles ignored when sorting
    uint32_t root_hash;
    uint32_t crc_blocks[3];           // block count of tracks/strings/files (CRC tables follow)
    uint32_t seq;                     // commit sequence: the highest complete one is the index
    uint32_t reserved[3];
} db_info_t;

// ------------------------------------------------------------ block cache ----
typedef struct {
    uint8_t *data;
    uint32_t block;
    uint32_t len;
    uint32_t stamp;
    int8_t file;  // DBF_TRACKS/STRINGS/FILES, -1 = empty
} cache_slot_t;

// Loaded snapshot. All arrays live in one or a few PSRAM allocations.
typedef struct {
    bool loaded;
    bool corrupt;  // a block failed its CRC: the next scan re-reads affected files
    collate_opts_t sort_opts;  // options the views were sorted with (letters must agree)
    uint32_t generation;
    db_info_t info;
    FILE *f[3];            // tracks, strings, files: opened on the first cache miss
    uint32_t *block_crc[3];
    uint32_t *files_fence; // first hash of each files.bin block
    db_file_ent_t *files_mem;  // files.bin in RAM while a scan looks up every file (or NULL)
    void *info_buf;
    void *ent_buf;
    db_artist_t *artists;
    db_album_t *albums;
    db_genre_t *genres;
    uint32_t *album_map, *genre_map;
    const char *names;
    void *view_buf[DBF_V_BOOKS - DBF_V_TRACKS + 1];
    uint32_t *v_tracks, *v_recent, *v_albums, *v_albtrk, *v_galbums, *v_books;
    db_jump_t *jumps;
} db_snap_t;

typedef struct {
    uint64_t hash;
    uint32_t ms;
    uint32_t stamp;
} db_pos_t;

struct library {
    core_mutex_t *mutex;      // snapshot, cache, id map, positions table
    core_mutex_t *save_mutex; // positions.bin writes (taken without mutex held)
    char root[CORE_PATH_MAX];
    char db_dir[CORE_PATH_MAX];
    collate_opts_t opts;
    lib_throttle_cb_t throttle;
    void *throttle_user;
    volatile bool scanning;
    volatile uint32_t pub_gen;  // snap.generation, readable without the lock
    db_snap_t snap;
    cache_slot_t cache[LIB_CACHE_BLOCKS];
    uint8_t *cache_mem;
    uint32_t cache_clock;
    // Track ids of the previous snapshot -> ids of the current one (LIB_ID_NONE: gone).
    uint32_t *idmap;
    uint32_t idmap_n, idmap_gen;
    bool idmap_valid;         // false: the previous ids cannot be translated (no map)
    db_pos_t *pos;
    uint32_t pos_count, pos_stamp;
    uint32_t pos_version, pos_saved;  // edits of pos[] / the edit last written to the card
};

// ------------------------------------------------------ shared internals ----
// Files of one snapshot carry its generation in their names ("tracks-0a1b2c3d.bin"), so a
// scan writes the next snapshot next to the loaded one and the swap renames nothing:
// info-<gen>.bin, written last, commits it (the complete info with the highest seq wins).
int lib_db_path(const library_t *lib, uint32_t gen, int file, char *out, size_t cap);
// Validate a header read from a file of the given kind (generation 0 = any).
bool lib_hdr_ok(const db_hdr_t *h, int kind, uint32_t generation);
// Read helpers on the loaded snapshot (caller holds the lock).
int lib_read_rec(library_t *lib, uint32_t id, db_rec_t *out);
int lib_read_str(library_t *lib, uint32_t off, char *dst, size_t cap);
uint32_t lib_find_path_locked(library_t *lib, const char *path, db_rec_t *rec_out);
// Loads files.bin of the snapshot into RAM for the lookups of a scan (and closes its
// handle), or frees that copy (load = false). Caller holds the lock.
void lib_files_mem(library_t *lib, bool load);
// Build a fresh index of a new generation and swap it in (lib_scan.c).
int lib_scan_run(library_t *lib, bool full_rebuild, lib_scan_cb_t cb, void *user);
// Swap support (library.c). lib_commit() loads the complete snapshot `gen` (outside the
// lock), swaps it in under the lock, takes over idmap (ids of the old snapshot -> new ids;
// n = track count of the old snapshot, idmap NULL when it could not be built) and deletes
// the old files. On failure the old snapshot stays and idmap is freed.
int lib_commit(library_t *lib, uint32_t gen, uint32_t seq, uint32_t *idmap, uint32_t n);
// Deletes the files of every generation but keep (and of the old fixed-name layout).
void lib_remove_other_gens(const library_t *lib, uint32_t keep);
void lib_remove_gen(const library_t *lib, uint32_t gen);
