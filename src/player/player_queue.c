// SPDX-License-Identifier: Apache-2.0
// Play queue: compact storage (16 bytes per entry plus the path pool), shuffle order and
// edits. items[] keeps the insertion order, order[] the play (= display) order, so
// switching shuffle off restores the original sequence. Not thread-safe by itself: the
// player calls it under its lock.
#include <string.h>

#include "player/player_priv.h"

#define TAG PLAYER_TAG
#define PQ_MIN_CAP 64u
#define PQ_POOL_MIN 1024u

// -------------------------------------------------------------------------------- random ----
uint32_t pq_rand(uint32_t *state) {
    uint32_t x = *state ? *state : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static uint32_t rand_below(uint32_t *state, uint32_t bound) {
    return (uint32_t)(((uint64_t)pq_rand(state) * bound) >> 32);
}

// Fisher-Yates over order[1..n-1] with `first` moved to order[0].
void pq_shuffle_order(uint32_t *order, uint32_t n, uint32_t first, uint32_t seed) {
    if (!order || n == 0) return;
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    if (first >= n) first = 0;
    order[first] = 0;
    order[0] = first;
    uint32_t s = seed ? seed : 0x9E3779B9u;
    for (uint32_t i = n - 1; i > 1; i--) {
        uint32_t j = 1 + rand_below(&s, i);  // 1..i
        uint32_t t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
}

// ------------------------------------------------------------------------------- batches ----
static size_t path_len(const char *path) {
    size_t n = 0;
    while (n < CORE_PATH_MAX - 1 && path[n]) n++;
    return n;
}

void pq_batch_free(pq_batch_t *b) {
    if (!b) return;
    pq_prep_free(b->prep);
    core_free(b->items);
    core_free(b->pool);
    core_free(b->order);
    core_free(b);
}

pq_batch_t *pq_batch_new(uint32_t n, uint32_t pool_cap) {
    if (n == 0) n = 1;
    pq_batch_t *b = core_calloc(1, sizeof *b);
    if (!b) return NULL;
    b->items = core_malloc((size_t)n * sizeof *b->items);
    b->order = core_malloc((size_t)n * sizeof *b->order);
    b->pool = core_malloc(pool_cap ? pool_cap : 1);
    if (!b->items || !b->order || !b->pool) {
        pq_batch_free(b);
        return NULL;
    }
    return b;
}

bool pq_batch_push(pq_batch_t *b, uint32_t pool_cap, lib_id_t id, const char *path, uint32_t start_ms,
                   uint32_t end_ms) {
    pq_item_t *it = &b->items[b->count];
    it->track_id = id;
    it->start_ms = start_ms;
    it->end_ms = end_ms > start_ms ? end_ms : 0;
    it->path = PQ_NO_PATH;
    if (path && path[0]) {
        size_t len = path_len(path);
        if (b->pool_len + len + 1 > pool_cap) return false;
        memcpy(b->pool + b->pool_len, path, len);
        b->pool[b->pool_len + len] = 0;
        it->path = b->pool_len;
        b->pool_len += (uint32_t)len + 1;
    } else if (id == LIB_ID_NONE) {
        return true;  // nothing to play: silently ignored
    }
    b->order[b->count] = b->count;
    b->count++;
    return true;
}

pq_batch_t *pq_batch_from_items(const queue_item_t *items, uint32_t n, bool keep_lib_paths) {
    if (!items || n == 0) return NULL;
    if (n > PLAYER_QUEUE_MAX) n = PLAYER_QUEUE_MAX;
    size_t pool = 0;
    for (uint32_t i = 0; i < n; i++) {
        const queue_item_t *it = &items[i];
        if (it->track_id != LIB_ID_NONE && !keep_lib_paths) continue;
        size_t len = path_len(it->path);
        if (len) pool += len + 1;
    }
    if (pool > UINT32_MAX / 2) return NULL;
    pq_batch_t *b = pq_batch_new(n, (uint32_t)pool);
    if (!b) return NULL;
    for (uint32_t i = 0; i < n; i++) {
        const queue_item_t *it = &items[i];
        bool keep = it->track_id == LIB_ID_NONE || keep_lib_paths;
        pq_batch_push(b, (uint32_t)pool, it->track_id, keep ? it->path : NULL, it->start_ms, it->end_ms);
    }
    if (b->count == 0) {
        pq_batch_free(b);
        return NULL;
    }
    return b;
}

pq_batch_t *pq_batch_from_ids(const lib_id_t *ids, uint32_t n) {
    if (!ids || n == 0) return NULL;
    if (n > PLAYER_QUEUE_MAX) n = PLAYER_QUEUE_MAX;
    pq_batch_t *b = pq_batch_new(n, 1);
    if (!b) return NULL;
    for (uint32_t i = 0; i < n; i++) pq_batch_push(b, 1, ids[i], NULL, 0, 0);
    if (b->count == 0) {
        pq_batch_free(b);
        return NULL;
    }
    return b;
}

// --------------------------------------------------------------------------------- queue ----
void pq_init(pq_t *q) { memset(q, 0, sizeof *q); }

void pq_free(pq_t *q) {
    core_free(q->items);
    core_free(q->order);
    core_free(q->pool);
    uint32_t gen = q->gen;
    memset(q, 0, sizeof *q);
    q->gen = gen + 1;
}

void pq_clear(pq_t *q) {
    bool shuffled = q->shuffled;
    uint32_t seed = q->seed;
    pq_free(q);
    q->shuffled = shuffled;
    q->seed = seed;
}

static bool reserve(pq_t *q, uint32_t n) {
    if (n <= q->cap) return true;
    if (n > PLAYER_QUEUE_MAX) return false;
    uint32_t cap = q->cap ? q->cap : PQ_MIN_CAP;
    while (cap < n) cap = cap > PLAYER_QUEUE_MAX / 2 ? PLAYER_QUEUE_MAX : cap * 2;
    pq_item_t *items = core_realloc(q->items, (size_t)cap * sizeof *items);
    if (!items) return false;
    q->items = items;
    uint32_t *order = core_realloc(q->order, (size_t)cap * sizeof *order);
    if (!order) return false;  // items[] is larger than needed, which is harmless
    q->order = order;
    q->cap = cap;
    return true;
}

// Drops the text of removed entries once it makes up most of the pool.
static void pool_compact(pq_t *q) {
    if (q->pool_dead < PQ_POOL_MIN || q->pool_dead < q->pool_len / 2) return;
    uint32_t need = q->pool_len - q->pool_dead;
    char *np = core_malloc(need ? need : 1);
    if (!np) return;  // keep the old pool, try again at the next removal
    uint32_t len = 0;
    for (uint32_t i = 0; i < q->count; i++) {
        pq_item_t *it = &q->items[i];
        if (it->path == PQ_NO_PATH) continue;
        size_t n = strlen(q->pool + it->path) + 1;
        if (len + n > need) {  // cannot happen while pool_dead is exact; stay safe
            core_free(np);
            return;
        }
        memcpy(np + len, q->pool + it->path, n);
        it->path = len;
        len += (uint32_t)n;
    }
    core_free(q->pool);
    q->pool = np;
    q->pool_len = len;
    q->pool_cap = need ? need : 1;
    q->pool_dead = 0;
}

static bool pool_reserve(pq_t *q, uint32_t extra) {
    if ((uint64_t)q->pool_len + extra <= q->pool_cap) return true;
    uint64_t cap = q->pool_cap ? q->pool_cap : PQ_POOL_MIN;
    while (cap < (uint64_t)q->pool_len + extra) cap *= 2;
    if (cap > UINT32_MAX / 2) return false;
    char *np = core_realloc(q->pool, (size_t)cap);
    if (!np) return false;
    q->pool = np;
    q->pool_cap = (uint32_t)cap;
    return true;
}

void pq_replace(pq_t *q, pq_batch_t *b, uint32_t start, bool shuffle, bool keep_order, uint32_t seed) {
    uint32_t gen = q->gen;
    core_free(q->items);
    core_free(q->order);
    core_free(q->pool);
    memset(q, 0, sizeof *q);
    q->gen = gen + 1;
    if (!b) return;
    q->items = b->items;
    q->order = b->order;
    q->count = q->cap = b->count;
    q->pool = b->pool;
    q->pool_len = q->pool_cap = b->pool_len;
    q->lib_gen = b->lib_gen;
    b->items = NULL;
    b->order = NULL;
    b->pool = NULL;
    pq_batch_free(b);
    if (q->count == 0) return;
    if (start >= q->count) start = 0;
    q->seed = seed;
    if (keep_order) {
        q->shuffled = shuffle;
        q->cur = start;
    } else if (shuffle) {
        pq_shuffle_order(q->order, q->count, start, seed);
        q->shuffled = true;
        q->cur = 0;
    } else {
        for (uint32_t i = 0; i < q->count; i++) q->order[i] = i;
        q->shuffled = false;
        q->cur = start;
    }
}

uint32_t pq_insert(pq_t *q, const pq_batch_t *b, bool play_next) {
    if (!b || b->count == 0) return 0;
    uint32_t n = b->count;
    if (q->count + n > PLAYER_QUEUE_MAX) n = PLAYER_QUEUE_MAX - q->count;
    if (n == 0) return 0;
    uint32_t pool_need = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (b->items[i].path != PQ_NO_PATH) pool_need += (uint32_t)strlen(b->pool + b->items[i].path) + 1;
    }
    if (!reserve(q, q->count + n) || !pool_reserve(q, pool_need)) {
        CORE_LOGE(TAG, "queue: out of memory adding %lu entries", (unsigned long)n);
        return 0;
    }
    bool was_empty = q->count == 0;
    if (was_empty) q->lib_gen = b->lib_gen;
    // Where the entries go in items[] (original order) and in order[] (play order).
    uint32_t at_item = q->count, at_pos = q->count;
    if (play_next && !was_empty) {
        at_item = q->order[q->cur] + 1;
        at_pos = q->cur + 1;
    }
    if (at_item < q->count) {
        memmove(&q->items[at_item + n], &q->items[at_item], (size_t)(q->count - at_item) * sizeof *q->items);
        for (uint32_t i = 0; i < q->count; i++) {
            if (q->order[i] >= at_item) q->order[i] += n;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        pq_item_t it = b->items[i];
        if (it.path != PQ_NO_PATH) {
            size_t len = strlen(b->pool + it.path) + 1;
            memcpy(q->pool + q->pool_len, b->pool + it.path, len);
            it.path = q->pool_len;
            q->pool_len += (uint32_t)len;
        }
        q->items[at_item + i] = it;
    }
    if (at_pos < q->count) memmove(&q->order[at_pos + n], &q->order[at_pos], (size_t)(q->count - at_pos) * 4u);
    for (uint32_t i = 0; i < n; i++) q->order[at_pos + i] = at_item + i;
    q->count += n;
    if (was_empty) q->cur = 0;
    q->gen++;
    return n;
}

void pq_remove(pq_t *q, uint32_t pos) {
    if (pos >= q->count) return;
    uint32_t k = q->order[pos];
    pq_item_t *it = &q->items[k];
    if (it->path != PQ_NO_PATH) q->pool_dead += (uint32_t)strlen(q->pool + it->path) + 1;
    memmove(&q->items[k], &q->items[k + 1], (size_t)(q->count - k - 1) * sizeof *q->items);
    memmove(&q->order[pos], &q->order[pos + 1], (size_t)(q->count - pos - 1) * 4u);
    q->count--;
    for (uint32_t i = 0; i < q->count; i++) {
        if (q->order[i] > k) q->order[i]--;
    }
    if (pos < q->cur) q->cur--;
    if (q->count == 0) {
        pq_clear(q);
        return;
    }
    if (q->cur >= q->count) q->cur = q->count - 1;
    q->gen++;
    pool_compact(q);
}

void pq_set_shuffle(pq_t *q, bool on, uint32_t seed) {
    if (q->count == 0) {
        q->shuffled = on;
        if (on) q->seed = seed;
        return;
    }
    uint32_t cur_item = q->order[q->cur];
    if (on) {
        pq_shuffle_order(q->order, q->count, cur_item, seed);
        q->seed = seed;
        q->cur = 0;
    } else {
        for (uint32_t i = 0; i < q->count; i++) q->order[i] = i;
        q->cur = cur_item;
    }
    q->shuffled = on;
    q->gen++;
}

// -------------------------------------------------------------------- prepared edits ----
void pq_prep_free(pq_prep_t *pr) {
    if (!pr) return;
    pq_free(&pr->q);
    core_free(pr);
}

bool pq_prep_apply(pq_t *q, pq_prep_t *pr, pq_t *old) {
    // An unchanged generation means unchanged entries, but the library ids may have been
    // translated since (that keeps gen): the copy then holds the old ids.
    if (!pr || !pr->kind || pr->gen != q->gen || pr->q.lib_gen != q->lib_gen) return false;
    uint32_t cur = q->cur;  // the current entry may have moved on since the copy
    switch ((pq_prep_kind_t)pr->kind) {
    case PQ_PREP_REMOVE:
        if (pr->pos < cur) cur--;
        if (pr->q.count == 0) cur = 0;
        else if (cur >= pr->q.count) cur = pr->q.count - 1;
        break;
    case PQ_PREP_INSERT:
        // Play next went after the entry that was current at the copy.
        if (pr->play_next && q->count && cur != pr->cur) return false;
        if (q->count == 0) cur = 0;
        break;
    case PQ_PREP_SHUFFLE:
        if (pr->q.count != q->count) return false;
        if (q->count) {
            const uint32_t item = q->order[cur];
            if (pr->q.shuffled) {
                // The order starts with the entry current at the copy. If another one plays
                // now, the order must be the one pq_set_shuffle() gives for it: in place.
                if (pr->q.order[0] != item) return false;
                cur = 0;
            } else {
                cur = item;  // original order: position = item index
            }
        }
        break;
    default:
        return false;
    }
    *old = *q;
    *q = pr->q;
    q->cur = cur;
    memset(&pr->q, 0, sizeof pr->q);
    pr->kind = 0;
    return true;
}

const pq_item_t *pq_at(const pq_t *q, uint32_t pos) { return pos < q->count ? &q->items[q->order[pos]] : NULL; }

const char *pq_path(const pq_t *q, const pq_item_t *it) {
    if (!it || it->path == PQ_NO_PATH || it->path >= q->pool_len) return NULL;
    return q->pool + it->path;
}

void pq_get(const pq_t *q, uint32_t pos, queue_item_t *out) {
    const pq_item_t *it = pq_at(q, pos);
    memset(out, 0, sizeof *out);
    out->track_id = LIB_ID_NONE;
    if (!it) return;
    out->track_id = it->track_id;
    out->start_ms = it->start_ms;
    out->end_ms = it->end_ms;
    const char *path = pq_path(q, it);
    if (path) core_strlcpy(out->path, path, sizeof out->path);
}

bool pq_step(const pq_t *q, uint32_t pos, int dir, bool wrap, uint32_t *out) {
    if (q->count == 0) return false;
    if (dir > 0) {
        if (pos + 1 < q->count) {
            *out = pos + 1;
            return true;
        }
        if (!wrap) return false;
        *out = 0;
        return true;
    }
    if (pos > 0 && pos <= q->count) {
        *out = pos - 1;
        return true;
    }
    if (!wrap) return false;
    *out = q->count - 1;
    return true;
}
