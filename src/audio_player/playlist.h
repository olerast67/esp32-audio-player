// SPDX-License-Identifier: Apache-2.0
/// \file
/// M3U/M3U8 playlists (read: relative/absolute paths, CP1251 or UTF-8 with or
/// without BOM, #EXTINF ignored except title; write: UTF-8 .m3u8 with relative
/// paths, CUE ranges in "#EXT-EMP-RANGE:start,end" lines that are read back) and CUE
/// sheets (FILE/TRACK/INDEX 01, TITLE, PERFORMER, REM GAIN). A playlist whose save was
/// interrupted is read from (and on the next save restored from) "<path>.tmp".
#pragma once

#include "audio_player/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char path[CORE_PATH_MAX];  ///< absolute, resolved against the playlist folder
    char title[128];           ///< from #EXTINF or CUE TITLE, may be empty
    char artist[128];
    uint32_t start_ms;         ///< CUE virtual track start (0 for whole file)
    uint32_t end_ms;           ///< 0 = until end of file
} playlist_entry_t;

typedef struct playlist playlist_t;

playlist_t *playlist_load_m3u(const char *path);
playlist_t *playlist_load_cue(const char *path);  ///< entries are virtual tracks
playlist_t *playlist_new(void);
int playlist_add(playlist_t *p, const playlist_entry_t *e);
uint32_t playlist_count(const playlist_t *p);
const playlist_entry_t *playlist_get(const playlist_t *p, uint32_t index);
int playlist_save_m3u8(const playlist_t *p, const char *path);  ///< atomic (tmp + rename)
void playlist_free(playlist_t *p);

#ifdef __cplusplus
}
#endif
