// SPDX-License-Identifier: Apache-2.0
// Resume state and the Rockbox-format scrobbler log.
//
// <state_dir>/queue.m3u8  the queue in play order: a plain UTF-8 playlist with absolute
//                         paths. Before each path a "#EXT-EMP:<orig>,<start>,<end>,<id>"
//                         comment keeps the original (unshuffled) position, the CUE range
//                         in ms and the library id (-1 = none) as a lookup hint.
// <state_dir>/player.ini  key = value: index, position_ms, volume_db, shuffle, seed,
//                         repeat, state.
// Both are written to "<name>.tmp", synced, then the old file is removed and the new one
// renamed (FAT cannot rename over an existing file). A missing file with a complete .tmp
// next to it (power loss between remove and rename) is recovered on restore.
//
// .scrobbler.log follows the Audioscrobbler portable player format 1.1 (Rockbox):
//   ARTIST \t ALBUM \t TITLE \t TRACKNUM \t LENGTH(s) \t RATING(L|S) \t UNIX TIMESTAMP \t MBID
// RATING is L when at least half of the track (or 240 s) was played. Without a valid clock
// (no RTC, no SNTP yet) entries wait in RAM and are written with their start time once the
// clock is set.
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "player/player_priv.h"

#define TAG PLAYER_TAG
#define QUEUE_FILE "queue.m3u8"
#define STATE_FILE "player.ini"
#define SCROBBLE_FILE ".scrobbler.log"
#define SAVE_SLICE 64u
#define SAVE_ATTEMPTS 3
#define LINE_MAX 1024

void player_push_restore(player_t *p, prestore_t *r);  // player.c

// -------------------------------------------------------------------------- file helpers ----
static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int make_dir(const char *path) {
#if defined(_WIN32)
    int r = _mkdir(path);
#else
    int r = mkdir(path, 0775);
#endif
    if (r == 0) return CORE_OK;
    struct stat st;
    return (stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) ? CORE_OK : CORE_EIO;
}

static int make_dirs(const char *path) {
    char buf[CORE_PATH_MAX];
    if (core_strlcpy(buf, path, sizeof buf) >= sizeof buf) return CORE_EINVAL;
    size_t len = strlen(buf);
    for (size_t i = 1; i < len; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        if (!file_exists(buf)) make_dir(buf);  // mount points and drive roots may refuse: fine
        buf[i] = '/';
    }
    return make_dir(buf);
}

static int file_sync(FILE *f) {
    if (fflush(f) != 0) return CORE_EIO;
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0 ? CORE_OK : CORE_EIO;
#else
    return fsync(fileno(f)) == 0 ? CORE_OK : CORE_EIO;
#endif
}

// tmp -> path: remove the old file first (FAT and Windows do not rename over a file).
static int file_replace(const char *tmp, const char *path) {
    if (file_exists(path) && remove(path) != 0) return CORE_EIO;
    return rename(tmp, path) == 0 ? CORE_OK : CORE_EIO;
}

static int close_and_replace(FILE *f, bool ok, const char *tmp, const char *path) {
    int r = ok ? file_sync(f) : CORE_EIO;
    if (fclose(f) != 0 && r == CORE_OK) r = CORE_EIO;
    if (r == CORE_OK) r = file_replace(tmp, path);
    if (r != CORE_OK) remove(tmp);
    return r;
}

static int state_path(const player_t *p, const char *name, char *out, size_t cap) {
    return core_path_join(out, cap, p->state_dir, name);
}

// Opens name for reading, falling back to a complete name.tmp left by an interrupted replace.
static FILE *open_state_file(const player_t *p, const char *name) {
    char path[CORE_PATH_MAX], tmp[CORE_PATH_MAX + 8];
    if (state_path(p, name, path, sizeof path) != CORE_OK) return NULL;
    FILE *f = fopen(path, "rb");
    if (f) return f;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "rb");
    if (f) {
        CORE_LOGW(TAG, "recovering %s from its .tmp file", name);
        fclose(f);
        if (rename(tmp, path) == 0) return fopen(path, "rb");
        return fopen(tmp, "rb");
    }
    return NULL;
}

// Block-buffered line reader. A 20 000-entry queue.m3u8 is a few MB, and fgetc() takes and
// releases the FILE lock for every byte on ESP-IDF: reading blocks keeps the restore (which
// runs before the session is published) short.
#define RD_BLOCK 4096u

typedef struct {
    FILE *f;
    char *buf;
    size_t len, pos;
} rd_t;

// Takes f (closed by rd_close() even when the buffer cannot be allocated).
static bool rd_open(rd_t *r, FILE *f) {
    r->f = f;
    r->len = r->pos = 0;
    r->buf = core_malloc(RD_BLOCK);
    return r->buf != NULL;
}

static void rd_close(rd_t *r) {
    core_free(r->buf);
    r->buf = NULL;
    if (r->f) fclose(r->f);
    r->f = NULL;
}

static void rd_rewind(rd_t *r) {
    rewind(r->f);
    r->len = r->pos = 0;
}

// Reads one line (without CR/LF). Returns false at the end of the file. Overlong lines
// are cut and the rest is skipped.
static bool read_line(rd_t *r, char *buf, size_t cap) {
    size_t n = 0;
    bool any = false;
    for (;;) {
        if (r->pos == r->len) {
            r->len = fread(r->buf, 1, RD_BLOCK, r->f);
            r->pos = 0;
            if (r->len == 0) break;
        }
        const char *start = r->buf + r->pos;
        const size_t avail = r->len - r->pos;
        const char *nl = memchr(start, '\n', avail);
        const size_t span = nl ? (size_t)(nl - start) : avail;
        any = true;
        for (size_t i = 0; i < span; i++) {
            if (start[i] != '\r' && n + 1 < cap) buf[n++] = start[i];
        }
        r->pos += span + (nl ? 1 : 0);
        if (nl) break;
    }
    buf[n] = 0;
    return any;
}

// -------------------------------------------------------------------------------- save ----
typedef struct {
    pq_item_t it;
    uint32_t orig;
    char path[CORE_PATH_MAX];
} save_ent_t;

// "-12.50": no floating-point printf (the device libc may be built without it).
static void format_db(char *out, size_t n, float v) {
    if (!isfinite(v)) v = PLAYER_VOLUME_MIN_DB;
    long c = lrintf(v * 100.0f);
    unsigned long a = (unsigned long)(c < 0 ? -c : c);
    snprintf(out, n, "%s%lu.%02lu", c < 0 ? "-" : "", a / 100u, a % 100u);
}

static const char *repeat_name(repeat_mode_t r) { return r == REPEAT_ALL ? "all" : r == REPEAT_ONE ? "one" : "off"; }
static const char *state_name(player_state_t s) {
    return s == PLAYER_PLAYING ? "playing" : s == PLAYER_PAUSED ? "paused" : "stopped";
}

// Writes the queue. *index_out receives the position of the current entry among the
// written ones. Returns CORE_EAGAIN when the queue kept changing while it was copied.
// Writes the queue in play order. *count_out = entries written, *gen_out / *total_out = the
// queue generation and length that were saved (entries with an unknown library id are left out,
// so *count_out can be smaller than *total_out).
static int save_queue(player_t *p, const char *path, uint32_t *index_out, uint32_t *count_out, uint32_t *gen_out,
                      uint32_t *total_out) {
    char tmp[CORE_PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    save_ent_t *ents = core_malloc(SAVE_SLICE * sizeof *ents);
    lib_track_t *lt = p->cfg.library ? core_malloc(sizeof *lt) : NULL;
    if (!ents || (p->cfg.library && !lt)) {
        core_free(ents);
        core_free(lt);
        return CORE_ENOMEM;
    }
    int r = CORE_EAGAIN;
    for (int attempt = 0; attempt < SAVE_ATTEMPTS && r == CORE_EAGAIN; attempt++) {
        FILE *f = fopen(tmp, "wb");
        if (!f) {
            r = CORE_EIO;
            break;
        }
        bool ok = fputs("#EXTM3U\n", f) >= 0;
        core_mutex_lock(p->mu);
        uint32_t gen = p->q.gen, count = p->q.count, cur = p->q.cur;
        core_mutex_unlock(p->mu);
        uint32_t written = 0, index = 0;
        bool changed = false;
        for (uint32_t pos = 0; ok && pos < count && !changed; pos += SAVE_SLICE) {
            uint32_t n = CORE_MIN(SAVE_SLICE, count - pos);
            core_mutex_lock(p->mu);
            changed = p->q.gen != gen;
            const uint32_t lib_gen = p->q.lib_gen;  // a rescan may translate the ids between slices
            for (uint32_t i = 0; !changed && i < n; i++) {
                const pq_item_t *it = pq_at(&p->q, pos + i);
                ents[i].it = *it;
                ents[i].orig = p->q.order[pos + i];
                const char *qp = pq_path(&p->q, it);
                core_strlcpy(ents[i].path, qp ? qp : "", sizeof ents[i].path);
            }
            core_mutex_unlock(p->mu);
            for (uint32_t i = 0; !changed && ok && i < n; i++) {
                save_ent_t *e = &ents[i];
                e->it.track_id = player_lib_id(p, e->it.track_id, lib_gen);
                if (!e->path[0] && e->it.track_id != LIB_ID_NONE && lt &&
                    library_get_track(p->cfg.library, e->it.track_id, lt) == CORE_OK) {
                    core_strlcpy(e->path, lt->path, sizeof e->path);
                }
                if (!e->path[0]) continue;  // unknown library id: nothing to save
                if (pos + i == cur) index = written;
                long id = e->it.track_id == LIB_ID_NONE ? -1L : (long)e->it.track_id;
                ok = fprintf(f, "#EXT-EMP:%lu,%lu,%lu,%ld\n%s\n", (unsigned long)e->orig,
                             (unsigned long)e->it.start_ms, (unsigned long)e->it.end_ms, id, e->path) > 0;
                written++;
            }
        }
        if (changed) {
            fclose(f);
            remove(tmp);
            continue;  // r stays CORE_EAGAIN: try again
        }
        r = close_and_replace(f, ok, tmp, path);
        *index_out = index;
        *count_out = written;
        *gen_out = gen;
        *total_out = count;
    }
    core_free(ents);
    core_free(lt);
    return r;
}

static int save_state_locked(player_t *p) {
    make_dirs(p->state_dir);
    char qpath[CORE_PATH_MAX], ipath[CORE_PATH_MAX], tmp[CORE_PATH_MAX + 8];
    if (state_path(p, QUEUE_FILE, qpath, sizeof qpath) != CORE_OK ||
        state_path(p, STATE_FILE, ipath, sizeof ipath) != CORE_OK) {
        return CORE_EINVAL;
    }
    core_mutex_lock(p->mu);
    player_status_t st = p->st;
    bool shuffled = p->q.shuffled;
    uint32_t seed = p->q.seed;
    uint32_t qgen = p->q.gen, qcount = p->q.count, qcur = p->q.cur;
    // Right after a gapless switch the status still shows the previous (audible) entry:
    // its position does not belong to the current one.
    if (st.queue_index != p->q.cur) st.position_ms = 0;
    core_mutex_unlock(p->mu);

    uint32_t index = 0, count = 0;
    int r = CORE_OK;
    // The queue file only changes with the queue itself (items, order). Periodic saves and
    // track changes then write just player.ini, which saves card time and power with long
    // queues. The index is the play position, as save_queue() computes it when it wrote
    // every entry (a saved file without skipped entries is required for the shortcut).
    if (p->saved_q_valid && p->saved_q_gen == qgen && p->saved_q_count == qcount && file_exists(qpath)) {
        index = qcount ? qcur : 0;
        count = qcount;
    } else {
        uint32_t saved_gen = 0, saved_total = 0;
        p->saved_q_valid = false;
        r = save_queue(p, qpath, &index, &count, &saved_gen, &saved_total);
        if (r != CORE_OK) {
            CORE_LOGE(TAG, "saving the queue: %s", core_err_name(r));
            return r;
        }
        p->saved_q_valid = count == saved_total;
        p->saved_q_gen = saved_gen;
        p->saved_q_count = saved_total;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", ipath);
    char vol[24];
    format_db(vol, sizeof vol, st.volume_db);
    FILE *f = fopen(tmp, "wb");
    if (!f) return CORE_EIO;
    bool ok = fprintf(f,
                      "# esp32-audio-player playback state\n"
                      "version = 1\n"
                      "count = %lu\n"
                      "index = %lu\n"
                      "position_ms = %lu\n"
                      "volume_db = %s\n"
                      "shuffle = %d\n"
                      "seed = %lu\n"
                      "repeat = %s\n"
                      "state = %s\n",
                      (unsigned long)count, (unsigned long)index,
                      (unsigned long)(st.state == PLAYER_STOPPED ? 0u : st.position_ms), vol,
                      shuffled ? 1 : 0, (unsigned long)seed, repeat_name(st.repeat), state_name(st.state)) > 0;
    r = close_and_replace(f, ok, tmp, ipath);
    if (r != CORE_OK) CORE_LOGE(TAG, "saving %s: %s", STATE_FILE, core_err_name(r));
    return r;
}

int player_save_state(player_t *p) {
    if (!p) return CORE_EINVAL;
    if (!p->state_dir[0]) return CORE_EINVAL;
    core_mutex_lock(p->file_mu);
    int r = save_state_locked(p);
    core_mutex_unlock(p->file_mu);
    return r;
}

// ----------------------------------------------------------------------------- restore ----
typedef struct {
    uint32_t count, index, position_ms, seed;
    float volume_db;
    bool has_volume, shuffle;
    repeat_mode_t repeat;
} ini_t;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

static uint32_t parse_u32(const char *s) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    return (end == s || v > UINT32_MAX) ? 0 : (uint32_t)v;
}

static int read_ini(player_t *p, ini_t *ini) {
    memset(ini, 0, sizeof *ini);
    FILE *f = open_state_file(p, STATE_FILE);
    if (!f) return CORE_ENOTFOUND;
    rd_t rd;
    if (!rd_open(&rd, f)) {
        rd_close(&rd);
        return CORE_ENOMEM;
    }
    char line[256];
    while (read_line(&rd, line, sizeof line)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (!strcmp(key, "count")) {
            ini->count = parse_u32(val);
        } else if (!strcmp(key, "index")) {
            ini->index = parse_u32(val);
        } else if (!strcmp(key, "position_ms")) {
            ini->position_ms = parse_u32(val);
        } else if (!strcmp(key, "seed")) {
            ini->seed = parse_u32(val);
        } else if (!strcmp(key, "volume_db")) {
            char *end = NULL;
            float v = strtof(val, &end);
            if (end != val && isfinite(v)) {
                ini->volume_db = v;
                ini->has_volume = true;
            }
        } else if (!strcmp(key, "shuffle")) {
            ini->shuffle = parse_u32(val) != 0;
        } else if (!strcmp(key, "repeat")) {
            ini->repeat = !strcmp(val, "all") ? REPEAT_ALL : !strcmp(val, "one") ? REPEAT_ONE : REPEAT_OFF;
        }
    }
    rd_close(&rd);
    return CORE_OK;
}

typedef struct {
    uint32_t key, pos;
} rank_t;

static int rank_cmp(const void *a, const void *b) {
    const rank_t *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}

static bool parse_emp(const char *s, uint32_t *orig, uint32_t *start, uint32_t *end, long *id) {
    unsigned long o, a, b;
    long i;
    if (sscanf(s, "#EXT-EMP:%lu,%lu,%lu,%ld", &o, &a, &b, &i) != 4) return false;
    *orig = (uint32_t)CORE_MIN(o, (unsigned long)UINT32_MAX);
    *start = (uint32_t)CORE_MIN(a, (unsigned long)UINT32_MAX);
    *end = (uint32_t)CORE_MIN(b, (unsigned long)UINT32_MAX);
    *id = i;
    return true;
}

// Resolves a saved entry to a library id: the saved id when it still names this path,
// else a lookup by path. CUE ranges stay path entries (the index knows whole files only).
static lib_id_t resolve_id(player_t *p, lib_track_t *lt, const char *path, long hint, bool ranged) {
    library_t *lib = p->cfg.library;
    if (!lib || ranged) return LIB_ID_NONE;
    if (hint >= 0 && library_get_track(lib, (lib_id_t)hint, lt) == CORE_OK && strcmp(lt->path, path) == 0)
        return (lib_id_t)hint;
    return library_find_path(lib, path);
}

// Line interpreter shared by both passes over queue.m3u8.
typedef struct {
    uint32_t orig, start, end;
    long id;
    bool have_emp;
    char full[CORE_PATH_MAX];
} qline_t;

// Returns the entry path for a playlist line, or NULL for comments and blank lines.
// Our entries carry absolute or player-relative paths exactly as they were queued; a
// relative line without our comment (edited by hand) is relative to the state folder.
static const char *queue_line(player_t *p, qline_t *st, char *line) {
    char *s = trim(line);
    if (!*s) return NULL;
    if (*s == '#') {
        if (!strncmp(s, "#EXT-EMP:", 9)) st->have_emp = parse_emp(s, &st->orig, &st->start, &st->end, &st->id);
        return NULL;
    }
    if (!st->have_emp) {
        st->start = st->end = 0;
        st->id = -1;
        if (s[0] != '/' && !(s[0] && s[1] == ':')) {
            if (core_path_join(st->full, sizeof st->full, p->state_dir, s) != CORE_OK) return NULL;
            return st->full;
        }
    }
    return s;
}

static int read_queue(player_t *p, pq_batch_t **out) {
    *out = NULL;
    FILE *f = open_state_file(p, QUEUE_FILE);
    if (!f) return CORE_ENOTFOUND;
    rd_t rd;
    bool rd_ok = rd_open(&rd, f);
    char *line = core_malloc(LINE_MAX);
    qline_t *ql = core_calloc(1, sizeof *ql);
    if (!rd_ok || !line || !ql) {
        core_free(line);
        core_free(ql);
        rd_close(&rd);
        return CORE_ENOMEM;
    }
    // Pass 1: size the batch.
    uint32_t n = 0;
    uint64_t pool = 1;
    while (n < PLAYER_QUEUE_MAX && read_line(&rd, line, LINE_MAX)) {
        const char *path = queue_line(p, ql, line);
        if (!path) continue;
        size_t len = strlen(path);
        pool += (len < CORE_PATH_MAX ? len : CORE_PATH_MAX - 1) + 1;
        ql->have_emp = false;
        n++;
    }
    if (n == 0 || pool > UINT32_MAX / 2) {
        core_free(line);
        core_free(ql);
        rd_close(&rd);
        return n == 0 ? CORE_OK : CORE_ENOMEM;
    }
    pq_batch_t *b = pq_batch_new(n, (uint32_t)pool);
    uint32_t *keys = core_malloc((size_t)n * sizeof *keys);
    lib_track_t *lt = p->cfg.library ? core_malloc(sizeof *lt) : NULL;
    if (!b || !keys || (p->cfg.library && !lt)) {
        pq_batch_free(b);
        core_free(keys);
        core_free(lt);
        core_free(line);
        core_free(ql);
        rd_close(&rd);
        return CORE_ENOMEM;
    }
    // Pass 2: entries in play order.
    rd_rewind(&rd);
    memset(ql, 0, sizeof *ql);
    b->lib_gen = library_generation(p->cfg.library);  // of the ids resolve_id() finds
    bool all_keys = true;
    while (b->count < n && read_line(&rd, line, LINE_MAX)) {
        const char *path = queue_line(p, ql, line);
        if (!path) continue;
        if (!ql->have_emp) all_keys = false;
        bool ranged = ql->start || ql->end;
        lib_id_t lid = resolve_id(p, lt, path, ql->have_emp ? ql->id : -1, ranged);
        keys[b->count] = ql->have_emp ? ql->orig : b->count;
        if (!pq_batch_push(b, (uint32_t)pool, lid, lid == LIB_ID_NONE ? path : NULL, ql->start, ql->end)) break;
        ql->have_emp = false;
    }
    core_free(ql);
    core_free(lt);
    core_free(line);
    rd_close(&rd);
    if (b->count == 0) {
        pq_batch_free(b);
        core_free(keys);
        return CORE_OK;
    }
    // Original order = entries sorted by their saved position; order[] maps back.
    uint32_t cnt = b->count;
    rank_t *rk = all_keys ? core_malloc((size_t)cnt * sizeof *rk) : NULL;
    pq_item_t *items = rk ? core_malloc((size_t)cnt * sizeof *items) : NULL;
    if (rk && items) {
        for (uint32_t i = 0; i < cnt; i++) rk[i] = (rank_t){keys[i], i};
        qsort(rk, cnt, sizeof *rk, rank_cmp);
        for (uint32_t j = 0; j < cnt; j++) {
            items[j] = b->items[rk[j].pos];
            b->order[rk[j].pos] = j;
        }
        core_free(b->items);
        b->items = items;
        items = NULL;
    } else {
        for (uint32_t i = 0; i < cnt; i++) b->order[i] = i;
    }
    core_free(items);
    core_free(rk);
    core_free(keys);
    *out = b;
    return CORE_OK;
}

static int restore_state_locked(player_t *p, bool autoplay) {
    ini_t ini;
    int r = read_ini(p, &ini);
    if (r != CORE_OK) return r;
    pq_batch_t *b = NULL;
    r = read_queue(p, &b);
    if (r != CORE_OK && r != CORE_ENOTFOUND) return r;
    prestore_t *rs = core_calloc(1, sizeof *rs);
    if (!rs) {
        pq_batch_free(b);
        return CORE_ENOMEM;
    }
    rs->batch = b;
    rs->index = (b && ini.index < b->count) ? ini.index : 0;
    rs->position_ms = ini.position_ms;
    rs->volume_db = ini.volume_db;
    rs->has_volume = ini.has_volume;
    rs->shuffle = ini.shuffle;
    rs->seed = ini.seed;
    rs->repeat = ini.repeat;
    rs->autoplay = autoplay;
    if (!ini.shuffle && b) {
        // Unshuffled: play order is the original order.
        for (uint32_t i = 0; i < b->count; i++) b->order[i] = i;
    }
    if (b && ini.count && ini.count != b->count) {
        CORE_LOGW(TAG, "restore: %lu of %lu queue entries", (unsigned long)b->count, (unsigned long)ini.count);
    }
    player_push_restore(p, rs);
    return CORE_OK;
}

int player_restore_state(player_t *p, bool autoplay) {
    if (!p) return CORE_EINVAL;
    if (!p->state_dir[0]) return CORE_EINVAL;
    core_mutex_lock(p->file_mu);
    int r = restore_state_locked(p, autoplay);
    core_mutex_unlock(p->file_mu);
    return r;
}

// ---------------------------------------------------------------------------- scrobbler ----
static void field(char *dst, size_t cap, size_t *len, const char *src) {
    for (; *src && *len + 1 < cap; src++) dst[(*len)++] = (*src == '\t' || *src == '\n' || *src == '\r') ? ' ' : *src;
    if (*len + 1 < cap) dst[(*len)++] = '\t';
    dst[*len] = 0;
}

static bool clock_valid(int64_t t) { return t > PLAYER_CLOCK_VALID; }

static int64_t entry_time(const pscrobble_t *e, int64_t now) {
    if (e->unix_ts) return e->unix_ts;
    uint32_t ago_s = (core_now_ms() - e->mono_ms) / 1000u;
    return now - (int64_t)ago_s;
}

// Appends entries to the log, creating it with the header when needed.
static int log_append(player_t *p, const pscrobble_t *ents, uint32_t n, int64_t now) {
    char path[CORE_PATH_MAX];
    if (state_path(p, SCROBBLE_FILE, path, sizeof path) != CORE_OK) return CORE_EINVAL;
    bool fresh = !file_exists(path);
    if (fresh) make_dirs(p->state_dir);
    FILE *f = fopen(path, "ab");
    if (!f) return CORE_EIO;
    bool ok = true;
    if (fresh) {
        ok = fprintf(f, "#AUDIOSCROBBLER/1.1\n#TZ/UTC\n#CLIENT/esp32-audio-player %s\n", PLAYER_VERSION_STRING) > 0;
    }
    for (uint32_t i = 0; ok && i < n; i++) {
        const pscrobble_t *e = &ents[i];
        ok = fprintf(f, "%s%lld\t\n", e->line, (long long)entry_time(e, now)) > 0;
    }
    if (fclose(f) != 0) ok = false;
    return ok ? CORE_OK : CORE_EIO;
}

void player_scrobble_flush(player_t *p) {
    if (!p || !p->scrobble_count || !p->scrobbles) return;
    int64_t now = player_wall_now(p);
    if (!clock_valid(now)) return;
    if (log_append(p, p->scrobbles, p->scrobble_count, now) == CORE_OK) p->scrobble_count = 0;
}

void player_scrobble_poll(player_t *p) {
    if (!p->scrobble_count) return;
    uint32_t now = core_now_ms();
    if (now - p->scrobble_check_ms < 1000u) return;
    p->scrobble_check_ms = now;
    player_scrobble_flush(p);
}

void player_scrobble_free(player_t *p) {
    if (p->scrobble_count) CORE_LOGW(TAG, "%lu scrobbles lost (clock never set)", (unsigned long)p->scrobble_count);
    core_free(p->scrobbles);
    p->scrobbles = NULL;
    p->scrobble_count = 0;
}

void player_scrobble_track(player_t *p, ptrack_t *t, bool completed) {
    if (!p->cfg.scrobble_log || !p->state_dir[0] || !t->open) return;
    uint32_t rate = p->sink_fmt.sample_rate ? p->sink_fmt.sample_rate : t->info.fmt.sample_rate;
    uint32_t played_ms = audio_frames_to_ms(t->played_out, rate);
    if (played_ms == 0) return;
    const pmeta_t *m = &t->meta;
    uint32_t dur_ms = m->duration_ms;
    bool listened = played_ms >= 240000u || (dur_ms && played_ms >= dur_ms / 2u) || (!dur_ms && completed);
    pscrobble_t e;
    size_t len = 0;
    e.line[0] = 0;
    field(e.line, sizeof e.line, &len, m->artist);
    field(e.line, sizeof e.line, &len, m->album);
    field(e.line, sizeof e.line, &len, m->title);
    char num[12] = "";
    if (m->track_no) snprintf(num, sizeof num, "%u", (unsigned)m->track_no);
    field(e.line, sizeof e.line, &len, num);
    char tail[24];
    snprintf(tail, sizeof tail, "%lu\t%c\t", (unsigned long)((dur_ms ? dur_ms : played_ms) / 1000u), listened ? 'L' : 'S');
    core_strlcat(e.line, tail, sizeof e.line);
    e.unix_ts = t->start_unix;
    e.mono_ms = t->start_mono_ms;

    int64_t now = player_wall_now(p);
    if (clock_valid(now) && p->scrobble_count == 0) {
        if (log_append(p, &e, 1, now) == CORE_OK) return;
    }
    // Keep it in RAM: no clock yet, older entries waiting, or the card refused the write.
    if (!p->scrobbles) {
        p->scrobbles = core_malloc(PLAYER_SCROBBLE_MAX * sizeof *p->scrobbles);
        if (!p->scrobbles) return;
    }
    if (p->scrobble_count == PLAYER_SCROBBLE_MAX) {
        memmove(&p->scrobbles[0], &p->scrobbles[1], (PLAYER_SCROBBLE_MAX - 1) * sizeof *p->scrobbles);
        p->scrobble_count--;
    }
    p->scrobbles[p->scrobble_count++] = e;
    if (clock_valid(now)) player_scrobble_flush(p);
}
