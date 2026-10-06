// SPDX-License-Identifier: Apache-2.0
// Music library index on the SD card.
//
// DESIGN
// ------
// Entities. Every audio file under music_root becomes a track. Tracks are grouped into
//   artists  = album artist, falling back to the track artist ("" = Unknown artist),
//   albums   = (album title, artist); "" = Unknown album of that artist,
//   genres   = genre tag ("" = Unknown genre).
// Grouping uses primary collation equality, so "Pink Floyd" / "pink floyd" / "Pink
// Floyd " and "Ёлка" / "Елка" are one artist. The UI shows localized text for "".
// Audiobooks: an album is an audiobook when any of its tracks has an audiobook genre
// (Audiobook, Аудиокнига, ...), is an .m4b / iTunes audiobook, or lies under a folder
// named Audiobooks / Аудиокниги. Audiobook albums are listed only in
// LIB_VIEW_AUDIOBOOKS (and their own ALBUM_TRACKS), so "shuffle all" and the music
// views are not flooded with chapters. Authors of audiobooks only are not in ARTISTS.
//
// Ids. A scan assigns ids in sort order:
//   artist ids  : artists with music first, then by name (articles optionally ignored)
//   album ids   : music albums first, then by artist id, year (unknown last), title
//   genre ids   : genres with music first, then by name
//   track ids   : scan order (depth-first walk, folders and files in collation order)
// Ids are valid for one snapshot: after a scan completes the UI must re-query views.
// The scan also keeps a map from the ids of the previous snapshot to the new ones
// (library_translate_ids()), so ids held for a long time (the play queue) can follow.
// Because album ids are grouped by artist, "albums of an artist" is a contiguous id
// range and "tracks of an album/artist" is a contiguous slice of one array ordered by
// (album id, disc, track number, file order). Most views need no per-view array.
//
// Files in <db_dir> (all start with a 32-byte header: magic, version, kind,
// generation, count, payload size, CRC). Each name carries the snapshot generation
// ("tracks-0a1b2c3d.bin"):
//   tracks.bin   84-byte records (string offsets, album/genre build ids, stream info,
//                ReplayGain, cover offset, size + mtime for incremental scans)
//   strings.bin  UTF-8 pool of per-track strings (folder paths, names, titles,
//                artists, composers deduplicated where it pays)
//   files.bin    (path hash, track id) sorted by hash: path lookup and the "files
//                table" of the incremental scan (the record keeps size + mtime)
//   entities.bin artists, albums, genres, build-id maps and the entity name pool
//   *.idx        uint32 arrays: tracks.idx (TRACKS view + letter jump table),
//                recent.idx, albums.idx (by title), albtrk.idx (album order),
//                galbums.idx (albums per genre), books.idx (audiobooks by title)
//   info.bin     counts, options, commit sequence and the CRC-32 of every 4 KiB block
//                of tracks.bin, strings.bin and files.bin; written last = commit record
//   positions.bin  playback positions (separate life cycle, atomic rewrite)
// Small files are checked with a whole-file CRC at load. The big ones are checked
// per 4 KiB block when a block enters the cache; a bad block marks the snapshot
// corrupt, its rows read as empty, and the next scan re-reads the affected files
// from their tags instead of trusting the old records.
//
// RAM (loaded snapshot). In RAM: entity tables and names, view arrays, block CRCs,
// the files.bin fence, 128 KiB block cache, 16 KiB positions table, and after a scan the
// id map of the previous snapshot (4 B per old track). On the card: records and
// per-track strings. Per 30 000 tracks with ~3 000 albums:
//   view arrays   tracks + recent + album order: 3 x 4 B per track    = 352 KiB
//                 albums + genre albums + audiobooks, 4 B per album   ~  30 KiB
//   entities      24 B per artist/album, 12 B per genre, bid maps     ~  95 KiB
//   entity names  ~25 B per name                                      ~  90 KiB
//   block CRCs    4 B per 4 KiB of tracks/strings/files (~5 MiB)      ~   5 KiB
//   fixed         block cache 128 KiB + positions 16 KiB + state      ~ 146 KiB
//   id map        after the first scan of a session, 4 B per track     = 117 KiB
// Measured by test_library_perf (5 000 tracks, counting allocator): 146 KiB fixed +
// 19.6 B per track, i.e. ~720 KiB for 30 000 tracks.
// View rows are O(1): artist/album/genre rows come from RAM, track rows cost one
// record read and one or two string reads through the cache.
//
// RAM while scanning: the old snapshot stays loaded for readers; the builder keeps
// 20 B per track (album, disc/track, mtime, path hash, key offset) + a title sort key
// (<= 16 B), entity tables, two uint32 arrays per track while sorting a view, files.bin
// of the old snapshot (8 B per track: one lookup per file without card reads) and the
// id map (4 B per old track).
// Measured: ~86 B per track in total with the old snapshot (at 10 tracks per album),
// i.e. ~2.6 MiB of PSRAM for a 30 000-track rescan, ~1 MiB for 10 000 tracks.
//
// Scan (lib_scan.c). Pass 1 counts audio files (for progress). Pass 2 walks the tree
// (hidden folders and "System Volume Information" skipped), stats each file right
// after readdir (served from the FAT directory cache on ESP-IDF) and compares size +
// mtime with the old snapshot: unchanged files reuse the old record, others are
// parsed. Records and strings of the new generation are written next to the old
// snapshot, which keeps serving queries. Entity names use the majority spelling of the
// merged variants. At the end all files are written, info.bin last; the new snapshot is
// loaded and checked without the lock, swapped in under it (a pointer swap: readers,
// the audio thread among them, never wait for card writes), and the old files are
// deleted. After a crash library_open() takes the complete generation with the highest
// commit sequence and deletes the rest; a snapshot that fails validation loads as empty
// and the next scan rebuilds everything.
//
// File handles: the block-checked files are opened on the first cache miss, and the
// scanner writes through buffers it flushes by append, so a scan holds at most three
// handles at a time (FAT allows few: the readahead and state saves need theirs).
#include "audio_player/library.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lib_db.h"

#define TAG "library"

const char *const g_lib_db_names[DBF_COUNT] = {
    "tracks.bin", "strings.bin", "files.bin",   "entities.bin", "tracks.idx", "recent.idx",
    "albums.idx", "albtrk.idx",  "galbums.idx", "books.idx",    "info.bin",
};

int lib_db_path(const library_t *lib, uint32_t gen, int file, char *out, size_t cap) {
    int n;
    if (file == DBF_POSITIONS) {
        n = snprintf(out, cap, "%s/positions.bin", lib->db_dir);
    } else {
        const char *name = g_lib_db_names[file], *dot = strchr(name, '.');
        n = snprintf(out, cap, "%s/%.*s-%08lx%s", lib->db_dir, (int)(dot - name), name, (unsigned long)gen, dot);
    }
    return (n < 0 || (size_t)n >= cap) ? CORE_EINVAL : CORE_OK;
}

// "tracks-0a1b2c3d.bin" -> DBF_TRACKS and its generation. False for other names.
static bool parse_db_name(const char *name, int *kind, uint32_t *gen) {
    for (int k = 0; k < DBF_COUNT; k++) {
        const char *base = g_lib_db_names[k], *dot = strchr(base, '.');
        const size_t bl = (size_t)(dot - base);
        if (strncmp(name, base, bl) != 0 || name[bl] != '-') continue;
        uint32_t g = 0;
        const char *h = name + bl + 1;
        int i = 0;
        for (; i < 8; i++) {
            const char c = h[i];
            const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (v < 0) break;
            g = g << 4 | (uint32_t)v;
        }
        if (i != 8 || strcmp(h + 8, dot) != 0) continue;
        *kind = k;
        *gen = g;
        return true;
    }
    return false;
}

bool lib_hdr_ok(const db_hdr_t *h, int kind, uint32_t generation) {
    return h->magic == LIB_MAGIC && h->version == LIB_VERSION && h->kind == kind &&
           (generation == 0 || h->generation == generation);
}

static int64_t file_size_of(FILE *f) {
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    long n = ftell(f);
    return n < 0 ? -1 : (int64_t)n;
}

// Load a CRC-checked file completely. Returns the payload (caller frees) or NULL.
static void *load_crc_file(const library_t *lib, int kind, uint32_t generation, uint32_t payload_expected,
                           db_hdr_t *hdr) {
    char path[CORE_PATH_MAX];
    if (lib_db_path(lib, generation, kind, path, sizeof path)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    void *buf = NULL;
    db_hdr_t h;
    bool ok = fread(&h, 1, sizeof h, f) == sizeof h && lib_hdr_ok(&h, kind, generation) &&
              (payload_expected == UINT32_MAX || h.payload == payload_expected) &&
              file_size_of(f) == (int64_t)LIB_HDR_SIZE + h.payload && fseek(f, LIB_HDR_SIZE, SEEK_SET) == 0;
    if (ok) {
        buf = core_malloc(h.payload ? h.payload : 1);
        ok = buf && (h.payload == 0 || fread(buf, 1, h.payload, f) == h.payload) &&
             lib_crc32(0, buf, h.payload) == h.crc;
    }
    fclose(f);
    if (!ok) {
        core_free(buf);
        return NULL;
    }
    if (hdr) *hdr = h;
    return buf;
}

// --------------------------------------------------------------- snapshot ----
static void cache_invalidate(library_t *lib) {
    for (int i = 0; i < LIB_CACHE_BLOCKS; i++) lib->cache[i].file = -1;
}

// Frees a snapshot that no reader can reach any more (closes its files).
static void snap_release(db_snap_t *s) {
    for (int i = 0; i < 3; i++) {
        if (s->f[i]) fclose(s->f[i]);
    }
    core_free(s->info_buf);
    core_free(s->ent_buf);
    core_free(s->files_mem);
    for (size_t i = 0; i < CORE_ARRAY_SIZE(s->view_buf); i++) core_free(s->view_buf[i]);
    memset(s, 0, sizeof *s);
}

static void snap_free(library_t *lib) {
    snap_release(&lib->snap);
    lib->pub_gen = 0;
    cache_invalidate(lib);
}

static uint32_t payload_of(const db_snap_t *s, int file) {
    switch (file) {
    case DBF_TRACKS: return s->info.tracks * (uint32_t)sizeof(db_rec_t);
    case DBF_STRINGS: return s->info.strings_bytes;
    default: return s->info.tracks * (uint32_t)sizeof(db_file_ent_t);
    }
}

static uint32_t blocks_of(uint32_t payload) { return (payload + LIB_BLOCK - 1) / LIB_BLOCK; }

static bool ids_below(const uint32_t *a, uint32_t n, uint32_t limit) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] >= limit) return false;
    }
    return true;
}

static bool range_ok(uint32_t first, uint32_t count, uint32_t limit) {
    return first <= limit && count <= limit - first;
}

// Loads and checks snapshot `gen` into s (empty on entry). Touches only the files of that
// generation, so it runs without the lock while the loaded snapshot serves queries.
static int snap_load(library_t *lib, db_snap_t *s, uint32_t gen) {
    db_hdr_t h;
    uint8_t *ib = load_crc_file(lib, DBF_INFO, gen, UINT32_MAX, &h);
    if (!ib) return CORE_ENOTFOUND;
    s->info_buf = ib;
    s->generation = h.generation;
    if (h.payload < sizeof(db_info_t)) goto corrupt;
    memcpy(&s->info, ib, sizeof s->info);
    s->sort_opts.ignore_articles = (s->info.flags & 1) != 0;
    const db_info_t *in = &s->info;
    if (in->tracks > LIB_MAX_TRACKS || in->music_tracks > in->tracks || in->artists > LIB_MAX_ENTITIES ||
        in->music_artists > in->artists || in->albums > LIB_MAX_ENTITIES || in->music_albums > in->albums ||
        in->genres > 65536 || in->music_genres > in->genres || in->album_bids > LIB_MAX_ENTITIES ||
        in->genre_bids > 65536 || in->names_bytes == 0 || in->galbums > LIB_MAX_ENTITIES * 4u) {
        goto corrupt;
    }
    {
        uint64_t need = sizeof(db_info_t);
        for (int i = 0; i < 3; i++) {
            if (in->crc_blocks[i] != blocks_of(payload_of(s, i))) goto corrupt;
            need += 4ull * in->crc_blocks[i];
        }
        need += 4ull * in->crc_blocks[DBF_FILES];  // files.bin fence
        if (h.payload != need) goto corrupt;
        uint32_t *p = (uint32_t *)(ib + sizeof(db_info_t));
        for (int i = 0; i < 3; i++) {
            s->block_crc[i] = p;
            p += in->crc_blocks[i];
        }
        s->files_fence = p;
    }
    // Block-checked files: checked here, opened again on the first cache miss.
    for (int i = 0; i < 3; i++) {
        char path[CORE_PATH_MAX];
        if (lib_db_path(lib, s->generation, i, path, sizeof path)) goto corrupt;
        FILE *f = fopen(path, "rb");
        db_hdr_t fh;
        const bool ok = f && fread(&fh, 1, sizeof fh, f) == sizeof fh && lib_hdr_ok(&fh, i, s->generation) &&
                        fh.payload == payload_of(s, i) && file_size_of(f) == (int64_t)LIB_HDR_SIZE + fh.payload;
        if (f) fclose(f);
        if (!ok) goto corrupt;
    }
    {
        uint64_t ent = (uint64_t)in->artists * sizeof(db_artist_t) + (uint64_t)in->albums * sizeof(db_album_t) +
                       (uint64_t)in->genres * sizeof(db_genre_t) + 4ull * in->album_bids + 4ull * in->genre_bids +
                       in->names_bytes;
        if (ent > UINT32_MAX) goto corrupt;
        uint8_t *eb = load_crc_file(lib, DBF_ENTITIES, s->generation, (uint32_t)ent, NULL);
        if (!eb) goto corrupt;
        s->ent_buf = eb;
        s->artists = (db_artist_t *)eb;
        eb += (size_t)in->artists * sizeof(db_artist_t);
        s->albums = (db_album_t *)eb;
        eb += (size_t)in->albums * sizeof(db_album_t);
        s->genres = (db_genre_t *)eb;
        eb += (size_t)in->genres * sizeof(db_genre_t);
        s->album_map = (uint32_t *)eb;
        eb += 4u * in->album_bids;
        s->genre_map = (uint32_t *)eb;
        eb += 4u * in->genre_bids;
        s->names = (const char *)eb;
        if (s->names[in->names_bytes - 1] != 0) goto corrupt;
    }
    {
        const uint32_t sizes[] = {
            in->music_tracks * 4u + in->jumps * (uint32_t)sizeof(db_jump_t),  // tracks.idx
            in->music_tracks * 4u,                                            // recent.idx
            in->music_albums * 4u,                                            // albums.idx
            in->tracks * 4u,                                                  // albtrk.idx
            in->galbums * 4u,                                                 // galbums.idx
            (in->albums - in->music_albums) * 4u,                             // books.idx
        };
        for (int i = 0; i < 6; i++) {
            s->view_buf[i] = load_crc_file(lib, DBF_V_TRACKS + i, s->generation, sizes[i], NULL);
            if (!s->view_buf[i]) goto corrupt;
        }
        s->v_tracks = s->view_buf[0];
        s->jumps = (db_jump_t *)(s->v_tracks + in->music_tracks);
        s->v_recent = s->view_buf[1];
        s->v_albums = s->view_buf[2];
        s->v_albtrk = s->view_buf[3];
        s->v_galbums = s->view_buf[4];
        s->v_books = s->view_buf[5];
    }
    // Cross-references: a CRC-valid file can still come from a buggy writer.
    for (uint32_t i = 0; i < in->artists; i++) {
        const db_artist_t *a = &s->artists[i];
        if (a->name >= in->names_bytes || !range_ok(a->album_first, a->album_count, in->albums) ||
            !range_ok(a->track_first, a->track_count, in->tracks)) {
            goto corrupt;
        }
    }
    for (uint32_t i = 0; i < in->albums; i++) {
        const db_album_t *a = &s->albums[i];
        if (a->title >= in->names_bytes || a->artist >= in->artists ||
            !range_ok(a->track_first, a->track_count, in->tracks)) {
            goto corrupt;
        }
    }
    for (uint32_t i = 0; i < in->genres; i++) {
        const db_genre_t *g = &s->genres[i];
        if (g->name >= in->names_bytes || !range_ok(g->album_first, g->album_count, in->galbums)) goto corrupt;
    }
    for (uint32_t i = 0; i < in->jumps; i++) {
        if (s->jumps[i].row >= in->music_tracks) goto corrupt;
    }
    if (!ids_below(s->album_map, in->album_bids, in->albums) || !ids_below(s->genre_map, in->genre_bids, in->genres) ||
        !ids_below(s->v_tracks, in->music_tracks, in->tracks) ||
        !ids_below(s->v_recent, in->music_tracks, in->tracks) ||
        !ids_below(s->v_albums, in->music_albums, in->albums) || !ids_below(s->v_albtrk, in->tracks, in->tracks) ||
        !ids_below(s->v_galbums, in->galbums, in->albums) ||
        !ids_below(s->v_books, in->albums - in->music_albums, in->albums)) {
        goto corrupt;
    }
    s->loaded = true;
    CORE_LOGI(TAG, "index loaded: %u tracks, %u albums, %u artists", (unsigned)in->tracks, (unsigned)in->albums,
              (unsigned)in->artists);
    return CORE_OK;
corrupt:
    CORE_LOGW(TAG, "index %08lx in %s is damaged or incomplete; it will be rebuilt by the next scan",
              (unsigned long)gen, lib->db_dir);
    snap_release(s);
    return CORE_ECORRUPT;
}

// ------------------------------------------------------------ block cache ----
static int cache_block(library_t *lib, int file, uint32_t block, const cache_slot_t **out) {
    cache_slot_t *victim = NULL;
    for (int i = 0; i < LIB_CACHE_BLOCKS; i++) {
        cache_slot_t *c = &lib->cache[i];
        if (c->file == file && c->block == block) {
            c->stamp = ++lib->cache_clock;
            *out = c;
            return CORE_OK;
        }
        if (!victim || c->file < 0 || (victim->file >= 0 && c->stamp < victim->stamp)) victim = c;
    }
    db_snap_t *s = &lib->snap;
    uint32_t payload = payload_of(s, file);
    uint32_t off = block * LIB_BLOCK;
    if (off >= payload) return CORE_EINVAL;
    uint32_t len = CORE_MIN(LIB_BLOCK, payload - off);
    victim->file = -1;
    if (!s->f[file]) {
        char path[CORE_PATH_MAX];
        if (lib_db_path(lib, s->generation, file, path, sizeof path) || !(s->f[file] = fopen(path, "rb"))) {
            return CORE_EIO;
        }
    }
    if (fseek(s->f[file], (long)(LIB_HDR_SIZE + off), SEEK_SET) != 0 ||
        fread(victim->data, 1, len, s->f[file]) != len) {
        // Opened again at the next miss: a transient error (card busy, no free handle)
        // must not stick to this handle.
        fclose(s->f[file]);
        s->f[file] = NULL;
        return CORE_EIO;
    }
    if (lib_crc32(0, victim->data, len) != s->block_crc[file][block]) {
        if (!s->corrupt) CORE_LOGW(TAG, "%s: block %u failed its checksum", g_lib_db_names[file], (unsigned)block);
        s->corrupt = true;
        return CORE_ECORRUPT;
    }
    victim->file = (int8_t)file;
    victim->block = block;
    victim->len = len;
    victim->stamp = ++lib->cache_clock;
    *out = victim;
    return CORE_OK;
}

static int cache_read(library_t *lib, int file, uint32_t off, void *dst, size_t len) {
    if (!lib->snap.loaded) return CORE_ENOTFOUND;
    if ((uint64_t)off + len > payload_of(&lib->snap, file)) return CORE_ECORRUPT;
    uint8_t *out = dst;
    while (len) {
        const cache_slot_t *c;
        int r = cache_block(lib, file, off / LIB_BLOCK, &c);
        if (r) return r;
        uint32_t in = off % LIB_BLOCK;
        size_t n = CORE_MIN(len, (size_t)(c->len - in));
        memcpy(out, c->data + in, n);
        out += n;
        off += (uint32_t)n;
        len -= n;
    }
    return CORE_OK;
}

int lib_read_rec(library_t *lib, uint32_t id, db_rec_t *out) {
    if (!lib->snap.loaded || id >= lib->snap.info.tracks) return CORE_ENOTFOUND;
    int r = cache_read(lib, DBF_TRACKS, id * (uint32_t)sizeof(db_rec_t), out, sizeof *out);
    if (r) return r;
    if (out->album >= lib->snap.info.album_bids || out->genre >= lib->snap.info.genre_bids) {
        lib->snap.corrupt = true;
        return CORE_ECORRUPT;
    }
    return CORE_OK;
}

int lib_read_str(library_t *lib, uint32_t off, char *dst, size_t cap) {
    dst[0] = 0;
    if (off == 0) return CORE_OK;
    db_snap_t *s = &lib->snap;
    if (!s->loaded || off >= s->info.strings_bytes) return CORE_ECORRUPT;
    size_t n = 0;
    while (n + 1 < cap && off < s->info.strings_bytes) {
        const cache_slot_t *c;
        int r = cache_block(lib, DBF_STRINGS, off / LIB_BLOCK, &c);
        if (r) {
            dst[0] = 0;
            return r;
        }
        uint32_t in = off % LIB_BLOCK;
        const uint8_t *src = c->data + in;
        size_t avail = CORE_MIN((size_t)(c->len - in), cap - 1 - n);
        const uint8_t *z = memchr(src, 0, avail);
        size_t take = z ? (size_t)(z - src) : avail;
        memcpy(dst + n, src, take);
        n += take;
        off += (uint32_t)take;
        if (z) break;
    }
    dst[n] = 0;
    utf8_truncate(dst, cap - 1);
    return CORE_OK;
}

static int rec_path(library_t *lib, const db_rec_t *rec, char *out, size_t cap) {
    char dir[CORE_PATH_MAX], name[CORE_PATH_MAX];
    int r = lib_read_str(lib, rec->dir, dir, sizeof dir);
    if (!r) r = lib_read_str(lib, rec->name, name, sizeof name);
    if (r) return r;
    return core_path_join(out, cap, dir, name);
}

// Entry i of files.bin: from the RAM copy during a scan, else through the cache.
static int files_ent(library_t *lib, uint32_t i, db_file_ent_t *e) {
    if (lib->snap.files_mem) {
        *e = lib->snap.files_mem[i];
        return CORE_OK;
    }
    return cache_read(lib, DBF_FILES, i * (uint32_t)sizeof *e, e, sizeof *e);
}

uint32_t lib_find_path_locked(library_t *lib, const char *path, db_rec_t *rec_out) {
    db_snap_t *s = &lib->snap;
    if (!s->loaded || !s->info.tracks) return LIB_ID_NONE;
    uint32_t h = lib_path_hash(path);
    // The fence narrows the search to one or two blocks of files.bin.
    uint32_t nb = s->info.crc_blocks[DBF_FILES], lo_b = 0, hi_b = nb;
    while (lo_b < hi_b) {
        uint32_t mid = lo_b + (hi_b - lo_b) / 2;
        if (s->files_fence[mid] < h) lo_b = mid + 1;
        else hi_b = mid;
    }
    // Block lo_b starts at a hash >= h, so the first match is in block lo_b - 1 or is
    // the first entry of block lo_b.
    uint32_t lo = lo_b ? (lo_b - 1) * (uint32_t)DB_FILES_PER_BLOCK : 0;
    uint32_t hi = CORE_MIN(s->info.tracks, lo_b * (uint32_t)DB_FILES_PER_BLOCK + 1);
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        db_file_ent_t e;
        if (files_ent(lib, mid, &e)) return LIB_ID_NONE;
        if (e.hash < h) lo = mid + 1;
        else hi = mid;
    }
    char full[CORE_PATH_MAX];
    for (uint32_t i = lo; i < s->info.tracks; i++) {
        db_file_ent_t e;
        if (files_ent(lib, i, &e) || e.hash != h) break;
        db_rec_t rec;
        if (e.id >= s->info.tracks || lib_read_rec(lib, e.id, &rec)) continue;
        if (rec_path(lib, &rec, full, sizeof full) == CORE_OK && strcmp(full, path) == 0) {
            if (rec_out) *rec_out = rec;
            return e.id;
        }
    }
    return LIB_ID_NONE;
}

// ---------------------------------------------------------- scan support ----
void lib_files_mem(library_t *lib, bool load) {
    if (!load) {
        core_mutex_lock(lib->mutex);
        core_free(lib->snap.files_mem);
        lib->snap.files_mem = NULL;
        core_mutex_unlock(lib->mutex);
        return;
    }
    // Only the scanning thread swaps snapshots, and it is the caller: the snapshot stays
    // while the file is read without the lock.
    core_mutex_lock(lib->mutex);
    const db_snap_t *s = &lib->snap;
    const bool want = s->loaded && !s->files_mem && s->info.tracks;
    const uint32_t gen = s->generation, n = s->info.tracks, nb = s->info.crc_blocks[DBF_FILES];
    const uint32_t *crc = s->block_crc[DBF_FILES];
    core_mutex_unlock(lib->mutex);
    if (!want) return;
    const size_t bytes = (size_t)n * sizeof(db_file_ent_t);
    db_file_ent_t *mem = core_malloc(bytes);
    char path[CORE_PATH_MAX];
    FILE *f = NULL;
    bool ok = mem && lib_db_path(lib, gen, DBF_FILES, path, sizeof path) == CORE_OK && (f = fopen(path, "rb")) &&
              fseek(f, LIB_HDR_SIZE, SEEK_SET) == 0 && fread(mem, 1, bytes, f) == bytes;
    if (f) fclose(f);
    for (uint32_t b = 0; ok && b < nb; b++) {
        const size_t off = (size_t)b * LIB_BLOCK;
        ok = lib_crc32(0, (const uint8_t *)mem + off, CORE_MIN((size_t)LIB_BLOCK, bytes - off)) == crc[b];
    }
    if (!ok) {
        core_free(mem);  // lookups go through the cache, which reports damaged blocks
        return;
    }
    core_mutex_lock(lib->mutex);
    db_snap_t *ls = &lib->snap;
    if (ls->generation == gen && !ls->files_mem) {
        ls->files_mem = mem;
        mem = NULL;
        // The scan looks up every file in RAM: the handle is not needed meanwhile.
        if (ls->f[DBF_FILES]) fclose(ls->f[DBF_FILES]);
        ls->f[DBF_FILES] = NULL;
    }
    core_mutex_unlock(lib->mutex);
    core_free(mem);
}

// ------------------------------------------------------------ generations ----
// Calls fn for every index file in db_dir (name, kind, generation).
typedef void (*gen_file_fn)(const library_t *lib, const char *name, int kind, uint32_t gen, void *ctx);

#define DB_NAME_MAX 32
#define DB_NAMES_MAX 128

static void for_each_db_file(const library_t *lib, gen_file_fn fn, void *ctx) {
    lfs_dir_t *d = lfs_opendir(lib->db_dir);
    if (!d) return;
    // Names first, actions after: removing entries while a directory is read is not portable.
    char (*names)[DB_NAME_MAX] = core_malloc((size_t)DB_NAMES_MAX * DB_NAME_MAX);
    uint32_t n = 0;
    lfs_dirent_t e;
    while (names && n < DB_NAMES_MAX && lfs_readdir(d, &e)) {
        int kind;
        uint32_t gen;
        if (strlen(e.name) < DB_NAME_MAX && parse_db_name(e.name, &kind, &gen)) {
            core_strlcpy(names[n++], e.name, DB_NAME_MAX);
        }
    }
    lfs_closedir(d);
    for (uint32_t i = 0; i < n; i++) {
        int kind;
        uint32_t gen;
        if (parse_db_name(names[i], &kind, &gen)) fn(lib, names[i], kind, gen, ctx);
    }
    core_free(names);
}

static void remove_if_other(const library_t *lib, const char *name, int kind, uint32_t gen, void *ctx) {
    (void)kind;
    if (gen == *(const uint32_t *)ctx) return;
    char path[CORE_PATH_MAX];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", lib->db_dir, name) < sizeof path) lfs_remove(path);
}

void lib_remove_other_gens(const library_t *lib, uint32_t keep) {
    for_each_db_file(lib, remove_if_other, &keep);
    // The fixed-name layout of index version 1 ("tracks.bin", "tmp/tracks.bin").
    char path[CORE_PATH_MAX];
    for (int i = 0; i < DBF_COUNT; i++) {
        if ((size_t)snprintf(path, sizeof path, "%s/%s", lib->db_dir, g_lib_db_names[i]) < sizeof path) {
            lfs_remove(path);
        }
        if ((size_t)snprintf(path, sizeof path, "%s/tmp/%s", lib->db_dir, g_lib_db_names[i]) < sizeof path) {
            lfs_remove(path);
        }
    }
    if ((size_t)snprintf(path, sizeof path, "%s/tmp", lib->db_dir) < sizeof path && lfs_exists(path)) {
        lfs_rmdir(path);
    }
}

void lib_remove_gen(const library_t *lib, uint32_t gen) {
    char path[CORE_PATH_MAX];
    for (int i = 0; i < DBF_COUNT; i++) {
        if (lib_db_path(lib, gen, i, path, sizeof path) == CORE_OK) lfs_remove(path);
    }
}

typedef struct {
    uint32_t gen[8], seq[8];
    uint32_t n;
} gen_list_t;

static void collect_info(const library_t *lib, const char *name, int kind, uint32_t gen, void *ctx) {
    (void)name;
    gen_list_t *l = ctx;
    if (kind != DBF_INFO || l->n >= CORE_ARRAY_SIZE(l->gen)) return;
    db_hdr_t h;
    uint8_t *ib = load_crc_file(lib, DBF_INFO, gen, UINT32_MAX, &h);
    if (!ib) return;
    if (h.payload >= sizeof(db_info_t)) {
        db_info_t in;
        memcpy(&in, ib, sizeof in);
        l->gen[l->n] = gen;
        l->seq[l->n] = in.seq;
        l->n++;
    }
    core_free(ib);
}

// Loads the complete snapshot with the highest commit sequence and deletes the others: an
// interrupted commit leaves the old one too, a crashed build leaves files without info.
static void open_index(library_t *lib) {
    gen_list_t l;
    memset(&l, 0, sizeof l);
    for_each_db_file(lib, collect_info, &l);
    bool tried[CORE_ARRAY_SIZE(l.gen)] = {false};
    for (uint32_t k = 0; k < l.n; k++) {
        uint32_t best = UINT32_MAX;
        for (uint32_t i = 0; i < l.n; i++) {
            if (!tried[i] && (best == UINT32_MAX || l.seq[i] > l.seq[best])) best = i;
        }
        tried[best] = true;
        if (snap_load(lib, &lib->snap, l.gen[best]) == CORE_OK) {
            lib->pub_gen = lib->snap.generation;
            lib_remove_other_gens(lib, l.gen[best]);
            return;
        }
    }
    // Nothing loads: the next scan deletes the leftovers before it writes a new index.
}

int lib_commit(library_t *lib, uint32_t gen, uint32_t seq, uint32_t *idmap, uint32_t n) {
    db_snap_t ns;
    memset(&ns, 0, sizeof ns);
    int r = snap_load(lib, &ns, gen);
    if (r == CORE_OK && ns.info.seq != seq) {
        snap_release(&ns);
        r = CORE_ECORRUPT;
    }
    if (r != CORE_OK) {
        core_free(idmap);
        return r;
    }
    core_mutex_lock(lib->mutex);
    db_snap_t old = lib->snap;
    uint32_t *old_map = lib->idmap;
    lib->snap = ns;
    lib->idmap = idmap;
    lib->idmap_n = idmap ? n : 0;
    lib->idmap_gen = old.generation;
    lib->idmap_valid = idmap || n == 0;  // an empty old snapshot has no ids to translate
    lib->pub_gen = gen;
    cache_invalidate(lib);
    core_mutex_unlock(lib->mutex);
    // Nobody reaches the old snapshot any more: close its files, then delete them.
    snap_release(&old);
    core_free(old_map);
    lib_remove_other_gens(lib, gen);
    return CORE_OK;
}

// -------------------------------------------------------------- positions ----
static void pos_load(library_t *lib) {
    char path[CORE_PATH_MAX], tmp[CORE_PATH_MAX];
    if (lib_db_path(lib, 0, DBF_POSITIONS, path, sizeof path)) return;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return;
    for (int attempt = 0; attempt < 2; attempt++) {
        // A crash between "remove" and "rename" of the atomic save leaves only the .tmp file.
        FILE *f = fopen(attempt == 0 ? path : tmp, "rb");
        if (!f) continue;
        db_hdr_t h;
        bool ok = fread(&h, 1, sizeof h, f) == sizeof h && lib_hdr_ok(&h, DBF_POSITIONS, 0) &&
                  h.count <= LIB_POS_MAX && h.payload == h.count * sizeof(db_pos_t);
        if (ok && h.count) {
            ok = fread(lib->pos, 1, h.payload, f) == h.payload && lib_crc32(0, lib->pos, h.payload) == h.crc;
        }
        fclose(f);
        if (ok) {
            lib->pos_count = h.count;
            for (uint32_t i = 0; i < h.count; i++) lib->pos_stamp = CORE_MAX(lib->pos_stamp, lib->pos[i].stamp);
            return;
        }
        lib->pos_count = 0;
    }
}

// Writes positions.bin from a copy of the table (buf: header room + count entries), without
// the library lock: the audio thread and the list rows never wait for the card here.
// version orders concurrent saves, so an older copy never overwrites a newer file.
static int pos_write(library_t *lib, uint8_t *buf, uint32_t count, uint32_t version) {
    char path[CORE_PATH_MAX];
    if (lib_db_path(lib, 0, DBF_POSITIONS, path, sizeof path)) return CORE_EINVAL;
    const size_t payload = (size_t)count * sizeof(db_pos_t);
    db_hdr_t h = {LIB_MAGIC, LIB_VERSION, DBF_POSITIONS, 0, count, (uint32_t)payload,
                  lib_crc32(0, buf + LIB_HDR_SIZE, payload), {0, 0}};
    memcpy(buf, &h, sizeof h);
    int r = CORE_OK;
    core_mutex_lock(lib->save_mutex);
    if ((int32_t)(version - lib->pos_saved) > 0) {
        lfs_mkdirs(lib->db_dir);
        r = lfs_write_atomic(path, buf, LIB_HDR_SIZE + payload);
        if (r == CORE_OK) lib->pos_saved = version;
    }
    core_mutex_unlock(lib->save_mutex);
    return r;
}

int library_save_position(library_t *lib, const char *path, uint32_t position_ms) {
    if (!lib || !path || !*path) return CORE_EINVAL;
    uint64_t h = lib_path_hash64(path);
    uint8_t *buf = core_malloc(LIB_HDR_SIZE + (size_t)LIB_POS_MAX * sizeof(db_pos_t));
    if (!buf) return CORE_ENOMEM;
    core_mutex_lock(lib->mutex);
    int found = -1;
    for (uint32_t i = 0; i < lib->pos_count; i++) {
        if (lib->pos[i].hash == h) {
            found = (int)i;
            break;
        }
    }
    bool changed = false;
    if (position_ms == 0) {
        if (found >= 0) {
            lib->pos[found] = lib->pos[--lib->pos_count];
            changed = true;
        }
    } else if (found < 0 || lib->pos[found].ms != position_ms) {
        if (found < 0) {
            if (lib->pos_count < LIB_POS_MAX) {
                found = (int)lib->pos_count++;
            } else {
                found = 0;  // forget the least recently saved position
                for (uint32_t i = 1; i < lib->pos_count; i++) {
                    if (lib->pos[i].stamp < lib->pos[found].stamp) found = (int)i;
                }
            }
        }
        lib->pos[found].hash = h;
        lib->pos[found].ms = position_ms;
        lib->pos[found].stamp = ++lib->pos_stamp;
        changed = true;
    }
    const uint32_t count = lib->pos_count, version = changed ? ++lib->pos_version : 0;
    if (changed) memcpy(buf + LIB_HDR_SIZE, lib->pos, (size_t)count * sizeof(db_pos_t));
    core_mutex_unlock(lib->mutex);
    int r = changed ? pos_write(lib, buf, count, version) : CORE_OK;
    core_free(buf);
    return r;
}

uint32_t library_load_position(library_t *lib, const char *path) {
    if (!lib || !path) return 0;
    uint64_t h = lib_path_hash64(path);
    uint32_t ms = 0;
    core_mutex_lock(lib->mutex);
    for (uint32_t i = 0; i < lib->pos_count; i++) {
        if (lib->pos[i].hash == h) {
            ms = lib->pos[i].ms;
            break;
        }
    }
    core_mutex_unlock(lib->mutex);
    return ms;
}

// ------------------------------------------------------------ open/close ----
library_t *library_open(const library_config_t *cfg) {
    if (!cfg || !cfg->music_root || !cfg->db_dir || strlen(cfg->db_dir) + 24 >= CORE_PATH_MAX) return NULL;
    library_t *lib = core_calloc(1, sizeof *lib);
    if (!lib) return NULL;
    core_strlcpy(lib->root, cfg->music_root, sizeof lib->root);
    core_strlcpy(lib->db_dir, cfg->db_dir, sizeof lib->db_dir);
    size_t rl = strlen(lib->root);
    while (rl > 1 && lib->root[rl - 1] == '/') lib->root[--rl] = 0;
    lib->opts.ignore_articles = cfg->ignore_articles;
    lib->throttle = cfg->throttle;
    lib->throttle_user = cfg->throttle_user;
    lib->mutex = core_mutex_create();
    lib->save_mutex = core_mutex_create();
    lib->cache_mem = core_malloc((size_t)LIB_CACHE_BLOCKS * LIB_BLOCK);
    lib->pos = core_calloc(LIB_POS_MAX, sizeof *lib->pos);
    if (!lib->mutex || !lib->save_mutex || !lib->cache_mem || !lib->pos) {
        library_close(lib);
        return NULL;
    }
    for (int i = 0; i < LIB_CACHE_BLOCKS; i++) {
        lib->cache[i].data = lib->cache_mem + (size_t)i * LIB_BLOCK;
        lib->cache[i].file = -1;
    }
    open_index(lib);
    pos_load(lib);
    return lib;
}

void library_close(library_t *lib) {
    if (!lib) return;
    if (lib->scanning) CORE_LOGE(TAG, "library_close() during a scan: cancel and wait for library_scan() first");
    snap_free(lib);
    core_free(lib->idmap);
    core_free(lib->cache_mem);
    core_free(lib->pos);
    if (lib->save_mutex) core_mutex_destroy(lib->save_mutex);
    if (lib->mutex) core_mutex_destroy(lib->mutex);
    core_free(lib);
}

int library_scan(library_t *lib, bool full_rebuild, lib_scan_cb_t cb, void *user) {
    if (!lib) return CORE_EINVAL;
    core_mutex_lock(lib->mutex);
    bool busy = lib->scanning;
    if (!busy) lib->scanning = true;
    core_mutex_unlock(lib->mutex);
    if (busy) return CORE_EAGAIN;
    int r = lib_scan_run(lib, full_rebuild, cb, user);
    core_mutex_lock(lib->mutex);
    lib->scanning = false;
    core_mutex_unlock(lib->mutex);
    return r;
}

bool library_is_scanning(const library_t *lib) { return lib && lib->scanning; }

uint32_t library_generation(const library_t *lib) { return lib ? lib->pub_gen : 0; }

bool library_translate_ids(library_t *lib, uint32_t *gen, lib_id_t *ids, uint32_t n) {
    if (!lib || !gen) return false;
    core_mutex_lock(lib->mutex);
    const uint32_t cur = lib->snap.generation;
    bool ok = *gen == cur;
    if (!ok && lib->idmap_valid && *gen == lib->idmap_gen) {
        for (uint32_t i = 0; ids && i < n; i++) {
            if (ids[i] != LIB_ID_NONE) ids[i] = ids[i] < lib->idmap_n ? lib->idmap[ids[i]] : LIB_ID_NONE;
        }
        *gen = cur;
        ok = true;
    }
    core_mutex_unlock(lib->mutex);
    return ok;
}

uint32_t library_track_count(const library_t *lib) {
    if (!lib) return 0;
    library_t *l = (library_t *)lib;
    core_mutex_lock(l->mutex);
    uint32_t n = l->snap.loaded ? l->snap.info.tracks : 0;
    core_mutex_unlock(l->mutex);
    return n;
}

// ------------------------------------------------------------------ views ----
typedef enum { ROW_ARTIST, ROW_ALBUM, ROW_GENRE, ROW_TRACK } row_kind_t;

typedef struct {
    const uint32_t *ids;  // NULL: row i is id first + i
    uint32_t first, count;
    row_kind_t kind;
    bool letter_sorted;   // rows are in collation order of their titles
} view_t;

static bool view_resolve(library_t *lib, lib_view_t view, lib_id_t parent, view_t *v) {
    db_snap_t *s = &lib->snap;
    memset(v, 0, sizeof *v);
    if (!s->loaded) return false;
    const db_info_t *in = &s->info;
    switch (view) {
    case LIB_VIEW_ARTISTS:
        v->count = in->music_artists;
        v->kind = ROW_ARTIST;
        v->letter_sorted = true;
        return true;
    case LIB_VIEW_ALBUMS:
        v->ids = s->v_albums;
        v->count = in->music_albums;
        v->kind = ROW_ALBUM;
        v->letter_sorted = true;
        return true;
    case LIB_VIEW_ARTIST_ALBUMS:
        if (parent >= in->artists) return false;
        v->first = s->artists[parent].album_first;
        v->count = s->artists[parent].album_count;
        v->kind = ROW_ALBUM;
        return true;
    case LIB_VIEW_ALBUM_TRACKS:
        if (parent >= in->albums) return false;
        v->ids = s->v_albtrk;
        v->first = s->albums[parent].track_first;
        v->count = s->albums[parent].track_count;
        v->kind = ROW_TRACK;
        return true;
    case LIB_VIEW_ARTIST_TRACKS:
        if (parent >= in->artists) return false;
        v->ids = s->v_albtrk;
        v->first = s->artists[parent].track_first;
        v->count = s->artists[parent].track_count;
        v->kind = ROW_TRACK;
        return true;
    case LIB_VIEW_TRACKS:
        v->ids = s->v_tracks;
        v->count = in->music_tracks;
        v->kind = ROW_TRACK;
        v->letter_sorted = true;
        return true;
    case LIB_VIEW_GENRES:
        v->count = in->music_genres;
        v->kind = ROW_GENRE;
        v->letter_sorted = true;
        return true;
    case LIB_VIEW_GENRE_ALBUMS:
        if (parent >= in->genres) return false;
        v->ids = s->v_galbums;
        v->first = s->genres[parent].album_first;
        v->count = s->genres[parent].album_count;
        v->kind = ROW_ALBUM;
        v->letter_sorted = true;
        return true;
    case LIB_VIEW_RECENT:
        v->ids = s->v_recent;
        v->count = in->music_tracks;
        v->kind = ROW_TRACK;
        return true;
    case LIB_VIEW_AUDIOBOOKS:
        v->ids = s->v_books;
        v->count = in->albums - in->music_albums;
        v->kind = ROW_ALBUM;
        v->letter_sorted = true;
        return true;
    default:
        return false;
    }
}

static inline uint32_t view_id(const view_t *v, uint32_t i) { return v->ids ? v->ids[v->first + i] : v->first + i; }

// Title of a RAM row (artist, album, genre).
static const char *ram_title(const db_snap_t *s, row_kind_t kind, uint32_t id) {
    switch (kind) {
    case ROW_ARTIST: return s->names + s->artists[id].name;
    case ROW_ALBUM: return s->names + s->albums[id].title;
    default: return s->names + s->genres[id].name;
    }
}

static uint32_t row_rank(library_t *lib, const view_t *v, uint32_t i) {
    uint32_t id = view_id(v, i);
    if (v->kind != ROW_TRACK) {
        return collate_letter_rank(collate_index_letter(ram_title(&lib->snap, v->kind, id), &lib->snap.sort_opts));
    }
    db_rec_t rec;
    char title[TAG_TEXT_MAX];
    if (lib_read_rec(lib, id, &rec) || lib_read_str(lib, rec.title, title, sizeof title)) return 0;
    return collate_letter_rank(collate_index_letter(title, &lib->snap.sort_opts));
}

uint32_t library_view_count(library_t *lib, lib_view_t view, lib_id_t parent) {
    if (!lib) return 0;
    core_mutex_lock(lib->mutex);
    view_t v;
    uint32_t n = view_resolve(lib, view, parent, &v) ? v.count : 0;
    core_mutex_unlock(lib->mutex);
    return n;
}

static void fill_track_item(library_t *lib, uint32_t id, lib_item_t *out) {
    db_snap_t *s = &lib->snap;
    db_rec_t rec;
    if (lib_read_rec(lib, id, &rec) != CORE_OK) return;  // damaged block: empty row
    lib_read_str(lib, rec.title, out->title, sizeof out->title);
    lib_read_str(lib, rec.artist, out->subtitle, sizeof out->subtitle);
    if (!out->subtitle[0]) {
        const db_album_t *al = &s->albums[s->album_map[rec.album]];
        core_strlcpy(out->subtitle, s->names + s->artists[al->artist].name, sizeof out->subtitle);
    }
    out->duration_ms = rec.duration_ms;
    out->year = rec.year;
    out->sort_letter = collate_index_letter(out->title, &lib->snap.sort_opts);
}

int library_view_item(library_t *lib, lib_view_t view, lib_id_t parent, uint32_t index, lib_item_t *out) {
    if (!lib || !out) return CORE_EINVAL;
    memset(out, 0, sizeof *out);
    core_mutex_lock(lib->mutex);
    view_t v;
    int r = CORE_OK;
    if (!view_resolve(lib, view, parent, &v) || index >= v.count) {
        r = CORE_ENOTFOUND;
    } else {
        db_snap_t *s = &lib->snap;
        uint32_t id = view_id(&v, index);
        out->id = id;
        switch (v.kind) {
        case ROW_ARTIST: {
            const db_artist_t *a = &s->artists[id];
            core_strlcpy(out->title, s->names + a->name, sizeof out->title);
            out->count = a->album_count;
            out->duration_ms = a->duration_ms;
            break;
        }
        case ROW_ALBUM: {
            const db_album_t *al = &s->albums[id];
            core_strlcpy(out->title, s->names + al->title, sizeof out->title);
            core_strlcpy(out->subtitle, s->names + s->artists[al->artist].name, sizeof out->subtitle);
            out->count = al->track_count;
            out->duration_ms = al->duration_ms;
            out->year = al->year;
            break;
        }
        case ROW_GENRE: {
            const db_genre_t *g = &s->genres[id];
            core_strlcpy(out->title, s->names + g->name, sizeof out->title);
            out->count = g->album_count;
            break;
        }
        case ROW_TRACK:
            fill_track_item(lib, id, out);
            break;
        }
        if (v.kind != ROW_TRACK) out->sort_letter = collate_index_letter(out->title, &lib->snap.sort_opts);
    }
    core_mutex_unlock(lib->mutex);
    return r;
}

uint32_t library_view_find_letter(library_t *lib, lib_view_t view, lib_id_t parent, uint32_t letter) {
    if (!lib) return 0;
    core_mutex_lock(lib->mutex);
    view_t v;
    uint32_t result = 0;
    if (view_resolve(lib, view, parent, &v)) {
        uint32_t rank = collate_letter_rank(letter);
        if (view == LIB_VIEW_TRACKS) {
            // Jump table built at scan time: no card access.
            const db_jump_t *j = lib->snap.jumps;
            uint32_t n = lib->snap.info.jumps, lo = 0, hi = n;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                if (j[mid].rank < rank) lo = mid + 1;
                else hi = mid;
            }
            result = lo < n ? j[lo].row : v.count;
        } else if (v.letter_sorted) {
            uint32_t lo = 0, hi = v.count;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                if (row_rank(lib, &v, mid) < rank) lo = mid + 1;
                else hi = mid;
            }
            result = lo;
        } else {
            // Not sorted by title (by year, track number or date): first row at or past the letter.
            result = v.count;
            for (uint32_t i = 0; i < v.count; i++) {
                if (row_rank(lib, &v, i) >= rank) {
                    result = i;
                    break;
                }
            }
        }
    }
    core_mutex_unlock(lib->mutex);
    return result;
}

// Append the tracks of one album (optionally only those of one genre).
static void add_album_tracks(library_t *lib, uint32_t album, uint32_t genre, lib_id_t *out, uint32_t max,
                             uint32_t *n) {
    db_snap_t *s = &lib->snap;
    const db_album_t *al = &s->albums[album];
    for (uint32_t k = 0; k < al->track_count; k++) {
        uint32_t id = s->v_albtrk[al->track_first + k];
        if (genre != LIB_ID_NONE) {
            db_rec_t rec;
            if (lib_read_rec(lib, id, &rec) || s->genre_map[rec.genre] != genre) continue;
        }
        if (*n < max) out[*n] = id;
        (*n)++;
    }
}

uint32_t library_view_track_ids(library_t *lib, lib_view_t view, lib_id_t parent, lib_id_t *out, uint32_t max) {
    if (!lib) return 0;
    if (!out) max = 0;
    core_mutex_lock(lib->mutex);
    db_snap_t *s = &lib->snap;
    view_t v;
    uint32_t n = 0;
    if (view_resolve(lib, view, parent, &v)) {
        switch (view) {
        case LIB_VIEW_ARTISTS:
            // All music tracks in artist, album, track order (the head of the album-ordered array).
            for (uint32_t i = 0; i < s->info.music_tracks; i++, n++) {
                if (n < max) out[n] = s->v_albtrk[i];
            }
            break;
        case LIB_VIEW_ARTIST_ALBUMS:
            if (parent < s->info.artists) {
                const db_artist_t *a = &s->artists[parent];
                for (uint32_t i = 0; i < a->track_count; i++, n++) {
                    if (n < max) out[n] = s->v_albtrk[a->track_first + i];
                }
            }
            break;
        case LIB_VIEW_ALBUMS:
        case LIB_VIEW_AUDIOBOOKS:
            for (uint32_t i = 0; i < v.count; i++) add_album_tracks(lib, view_id(&v, i), LIB_ID_NONE, out, max, &n);
            break;
        case LIB_VIEW_GENRES:
            for (uint32_t g = 0; g < s->info.music_genres; g++) {
                const db_genre_t *ge = &s->genres[g];
                for (uint32_t i = 0; i < ge->album_count; i++) {
                    add_album_tracks(lib, s->v_galbums[ge->album_first + i], g, out, max, &n);
                }
            }
            break;
        case LIB_VIEW_GENRE_ALBUMS:
            for (uint32_t i = 0; i < v.count; i++) add_album_tracks(lib, view_id(&v, i), parent, out, max, &n);
            break;
        default:  // track views
            for (uint32_t i = 0; i < v.count; i++, n++) {
                if (n < max) out[n] = view_id(&v, i);
            }
            break;
        }
    }
    core_mutex_unlock(lib->mutex);
    return n;
}

int library_get_track(library_t *lib, lib_id_t track_id, lib_track_t *out) {
    if (!lib || !out) return CORE_EINVAL;
    memset(out, 0, sizeof *out);
    tags_clear(&out->tags);
    core_mutex_lock(lib->mutex);
    db_snap_t *s = &lib->snap;
    db_rec_t rec;
    int r = lib_read_rec(lib, track_id, &rec);
    if (r == CORE_OK) r = rec_path(lib, &rec, out->path, sizeof out->path);
    if (r == CORE_OK) {
        track_tags_t *t = &out->tags;
        out->id = track_id;
        out->album_id = s->album_map[rec.album];
        const db_album_t *al = &s->albums[out->album_id];
        out->artist_id = al->artist;
        out->genre_id = s->genre_map[rec.genre];
        out->file_size = rec.file_size;
        out->mtime = rec.mtime;
        lib_read_str(lib, rec.title, t->title, sizeof t->title);
        lib_read_str(lib, rec.artist, t->artist, sizeof t->artist);
        lib_read_str(lib, rec.composer, t->composer, sizeof t->composer);
        core_strlcpy(t->album, s->names + al->title, sizeof t->album);
        core_strlcpy(t->album_artist, s->names + s->artists[al->artist].name, sizeof t->album_artist);
        core_strlcpy(t->genre, s->names + s->genres[out->genre_id].name, sizeof t->genre);
        t->year = rec.year;
        t->track_no = rec.track_no;
        t->track_total = rec.track_total;
        t->disc_no = rec.disc_no;
        t->disc_total = rec.disc_total;
        t->rg_track_gain_db = rec.rg_track_gain;
        t->rg_track_peak = rec.rg_track_peak;
        t->rg_album_gain_db = rec.rg_album_gain;
        t->rg_album_peak = rec.rg_album_peak;
        t->cover_offset = rec.cover_offset;
        t->cover_size = rec.cover_size;
        t->cover_mime = rec.cover_mime;
        t->duration_ms = rec.duration_ms;
        t->sample_rate = rec.sample_rate;
        t->bits = rec.bits;
        t->channels = rec.channels;
        t->codec = (codec_id_t)(rec.codec < CODEC_COUNT ? rec.codec : CODEC_UNKNOWN);
        t->is_audiobook_hint = (al->flags & ALB_F_BOOK) != 0;
    }
    core_mutex_unlock(lib->mutex);
    return r;
}

lib_id_t library_find_path(library_t *lib, const char *path) {
    if (!lib || !path) return LIB_ID_NONE;
    core_mutex_lock(lib->mutex);
    lib_id_t id = lib_find_path_locked(lib, path, NULL);
    core_mutex_unlock(lib->mutex);
    return id;
}
