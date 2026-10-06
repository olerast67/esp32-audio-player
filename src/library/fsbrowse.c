// SPDX-License-Identifier: Apache-2.0
// Folder browsing without the index. One directory is read, filtered (folders, audio
// by extension, .m3u/.m3u8 playlists, .cue sheets) and sorted: folders first, then
// files, each group with collate_cmp (natural numbers: "2" < "10", case-insensitive,
// ё = е). The entry array is sized to the directory (up to max_entries).
#include "audio_player/fsbrowse.h"

#include <stdlib.h>
#include <string.h>

#include "audio_player/collate.h"
#include "audio_player/decoder.h"
#include "lib_priv.h"

#define TAG "fsbrowse"
#define FS_DEFAULT_MAX 4096

struct fs_listing {
    fs_entry_t *entries;
    uint32_t count;
    uint32_t cap;
    uint32_t dir_count;
    bool truncated;  // entries were dropped (max_entries or out of memory)
};

static int entry_cmp(const void *pa, const void *pb) {
    const fs_entry_t *a = pa, *b = pb;
    bool da = a->type == FS_ENTRY_DIR, db = b->type == FS_ENTRY_DIR;
    if (da != db) return da ? -1 : 1;
    int r = collate_cmp(a->name, b->name, NULL);
    if (r) return r;
    return (int)a->type - (int)b->type;
}

static bool classify_file(const char *name, fs_entry_type_t *type) {
    const char *ext = core_path_ext(name);
    if (!*ext) return false;
    if (core_str_ends_with_ci(name, ".m3u") || core_str_ends_with_ci(name, ".m3u8")) {
        *type = FS_ENTRY_PLAYLIST;
        return true;
    }
    if (core_str_ends_with_ci(name, ".cue")) {
        *type = FS_ENTRY_CUE;
        return true;
    }
    if (codec_is_audio_extension(ext)) {
        *type = FS_ENTRY_AUDIO;
        return true;
    }
    return false;
}

static bool grow(fs_listing_t *l, uint32_t max) {
    if (l->count < l->cap) return true;
    uint32_t cap = l->cap ? l->cap * 2 : 64;
    if (cap > max) cap = max;
    if (cap <= l->cap) return false;
    fs_entry_t *e = core_realloc(l->entries, (size_t)cap * sizeof *e);
    if (!e) return false;
    l->entries = e;
    l->cap = cap;
    return true;
}

fs_listing_t *fs_list_dir(const char *path, uint32_t max_entries) {
    if (!path) return NULL;
    if (!max_entries) max_entries = FS_DEFAULT_MAX;
    lfs_dir_t *d = lfs_opendir(path);
    if (!d) return NULL;
    fs_listing_t *l = core_calloc(1, sizeof *l);
    if (!l) {
        lfs_closedir(d);
        return NULL;
    }
    lfs_dirent_t de;
    char full[CORE_PATH_MAX];
    while (lfs_readdir(d, &de)) {
        if (de.name[0] == '.') continue;  // hidden files, "._" resource forks
        if (strlen(de.name) >= CORE_PATH_MAX) continue;
        if (core_path_join(full, sizeof full, path, de.name) != CORE_OK) continue;  // unusable path
        fs_entry_type_t type;
        uint32_t size = 0;
        lfs_type_t kind = de.type;
        lfs_stat_t st;
        bool have_stat = false;
        if (kind != LFS_TYPE_DIR) {
            // stat() right after readdir() is served from the directory cache on the device.
            if (lfs_stat(full, &st) != CORE_OK) continue;
            have_stat = true;
            kind = st.is_dir ? LFS_TYPE_DIR : LFS_TYPE_FILE;
        }
        if (kind == LFS_TYPE_DIR) {
            if (lfs_is_skipped_dir(de.name)) continue;
            type = FS_ENTRY_DIR;
        } else {
            if (!classify_file(de.name, &type)) continue;
            size = have_stat ? (uint32_t)CORE_MIN(st.size, (uint64_t)UINT32_MAX) : 0;
        }
        if (l->count >= max_entries || !grow(l, max_entries)) {
            l->truncated = true;
            break;  // the entries read so far are still listed
        }
        fs_entry_t *e = &l->entries[l->count++];
        e->type = type;
        e->size = size;
        core_strlcpy(e->name, de.name, sizeof e->name);
    }
    lfs_closedir(d);
    if (l->truncated)
        CORE_LOGW(TAG, "%s: list truncated after %u entries (%s)", path, (unsigned)l->count,
                  l->count >= max_entries ? "limit" : "out of memory");
    if (l->count > 1) qsort(l->entries, l->count, sizeof *l->entries, entry_cmp);
    while (l->dir_count < l->count && l->entries[l->dir_count].type == FS_ENTRY_DIR) l->dir_count++;
    return l;
}

uint32_t fs_listing_count(const fs_listing_t *l) { return l ? l->count : 0; }

bool fs_listing_truncated(const fs_listing_t *l) { return l && l->truncated; }

const fs_entry_t *fs_listing_get(const fs_listing_t *l, uint32_t index) {
    return (l && index < l->count) ? &l->entries[index] : NULL;
}

static uint32_t entry_rank(const fs_listing_t *l, uint32_t i) {
    return collate_letter_rank(collate_index_letter(l->entries[i].name, NULL));
}

// First index in [lo, hi) whose letter rank is >= rank, or hi.
static uint32_t lower_bound(const fs_listing_t *l, uint32_t lo, uint32_t hi, uint32_t rank) {
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (entry_rank(l, mid) < rank) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

uint32_t fs_listing_find_letter(const fs_listing_t *l, uint32_t letter) {
    if (!l || !l->count) return 0;
    uint32_t rank = collate_letter_rank(letter);
    // Folders and files are two sorted runs: prefer an exact letter match (folders
    // first), otherwise the first row at or after the letter in display order.
    uint32_t d = lower_bound(l, 0, l->dir_count, rank);
    uint32_t f = lower_bound(l, l->dir_count, l->count, rank);
    if (d < l->dir_count && entry_rank(l, d) == rank) return d;
    if (f < l->count && entry_rank(l, f) == rank) return f;
    if (d < l->dir_count) return d;
    return f;
}

uint32_t fs_listing_audio_paths(const fs_listing_t *l, const char *dir, char (*out)[CORE_PATH_MAX], uint32_t max) {
    if (!l || !dir || !out) return 0;
    uint32_t n = 0;
    for (uint32_t i = l->dir_count; i < l->count && n < max; i++) {
        if (l->entries[i].type != FS_ENTRY_AUDIO) continue;
        if (core_path_join(out[n], CORE_PATH_MAX, dir, l->entries[i].name) == CORE_OK) n++;
    }
    return n;
}

void fs_listing_free(fs_listing_t *l) {
    if (!l) return;
    core_free(l->entries);
    core_free(l);
}
