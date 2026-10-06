// SPDX-License-Identifier: Apache-2.0
// Library scanner and index builder. Walks music_root, reuses unchanged records of
// the loaded snapshot, parses tags of new and changed files, and writes a complete
// index of a new generation, which library.c swaps in. Design: top of library.c.
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio_player/decoder.h"
#include "lib_db.h"

#define TAG "libscan"
#define SCAN_MAX_DEPTH 32
#define SCAN_DIR_MAX_ENTRIES 20000

// ================================================================== pacing ===
// The scan task may run above the idle task: CPU-bound stretches (sorting tens of thousands
// of entries takes seconds on the device) call the throttle at least every PACE_MS, which
// lets the platform yield, and poll for cancellation through the progress callback.
#define PACE_MS 5u
#define PACE_STEPS 64u         // cheap steps between two looks at the clock
#define PACE_FORCE_MIN 4096u   // sorts from this size also pace after every merge pass

typedef struct {
    library_t *lib;
    lib_scan_cb_t cb;
    void *user;
    const lib_scan_progress_t *prog;
    uint32_t last_ms, steps;
    bool counting;   // pass 1: progress reports no total yet
    bool cancelled;
} pace_t;

// Throttle and cancel poll now. Returns false once the scan is cancelled.
static bool pace_now(pace_t *p) {
    if (!p) return true;
    if (p->lib->throttle) p->lib->throttle(p->lib->throttle_user);
    if (p->cb && !p->cancelled) {
        lib_scan_progress_t pr = *p->prog;
        pr.current_path = "";
        if (p->counting) pr.files_seen = pr.files_total = 0;
        if (!p->cb(&pr, p->user)) p->cancelled = true;
    }
    p->last_ms = core_now_ms();
    p->steps = 0;
    return !p->cancelled;
}

// pace_now() once PACE_MS have passed since the last one: call it from every loop step.
static inline bool pace(pace_t *p) {
    if (!p) return true;
    if (p->cancelled) return false;
    if (++p->steps < PACE_STEPS) return true;
    p->steps = 0;
    if (core_now_ms() - p->last_ms < PACE_MS) return true;
    return pace_now(p);
}

// ================================================================= sorting ===
// Stable merge sort of uint32 items with a context (qsort has none in C17).
typedef int (*u32_cmp_t)(const void *ctx, uint32_t a, uint32_t b);

static bool merge_pass(uint32_t *a, uint32_t *tmp, uint32_t n, u32_cmp_t cmp, const void *ctx, pace_t *pc) {
    for (uint32_t width = 1; width < n; width *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * width) {
            uint32_t mid = CORE_MIN(lo + width, n), hi = CORE_MIN(lo + 2 * width, n);
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                tmp[k++] = cmp(ctx, a[j], a[i]) < 0 ? a[j++] : a[i++];
                if (!pace(pc)) return false;
            }
            while (i < mid) tmp[k++] = a[i++];
            while (j < hi) tmp[k++] = a[j++];
        }
        memcpy(a, tmp, (size_t)n * sizeof *a);
        if (!(n >= PACE_FORCE_MIN ? pace_now(pc) : pace(pc))) return false;
    }
    return true;
}

// False when memory is short or the scan was cancelled (pc->cancelled tells which).
static bool sort_u32(uint32_t *a, uint32_t n, u32_cmp_t cmp, const void *ctx, pace_t *pc) {
    if (n < 2) return true;
    uint32_t *tmp = core_malloc((size_t)n * sizeof *tmp);
    if (!tmp) return false;
    bool ok = merge_pass(a, tmp, n, cmp, ctx, pc);
    core_free(tmp);
    return ok;
}

// ============================================================ block writer ===
// Writes through a RAM buffer and appends it to the file when full: the file is open
// only for that moment, so a scan does not hold FAT file handles between flushes.
#define BW_BUF (16u * 1024)

typedef struct {
    char path[CORE_PATH_MAX];
    uint8_t *buf;
    uint32_t blen;       // bytes waiting in buf
    uint32_t len;        // payload bytes written (buffered included)
    uint32_t block_crc;  // CRC of the unfinished block
    uint32_t *crcs;
    uint32_t ncrc, cap;
    bool open, err;
} bw_t;

static bool bw_open(bw_t *w, const char *path) {
    memset(w, 0, sizeof *w);
    w->err = true;
    if (core_strlcpy(w->path, path, sizeof w->path) >= sizeof w->path || !(w->buf = core_malloc(BW_BUF))) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    static const uint8_t zero[LIB_HDR_SIZE];
    w->err = fwrite(zero, 1, sizeof zero, f) != sizeof zero;
    if (fclose(f) != 0) w->err = true;
    w->open = true;
    return !w->err;
}

static void bw_flush(bw_t *w) {
    if (w->err || !w->blen) return;
    FILE *f = fopen(w->path, "ab");
    if (!f || fwrite(w->buf, 1, w->blen, f) != w->blen) w->err = true;
    if (f && fclose(f) != 0) w->err = true;
    w->blen = 0;
}

static void bw_push_crc(bw_t *w) {
    if (w->ncrc == w->cap) {
        uint32_t cap = w->cap ? w->cap * 2 : 256;
        uint32_t *n = core_realloc(w->crcs, (size_t)cap * sizeof *n);
        if (!n) {
            w->err = true;
            return;
        }
        w->crcs = n;
        w->cap = cap;
    }
    w->crcs[w->ncrc++] = w->block_crc;
    w->block_crc = 0;
}

static void bw_write(bw_t *w, const void *data, size_t len) {
    if (w->err || !len) return;
    if ((uint64_t)w->len + len > UINT32_MAX - LIB_BLOCK) {
        w->err = true;
        return;
    }
    const uint8_t *p = data;
    size_t left = len;
    while (left) {
        uint32_t in = w->len % LIB_BLOCK;
        size_t n = CORE_MIN(left, (size_t)(LIB_BLOCK - in));
        w->block_crc = lib_crc32(w->block_crc, p, n);
        w->len += (uint32_t)n;
        p += n;
        left -= n;
        if (w->len % LIB_BLOCK == 0) bw_push_crc(w);
    }
    p = data;
    for (left = len; left && !w->err;) {
        size_t n = CORE_MIN(left, (size_t)(BW_BUF - w->blen));
        memcpy(w->buf + w->blen, p, n);
        w->blen += (uint32_t)n;
        p += n;
        left -= n;
        if (w->blen == BW_BUF) bw_flush(w);
    }
}

static int bw_finish(bw_t *w, int kind, uint32_t gen, uint32_t count) {
    if (!w->open) return CORE_EIO;
    w->open = false;
    if (w->len % LIB_BLOCK) bw_push_crc(w);
    bw_flush(w);
    core_free(w->buf);
    w->buf = NULL;
    db_hdr_t h = {LIB_MAGIC, LIB_VERSION, (uint16_t)kind, gen, count, w->len, 0, {0, 0}};
    FILE *f = w->err ? NULL : fopen(w->path, "r+b");
    if (!f || fwrite(&h, 1, sizeof h, f) != sizeof h || lfs_fsync(f) != CORE_OK) w->err = true;
    if (f && fclose(f) != 0) w->err = true;
    return w->err ? CORE_EIO : CORE_OK;
}

// Frees the writer (the caller removes its file).
static void bw_abort(bw_t *w) {
    w->open = false;
    core_free(w->buf);
    w->buf = NULL;
    core_free(w->crcs);
    w->crcs = NULL;
}

// Small file with a whole-payload CRC, written from several parts.
typedef struct {
    const void *p;
    size_t n;
} part_t;

static int write_crc_file(const library_t *lib, int kind, uint32_t gen, uint32_t count, const part_t *parts,
                          int nparts) {
    char path[CORE_PATH_MAX];
    if (lib_db_path(lib, gen, kind, path, sizeof path)) return CORE_EINVAL;
    uint32_t crc = 0;
    uint64_t total = 0;
    for (int i = 0; i < nparts; i++) {
        if (parts[i].n) crc = lib_crc32(crc, parts[i].p, parts[i].n);
        total += parts[i].n;
    }
    if (total > UINT32_MAX) return CORE_ENOMEM;
    FILE *f = fopen(path, "wb");
    if (!f) return CORE_EIO;
    db_hdr_t h = {LIB_MAGIC, LIB_VERSION, (uint16_t)kind, gen, count, (uint32_t)total, crc, {0, 0}};
    bool ok = fwrite(&h, 1, sizeof h, f) == sizeof h;
    for (int i = 0; ok && i < nparts; i++) {
        if (parts[i].n) ok = fwrite(parts[i].p, 1, parts[i].n, f) == parts[i].n;
    }
    if (ok) ok = lfs_fsync(f) == CORE_OK;
    if (fclose(f) != 0) ok = false;
    return ok ? CORE_OK : CORE_EIO;
}

// ================================================================ builder ====
// Spelling of a merged entity ("Pink Floyd" / "PINK FLOYD"): the majority spelling,
// found with a Boyer-Moore vote in O(1) memory; `alt` remembers the previous candidate so
// alternating spellings do not grow the name pool.
typedef struct {
    uint32_t name, alt, votes;
} b_spelling_t;

typedef struct {
    b_spelling_t sp;
    uint32_t hash;
    uint8_t has_music;
} b_named_t;  // artists and genres

typedef struct {
    b_spelling_t sp;
    uint32_t hash, artist;
    uint32_t duration_ms, tracks;
    uint16_t year;
    uint8_t flags;
} b_album_t;

// Per-track data kept in RAM while building (20 bytes per track).
typedef struct {
    uint32_t album;       // album build id
    uint32_t disc_track;  // disc << 16 | track number
    uint32_t mtime;
    uint32_t path_hash;
    uint32_t key;         // offset of the length-prefixed title sort key in the key pool
} b_track_t;

// Open-addressing index over entity build ids (slot = bid + 1, 0 = empty).
typedef struct {
    uint32_t *slot;
    uint32_t mask;
} b_index_t;

typedef struct {
    library_t *lib;
    uint32_t gen, seq;
    collate_opts_t sort_opts;
    pace_t pace;
    bw_t rec, str;
    uint32_t last_dir, last_artist, last_composer;  // offsets of the previous values (dedup)
    char last_dir_s[CORE_PATH_MAX], last_artist_s[TAG_TEXT_MAX], last_composer_s[TAG_TEXT_MAX];
    b_named_t *artists, *genres;
    b_album_t *albums;
    uint32_t n_artists, cap_artists, n_genres, cap_genres, n_albums, cap_albums;
    b_index_t ix_artists, ix_genres, ix_albums;
    char *names;
    uint32_t names_len, names_cap;
    b_track_t *tracks;
    uint32_t n_tracks, cap_tracks;
    uint8_t *keys;
    uint32_t keys_len, keys_cap;
    uint64_t *pairs;  // (genre bid << 32 | album bid) + 1, open addressing
    uint32_t pairs_mask, n_pairs;
    uint32_t *idmap;  // old track id -> new track id (LIB_ID_NONE: not in the new index)
    uint32_t idmap_n;
    bool oom, full_warned;
} build_t;

// Grow by 1.5x (large per-track arrays are pre-sized from the file count of pass 1).
static bool grow_arr(void **p, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return true;
    uint64_t nc = *cap ? *cap : 64;
    while (nc < need) nc += nc / 2;
    if (nc * elem > SIZE_MAX / 2) return false;
    void *np = core_realloc(*p, (size_t)(nc * elem));
    if (!np) return false;
    *p = np;
    *cap = (uint32_t)nc;
    return true;
}

static uint32_t names_add(build_t *b, const char *s) {
    char tmp[TAG_TEXT_MAX];
    core_strlcpy(tmp, s, sizeof tmp);
    utf8_truncate(tmp, sizeof tmp - 1);
    size_t n = strlen(tmp);
    if (!n) return 0;
    if (!grow_arr((void **)&b->names, &b->names_cap, b->names_len + (uint32_t)n + 1, 1)) {
        b->oom = true;
        return 0;
    }
    uint32_t off = b->names_len;
    memcpy(b->names + off, tmp, n + 1);
    b->names_len += (uint32_t)n + 1;
    return off;
}

static void spelling_init(build_t *b, b_spelling_t *sp, const char *s) {
    sp->name = names_add(b, s);
    sp->alt = 0;
    sp->votes = 1;
}

static void spelling_vote(build_t *b, b_spelling_t *sp, const char *s) {
    char tmp[TAG_TEXT_MAX];
    core_strlcpy(tmp, s, sizeof tmp);
    utf8_truncate(tmp, sizeof tmp - 1);
    if (strcmp(b->names + sp->name, tmp) == 0) {
        sp->votes++;
    } else if (sp->votes > 0) {
        sp->votes--;
    } else {
        uint32_t off = (sp->alt && strcmp(b->names + sp->alt, tmp) == 0) ? sp->alt : names_add(b, tmp);
        if (b->oom) return;
        sp->alt = sp->name;
        sp->name = off;
        sp->votes = 1;
    }
}

// Grouping key: primary collation equality; names without letters or digits ("!!!")
// compare exactly so they do not merge with the unknown ("") bucket.
static uint32_t group_hash(const char *s) {
    if (collate_key(s, NULL, NULL, 0) == 0) return lib_fnv1a32(LIB_FNV32_INIT, s, strlen(s)) ^ 0x5A5A5A5Au;
    return collate_hash_primary(s, NULL);
}

static bool group_eq(const char *a, const char *b) {
    if (collate_cmp_primary(a, b, NULL) != 0) return false;
    return collate_key(a, NULL, NULL, 0) != 0 || strcmp(a, b) == 0;
}

// Make room for one more entry: rehash the `existing` entries when the table would be
// more than half full. hash0 points at the hash field of entry 0.
static bool index_reserve(b_index_t *ix, uint32_t existing, const uint32_t *hash0, size_t stride) {
    if (ix->slot && (existing + 1) * 2 <= ix->mask + 1) return true;
    uint32_t size = ix->slot ? (ix->mask + 1) * 2 : 256;
    uint32_t *ns = core_calloc(size, sizeof *ns);
    if (!ns) return false;
    for (uint32_t i = 0; i < existing; i++) {
        uint32_t h = *(const uint32_t *)((const uint8_t *)hash0 + i * stride);
        uint32_t k = h & (size - 1);
        while (ns[k]) k = (k + 1) & (size - 1);
        ns[k] = i + 1;
    }
    core_free(ix->slot);
    ix->slot = ns;
    ix->mask = size - 1;
    return true;
}

static uint32_t named_find_or_add(build_t *b, b_named_t **arr, uint32_t *n, uint32_t *cap, b_index_t *ix,
                                  const char *name, uint32_t limit) {
    uint32_t h = group_hash(name);
    if (ix->slot) {
        for (uint32_t k = h & ix->mask; ix->slot[k]; k = (k + 1) & ix->mask) {
            uint32_t id = ix->slot[k] - 1;
            if ((*arr)[id].hash == h && group_eq(b->names + (*arr)[id].sp.name, name)) {
                spelling_vote(b, &(*arr)[id].sp, name);
                return id;
            }
        }
    }
    if (*n >= limit) return 0;  // table full: fall back to the first entity (the unknown bucket in practice)
    if (!grow_arr((void **)arr, cap, *n + 1, sizeof **arr) || !index_reserve(ix, *n, &(*arr)[0].hash, sizeof **arr)) {
        b->oom = true;
        return 0;
    }
    uint32_t id = (*n)++;
    spelling_init(b, &(*arr)[id].sp, name);
    (*arr)[id].hash = h;
    (*arr)[id].has_music = 0;
    uint32_t k = h & ix->mask;
    while (ix->slot[k]) k = (k + 1) & ix->mask;
    ix->slot[k] = id + 1;
    return id;
}

static uint32_t album_find_or_add(build_t *b, const char *title, uint32_t artist) {
    uint32_t h = group_hash(title) * 0x9E3779B1u ^ artist;
    if (b->ix_albums.slot) {
        for (uint32_t k = h & b->ix_albums.mask; b->ix_albums.slot[k]; k = (k + 1) & b->ix_albums.mask) {
            uint32_t id = b->ix_albums.slot[k] - 1;
            const b_album_t *a = &b->albums[id];
            if (a->hash == h && a->artist == artist && group_eq(b->names + a->sp.name, title)) {
                spelling_vote(b, &b->albums[id].sp, title);
                return id;
            }
        }
    }
    if (b->n_albums >= LIB_MAX_ENTITIES) return 0;
    if (!grow_arr((void **)&b->albums, &b->cap_albums, b->n_albums + 1, sizeof *b->albums) ||
        !index_reserve(&b->ix_albums, b->n_albums, &b->albums[0].hash, sizeof *b->albums)) {
        b->oom = true;
        return 0;
    }
    uint32_t id = b->n_albums++;
    b_album_t *a = &b->albums[id];
    memset(a, 0, sizeof *a);
    spelling_init(b, &a->sp, title);
    a->hash = h;
    a->artist = artist;
    uint32_t k = h & b->ix_albums.mask;
    while (b->ix_albums.slot[k]) k = (k + 1) & b->ix_albums.mask;
    b->ix_albums.slot[k] = id + 1;
    return id;
}

static void pair_add(build_t *b, uint32_t genre, uint32_t album) {
    if (!b->pairs || (b->n_pairs + 1) * 2 > b->pairs_mask + 1) {
        uint32_t size = b->pairs ? (b->pairs_mask + 1) * 2 : 1024;
        uint64_t *np = core_calloc(size, sizeof *np);
        if (!np) {
            b->oom = true;
            return;
        }
        for (uint32_t i = 0; b->pairs && i <= b->pairs_mask; i++) {
            if (!b->pairs[i]) continue;
            uint32_t k = (uint32_t)lib_fnv1a64(LIB_FNV64_INIT, &b->pairs[i], 8) & (size - 1);
            while (np[k]) k = (k + 1) & (size - 1);
            np[k] = b->pairs[i];
        }
        core_free(b->pairs);
        b->pairs = np;
        b->pairs_mask = size - 1;
    }
    uint64_t v = ((uint64_t)genre << 32 | album) + 1;
    uint32_t k = (uint32_t)lib_fnv1a64(LIB_FNV64_INIT, &v, 8) & b->pairs_mask;
    while (b->pairs[k]) {
        if (b->pairs[k] == v) return;
        k = (k + 1) & b->pairs_mask;
    }
    b->pairs[k] = v;
    b->n_pairs++;
}

static uint32_t str_add(build_t *b, const char *s) {
    size_t n = strlen(s);
    if (!n) return 0;
    uint32_t off = b->str.len;
    bw_write(&b->str, s, n + 1);
    return off;
}

// Consecutive files usually share folder and artist: remember the last value.
static uint32_t str_add_dedup(build_t *b, const char *s, char *last_s, size_t cap, uint32_t *last_off) {
    if (*last_off && strcmp(s, last_s) == 0) return *last_off;
    uint32_t off = str_add(b, s);
    core_strlcpy(last_s, s, cap);
    *last_off = off;
    return off;
}


// rec_flags: REC_F_RETRY when the tags could not be read (the record is parsed again).
static void build_add(build_t *b, const char *dir, const char *name, const track_tags_t *t, uint32_t size,
                      uint32_t mtime, bool folder_book, uint8_t rec_flags) {
    if (b->oom) return;
    if (b->n_tracks >= LIB_MAX_TRACKS) {
        if (!b->full_warned) CORE_LOGW(TAG, "more than %u tracks: the rest is not indexed", (unsigned)LIB_MAX_TRACKS);
        b->full_warned = true;
        return;
    }
    char title[TAG_TEXT_MAX];
    if (t->title[0]) {
        core_strlcpy(title, t->title, sizeof title);
    } else {
        // No title tag: the file name without extension.
        core_strlcpy(title, name, sizeof title);
        char *dot = strrchr(title, '.');
        if (dot && dot != title) *dot = 0;
        utf8_truncate(title, sizeof title - 1);
    }
    const char *eff_artist = t->album_artist[0] ? t->album_artist : t->artist;
    uint32_t artist = named_find_or_add(b, &b->artists, &b->n_artists, &b->cap_artists, &b->ix_artists, eff_artist,
                                        LIB_MAX_ENTITIES);
    uint32_t album = album_find_or_add(b, t->album, artist);
    uint32_t genre = named_find_or_add(b, &b->genres, &b->n_genres, &b->cap_genres, &b->ix_genres, t->genre, 65535);
    if (b->oom) return;
    b_album_t *al = &b->albums[album];
    if (t->year > al->year) al->year = t->year;
    if (t->is_audiobook_hint || folder_book) al->flags |= ALB_F_BOOK;
    al->duration_ms = (uint32_t)CORE_MIN((uint64_t)al->duration_ms + t->duration_ms, (uint64_t)UINT32_MAX);
    al->tracks++;
    pair_add(b, genre, album);

    db_rec_t r;
    memset(&r, 0, sizeof r);
    r.dir = str_add_dedup(b, dir, b->last_dir_s, sizeof b->last_dir_s, &b->last_dir);
    r.name = str_add(b, name);
    r.title = str_add(b, title);
    r.artist = str_add_dedup(b, t->artist, b->last_artist_s, sizeof b->last_artist_s, &b->last_artist);
    r.composer = str_add_dedup(b, t->composer, b->last_composer_s, sizeof b->last_composer_s, &b->last_composer);
    r.album = album;
    r.genre = (uint16_t)genre;
    r.duration_ms = t->duration_ms;
    r.sample_rate = t->sample_rate;
    r.file_size = size;
    r.mtime = mtime;
    if (t->cover_size && t->cover_offset <= UINT32_MAX) {
        r.cover_offset = (uint32_t)t->cover_offset;
        r.cover_size = t->cover_size;
        r.cover_mime = t->cover_mime;
    }
    r.rg_track_gain = t->rg_track_gain_db;
    r.rg_track_peak = t->rg_track_peak;
    r.rg_album_gain = t->rg_album_gain_db;
    r.rg_album_peak = t->rg_album_peak;
    r.year = t->year;
    r.track_no = t->track_no;
    r.track_total = t->track_total;
    r.disc_no = t->disc_no;
    r.disc_total = t->disc_total;
    r.bits = t->bits;
    r.channels = t->channels;
    r.codec = (uint8_t)t->codec;
    r.flags = (uint8_t)((t->is_audiobook_hint ? REC_F_BOOK_HINT : 0) | rec_flags);
    bw_write(&b->rec, &r, sizeof r);

    uint8_t key[LIB_TITLE_KEY_MAX];
    size_t kl = CORE_MIN(collate_key(title, &b->sort_opts, key, sizeof key), sizeof key);
    if (!grow_arr((void **)&b->tracks, &b->cap_tracks, b->n_tracks + 1, sizeof *b->tracks) ||
        !grow_arr((void **)&b->keys, &b->keys_cap, b->keys_len + 1 + (uint32_t)kl, 1)) {
        b->oom = true;
        return;
    }
    b_track_t *bt = &b->tracks[b->n_tracks++];
    uint16_t disc = t->disc_no ? t->disc_no : 1;
    bt->album = album;
    bt->disc_track = (uint32_t)disc << 16 | t->track_no;
    bt->mtime = mtime;
    char full[CORE_PATH_MAX];
    core_path_join(full, sizeof full, dir, name);
    bt->path_hash = lib_path_hash(full);
    bt->key = b->keys_len;
    b->keys[b->keys_len++] = (uint8_t)kl;
    memcpy(b->keys + b->keys_len, key, kl);
    b->keys_len += (uint32_t)kl;
}

// Size the per-track arrays for the files counted in pass 1 (no growth slack).
static void build_reserve(build_t *b, uint32_t files) {
    uint32_t n = CORE_MIN(files, LIB_MAX_TRACKS) + 1;
    if (!grow_arr((void **)&b->tracks, &b->cap_tracks, n, sizeof *b->tracks) ||
        !grow_arr((void **)&b->keys, &b->keys_cap, n * 15, 1)) {
        b->oom = true;
    }
}

static void build_free(build_t *b) {
    bw_abort(&b->rec);
    bw_abort(&b->str);
    core_free(b->idmap);
    core_free(b->artists);
    core_free(b->genres);
    core_free(b->albums);
    core_free(b->ix_artists.slot);
    core_free(b->ix_genres.slot);
    core_free(b->ix_albums.slot);
    core_free(b->names);
    core_free(b->tracks);
    core_free(b->keys);
    core_free(b->pairs);
    memset(b, 0, sizeof *b);
}

// ----------------------------------------------------------------- finish ----
typedef struct {
    const build_t *b;
    const collate_opts_t *opts;
    const uint32_t *artist_id;  // artist bid -> id
    const uint32_t *album_id;   // album bid -> id
    const uint32_t *title_rank; // album id -> rank in its title-sorted view
} sort_ctx_t;

static int cmp_u32(uint32_t a, uint32_t b) { return a < b ? -1 : a > b ? 1 : 0; }

static int cmp_named(const b_named_t *arr, const char *names, const collate_opts_t *o, uint32_t a, uint32_t b) {
    if (arr[a].has_music != arr[b].has_music) return arr[a].has_music ? -1 : 1;
    int r = collate_cmp(names + arr[a].sp.name, names + arr[b].sp.name, o);
    return r ? r : cmp_u32(a, b);
}

static int cmp_artist(const void *c, uint32_t a, uint32_t b) {
    const sort_ctx_t *s = c;
    return cmp_named(s->b->artists, s->b->names, s->opts, a, b);
}

static int cmp_genre(const void *c, uint32_t a, uint32_t b) {
    const sort_ctx_t *s = c;
    return cmp_named(s->b->genres, s->b->names, s->opts, a, b);
}

static int cmp_album(const void *c, uint32_t a, uint32_t b) {
    const sort_ctx_t *s = c;
    const b_album_t *x = &s->b->albums[a], *y = &s->b->albums[b];
    int r = cmp_u32(x->flags & ALB_F_BOOK, y->flags & ALB_F_BOOK);
    if (!r) r = cmp_u32(s->artist_id[x->artist], s->artist_id[y->artist]);
    if (!r) r = cmp_u32(x->year ? x->year : 0xFFFF, y->year ? y->year : 0xFFFF);  // unknown year last
    if (!r) r = collate_cmp(s->b->names + x->sp.name, s->b->names + y->sp.name, s->opts);
    return r ? r : cmp_u32(a, b);
}

// Album order of tracks: album id, disc, track number, scan (file name) order.
static int cmp_track_album(const void *c, uint32_t a, uint32_t b) {
    const sort_ctx_t *s = c;
    const b_track_t *x = &s->b->tracks[a], *y = &s->b->tracks[b];
    int r = cmp_u32(s->album_id[x->album], s->album_id[y->album]);
    if (!r) r = cmp_u32(x->disc_track, y->disc_track);
    return r ? r : cmp_u32(a, b);
}

static int cmp_track_title(const void *c, uint32_t a, uint32_t b) {
    const sort_ctx_t *s = c;
    const uint8_t *ka = s->b->keys + s->b->tracks[a].key, *kb = s->b->keys + s->b->tracks[b].key;
    int r = collate_key_cmp(ka + 1, ka[0], kb + 1, kb[0]);
    return r ? r : cmp_track_album(c, a, b);
}

static int cmp_track_recent(const void *c, uint32_t a, uint32_t b) {
    const sort_ctx_t *s = c;
    int r = cmp_u32(s->b->tracks[b].mtime, s->b->tracks[a].mtime);  // newest first
    return r ? r : cmp_track_album(c, a, b);
}

typedef struct {
    const build_t *b;
    const collate_opts_t *opts;
    const db_album_t *albums;  // final album table (by id)
} title_ctx_t;

static int cmp_album_title(const void *c, uint32_t a, uint32_t b) {
    const title_ctx_t *s = c;
    const db_album_t *x = &s->albums[a], *y = &s->albums[b];
    int r = collate_cmp(s->b->names + x->title, s->b->names + y->title, s->opts);
    if (!r) r = cmp_u32(x->artist, y->artist);
    return r ? r : cmp_u32(a, b);
}

typedef struct {
    const uint64_t *pairs;     // genre id << 32 | album id
    const uint32_t *title_rank;
} pair_ctx_t;

static int cmp_pair(const void *c, uint32_t a, uint32_t b) {
    const pair_ctx_t *s = c;
    uint32_t ga = (uint32_t)(s->pairs[a] >> 32), gb = (uint32_t)(s->pairs[b] >> 32);
    int r = cmp_u32(ga, gb);
    if (!r) r = cmp_u32(s->title_rank[(uint32_t)s->pairs[a]], s->title_rank[(uint32_t)s->pairs[b]]);
    return r ? r : cmp_u32(a, b);
}

static int cmp_file_ent(const void *c, uint32_t a, uint32_t b) {
    const b_track_t *t = c;
    int r = cmp_u32(t[a].path_hash, t[b].path_hash);
    return r ? r : cmp_u32(a, b);
}

// Jump rank of a title key: the first unit of the key.
static uint32_t key_rank(const uint8_t *k, uint32_t len) {
    if (!len || k[0] < 0x40) return 0;  // digits, symbols, empty -> '#'
    if (k[0] < 0x60) return (uint32_t)k[0] << 16;
    if (k[0] < 0xE0) return (uint32_t)(0x60 + ((k[0] - 0x60) & ~3)) << 16;
    if (len >= 3) return (uint32_t)k[0] << 16 | (uint32_t)k[1] << 8 | k[2];
    return (uint32_t)k[0] << 16;
}

static uint32_t *alloc_u32(uint32_t n) { return core_malloc((size_t)(n ? n : 1) * sizeof(uint32_t)); }

static int write_view(const library_t *lib, int kind, uint32_t gen, const uint32_t *ids, uint32_t n, const void *extra,
                      size_t extra_len) {
    const part_t parts[] = {{ids, (size_t)n * 4}, {extra, extra_len}};
    return write_crc_file(lib, kind, gen, n, parts, 2);
}

// Collect the music (non-audiobook) tracks into view; returns their count.
static uint32_t music_tracks_of(const build_t *b, const db_album_t *albums, const uint32_t *album_id, uint32_t *view) {
    uint32_t m = 0;
    for (uint32_t i = 0; i < b->n_tracks; i++) {
        if (!(albums[album_id[b->tracks[i].album]].flags & ALB_F_BOOK)) view[m++] = i;
    }
    return m;
}

// Finish the build in stages: each view is written and freed before the next one is
// built, so the peak is the per-track build data plus two uint32 arrays per track. Sorts
// pace themselves; between the stages the scan yields and may be cancelled (CORE_EAGAIN).
static int build_finish(build_t *b) {
    library_t *lib = b->lib;
    const uint32_t nt = b->n_tracks, na = b->n_albums, nr = b->n_artists, ng = b->n_genres, gen = b->gen;
    int result = CORE_ENOMEM;
    uint32_t *artist_order = NULL, *artist_id = NULL, *album_order = NULL, *album_id = NULL, *genre_order = NULL,
             *genre_id = NULL, *view = NULL, *valbums = NULL, *vbooks = NULL, *title_rank = NULL, *pair_idx = NULL,
             *galbums = NULL, *fence = NULL;
    uint64_t *pairs = NULL;
    db_artist_t *artists = NULL;
    db_album_t *albums = NULL;
    db_genre_t *genres = NULL;
    db_jump_t *jumps = NULL;
    uint32_t music_albums = 0, music_artists = 0, music_genres = 0, music_tracks = 0, n_jumps = 0, cap_jumps = 0;
    uint32_t n_gal = 0, file_blocks = 0;
    bw_t fw;
    memset(&fw, 0, sizeof fw);
    sort_ctx_t sc = {b, &lib->opts, NULL, NULL, NULL};
    title_ctx_t tc;
    pair_ctx_t pc;

    // 1. Artists and genres with music (non-audiobook) albums; artist and album ids.
    for (uint32_t i = 0; i < na; i++) {
        if (!(b->albums[i].flags & ALB_F_BOOK)) b->artists[b->albums[i].artist].has_music = 1;
    }
    for (uint32_t k = 0; b->pairs && k <= b->pairs_mask; k++) {
        if (!b->pairs[k]) continue;
        uint64_t v = b->pairs[k] - 1;
        if (!(b->albums[(uint32_t)v].flags & ALB_F_BOOK)) b->genres[v >> 32].has_music = 1;
    }
    artist_order = alloc_u32(nr);
    artist_id = alloc_u32(nr);
    album_order = alloc_u32(na);
    album_id = alloc_u32(na);
    artists = core_calloc(nr ? nr : 1, sizeof *artists);
    albums = core_calloc(na ? na : 1, sizeof *albums);
    if (!artist_order || !artist_id || !album_order || !album_id || !artists || !albums) goto out;
    for (uint32_t i = 0; i < nr; i++) artist_order[i] = i;
    if (!sort_u32(artist_order, nr, cmp_artist, &sc, &b->pace)) goto out;
    for (uint32_t i = 0; i < nr; i++) {
        artist_id[artist_order[i]] = i;
        artists[i].name = b->artists[artist_order[i]].sp.name;
        if (b->artists[artist_order[i]].has_music) music_artists++;
    }
    sc.artist_id = artist_id;
    for (uint32_t i = 0; i < na; i++) album_order[i] = i;
    if (!sort_u32(album_order, na, cmp_album, &sc, &b->pace)) goto out;
    for (uint32_t i = 0; i < na; i++) {
        const b_album_t *src = &b->albums[album_order[i]];
        album_id[album_order[i]] = i;
        albums[i].title = src->sp.name;
        albums[i].artist = artist_id[src->artist];
        albums[i].duration_ms = src->duration_ms;
        albums[i].year = src->year;
        albums[i].flags = src->flags & ALB_F_BOOK;
        if (!(src->flags & ALB_F_BOOK)) music_albums++;
    }
    sc.album_id = album_id;
    if (!pace_now(&b->pace)) goto out;

    // 2. All tracks in album order: album and artist ranges, albtrk.idx.
    if (!(view = alloc_u32(nt))) goto out;
    for (uint32_t i = 0; i < nt; i++) view[i] = i;
    if (!sort_u32(view, nt, cmp_track_album, &sc, &b->pace)) goto out;
    for (uint32_t i = 0; i < nt; i++) {
        uint32_t a = album_id[b->tracks[view[i]].album];
        if (!albums[a].track_count) albums[a].track_first = i;
        albums[a].track_count++;
        if (!(albums[a].flags & ALB_F_BOOK)) music_tracks++;
    }
    for (uint32_t i = 0; i < na; i++) {
        // An artist lists its music albums; authors of audiobooks only list their books.
        db_artist_t *ar = &artists[albums[i].artist];
        bool book = albums[i].flags & ALB_F_BOOK;
        if (book == (bool)b->artists[artist_order[albums[i].artist]].has_music) continue;
        if (!ar->album_count) {
            ar->album_first = i;
            ar->track_first = albums[i].track_first;
        }
        ar->album_count++;
        ar->track_count += albums[i].track_count;
        ar->duration_ms = (uint32_t)CORE_MIN((uint64_t)ar->duration_ms + albums[i].duration_ms, (uint64_t)UINT32_MAX);
    }
    if ((result = write_view(lib, DBF_V_ALBTRK, gen, view, nt, NULL, 0)) != CORE_OK) goto out;
    result = CORE_ENOMEM;
    if (!pace_now(&b->pace)) goto out;

    // 3. TRACKS view (music only, by title) with the letter jump table.
    music_tracks_of(b, albums, album_id, view);
    if (!sort_u32(view, music_tracks, cmp_track_title, &sc, &b->pace)) goto out;
    for (uint32_t i = 0; i < music_tracks; i++) {
        const uint8_t *k = b->keys + b->tracks[view[i]].key;
        uint32_t rank = key_rank(k + 1, k[0]);
        if (n_jumps && jumps[n_jumps - 1].rank == rank) continue;
        if (!grow_arr((void **)&jumps, &cap_jumps, n_jumps + 1, sizeof *jumps)) goto out;
        jumps[n_jumps++] = (db_jump_t){rank, i};
    }
    result = write_view(lib, DBF_V_TRACKS, gen, view, music_tracks, jumps, (size_t)n_jumps * sizeof *jumps);
    if (result != CORE_OK) goto out;
    result = CORE_ENOMEM;
    core_free(b->keys);  // the sort keys are not needed any more
    b->keys = NULL;
    if (!pace_now(&b->pace)) goto out;

    // 4. RECENT view (music only, newest first).
    music_tracks_of(b, albums, album_id, view);
    if (!sort_u32(view, music_tracks, cmp_track_recent, &sc, &b->pace)) goto out;
    if ((result = write_view(lib, DBF_V_RECENT, gen, view, music_tracks, NULL, 0)) != CORE_OK) goto out;
    result = CORE_ENOMEM;
    if (!pace_now(&b->pace)) goto out;

    // 5. ALBUMS / AUDIOBOOKS by title, genres and their albums.
    valbums = alloc_u32(music_albums);
    vbooks = alloc_u32(na - music_albums);
    title_rank = alloc_u32(na);
    genre_order = alloc_u32(ng);
    genre_id = alloc_u32(ng);
    genres = core_calloc(ng ? ng : 1, sizeof *genres);
    if (!valbums || !vbooks || !title_rank || !genre_order || !genre_id || !genres) goto out;
    for (uint32_t i = 0; i < music_albums; i++) valbums[i] = i;
    for (uint32_t i = music_albums; i < na; i++) vbooks[i - music_albums] = i;
    tc = (title_ctx_t){b, &lib->opts, albums};
    if (!sort_u32(valbums, music_albums, cmp_album_title, &tc, &b->pace) ||
        !sort_u32(vbooks, na - music_albums, cmp_album_title, &tc, &b->pace)) {
        goto out;
    }
    for (uint32_t i = 0; i < music_albums; i++) title_rank[valbums[i]] = i;
    for (uint32_t i = 0; i < na - music_albums; i++) title_rank[vbooks[i]] = i;
    for (uint32_t i = 0; i < ng; i++) genre_order[i] = i;
    if (!sort_u32(genre_order, ng, cmp_genre, &sc, &b->pace)) goto out;
    for (uint32_t i = 0; i < ng; i++) {
        genre_id[genre_order[i]] = i;
        genres[i].name = b->genres[genre_order[i]].sp.name;
        if (b->genres[genre_order[i]].has_music) music_genres++;
    }
    pairs = core_malloc(sizeof *pairs * (b->n_pairs ? b->n_pairs : 1));
    pair_idx = alloc_u32(b->n_pairs);
    if (!pairs || !pair_idx) goto out;
    for (uint32_t k = 0; b->pairs && k <= b->pairs_mask; k++) {
        if (!b->pairs[k]) continue;
        uint64_t v = b->pairs[k] - 1;
        uint32_t g = genre_id[v >> 32], a = album_id[(uint32_t)v];
        // Music genres list music albums; audiobook-only genres list their books.
        if ((bool)(albums[a].flags & ALB_F_BOOK) == (g < music_genres)) continue;
        pairs[n_gal] = (uint64_t)g << 32 | a;
        pair_idx[n_gal] = n_gal;
        n_gal++;
    }
    core_free(b->pairs);
    b->pairs = NULL;
    pc = (pair_ctx_t){pairs, title_rank};
    if (!sort_u32(pair_idx, n_gal, cmp_pair, &pc, &b->pace) || !(galbums = alloc_u32(n_gal))) goto out;
    for (uint32_t i = 0; i < n_gal; i++) {
        uint64_t v = pairs[pair_idx[i]];
        db_genre_t *g = &genres[v >> 32];
        if (!g->album_count) g->album_first = i;
        g->album_count++;
        galbums[i] = (uint32_t)v;
    }
    result = CORE_EIO;
    if (write_view(lib, DBF_V_ALBUMS, gen, valbums, music_albums, NULL, 0) ||
        write_view(lib, DBF_V_BOOKS, gen, vbooks, na - music_albums, NULL, 0) ||
        write_view(lib, DBF_V_GALBUMS, gen, galbums, n_gal, NULL, 0)) {
        goto out;
    }
    result = CORE_ENOMEM;
    if (!pace_now(&b->pace)) goto out;

    // 6. files.bin: (path hash, id) in hash order, streamed; fence = first hash per block.
    for (uint32_t i = 0; i < nt; i++) view[i] = i;
    if (!sort_u32(view, nt, cmp_file_ent, b->tracks, &b->pace)) goto out;
    file_blocks = (uint32_t)(((uint64_t)nt * sizeof(db_file_ent_t) + LIB_BLOCK - 1) / LIB_BLOCK);
    if (!(fence = alloc_u32(file_blocks))) goto out;
    {
        char path[CORE_PATH_MAX];
        result = CORE_EIO;
        if (lib_db_path(lib, gen, DBF_FILES, path, sizeof path) || !bw_open(&fw, path)) goto out;
        for (uint32_t i = 0; i < nt; i++) {
            db_file_ent_t e = {b->tracks[view[i]].path_hash, view[i]};
            if (i % DB_FILES_PER_BLOCK == 0) fence[i / DB_FILES_PER_BLOCK] = e.hash;
            bw_write(&fw, &e, sizeof e);
        }
        if (bw_finish(&fw, DBF_FILES, gen, nt) || fw.ncrc != file_blocks) goto out;
    }

    // 7. Records, strings, entities; info.bin last (the commit record).
    if (bw_finish(&b->rec, DBF_TRACKS, gen, nt) || bw_finish(&b->str, DBF_STRINGS, gen, 0)) goto out;
    {
        const part_t ent[] = {
            {artists, (size_t)nr * sizeof *artists}, {albums, (size_t)na * sizeof *albums},
            {genres, (size_t)ng * sizeof *genres},   {album_id, (size_t)na * 4},
            {genre_id, (size_t)ng * 4},              {b->names, b->names_len},
        };
        if (write_crc_file(lib, DBF_ENTITIES, gen, nr, ent, 6)) goto out;
        db_info_t info;
        memset(&info, 0, sizeof info);
        info.tracks = nt;
        info.music_tracks = music_tracks;
        info.artists = nr;
        info.music_artists = music_artists;
        info.albums = na;
        info.music_albums = music_albums;
        info.genres = ng;
        info.music_genres = music_genres;
        info.album_bids = na;
        info.genre_bids = ng;
        info.names_bytes = b->names_len;
        info.strings_bytes = b->str.len;
        info.galbums = n_gal;
        info.jumps = n_jumps;
        info.flags = lib->opts.ignore_articles ? 1 : 0;
        info.root_hash = lib_path_hash(lib->root);
        info.crc_blocks[DBF_TRACKS] = b->rec.ncrc;
        info.crc_blocks[DBF_STRINGS] = b->str.ncrc;
        info.crc_blocks[DBF_FILES] = fw.ncrc;
        info.seq = b->seq;
        const part_t ip[] = {
            {&info, sizeof info},
            {b->rec.crcs, (size_t)b->rec.ncrc * 4},
            {b->str.crcs, (size_t)b->str.ncrc * 4},
            {fw.crcs, (size_t)fw.ncrc * 4},
            {fence, (size_t)file_blocks * 4},
        };
        if (write_crc_file(lib, DBF_INFO, gen, nt, ip, 5)) goto out;
    }
    result = CORE_OK;
out:
    if (result != CORE_OK && b->pace.cancelled) result = CORE_EAGAIN;
    bw_abort(&fw);
    core_free(artist_order);
    core_free(artist_id);
    core_free(album_order);
    core_free(album_id);
    core_free(genre_order);
    core_free(genre_id);
    core_free(view);
    core_free(valbums);
    core_free(vbooks);
    core_free(title_rank);
    core_free(pair_idx);
    core_free(galbums);
    core_free(pairs);
    core_free(artists);
    core_free(albums);
    core_free(genres);
    core_free(jumps);
    core_free(fence);
    return result;
}

// =================================================================== walk =====
typedef struct {
    uint32_t name;   // offset in pool
    uint32_t size;
    uint32_t mtime;
    uint8_t is_dir;
} dl_ent_t;

typedef struct {
    char *pool;
    uint32_t len, cap;
    dl_ent_t *e;
    uint32_t n, ecap;
} dirlist_t;

static void dl_free(dirlist_t *d) {
    core_free(d->pool);
    core_free(d->e);
    memset(d, 0, sizeof *d);
}

static int dl_cmp(const void *ctx, uint32_t a, uint32_t b) {
    const dirlist_t *d = ctx;
    return collate_cmp(d->pool + d->e[a].name, d->pool + d->e[b].name, NULL);
}

static bool is_audio_name(const char *name) {
    const char *ext = core_path_ext(name);
    return *ext && codec_is_audio_extension(ext);
}

// List one directory: sub-folders and audio files, each group in collation order.
// Files are stat'ed right after readdir (served from the FAT directory cache).
// CORE_ENOTFOUND: the folder is gone; CORE_EIO: it could not be read (completely).
static int dl_read(const char *path, dirlist_t *files, dirlist_t *dirs, bool want_stat, const char *skip_dir,
                   pace_t *pc) {
    errno = 0;
    lfs_dir_t *d = lfs_opendir(path);
    if (!d) return errno == ENOENT || errno == ENOTDIR ? CORE_ENOTFOUND : CORE_EIO;
    lfs_dirent_t de;
    char full[CORE_PATH_MAX];
    uint32_t seen = 0;
    while (lfs_readdir(d, &de)) {
        if (de.name[0] == '.' || ++seen > SCAN_DIR_MAX_ENTRIES) continue;
        if (core_path_join(full, sizeof full, path, de.name) != CORE_OK) continue;
        lfs_type_t type = de.type;
        lfs_stat_t st = {0};
        bool audio = false;
        if (type != LFS_TYPE_DIR) {
            audio = is_audio_name(de.name);
            if (type == LFS_TYPE_UNKNOWN || (audio && want_stat)) {
                if (lfs_stat(full, &st) != CORE_OK) continue;
                type = st.is_dir ? LFS_TYPE_DIR : LFS_TYPE_FILE;
            }
        }
        dirlist_t *dst;
        if (type == LFS_TYPE_DIR) {
            if (lfs_is_skipped_dir(de.name) || (skip_dir && strcmp(full, skip_dir) == 0)) continue;
            dst = dirs;
        } else {
            if (!audio) continue;
            dst = files;
        }
        size_t nl = strlen(de.name) + 1;
        if (!grow_arr((void **)&dst->pool, &dst->cap, dst->len + (uint32_t)nl, 1) ||
            !grow_arr((void **)&dst->e, &dst->ecap, dst->n + 1, sizeof *dst->e)) {
            lfs_closedir(d);
            return CORE_ENOMEM;
        }
        dl_ent_t *e = &dst->e[dst->n++];
        e->name = dst->len;
        e->size = (uint32_t)CORE_MIN(st.size, (uint64_t)UINT32_MAX);
        e->mtime = st.mtime;
        e->is_dir = type == LFS_TYPE_DIR;
        memcpy(dst->pool + dst->len, de.name, nl);
        dst->len += (uint32_t)nl;
    }
    const bool failed = lfs_dir_failed(d);
    lfs_closedir(d);
    if (failed) return CORE_EIO;
    if (seen > SCAN_DIR_MAX_ENTRIES) {
        CORE_LOGW(TAG, "%s: only the first %d entries are indexed", path, SCAN_DIR_MAX_ENTRIES);
    }
    // Sort both lists through an index permutation.
    for (int k = 0; k < 2; k++) {
        dirlist_t *l = k ? dirs : files;
        if (!l || l->n < 2) continue;
        uint32_t *idx = alloc_u32(l->n);
        dl_ent_t *sorted = core_malloc(sizeof *sorted * l->n);
        if (!idx || !sorted) {
            core_free(idx);
            core_free(sorted);
            return CORE_ENOMEM;
        }
        for (uint32_t i = 0; i < l->n; i++) idx[i] = i;
        bool ok = sort_u32(idx, l->n, dl_cmp, l, pc);
        for (uint32_t i = 0; ok && i < l->n; i++) sorted[i] = l->e[idx[i]];
        if (ok) memcpy(l->e, sorted, sizeof *sorted * l->n);
        core_free(idx);
        core_free(sorted);
        if (!ok) return pc && pc->cancelled ? CORE_EAGAIN : CORE_ENOMEM;
    }
    return CORE_OK;
}

static bool is_book_folder(const char *name) {
    static const char *const names[] = {"Audiobooks", "Audiobook", "Audio Books", "Аудиокниги", "Аудиокнига"};
    for (size_t i = 0; i < CORE_ARRAY_SIZE(names); i++) {
        if (collate_cmp_primary(name, names[i], NULL) == 0) return true;
    }
    return false;
}

typedef struct {
    char path[CORE_PATH_MAX];
    dirlist_t dirs;
    uint32_t next;
    bool book;
} frame_t;

typedef struct {
    library_t *lib;
    build_t b;
    bool full;
    bool counting;
    bool cancelled;
    lib_scan_cb_t cb;
    void *user;
    lib_scan_progress_t prog;
    uint32_t reused, retry;
    track_tags_t *tags;
    char skip_dir[CORE_PATH_MAX];
} scan_t;

// Rebuild tags from the old record (unchanged file). Caller holds the lock.
static bool old_tags(library_t *lib, const db_rec_t *rec, track_tags_t *t) {
    db_snap_t *s = &lib->snap;
    tags_clear(t);
    if (lib_read_str(lib, rec->title, t->title, sizeof t->title) ||
        lib_read_str(lib, rec->artist, t->artist, sizeof t->artist) ||
        lib_read_str(lib, rec->composer, t->composer, sizeof t->composer)) {
        return false;
    }
    const db_album_t *al = &s->albums[s->album_map[rec->album]];
    core_strlcpy(t->album, s->names + al->title, sizeof t->album);
    core_strlcpy(t->album_artist, s->names + s->artists[al->artist].name, sizeof t->album_artist);
    core_strlcpy(t->genre, s->names + s->genres[s->genre_map[rec->genre]].name, sizeof t->genre);
    t->year = rec->year;
    t->track_no = rec->track_no;
    t->track_total = rec->track_total;
    t->disc_no = rec->disc_no;
    t->disc_total = rec->disc_total;
    t->rg_track_gain_db = rec->rg_track_gain;
    t->rg_track_peak = rec->rg_track_peak;
    t->rg_album_gain_db = rec->rg_album_gain;
    t->rg_album_peak = rec->rg_album_peak;
    t->cover_offset = rec->cover_offset;
    t->cover_size = rec->cover_size;
    t->cover_mime = rec->cover_mime;
    t->duration_ms = rec->duration_ms;
    t->sample_rate = rec->sample_rate;
    t->bits = rec->bits;
    t->channels = rec->channels;
    t->codec = (codec_id_t)(rec->codec < CODEC_COUNT ? rec->codec : CODEC_UNKNOWN);
    t->is_audiobook_hint = (rec->flags & REC_F_BOOK_HINT) != 0;
    return true;
}

// Stream wrapper that notices read errors: a parser stops at one like at the end of the
// file and keeps what it found, which is not what the index should keep for good.
typedef struct {
    core_stream_t *s;
    bool io_error;
} watch_t;

static int32_t watch_read(void *c, void *buf, size_t len) {
    watch_t *w = c;
    int32_t n = core_stream_read(w->s, buf, len);
    if (n < 0 && n != CORE_EOF) w->io_error = true;
    return n;
}

static int watch_seek(void *c, int64_t off, int whence) {
    watch_t *w = c;
    int r = core_stream_seek(w->s, off, whence);
    if (r == CORE_EIO) w->io_error = true;
    return r;
}

static int64_t watch_tell(void *c) { return core_stream_tell(((watch_t *)c)->s); }
static int64_t watch_size(void *c) { return core_stream_size(((watch_t *)c)->s); }
static void watch_close(void *c) { core_stream_close(((watch_t *)c)->s); }

static const core_stream_ops_t s_watch_ops = {watch_read, watch_seek, watch_tell, watch_size, watch_close};

// tags_read_file() that tells transient failures (read errors, no memory, no free file
// handle) from files that are gone (CORE_ENOTFOUND) or simply have no usable tags.
static int scan_read_tags(const char *path, track_tags_t *t, bool *transient) {
    *transient = false;
    tags_clear(t);
    t->codec = codec_from_extension(core_path_ext(path));
    errno = 0;
    core_stream_t *fs = core_stream_open_file(path, 8 * 1024);
    if (!fs) {
        const int e = errno;
        if (e == ENOENT || e == ENOTDIR) return CORE_ENOTFOUND;
        *transient = true;
        return e == ENOMEM ? CORE_ENOMEM : CORE_EIO;
    }
    watch_t w = {fs, false};
    core_stream_t *s = core_stream_create(&s_watch_ops, &w);  // closes fs on failure
    if (!s) {
        *transient = true;
        return CORE_ENOMEM;
    }
    int r = tags_read_stream(s, path, t);
    core_stream_close(s);
    if (w.io_error || r == CORE_EIO || r == CORE_ENOMEM) *transient = true;
    return r;
}

static void scan_file(scan_t *sc, const char *dir, const char *name, uint32_t size, uint32_t mtime, bool book) {
    library_t *lib = sc->lib;
    if (lib->throttle) lib->throttle(lib->throttle_user);
    sc->b.pace.last_ms = core_now_ms();
    char path[CORE_PATH_MAX];
    if (core_path_join(path, sizeof path, dir, name) != CORE_OK) return;
    sc->prog.files_seen++;
    bool reused = false;
    uint8_t rec_flags = 0;
    // The old record is looked up even for a full rebuild, to report added vs updated.
    core_mutex_lock(lib->mutex);
    db_rec_t rec;
    uint32_t old_id = lib_find_path_locked(lib, path, &rec);
    if (!sc->full && old_id != LIB_ID_NONE && rec.file_size == size && rec.mtime == mtime &&
        !(rec.flags & REC_F_RETRY)) {
        reused = old_tags(lib, &rec, sc->tags);
    }
    core_mutex_unlock(lib->mutex);
    if (reused) {
        sc->reused++;
    } else {
        bool transient;
        int r = scan_read_tags(path, sc->tags, &transient);
        if (r == CORE_ENOTFOUND && !transient) return;  // gone since the folder was listed
        if (transient) {
            // Card or memory trouble, not the file: keep what the old record knew instead of an
            // empty record, and mark it so the next scan parses the file again.
            CORE_LOGW(TAG, "%s: tags not readable now (%s), retried by the next scan", path, core_err_name(r));
            rec_flags = REC_F_RETRY;
            sc->retry++;
            if (old_id != LIB_ID_NONE) {
                core_mutex_lock(lib->mutex);
                bool kept = old_tags(lib, &rec, sc->tags);
                core_mutex_unlock(lib->mutex);
                if (!kept) tags_clear(sc->tags);
            }
        } else if (r != CORE_OK) {
            CORE_LOGD(TAG, "%s: tags: %s", path, core_err_name(r));
        }
        if (old_id != LIB_ID_NONE) sc->prog.tracks_updated++;
        else sc->prog.tracks_added++;
    }
    const uint32_t new_id = sc->b.n_tracks;
    build_add(&sc->b, dir, name, sc->tags, size, mtime, book, rec_flags);
    if (old_id < sc->b.idmap_n && sc->b.n_tracks == new_id + 1) sc->b.idmap[old_id] = new_id;
    if (sc->cb) {
        sc->prog.current_path = path;
        if (!sc->cb(&sc->prog, sc->user)) sc->cancelled = true;
        sc->prog.current_path = NULL;
    }
}

// Depth-first walk. counting: only count audio files (pass 1).
static int walk(scan_t *sc) {
    frame_t *st = core_calloc(SCAN_MAX_DEPTH, sizeof *st);
    if (!st) return CORE_ENOMEM;
    int depth = 0, result = CORE_OK;
    core_strlcpy(st[0].path, sc->lib->root, sizeof st[0].path);
    st[0].book = false;
    dirlist_t files;
    memset(&files, 0, sizeof files);
    // Enter the root.
    bool entered = false;
    while (depth >= 0 && !sc->cancelled && !sc->b.oom) {
        frame_t *f = &st[depth];
        if (!entered) {
            entered = true;
            // Pass 1 touches the card too: yield to the audio reader once per folder, and see
            // a cancel (pass 2 sees it after every file).
            if (sc->counting && !pace_now(&sc->b.pace)) {
                sc->cancelled = true;
                break;
            }
            memset(&files, 0, sizeof files);
            int r = dl_read(f->path, &files, &f->dirs, !sc->counting, sc->skip_dir, &sc->b.pace);
            if (r == CORE_EAGAIN) {
                sc->cancelled = true;
                dl_free(&files);
                break;
            }
            if (r == CORE_ENOMEM || r == CORE_EIO || (r != CORE_OK && depth == 0)) {
                // No music root (card removed, not mounted), or a folder that cannot be read
                // now: keep the old index rather than commit one without that subtree.
                if (r != CORE_ENOMEM) CORE_LOGW(TAG, "cannot read the folder %s: index not updated", f->path);
                result = r == CORE_ENOTFOUND ? CORE_EIO : r;
                dl_free(&files);
                break;
            }
            if (r != CORE_OK) CORE_LOGW(TAG, "folder %s is gone", f->path);
            for (uint32_t i = 0; i < files.n && !sc->cancelled && !sc->b.oom; i++) {
                if (sc->counting) {
                    sc->prog.files_total++;
                } else {
                    const dl_ent_t *e = &files.e[i];
                    scan_file(sc, f->path, files.pool + e->name, e->size, e->mtime, f->book);
                }
            }
            dl_free(&files);
        }
        if (f->next < f->dirs.n && depth + 1 < SCAN_MAX_DEPTH) {
            const char *name = f->dirs.pool + f->dirs.e[f->next++].name;
            frame_t *c = &st[depth + 1];
            if (core_path_join(c->path, sizeof c->path, f->path, name) != CORE_OK) continue;
            memset(&c->dirs, 0, sizeof c->dirs);
            c->next = 0;
            c->book = f->book || is_book_folder(name);
            depth++;
            entered = false;
        } else {
            dl_free(&f->dirs);
            depth--;
        }
    }
    for (int i = 0; i < SCAN_MAX_DEPTH; i++) dl_free(&st[i].dirs);
    core_free(st);
    if (sc->b.oom) return CORE_ENOMEM;
    return result;
}

int lib_scan_run(library_t *lib, bool full_rebuild, lib_scan_cb_t cb, void *user) {
    uint32_t t0 = core_now_ms();
    char path[CORE_PATH_MAX];
    if (lfs_mkdirs(lib->db_dir) != CORE_OK) {
        CORE_LOGE(TAG, "cannot create %s", lib->db_dir);
        return CORE_EIO;
    }
    scan_t *sc = core_calloc(1, sizeof *sc);
    track_tags_t *tags = core_malloc(sizeof *tags);
    if (!sc || !tags) {
        core_free(sc);
        core_free(tags);
        return CORE_ENOMEM;
    }
    sc->lib = lib;
    sc->full = full_rebuild;
    sc->cb = cb;
    sc->user = user;
    sc->tags = tags;
    core_strlcpy(sc->skip_dir, lib->db_dir, sizeof sc->skip_dir);
    build_t *b = &sc->b;
    b->lib = lib;
    b->sort_opts = lib->opts;
    b->pace = (pace_t){lib, cb, user, &sc->prog, core_now_ms(), 0, false, false};
    core_mutex_lock(lib->mutex);
    uint32_t old_gen = lib->snap.generation;
    uint32_t old_tracks = lib->snap.loaded ? lib->snap.info.tracks : 0;
    b->seq = (lib->snap.loaded ? lib->snap.info.seq : 0) + 1;
    if (lib->snap.corrupt) sc->full = true;  // damaged blocks: do not trust old records
    core_mutex_unlock(lib->mutex);
    // Leftovers of an interrupted build (and the old layout) go first.
    lib_remove_other_gens(lib, old_gen);
    b->gen = (old_gen + 1) ^ (core_now_ms() << 12);
    if (!b->gen || b->gen == old_gen) b->gen = old_gen + 1 ? old_gen + 1 : 1;

    int r = CORE_OK;
    if (old_tracks) {
        b->idmap = core_malloc((size_t)old_tracks * sizeof *b->idmap);
        if (b->idmap) {
            memset(b->idmap, 0xFF, (size_t)old_tracks * sizeof *b->idmap);  // LIB_ID_NONE
            b->idmap_n = old_tracks;
        } else {
            CORE_LOGW(TAG, "no memory for the id map: ids held elsewhere cannot follow this scan");
        }
    }
    if (!r && (lib_db_path(lib, b->gen, DBF_TRACKS, path, sizeof path) || !bw_open(&b->rec, path))) r = CORE_EIO;
    if (!r && (lib_db_path(lib, b->gen, DBF_STRINGS, path, sizeof path) || !bw_open(&b->str, path))) r = CORE_EIO;
    if (!r) {
        // Pool offset 0 is the empty string in both pools.
        uint8_t zero = 0;
        bw_write(&b->str, &zero, 1);
        b->names = core_malloc(256);
        b->names_cap = b->names ? 256 : 0;
        if (b->names) {
            b->names[0] = 0;
            b->names_len = 1;
        } else {
            r = CORE_ENOMEM;
        }
    }
    if (!r) {
        sc->counting = b->pace.counting = true;
        r = walk(sc);
        sc->counting = b->pace.counting = false;
    }
    if (!r && sc->cancelled) r = CORE_EAGAIN;
    if (!r) build_reserve(b, sc->prog.files_total);
    // Every file is looked up in the old snapshot: files.bin in RAM spares the card a random
    // block read per file (and the block cache for the records and strings).
    if (!r) lib_files_mem(lib, true);
    if (!r) r = walk(sc);
    lib_files_mem(lib, false);
    if (!r && sc->cancelled) r = CORE_EAGAIN;
    if (!r && (b->rec.err || b->str.err)) r = CORE_EIO;
    if (!r) {
        uint32_t kept = sc->reused + sc->prog.tracks_updated;
        sc->prog.tracks_removed = old_tracks > kept ? old_tracks - kept : 0;
        r = build_finish(b);
    }
    uint32_t *idmap = b->idmap, idmap_n = old_tracks;
    b->idmap = NULL;
    const uint32_t gen = b->gen, seq = b->seq;
    build_free(b);
    if (!r) {
        r = lib_commit(lib, gen, seq, idmap, idmap_n);  // takes idmap
    } else {
        core_free(idmap);
    }
    if (r) {
        lib_remove_gen(lib, gen);
        CORE_LOGW(TAG, "scan stopped: %s", core_err_name(r));
    } else {
        CORE_LOGI(TAG, "scan done in %u ms: %u files, %u added, %u updated, %u removed, %u to retry",
                  (unsigned)(core_now_ms() - t0), (unsigned)sc->prog.files_seen, (unsigned)sc->prog.tracks_added,
                  (unsigned)sc->prog.tracks_updated, (unsigned)sc->prog.tracks_removed, (unsigned)sc->retry);
        if (cb) {
            sc->prog.current_path = "";
            cb(&sc->prog, user);
        }
    }
    core_free(tags);
    core_free(sc);
    return r;
}
