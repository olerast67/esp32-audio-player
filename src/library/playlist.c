// SPDX-License-Identifier: Apache-2.0
// Playlists: M3U / M3U8 reading and writing, CUE sheet reading.
//
// Storage: entries are kept compactly (offsets into one string pool), because
// playlist_entry_t is ~520 bytes. playlist_get() materialises an entry into a small
// ring of PL_RING slots owned by the playlist: the returned pointer stays valid until
// PL_RING further playlist_get() calls, playlist_add() or playlist_free() (enough for
// "get, copy" loops and for drawing a screen of rows).
//
// Paths: backslashes become '/', "file://" URLs are decoded, other URLs are skipped.
// Relative paths are resolved against the playlist folder. Absolute paths written on
// a PC ("D:\Music\a.mp3", "/Music/a.mp3") are mapped onto the card that holds the
// playlist ("/sdcard/Music/a.mp3"). Text encoding: UTF-8 with or without BOM, or
// CP1251 / Latin-1 detected with text_detect_legacy().
//
// CUE ranges of saved entries go into an extension line before the path
// ("#EXT-EMP-RANGE:<start ms>,<end ms>"), which other players ignore. A save that lost power
// between "remove" and "rename" leaves only "<name>.tmp": loading falls back to it and the
// next save puts it in place first, so the only complete copy is never overwritten.
#include "audio_player/playlist.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_player/text.h"
#include "lib_priv.h"

#define TAG "playlist"
#define PL_RING 16
#define PL_MAX_ENTRIES 100000
#define PL_LINE_MAX 2048
#define PL_CUE_MAX_TRACKS 999

typedef struct {
    uint32_t path, title, artist;  // pool offsets (0 = empty string)
    uint32_t start_ms, end_ms;
} pl_item_t;

struct playlist {
    pl_item_t *items;
    uint32_t count, cap;
    char *pool;
    size_t pool_len, pool_cap;
    playlist_entry_t *ring;
    uint32_t ring_next;
};

// ------------------------------------------------------------ container ----
playlist_t *playlist_new(void) {
    playlist_t *p = core_calloc(1, sizeof *p);
    if (!p) return NULL;
    p->pool_cap = 1024;
    p->pool = core_malloc(p->pool_cap);
    if (!p->pool) {
        core_free(p);
        return NULL;
    }
    p->pool[0] = 0;
    p->pool_len = 1;
    return p;
}

static int pool_add(playlist_t *p, const char *s, uint32_t *off) {
    size_t n = s ? strlen(s) : 0;
    if (!n) {
        *off = 0;
        return CORE_OK;
    }
    if (p->pool_len + n + 1 > p->pool_cap) {
        size_t cap = p->pool_cap;
        while (p->pool_len + n + 1 > cap) cap *= 2;
        if (cap > UINT32_MAX) return CORE_ENOMEM;
        char *np = core_realloc(p->pool, cap);
        if (!np) return CORE_ENOMEM;
        p->pool = np;
        p->pool_cap = cap;
    }
    *off = (uint32_t)p->pool_len;
    memcpy(p->pool + p->pool_len, s, n + 1);
    p->pool_len += n + 1;
    return CORE_OK;
}

int playlist_add(playlist_t *p, const playlist_entry_t *e) {
    if (!p || !e || !e->path[0]) return CORE_EINVAL;
    if (p->count >= PL_MAX_ENTRIES) return CORE_ENOMEM;
    if (p->count == p->cap) {
        uint32_t cap = p->cap ? p->cap * 2 : 32;
        pl_item_t *ni = core_realloc(p->items, (size_t)cap * sizeof *ni);
        if (!ni) return CORE_ENOMEM;
        p->items = ni;
        p->cap = cap;
    }
    pl_item_t it = {0, 0, 0, e->start_ms, e->end_ms};
    size_t saved = p->pool_len;
    if (pool_add(p, e->path, &it.path) || pool_add(p, e->title, &it.title) || pool_add(p, e->artist, &it.artist)) {
        p->pool_len = saved;
        return CORE_ENOMEM;
    }
    p->items[p->count++] = it;
    return CORE_OK;
}

uint32_t playlist_count(const playlist_t *p) { return p ? p->count : 0; }

const playlist_entry_t *playlist_get(const playlist_t *cp, uint32_t index) {
    playlist_t *p = (playlist_t *)cp;  // the ring is a cache, not observable state
    if (!p || index >= p->count) return NULL;
    if (!p->ring) {
        p->ring = core_malloc(PL_RING * sizeof *p->ring);
        if (!p->ring) return NULL;
    }
    playlist_entry_t *e = &p->ring[p->ring_next++ % PL_RING];
    const pl_item_t *it = &p->items[index];
    core_strlcpy(e->path, p->pool + it->path, sizeof e->path);
    core_strlcpy(e->title, p->pool + it->title, sizeof e->title);
    core_strlcpy(e->artist, p->pool + it->artist, sizeof e->artist);
    e->start_ms = it->start_ms;
    e->end_ms = it->end_ms;
    return e;
}

void playlist_free(playlist_t *p) {
    if (!p) return;
    core_free(p->items);
    core_free(p->pool);
    core_free(p->ring);
    core_free(p);
}

// ---------------------------------------------------------------- paths ----
// Collapse "//", "/./" and "dir/../" in place. Never climbs above the root.
static void normalize_path(char *path) {
    // The output is written over the input; it never overtakes the read position
    // because every input segment after the first is preceded by at least one '/'.
    const bool abs = path[0] == '/';
    const size_t root = abs ? 1 : 0;
    size_t o = root;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - seg);
        while (*p == '/') p++;
        if (len == 1 && seg[0] == '.') continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (o > root) {
                size_t start = o;
                while (start > root && path[start - 1] != '/') start--;
                bool prev_up = o - start == 2 && path[start] == '.' && path[start + 1] == '.';
                if (!prev_up) {
                    o = start > root ? start - 1 : root;  // drop the previous segment
                    continue;
                }
            } else if (abs) {
                continue;  // "/.." is "/"
            }
        }
        if (o > root) path[o++] = '/';
        memmove(path + o, seg, len);
        o += len;
    }
    path[o] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Mount prefix of a device path: "/sdcard/Playlists/a.m3u" -> "/sdcard". "" otherwise.
static void mount_of(const char *path, char *out, size_t cap) {
    out[0] = 0;
    if (path[0] != '/') return;
    const char *e = strchr(path + 1, '/');
    if (!e) return;
    size_t n = (size_t)(e - path);
    if (n >= cap) return;
    memcpy(out, path, n);
    out[n] = 0;
}

// Resolve one playlist line to an absolute path. Returns false for URLs and junk.
static bool resolve_entry(const char *dir, const char *mount, const char *entry, char *out, size_t cap) {
    char buf[CORE_PATH_MAX * 2];
    const char *s = entry;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return false;
    if (!strncmp(s, "file://", 7)) {
        s += 7;
        if (!strncmp(s, "localhost/", 10)) s += 9;
        // Percent-decode.
        size_t o = 0;
        for (; *s && o + 1 < sizeof buf; s++) {
            if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
                buf[o++] = (char)(hexval(s[1]) * 16 + hexval(s[2]));
                s += 2;
            } else {
                buf[o++] = *s;
            }
        }
        buf[o] = 0;
        // "file:///C:/x" -> "C:/x"
        if (buf[0] == '/' && ((buf[1] | 0x20) >= 'a' && (buf[1] | 0x20) <= 'z') && buf[2] == ':') {
            memmove(buf, buf + 1, strlen(buf));
        }
    } else {
        if (strstr(s, "://")) return false;  // http, rtsp, ...
        if (core_strlcpy(buf, s, sizeof buf) >= sizeof buf) return false;
    }
    for (char *p = buf; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    if (buf[0] == '/' && buf[1] == '/') return false;  // UNC share
    const char *rel = buf;
    char joined[CORE_PATH_MAX * 2];
    bool drive = ((buf[0] | 0x20) >= 'a' && (buf[0] | 0x20) <= 'z') && buf[1] == ':';
    if (drive && mount[0]) {
        // Windows path on the PC: the drive is the card.
        rel = buf + 2;
        if (snprintf(joined, sizeof joined, "%s%s%s", mount, rel[0] == '/' ? "" : "/", rel) >= (int)sizeof joined) {
            return false;
        }
    } else if (drive || buf[0] == '/') {
        size_t ml = strlen(mount);
        if (buf[0] == '/' && ml && !(strncmp(buf, mount, ml) == 0 && (buf[ml] == '/' || buf[ml] == 0))) {
            if (snprintf(joined, sizeof joined, "%s%s", mount, buf) >= (int)sizeof joined) return false;
        } else {
            core_strlcpy(joined, buf, sizeof joined);
        }
    } else {
        if (snprintf(joined, sizeof joined, "%s%s%s", dir, dir[0] ? "/" : "", buf) >= (int)sizeof joined) {
            return false;
        }
    }
    normalize_path(joined);
    if (!joined[0] || strlen(joined) >= cap) return false;
    core_strlcpy(out, joined, cap);
    return true;
}

// ------------------------------------------------------------ file input ----
// Streaming line reader: constant memory whatever the playlist size. The code page is
// detected on the first 64 KiB (BOM, UTF-8, CP1251 or Latin-1); a line that is not
// valid UTF-8 in a "UTF-8" file is detected on its own. UTF-16 files are read by code
// unit. Lines longer than PL_LINE_MAX are cut (such paths cannot be opened anyway).
#define LR_SAMPLE (64u * 1024)

typedef struct {
    FILE *f;
    text_encoding_t enc;  // for 8-bit files
    int utf16;            // 0 = no, 1 = little-endian, 2 = big-endian
    uint8_t buf[4096];
    size_t blen, bpos;
    bool eof;
    uint8_t raw[PL_LINE_MAX];
    char line[PL_LINE_MAX * 3];
} line_reader_t;

static line_reader_t *lr_open(const char *path) {
    errno = 0;
    FILE *f = fopen(path, "rb");
    if (!f && errno == ENOENT) {
        char tmp[CORE_PATH_MAX + 8];
        if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) < sizeof tmp && (f = fopen(tmp, "rb")) != NULL) {
            CORE_LOGW(TAG, "%s missing, reading %s (interrupted save)", path, tmp);
        }
    }
    if (!f) return NULL;
    line_reader_t *lr = core_calloc(1, sizeof *lr);
    uint8_t *sample = core_malloc(LR_SAMPLE);
    if (!lr || !sample) {
        core_free(lr);
        core_free(sample);
        fclose(f);
        return NULL;
    }
    lr->f = f;
    size_t n = fread(sample, 1, LR_SAMPLE, f);
    long skip = 0;
    if (n >= 3 && sample[0] == 0xEF && sample[1] == 0xBB && sample[2] == 0xBF) {
        lr->enc = TEXT_ENC_UTF8;
        skip = 3;
    } else if (n >= 2 && sample[0] == 0xFF && sample[1] == 0xFE) {
        lr->utf16 = 1;
        skip = 2;
    } else if (n >= 2 && sample[0] == 0xFE && sample[1] == 0xFF) {
        lr->utf16 = 2;
        skip = 2;
    } else {
        // NUL bytes would end the sample for the detector: ignore them.
        for (size_t i = 0; i < n; i++) {
            if (!sample[i]) sample[i] = ' ';
        }
        // A sample cut inside a multi-byte character is not valid UTF-8 and would be taken
        // for CP1251: drop the cut sequence.
        if (n == LR_SAMPLE) n = tagf_trim_partial_utf8(sample, n);
        lr->enc = text_detect_legacy(sample, n);
    }
    core_free(sample);
    if (fseek(f, skip, SEEK_SET) != 0) {
        fclose(f);
        core_free(lr);
        return NULL;
    }
    return lr;
}

static int lr_getc(line_reader_t *lr) {
    if (lr->bpos == lr->blen) {
        if (lr->eof) return -1;
        lr->blen = fread(lr->buf, 1, sizeof lr->buf, lr->f);
        lr->bpos = 0;
        if (lr->blen == 0) {
            lr->eof = true;
            return -1;
        }
    }
    return lr->buf[lr->bpos++];
}

// Next line as UTF-8 (without line break), or NULL at the end of the file.
static char *lr_next(line_reader_t *lr) {
    size_t n = 0;
    bool any = false;
    if (lr->utf16) {
        for (;;) {
            int a = lr_getc(lr), b = a < 0 ? -1 : lr_getc(lr);
            if (b < 0) break;
            any = true;
            uint32_t u = lr->utf16 == 1 ? (uint32_t)(b << 8 | a) : (uint32_t)(a << 8 | b);
            if (u == '\n') break;
            if (u == '\r' || u == 0) continue;
            if (n + 2 <= sizeof lr->raw) {
                lr->raw[n++] = (uint8_t)(u >> 8);  // stored big-endian
                lr->raw[n++] = (uint8_t)u;
            }
        }
        if (!any) return NULL;
        text_to_utf8(lr->raw, n, TEXT_ENC_UTF16BE, lr->line, sizeof lr->line);
        return lr->line;
    }
    for (;;) {
        int c = lr_getc(lr);
        if (c < 0) break;
        any = true;
        if (c == '\n') break;
        if (c == '\r' || c == 0) continue;
        if (n < sizeof lr->raw) lr->raw[n++] = (uint8_t)c;
    }
    if (!any) return NULL;
    text_encoding_t enc = lr->enc;
    bool high = false;
    for (size_t i = 0; i < n && !high; i++) high = lr->raw[i] >= 0x80;
    if (high && text_is_valid_utf8(lr->raw, n)) {
        // Valid UTF-8 with non-ASCII text is UTF-8 whatever the sample said (a CP1251 or
        // Latin-1 line practically never is valid UTF-8).
        enc = TEXT_ENC_UTF8;
    } else if (enc == TEXT_ENC_UTF8 && high) {
        enc = text_detect_legacy(lr->raw, n);
    }
    text_to_utf8(lr->raw, n, enc, lr->line, sizeof lr->line);
    return lr->line;
}

static void lr_close(line_reader_t *lr) {
    if (!lr) return;
    fclose(lr->f);
    core_free(lr);
}

// ------------------------------------------------------------------ M3U ----
// "#EXTINF:123 tvg-id="x",Artist - Title" -> artist, title.
static void parse_extinf(const char *s, char *title, size_t tcap, char *artist, size_t acap) {
    title[0] = artist[0] = 0;
    bool quoted = false;
    for (; *s; s++) {
        if (*s == '"') quoted = !quoted;
        if (*s == ',' && !quoted) break;
    }
    if (*s != ',') return;
    s++;
    char disp[PL_LINE_MAX];
    core_strlcpy(disp, s, sizeof disp);
    text_trim(disp);
    char *dash = strstr(disp, " - ");
    if (dash) {
        *dash = 0;
        core_strlcpy(artist, disp, acap);
        core_strlcpy(title, dash + 3, tcap);
    } else {
        core_strlcpy(title, disp, tcap);
    }
    utf8_truncate(title, tcap - 1);
    utf8_truncate(artist, acap - 1);
    text_trim(title);
    text_trim(artist);
}

playlist_t *playlist_load_m3u(const char *path) {
    if (!path) return NULL;
    line_reader_t *lr = lr_open(path);
    if (!lr) return NULL;
    playlist_t *p = playlist_new();
    if (!p) {
        lr_close(lr);
        return NULL;
    }
    char dir[CORE_PATH_MAX], mount[64];
    core_path_dirname(path, dir, sizeof dir);
    if (!strcmp(dir, ".") && !strchr(path, '/')) dir[0] = 0;
    mount_of(path, mount, sizeof mount);
    playlist_entry_t *e = core_calloc(1, sizeof *e);
    char *line;
    bool pending = false, ranged = false;
    unsigned long r_start = 0, r_end = 0;
    while (e && (line = lr_next(lr)) != NULL) {
        text_trim(line);
        if (!line[0]) continue;
        if (line[0] == '#') {
            if (!strncmp(line, "#EXTINF:", 8)) {
                parse_extinf(line + 8, e->title, sizeof e->title, e->artist, sizeof e->artist);
                pending = true;
            } else if (!strncmp(line, "#EXT-EMP-RANGE:", 15)) {
                ranged = sscanf(line + 15, "%lu,%lu", &r_start, &r_end) == 2 && r_start <= UINT32_MAX &&
                         r_end <= UINT32_MAX;
            }
            continue;
        }
        if (!pending) e->title[0] = e->artist[0] = 0;
        pending = false;
        e->start_ms = ranged ? (uint32_t)r_start : 0;
        e->end_ms = ranged && r_end > r_start ? (uint32_t)r_end : 0;
        ranged = false;
        if (!resolve_entry(dir, mount, line, e->path, sizeof e->path)) continue;
        if (playlist_add(p, e) == CORE_ENOMEM) {
            CORE_LOGW(TAG, "%s: out of memory after %u entries", path, (unsigned)p->count);
            break;
        }
        e->title[0] = e->artist[0] = 0;
    }
    core_free(e);
    lr_close(lr);
    return p;
}

// ------------------------------------------------------------------ CUE ----
typedef struct {
    char file[CORE_PATH_MAX];
    char title[128];
    char performer[128];
    uint32_t start_ms;
    bool has_start;
} cue_track_t;

// Next argument: "quoted string" or a bare word. Returns false at the end of line.
static bool cue_arg(const char **ps, char *out, size_t cap) {
    const char *s = *ps;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return false;
    size_t n = 0;
    if (*s == '"') {
        s++;
        while (*s && *s != '"') {
            if (n + 1 < cap) out[n++] = *s;
            s++;
        }
        if (*s == '"') s++;
    } else {
        while (*s && *s != ' ' && *s != '\t') {
            if (n + 1 < cap) out[n++] = *s;
            s++;
        }
    }
    out[n] = 0;
    *ps = s;
    return true;
}

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return false;
    }
    return *a == *b;
}

static bool parse_msf(const char *s, uint32_t *ms) {
    unsigned m = 0, sec = 0, fr = 0;
    char tail;
    if (sscanf(s, "%u:%u:%u%c", &m, &sec, &fr, &tail) != 3 || sec >= 60 || fr >= 75 || m > 6000) return false;
    *ms = (m * 60u + sec) * 1000u + fr * 1000u / 75u;
    return true;
}

// FILE "CDImage.wav" often survives a conversion to FLAC/APE/WavPack: find the real file.
static void cue_fix_file(const char *cue_path, char *file, size_t cap) {
    if (lfs_exists(file)) return;
    static const char *const exts[] = {"flac", "ape", "wv", "wav", "mp3", "m4a", "ogg", "opus", "aiff", "aif",
                                       "dsf", "dff", "tta", "tak"};
    char stem[CORE_PATH_MAX], cand[CORE_PATH_MAX];
    for (int pass = 0; pass < 2; pass++) {
        core_strlcpy(stem, pass == 0 ? file : cue_path, sizeof stem);
        char *dot = strrchr(stem, '.');
        char *slash = strrchr(stem, '/');
        if (dot && (!slash || dot > slash)) *dot = 0;
        for (size_t i = 0; i < CORE_ARRAY_SIZE(exts); i++) {
            if ((size_t)snprintf(cand, sizeof cand, "%s.%s", stem, exts[i]) >= sizeof cand) continue;
            if (lfs_exists(cand)) {
                core_strlcpy(file, cand, cap);
                return;
            }
        }
    }
}

playlist_t *playlist_load_cue(const char *path) {
    if (!path) return NULL;
    line_reader_t *lr = lr_open(path);
    if (!lr) return NULL;
    char dir[CORE_PATH_MAX], mount[64];
    core_path_dirname(path, dir, sizeof dir);
    if (!strcmp(dir, ".") && !strchr(path, '/')) dir[0] = 0;
    mount_of(path, mount, sizeof mount);

    uint32_t cap = 32, n = 0;
    cue_track_t *tracks = core_malloc(cap * sizeof *tracks);
    char album_performer[128] = "", cur_file[CORE_PATH_MAX] = "";
    char arg[CORE_PATH_MAX], cmd[32];
    cue_track_t *cur = NULL;
    bool in_audio = false;
    char *line;
    while (tracks && (line = lr_next(lr)) != NULL) {
        const char *s = line;
        if (!cue_arg(&s, cmd, sizeof cmd)) continue;
        if (ieq(cmd, "FILE")) {
            if (cue_arg(&s, arg, sizeof arg) && resolve_entry(dir, mount, arg, cur_file, sizeof cur_file)) {
                cue_fix_file(path, cur_file, sizeof cur_file);
            } else {
                cur_file[0] = 0;
            }
            cur = NULL;
        } else if (ieq(cmd, "TRACK")) {
            char type[16] = "";
            cue_arg(&s, arg, sizeof arg);
            cue_arg(&s, type, sizeof type);
            in_audio = ieq(type, "AUDIO") && cur_file[0];
            cur = NULL;
            if (!in_audio) continue;
            if (n >= PL_CUE_MAX_TRACKS) break;
            if (n == cap) {
                cue_track_t *nt = core_realloc(tracks, (size_t)cap * 2 * sizeof *nt);
                if (!nt) break;
                tracks = nt;
                cap *= 2;
            }
            cur = &tracks[n++];
            memset(cur, 0, sizeof *cur);
            core_strlcpy(cur->file, cur_file, sizeof cur->file);
        } else if (ieq(cmd, "TITLE") || ieq(cmd, "PERFORMER")) {
            char val[256];
            if (!cue_arg(&s, val, sizeof val)) continue;
            bool title = ieq(cmd, "TITLE");
            char *dst = cur ? (title ? cur->title : cur->performer) : (title ? NULL : album_performer);
            if (!dst) continue;  // album title is not part of playlist entries
            core_strlcpy(dst, val, 128);
            utf8_truncate(dst, 127);
            text_trim(dst);
        } else if (ieq(cmd, "INDEX") && cur) {
            char num[8];
            uint32_t ms;
            if (cue_arg(&s, num, sizeof num) && atoi(num) == 1 && cue_arg(&s, arg, sizeof arg) && parse_msf(arg, &ms)) {
                cur->start_ms = ms;
                cur->has_start = true;
            }
        }
    }
    lr_close(lr);
    playlist_t *p = tracks ? playlist_new() : NULL;
    playlist_entry_t *e = p ? core_calloc(1, sizeof *e) : NULL;
    if (!e) {
        playlist_free(p);
        core_free(tracks);
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
        cue_track_t *t = &tracks[i];
        if (!t->has_start) continue;
        core_strlcpy(e->path, t->file, sizeof e->path);
        core_strlcpy(e->title, t->title, sizeof e->title);
        core_strlcpy(e->artist, t->performer[0] ? t->performer : album_performer, sizeof e->artist);
        e->start_ms = t->start_ms;
        e->end_ms = 0;
        // The track ends where the next one in the same file starts.
        for (uint32_t k = i + 1; k < n; k++) {
            if (strcmp(tracks[k].file, t->file) != 0) break;
            if (tracks[k].has_start && tracks[k].start_ms > t->start_ms) {
                e->end_ms = tracks[k].start_ms;
                break;
            }
        }
        if (playlist_add(p, e) != CORE_OK) break;
    }
    core_free(e);
    core_free(tracks);
    return p;
}

// ----------------------------------------------------------------- save ----
// Path of target relative to dir ("../Music/a.flac"); absolute when there is no
// common root (different mounts).
static void relative_path(const char *dir, const char *target, char *out, size_t cap) {
    if ((dir[0] == '/') != (target[0] == '/')) {
        core_strlcpy(out, target, cap);
        return;
    }
    // Common prefix by whole components.
    size_t i = 0, common = 0;
    while (dir[i] && target[i] && dir[i] == target[i]) {
        if (dir[i] == '/') common = i + 1;
        i++;
    }
    if (!dir[i] && (target[i] == '/' || !target[i])) common = i + (target[i] == '/' ? 1 : 0);
    if (dir[0] == '/' && common <= 1) {
        // Only the root in common: different mounts, keep absolute.
        const char *e1 = strchr(dir + 1, '/'), *e2 = strchr(target + 1, '/');
        size_t l1 = e1 ? (size_t)(e1 - dir) : strlen(dir), l2 = e2 ? (size_t)(e2 - target) : strlen(target);
        if (l1 != l2 || strncmp(dir, target, l1) != 0) {
            core_strlcpy(out, target, cap);
            return;
        }
    }
    out[0] = 0;
    // One "../" per remaining component of dir.
    const char *rest = dir + CORE_MIN(common, strlen(dir));
    if (*rest && !(rest[0] == '.' && rest[1] == 0)) {
        for (const char *p = rest; *p;) {
            core_strlcat(out, "../", cap);
            while (*p && *p != '/') p++;
            while (*p == '/') p++;
        }
    }
    core_strlcat(out, target + common, cap);
}

int playlist_save_m3u8(const playlist_t *p, const char *path) {
    if (!p || !path) return CORE_EINVAL;
    char dir[CORE_PATH_MAX], tmp[CORE_PATH_MAX];
    core_path_dirname(path, dir, sizeof dir);
    if (!strcmp(dir, ".") && !strchr(path, '/')) dir[0] = 0;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return CORE_EINVAL;
    // An earlier save lost power between "remove" and "rename": the .tmp is the only complete
    // copy (playlist_load_m3u() read it). Put it in place before it is overwritten.
    if (!lfs_exists(path) && lfs_exists(tmp) && lfs_replace(tmp, path) != CORE_OK) return CORE_EIO;
    FILE *f = fopen(tmp, "wb");
    if (!f) return CORE_EIO;
    bool ok = fputs("#EXTM3U\n", f) >= 0;
    char rel[CORE_PATH_MAX * 2];
    for (uint32_t i = 0; ok && i < p->count; i++) {
        const pl_item_t *it = &p->items[i];
        const char *title = p->pool + it->title, *artist = p->pool + it->artist;
        if (*title || *artist) {
            long dur = it->end_ms > it->start_ms ? (long)((it->end_ms - it->start_ms) / 1000) : -1;
            ok = fprintf(f, "#EXTINF:%ld,%s%s%s\n", dur, artist, (*artist && *title) ? " - " : "", title) > 0;
        }
        if (ok && (it->start_ms || it->end_ms)) {
            // A CUE track: without its range the entry would play the whole image.
            ok = fprintf(f, "#EXT-EMP-RANGE:%lu,%lu\n", (unsigned long)it->start_ms, (unsigned long)it->end_ms) > 0;
        }
        relative_path(dir, p->pool + it->path, rel, sizeof rel);
        if (ok) ok = fprintf(f, "%s\n", rel) > 0;
    }
    int r = ok ? lfs_fsync(f) : CORE_EIO;
    if (fclose(f) != 0 && r == CORE_OK) r = CORE_EIO;
    if (r == CORE_OK) r = lfs_replace(tmp, path);
    if (r != CORE_OK) lfs_remove(tmp);
    return r;
}
