// SPDX-License-Identifier: Apache-2.0
/// \file
/// Folder browsing without the index: works right after inserting a card.
/// Lists subdirectories first, then audio files and playlists, sorted with
/// collate_cmp. Hidden entries (starting with '.') and system folders are skipped.
#pragma once

#include "audio_player/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FS_ENTRY_DIR = 0, FS_ENTRY_AUDIO, FS_ENTRY_PLAYLIST, FS_ENTRY_CUE } fs_entry_type_t;

typedef struct {
    fs_entry_type_t type;
    char name[CORE_PATH_MAX];  ///< file name only, UTF-8
    uint32_t size;             ///< bytes (files)
} fs_entry_t;

typedef struct fs_listing fs_listing_t;

/// Read and sort a directory. Returns NULL on error. Limit protects RAM on huge folders.
fs_listing_t *fs_list_dir(const char *path, uint32_t max_entries);
uint32_t fs_listing_count(const fs_listing_t *l);
/// True when the directory has entries the listing left out: more than max_entries, or
/// memory ran out while reading. The listing holds the first entries read, sorted.
bool fs_listing_truncated(const fs_listing_t *l);
const fs_entry_t *fs_listing_get(const fs_listing_t *l, uint32_t index);
uint32_t fs_listing_find_letter(const fs_listing_t *l, uint32_t letter);
/// Paths of all audio files in this directory (not recursive) in display order.
uint32_t fs_listing_audio_paths(const fs_listing_t *l, const char *dir, char (*out)[CORE_PATH_MAX], uint32_t max);
void fs_listing_free(fs_listing_t *l);

#ifdef __cplusplus
}
#endif
