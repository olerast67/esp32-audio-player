// SPDX-License-Identifier: Apache-2.0
// player_cmd_play_folder(): the audio files of one folder, in browser order, as the queue.
#include <string.h>

#include "audio_player/fsbrowse.h"
#include "audio_player/player.h"

int player_cmd_play_folder(player_t *p, const char *dir, uint32_t start, bool shuffle) {
    if (!p || !dir || !dir[0]) return CORE_EINVAL;
    fs_listing_t *l = fs_list_dir(dir, PLAYER_FOLDER_MAX);
    if (!l) return CORE_ENOTFOUND;
    uint32_t total = fs_listing_count(l);
    char(*paths)[CORE_PATH_MAX] = total ? core_malloc((size_t)total * CORE_PATH_MAX) : NULL;
    uint32_t n = paths ? fs_listing_audio_paths(l, dir, paths, total) : 0;
    fs_listing_free(l);
    if (!n) {
        core_free(paths);
        return total && !paths ? CORE_ENOMEM : CORE_ENOTFOUND;
    }
    queue_item_t *items = core_calloc(n, sizeof *items);
    if (!items) {
        core_free(paths);
        return CORE_ENOMEM;
    }
    for (uint32_t i = 0; i < n; i++) {
        items[i].track_id = LIB_ID_NONE;
        core_strlcpy(items[i].path, paths[i], sizeof items[i].path);
    }
    core_free(paths);
    player_cmd_play_items(p, items, n, start < n ? start : 0, shuffle);
    core_free(items);
    return (int)n;
}
