// SPDX-License-Identifier: Apache-2.0
// Queue entry -> open decoder + metadata.
//
// Library entries take path, tags and ReplayGain from the index record; other entries
// read the tags from the file (a separate stream, so the decoder's read position and
// readahead are not disturbed). CUE virtual tracks (entries with a time range) get their
// title and performer from the CUE sheet next to the audio file: "<name>.cue",
// "<name>.<ext>.cue" or any .cue in the folder that references the file. The last sheet
// is cached, so consecutive tracks of one image do not reread it.
#include <dirent.h>
#include <math.h>
#include <string.h>

#include "player/player_priv.h"

#define TAG PLAYER_TAG
#define CUE_SCAN_MAX 16  // .cue files examined per folder

// ------------------------------------------------------------------------------ helpers ----
static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

// Paths on FAT are case-insensitive; CUE sheets often differ in case from the files.
static bool path_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) return false;
    }
    return *a == *b;
}

static size_t dir_len(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? (size_t)(slash - path) : 0;
}

static bool same_dir(const char *a, const char *b) {
    size_t la = dir_len(a), lb = dir_len(b);
    if (la != lb) return false;
    for (size_t i = 0; i < la; i++) {
        if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i])) return false;
    }
    return true;
}

// Copies a UTF-8 string, cutting only at character boundaries.
static void utf8_copy(char *dst, const char *src, size_t size) {
    if (!size) return;
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void meta_clear(pmeta_t *m) {
    memset(m, 0, sizeof *m);
    m->track_id = LIB_ID_NONE;
    m->album_id = LIB_ID_NONE;
    m->rg_track_db = m->rg_track_peak = m->rg_album_db = m->rg_album_peak = NAN;
}

static void meta_from_tags(pmeta_t *m, const track_tags_t *t) {
    utf8_copy(m->title, t->title, sizeof m->title);
    utf8_copy(m->artist, t->artist[0] ? t->artist : t->album_artist, sizeof m->artist);
    utf8_copy(m->album, t->album, sizeof m->album);
    m->year = t->year;
    m->track_no = t->track_no;
    m->cover_offset = t->cover_offset;
    m->cover_size = t->cover_size;
    m->rg_track_db = t->rg_track_gain_db;
    m->rg_track_peak = t->rg_track_peak;
    m->rg_album_db = t->rg_album_gain_db;
    m->rg_album_peak = t->rg_album_peak;
    m->duration_ms = t->duration_ms;
}

void player_meta_title_fallback(pmeta_t *m) {
    if (m->title[0]) return;
    const char *base = core_path_basename(m->path);
    utf8_copy(m->title, base, sizeof m->title);
    char *dot = strrchr(m->title, '.');
    if (dot && dot != m->title) *dot = 0;
}

static core_stream_t *open_stream(player_t *p, const char *path) {
    if (p->cfg.open_stream) return p->cfg.open_stream(path, p->cfg.open_stream_user);
    return core_stream_open_file(path, 0);
}

static int read_tags(player_t *p, const char *path, track_tags_t *out) {
    tags_clear(out);
    if (!p->cfg.open_stream) return tags_read_file(path, out);
    core_stream_t *s = p->cfg.open_stream(path, p->cfg.open_stream_user);
    if (!s) return CORE_ENOTFOUND;
    int r = tags_read_stream(s, path, out);
    core_stream_close(s);
    return r;
}

// ------------------------------------------------------------------------------- CUE ----
static bool cue_references(const playlist_t *pl, const char *audio) {
    uint32_t n = playlist_count(pl);
    for (uint32_t i = 0; i < n; i++) {
        const playlist_entry_t *e = playlist_get(pl, i);
        if (e && path_eq(e->path, audio)) return true;
    }
    return false;
}

static playlist_t *try_cue(const char *cue_path, const char *audio) {
    FILE *f = fopen(cue_path, "rb");
    if (!f) return NULL;
    fclose(f);
    playlist_t *pl = playlist_load_cue(cue_path);
    if (pl && cue_references(pl, audio)) return pl;
    playlist_free(pl);
    return NULL;
}

static playlist_t *find_cue(const char *audio) {
    char cand[CORE_PATH_MAX];
    // "<name>.cue"
    const char *ext = core_path_ext(audio);
    if (*ext) {
        size_t stem = (size_t)(ext - audio);  // includes the dot
        if (stem + 3 < sizeof cand) {
            memcpy(cand, audio, stem);
            memcpy(cand + stem, "cue", 4);
            playlist_t *pl = try_cue(cand, audio);
            if (pl) return pl;
        }
    }
    // "<name>.<ext>.cue"
    if ((size_t)snprintf(cand, sizeof cand, "%s.cue", audio) < sizeof cand) {
        playlist_t *pl = try_cue(cand, audio);
        if (pl) return pl;
    }
    // Any sheet in the folder that names this file.
    char dir[CORE_PATH_MAX];
    core_path_dirname(audio, dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return NULL;
    playlist_t *found = NULL;
    int examined = 0;
    struct dirent *de;
    while (!found && examined < CUE_SCAN_MAX && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' || !core_str_ends_with_ci(de->d_name, ".cue")) continue;
        if (core_path_join(cand, sizeof cand, dir, de->d_name) != CORE_OK) continue;
        examined++;
        found = try_cue(cand, audio);
    }
    closedir(d);
    return found;
}

void player_meta_cue_title(player_t *p, ptrack_t *t) {
    const char *audio = t->meta.path;
    if (!p->cue_checked || strcmp(p->cue_for, audio) != 0) {
        playlist_free(p->cue_pl);
        p->cue_pl = find_cue(audio);
        core_strlcpy(p->cue_for, audio, sizeof p->cue_for);
        p->cue_checked = true;
    }
    if (!p->cue_pl) return;
    uint32_t n = playlist_count(p->cue_pl), idx = 0;
    const playlist_entry_t *best = NULL;
    for (uint32_t i = 0; i < n; i++) {
        const playlist_entry_t *e = playlist_get(p->cue_pl, i);
        if (!e || !path_eq(e->path, audio)) continue;
        idx++;
        // CUE times have 1/75 s resolution; the queue entry holds the same rounded ms.
        uint32_t d = e->start_ms > t->start_ms ? e->start_ms - t->start_ms : t->start_ms - e->start_ms;
        if (d <= 1) {
            best = e;
            break;
        }
    }
    if (!best) return;
    if (best->title[0]) utf8_copy(t->meta.title, best->title, sizeof t->meta.title);
    if (best->artist[0]) utf8_copy(t->meta.artist, best->artist, sizeof t->meta.artist);
    t->meta.track_no = (uint16_t)idx;
}

void player_meta_free(player_t *p) {
    playlist_free(p->cue_pl);
    p->cue_pl = NULL;
    p->cue_checked = false;
}

// ------------------------------------------------------------------------------ open ----
static uint64_t ms_frames(uint32_t ms, uint32_t rate) { return audio_ms_to_frames(ms, rate); }

int player_meta_open(player_t *p, ptrack_t *t, const pq_item_t *qi, const char *qpath, uint32_t qpos) {
    memset(t, 0, sizeof *t);
    pmeta_t *m = &t->meta;
    meta_clear(m);
    bool have_tags = false;
    m->lib_gen = library_generation(p->cfg.library);
    if (qi->track_id != LIB_ID_NONE && p->cfg.library) {
        if (library_get_track(p->cfg.library, qi->track_id, &p->lt) == CORE_OK && p->lt.path[0]) {
            core_strlcpy(m->path, p->lt.path, sizeof m->path);
            meta_from_tags(m, &p->lt.tags);
            m->track_id = qi->track_id;
            m->album_id = p->lt.album_id;
            have_tags = true;
        }
    }
    if (!m->path[0]) {
        if (!qpath || !qpath[0]) return CORE_ENOTFOUND;
        core_strlcpy(m->path, qpath, sizeof m->path);
    }
    core_stream_t *s = open_stream(p, m->path);
    if (!s) return CORE_ENOTFOUND;
    int err = CORE_OK;
    t->dec = decoder_open(s, m->path, &t->info, &err);  // takes the stream in all cases
    if (!t->dec) return err < 0 ? err : CORE_ECORRUPT;
    if (!have_tags && read_tags(p, m->path, &p->tags) == CORE_OK) meta_from_tags(m, &p->tags);

    const uint32_t rate = t->info.fmt.sample_rate;
    const uint64_t total = t->info.total_frames;
    t->start_ms = qi->start_ms;
    t->end_ms = qi->end_ms > qi->start_ms ? qi->end_ms : 0;
    t->start_frame = ms_frames(t->start_ms, rate);
    t->end_frame = t->end_ms ? ms_frames(t->end_ms, rate) : 0;
    if (total && t->end_frame >= total) t->end_frame = 0;  // until the end of the file
    if (total && t->start_frame >= total) {
        CORE_LOGW(TAG, "%s: start %lu ms is past the end", m->path, (unsigned long)t->start_ms);
        decoder_close(t->dec);
        t->dec = NULL;
        return CORE_EINVAL;
    }
    if (t->start_frame) {
        if (decoder_seek(t->dec, t->start_frame) == CORE_OK) {
            t->pos = t->start_frame;
        } else {
            t->skip = t->start_frame;  // not seekable: decode and drop up to the start
        }
    }
    // Duration of the entry.
    if (t->end_frame) {
        m->duration_ms = t->end_ms - t->start_ms;
    } else if (total) {
        m->duration_ms = audio_frames_to_ms(total - t->start_frame, rate);
    } else if (m->duration_ms > t->start_ms) {
        m->duration_ms -= t->start_ms;
    } else if (t->start_ms) {
        m->duration_ms = 0;
    }
    if ((t->start_ms || t->end_ms) && m->track_id == LIB_ID_NONE) player_meta_cue_title(p, t);
    player_meta_title_fallback(m);
    t->open = true;
    t->qpos = qpos;
    return CORE_OK;
}

// ------------------------------------------------------------------- album context ----
// Album context for ReplayGain "auto": the queue plays in order (no shuffle) and the entry
// before or after this one belongs to the same album (same library album, else same folder).
static bool neighbour_same_album(player_t *p, const pq_item_t *it, uint32_t lib_gen, const char *path,
                                 const pmeta_t *m) {
    if (it->track_id != LIB_ID_NONE && p->cfg.library) {
        const lib_id_t id = player_lib_id(p, it->track_id, lib_gen);
        if (library_get_track(p->cfg.library, id, &p->lt) != CORE_OK) return false;
        if (m->album_id != LIB_ID_NONE) return p->lt.album_id == m->album_id;
        return same_dir(p->lt.path, m->path);
    }
    return path && same_dir(path, m->path);
}

bool player_meta_album_ctx(player_t *p, uint32_t qpos, const pmeta_t *m) {
    if (p->dsp_user.rg_mode != RG_AUTO) return p->dsp_user.rg_mode == RG_ALBUM;
    pq_item_t nb[2];
    char paths[2][CORE_PATH_MAX];
    bool has[2] = {false, false};
    core_mutex_lock(p->mu);
    const uint32_t lib_gen = p->q.lib_gen;
    bool ordered = !p->q.shuffled && p->q.count > 1 && qpos < p->q.count;
    if (ordered) {
        for (int k = 0; k < 2; k++) {
            uint32_t pos = k == 0 ? qpos - 1 : qpos + 1;
            if ((k == 0 && qpos == 0) || pos >= p->q.count) continue;
            const pq_item_t *it = pq_at(&p->q, pos);
            nb[k] = *it;
            const char *path = pq_path(&p->q, it);
            core_strlcpy(paths[k], path ? path : "", sizeof paths[k]);
            has[k] = true;
        }
    }
    core_mutex_unlock(p->mu);
    for (int k = 0; k < 2; k++) {
        if (has[k] && neighbour_same_album(p, &nb[k], lib_gen, paths[k][0] ? paths[k] : NULL, m)) return true;
    }
    return false;
}
