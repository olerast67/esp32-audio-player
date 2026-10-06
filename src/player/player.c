// SPDX-License-Identifier: Apache-2.0
// Player object, thread-safe command ring and queries.
//
// player_cmd_* may be called from any thread: they prepare their data outside the lock
// (item arrays become a compact batch; on a long queue, remove/insert/shuffle are applied to
// a copy of the queue), then append one fixed-size command to a ring under p->mu. Repeated
// volume and seek commands are merged with the previous one, so a fast wheel never fills the
// ring. The audio thread applies the commands at the start of player_run_once(), in order.
#include <math.h>
#include <string.h>
#include <time.h>

#include "player/player_priv.h"

#define TAG PLAYER_TAG

static int64_t default_wall_clock(void) { return (int64_t)time(NULL); }

// ---------------------------------------------------------------------------- lifecycle ----
static void status_reset_track(player_status_t *st) {
    st->path[0] = st->title[0] = st->artist[0] = st->album[0] = 0;
    st->year = st->track_no = 0;
    st->track_id = LIB_ID_NONE;
    st->cover_offset = 0;
    st->cover_size = 0;
    st->position_ms = st->duration_ms = 0;
    st->codec = CODEC_UNKNOWN;
    st->bitrate_kbps = 0;
    memset(&st->src_fmt, 0, sizeof st->src_fmt);
    memset(&st->out_fmt, 0, sizeof st->out_fmt);
    st->bitperfect = false;
    st->dsp_flags = 0;
}

static float finite_or(float v, float fallback) { return isfinite(v) ? v : fallback; }

player_t *player_create(const player_config_t *cfg) {
    player_t *p = core_calloc(1, sizeof *p);
    if (!p) return NULL;
    if (cfg) p->cfg = *cfg;
    if (p->cfg.state_dir) {
        core_strlcpy(p->state_dir, p->cfg.state_dir, sizeof p->state_dir);
        size_t n = strlen(p->state_dir);
        while (n > 1 && p->state_dir[n - 1] == '/') p->state_dir[--n] = 0;
    }
    p->cfg.state_dir = p->state_dir[0] ? p->state_dir : NULL;
    p->mu = core_mutex_create();
    p->file_mu = core_mutex_create();
    p->dsp = dsp_create();
    p->chunk = PLAYER_CHUNK_DEFAULT;
    p->dec_buf = core_malloc(PLAYER_BUF_SAMPLES(p->chunk) * sizeof(int32_t));
    if (!p->mu || !p->file_mu || !p->dsp || !p->dec_buf) {
        CORE_LOGE(TAG, "create: out of memory");
        player_destroy(p);
        return NULL;
    }
    pq_init(&p->q);
    dsp_config_defaults(&p->dsp_user);
    p->dsp_pending = p->dsp_user;
    p->sink = p->cfg.sink;
    p->gapless = true;
    p->repeat = REPEAT_OFF;
    p->state = PLAYER_STOPPED;
    p->vol_step = (p->cfg.volume_step_db > 0.0f && isfinite(p->cfg.volume_step_db)) ? p->cfg.volume_step_db : 1.0f;
    p->vol_max_db = CORE_CLAMP(finite_or(p->cfg.volume_max_db, 0.0f), PLAYER_VOLUME_MIN_DB, 0.0f);
    float v = finite_or(p->cfg.volume_default_db, -20.0f);
    p->vol_db = CORE_CLAMP(v, PLAYER_VOLUME_MIN_DB, p->vol_max_db);
    p->rng = core_now_ms() * 2654435761u ^ (uint32_t)(uintptr_t)p ^ 0xA5A5F00Du;
    if (!p->rng) p->rng = 1;
    p->prep_min = PQ_PREP_MIN;
    p->wall_clock = default_wall_clock;

    player_status_t *st = &p->st;
    memset(st, 0, sizeof *st);
    status_reset_track(st);
    st->state = PLAYER_STOPPED;
    st->volume_db = p->vol_db;
    st->volume_max_db = p->vol_max_db;
    st->repeat = REPEAT_OFF;
    if (p->sink && p->sink->name) core_strlcpy(st->sink_name, p->sink->name, sizeof st->sink_name);
    return p;
}

void player_destroy(player_t *p) {
    if (!p) return;
    // Commands still queued may own item batches and prepared edits.
    for (uint32_t i = 0; i < p->ring_count; i++) {
        player_cmd_free_payload(&p->ring[(p->ring_head + i) % PLAYER_CMD_RING]);
    }
    p->ring_count = 0;
    if (p->dsp) player_engine_close(p);
    player_scrobble_flush(p);
    player_scrobble_free(p);
    player_meta_free(p);
    pq_free(&p->q);
    resampler_destroy(p->rs);
    dsp_destroy(p->dsp);
    core_free(p->dec_buf);
    core_free(p->rs_buf);
    if (p->mu) core_mutex_destroy(p->mu);
    if (p->file_mu) core_mutex_destroy(p->file_mu);
    core_free(p);
}

// ------------------------------------------------------------------------------ commands ----
static int32_t sat_add(int32_t a, int32_t b) {
    int64_t s = (int64_t)a + b;
    return s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : (int32_t)s;
}

void player_cmd_free_payload(pcmd_t *c) {
    if (c->type == PCMD_PLAY || c->type == PCMD_ENQUEUE) pq_batch_free(c->ptr);
    if (c->type == PCMD_REMOVE || c->type == PCMD_SHUFFLE) pq_prep_free(c->ptr);
    if (c->type == PCMD_RESTORE) {
        prestore_t *r = c->ptr;
        if (r) pq_batch_free(r->batch);
        core_free(r);
    }
    c->ptr = NULL;
}

// Appends (or merges) one command. dsp: payload of PCMD_DSP, copied under the lock.
static void push(player_t *p, pcmd_t *c, const dsp_config_t *dsp) {
    if (!p) {
        player_cmd_free_payload(c);
        return;
    }
    bool dropped = false;
    core_mutex_lock(p->mu);
    if (dsp) p->dsp_pending = *dsp;
    pcmd_t *last = p->ring_count ? &p->ring[(p->ring_head + p->ring_count - 1) % PLAYER_CMD_RING] : NULL;
    bool merged = false;
    if (last && last->type == c->type) {
        switch (c->type) {
        case PCMD_SEEK:
            last->u = c->u;
            merged = true;
            break;
        case PCMD_VOLUME:
        case PCMD_VOLUME_LIMIT:
            last->f = c->f;
            merged = true;
            break;
        case PCMD_SEEK_REL:
        case PCMD_VOLUME_STEP:
            last->i = sat_add(last->i, c->i);
            merged = true;
            break;
        case PCMD_DSP:
            merged = true;  // both would apply the same (latest) payload
            break;
        default:
            break;
        }
    }
    if (!merged) {
        if (p->ring_count < PLAYER_CMD_RING) {
            p->ring[(p->ring_head + p->ring_count) % PLAYER_CMD_RING] = *c;
            p->ring_count++;
        } else {
            dropped = true;
        }
    }
    core_mutex_unlock(p->mu);
    if (dropped) {
        CORE_LOGW(TAG, "command ring full, command %u dropped", (unsigned)c->type);
        player_cmd_free_payload(c);
        return;
    }
    if (p->cfg.wake) p->cfg.wake(p->cfg.wake_user);
}

static void push_simple(player_t *p, pcmd_type_t type) {
    pcmd_t c = {.type = (uint8_t)type};
    push(p, &c, NULL);
}

// ------------------------------------------------------------------ prepared edits ----
#define COPY_ITEMS 256u   // entries copied per lock (4 KiB of items)
#define COPY_POOL 4096u   // path bytes copied per lock

static uint32_t next_seed(player_t *p) {
    core_mutex_lock(p->mu);
    uint32_t seed = pq_rand(&p->rng);
    core_mutex_unlock(p->mu);
    return seed;
}

// A copy of the live queue with room for `extra` more entries and `extra_pool` bytes of paths,
// taken in slices so p->mu is never held for long (the audio thread takes it every iteration).
// NULL when the queue is short (the audio thread edits it in place cheaply), changed while it
// was copied, or memory is short: the command then goes without a prepared edit.
static pq_prep_t *prep_begin(player_t *p, uint32_t extra, uint32_t extra_pool) {
    core_mutex_lock(p->mu);
    const uint32_t gen = p->q.gen, count = p->q.count, pool_len = p->q.pool_len, min = p->prep_min;
    const uint32_t lib_gen = p->q.lib_gen;  // a translation of the library ids keeps gen
    core_mutex_unlock(p->mu);
    if (count < min || (uint64_t)pool_len + extra_pool + 1 > UINT32_MAX / 2) return NULL;
    pq_prep_t *pr = core_calloc(1, sizeof *pr);
    if (!pr) return NULL;
    const uint32_t cap = count + extra ? count + extra : 1;
    const uint32_t pool_cap = pool_len + extra_pool + 1;
    pq_t *c = &pr->q;
    c->items = core_malloc((size_t)cap * sizeof *c->items);
    c->order = core_malloc((size_t)cap * sizeof *c->order);
    c->pool = core_malloc(pool_cap);
    bool ok = c->items && c->order && c->pool;
    for (uint32_t i = 0; ok && i < count; i += COPY_ITEMS) {
        const uint32_t n = CORE_MIN(COPY_ITEMS, count - i);
        core_mutex_lock(p->mu);
        ok = p->q.gen == gen && p->q.lib_gen == lib_gen;
        if (ok) {
            memcpy(c->items + i, p->q.items + i, (size_t)n * sizeof *c->items);
            memcpy(c->order + i, p->q.order + i, (size_t)n * sizeof *c->order);
        }
        core_mutex_unlock(p->mu);
    }
    for (uint32_t off = 0; ok && off < pool_len; off += COPY_POOL) {
        const uint32_t n = CORE_MIN(COPY_POOL, pool_len - off);
        core_mutex_lock(p->mu);
        ok = p->q.gen == gen && p->q.lib_gen == lib_gen;
        if (ok) memcpy(c->pool + off, p->q.pool + off, n);
        core_mutex_unlock(p->mu);
    }
    if (ok) {
        // The scalars last: an unchanged generation means unchanged arrays; only the current
        // position moves on its own (pq_prep_apply() handles that).
        core_mutex_lock(p->mu);
        ok = p->q.gen == gen && p->q.lib_gen == lib_gen;
        if (ok) {
            pq_t live = p->q;
            live.items = c->items;
            live.order = c->order;
            live.pool = c->pool;
            live.cap = cap;
            live.pool_cap = pool_cap;
            *c = live;
            pr->gen = gen;
            pr->cur = live.cur;
        }
        core_mutex_unlock(p->mu);
    }
    if (!ok) {
        pq_prep_free(pr);
        return NULL;
    }
    return pr;
}

static pq_prep_t *prep_remove(player_t *p, uint32_t index) {
    pq_prep_t *pr = prep_begin(p, 0, 0);
    if (!pr) return NULL;
    if (index >= pr->q.count) {
        pq_prep_free(pr);
        return NULL;
    }
    pq_remove(&pr->q, index);  // also compacts the path pool when it is due
    pr->pos = index;
    pr->kind = PQ_PREP_REMOVE;
    return pr;
}

static pq_prep_t *prep_insert(player_t *p, const pq_batch_t *b, bool play_next) {
    pq_prep_t *pr = prep_begin(p, b->count, b->pool_len);
    if (!pr) return NULL;
    if (pr->q.count && pr->q.lib_gen != b->lib_gen) {
        pq_prep_free(pr);  // ids of two library generations: the audio thread aligns them
        return NULL;
    }
    pr->added = pq_insert(&pr->q, b, play_next);
    pr->play_next = play_next;
    pr->kind = PQ_PREP_INSERT;
    return pr;
}

static pq_prep_t *prep_shuffle(player_t *p, bool on, uint32_t seed) {
    core_mutex_lock(p->mu);
    const bool change = p->q.shuffled != on;
    core_mutex_unlock(p->mu);
    if (!change) return NULL;
    pq_prep_t *pr = prep_begin(p, 0, 0);
    if (!pr) return NULL;
    pq_set_shuffle(&pr->q, on, seed);
    pr->kind = PQ_PREP_SHUFFLE;
    return pr;
}

lib_id_t player_lib_id(player_t *p, lib_id_t id, uint32_t gen) {
    library_t *lib = p->cfg.library;
    if (!lib || id == LIB_ID_NONE || gen == library_generation(lib)) return id;
    library_translate_ids(lib, &gen, &id, 1);  // unchanged when it is too old
    return id;
}

// The play order of a new queue (a shuffle is O(n) with random access) is set here too.
static void prepare_play(player_t *p, pq_batch_t *b, uint32_t *start, bool shuffle) {
    b->seed = next_seed(p);
    if (!shuffle) return;
    pq_shuffle_order(b->order, b->count, *start, b->seed);
    *start = 0;
}

void player_cmd_play_items(player_t *p, const queue_item_t *items, uint32_t n, uint32_t start, bool shuffle) {
    if (!p || !items || n == 0) return;
    if (n > PLAYER_QUEUE_MAX) n = PLAYER_QUEUE_MAX;
    // Skipped entries (no id, no path) shift the start index: count those before it.
    uint32_t skipped = 0;
    for (uint32_t i = 0; i < n && i < start; i++) {
        if (items[i].track_id == LIB_ID_NONE && items[i].path[0] == 0) skipped++;
    }
    pq_batch_t *b = pq_batch_from_items(items, n, p->cfg.library == NULL);
    if (!b) {
        CORE_LOGW(TAG, "play: no playable entries");
        return;
    }
    uint32_t at = start >= n ? 0 : start - skipped;
    b->lib_gen = library_generation(p->cfg.library);
    prepare_play(p, b, &at, shuffle);
    pcmd_t c = {.type = PCMD_PLAY, .u = at, .flag = shuffle, .ptr = b};
    push(p, &c, NULL);
}

void player_cmd_play_tracks(player_t *p, const lib_id_t *ids, uint32_t n, uint32_t start, bool shuffle) {
    if (!p || !ids || n == 0) return;
    if (n > PLAYER_QUEUE_MAX) n = PLAYER_QUEUE_MAX;
    pq_batch_t *b = pq_batch_from_ids(ids, n);
    if (!b) return;
    uint32_t skipped = 0;
    for (uint32_t i = 0; i < n && i < start; i++) skipped += ids[i] == LIB_ID_NONE;
    uint32_t at = start >= n ? 0 : start - skipped;
    b->lib_gen = library_generation(p->cfg.library);
    prepare_play(p, b, &at, shuffle);
    pcmd_t c = {.type = PCMD_PLAY, .u = at, .flag = shuffle, .ptr = b};
    push(p, &c, NULL);
}

void player_cmd_enqueue(player_t *p, const queue_item_t *items, uint32_t n, bool play_next) {
    if (!p || !items || n == 0) return;
    pq_batch_t *b = pq_batch_from_items(items, n, p->cfg.library == NULL);
    if (!b) return;
    b->lib_gen = library_generation(p->cfg.library);
    b->prep = prep_insert(p, b, play_next);
    pcmd_t c = {.type = PCMD_ENQUEUE, .flag = play_next, .ptr = b};
    push(p, &c, NULL);
}

void player_cmd_queue_remove(player_t *p, uint32_t index) {
    pcmd_t c = {.type = PCMD_REMOVE, .u = index, .ptr = p ? prep_remove(p, index) : NULL};
    push(p, &c, NULL);
}

void player_cmd_queue_jump(player_t *p, uint32_t index) {
    pcmd_t c = {.type = PCMD_JUMP, .u = index};
    push(p, &c, NULL);
}

void player_cmd_queue_clear(player_t *p) { push_simple(p, PCMD_CLEAR); }
void player_cmd_toggle_pause(player_t *p) { push_simple(p, PCMD_TOGGLE); }
void player_cmd_pause(player_t *p) { push_simple(p, PCMD_PAUSE); }
void player_cmd_resume(player_t *p) { push_simple(p, PCMD_RESUME); }
void player_cmd_stop(player_t *p) { push_simple(p, PCMD_STOP); }
void player_cmd_next(player_t *p) { push_simple(p, PCMD_NEXT); }
void player_cmd_prev(player_t *p) { push_simple(p, PCMD_PREV); }

void player_cmd_seek_ms(player_t *p, uint32_t ms) {
    pcmd_t c = {.type = PCMD_SEEK, .u = ms};
    push(p, &c, NULL);
}

void player_cmd_seek_relative_ms(player_t *p, int32_t delta_ms) {
    if (delta_ms == 0) return;
    pcmd_t c = {.type = PCMD_SEEK_REL, .i = delta_ms};
    push(p, &c, NULL);
}

void player_cmd_set_volume_db(player_t *p, float db) {
    if (isnan(db)) return;
    pcmd_t c = {.type = PCMD_VOLUME, .f = db};
    push(p, &c, NULL);
}

void player_cmd_volume_step(player_t *p, int steps) {
    if (steps == 0) return;
    pcmd_t c = {.type = PCMD_VOLUME_STEP, .i = steps};
    push(p, &c, NULL);
}

void player_cmd_set_shuffle(player_t *p, bool on) {
    if (!p) return;
    const uint32_t seed = next_seed(p);
    pcmd_t c = {.type = PCMD_SHUFFLE, .flag = on, .u = seed, .ptr = prep_shuffle(p, on, seed)};
    push(p, &c, NULL);
}

void player_cmd_set_repeat(player_t *p, repeat_mode_t mode) {
    if ((unsigned)mode > (unsigned)REPEAT_ONE) return;
    pcmd_t c = {.type = PCMD_REPEAT, .u = (uint32_t)mode};
    push(p, &c, NULL);
}

void player_cmd_set_dsp(player_t *p, const dsp_config_t *cfg) {
    if (!p || !cfg) return;
    pcmd_t c = {.type = PCMD_DSP};
    push(p, &c, cfg);
}

void player_cmd_set_gapless(player_t *p, bool on) {
    pcmd_t c = {.type = PCMD_GAPLESS, .flag = on};
    push(p, &c, NULL);
}

void player_cmd_set_sleep_timer(player_t *p, uint32_t minutes) {
    pcmd_t c = {.type = PCMD_SLEEP, .u = minutes > 24u * 60u ? 24u * 60u : minutes};
    push(p, &c, NULL);
}

void player_cmd_set_volume_limit(player_t *p, float max_db) {
    if (isnan(max_db)) return;
    pcmd_t c = {.type = PCMD_VOLUME_LIMIT, .f = max_db};
    push(p, &c, NULL);
}

void player_cmd_set_sink(player_t *p, audio_sink_t *sink) {
    pcmd_t c = {.type = PCMD_SINK, .ptr = sink};
    push(p, &c, NULL);
}

// Used by player_restore_state() (player_state.c).
void player_push_restore(player_t *p, prestore_t *r);
void player_push_restore(player_t *p, prestore_t *r) {
    pcmd_t c = {.type = PCMD_RESTORE, .ptr = r};
    push(p, &c, NULL);
}

// ------------------------------------------------------------------------------- queries ----
void player_get_status(player_t *p, player_status_t *out) {
    if (!out) return;
    if (!p) {
        memset(out, 0, sizeof *out);
        out->track_id = LIB_ID_NONE;
        return;
    }
    core_mutex_lock(p->mu);
    *out = p->st;
    core_mutex_unlock(p->mu);
}

uint32_t player_queue_count(player_t *p) {
    if (!p) return 0;
    core_mutex_lock(p->mu);
    uint32_t n = p->q.count;
    core_mutex_unlock(p->mu);
    return n;
}

int player_queue_get(player_t *p, uint32_t index, queue_item_t *out) {
    if (!p || !out) return CORE_EINVAL;
    core_mutex_lock(p->mu);
    bool ok = index < p->q.count;
    if (ok) pq_get(&p->q, index, out);
    const uint32_t lib_gen = p->q.lib_gen;
    core_mutex_unlock(p->mu);
    if (ok) out->track_id = player_lib_id(p, out->track_id, lib_gen);
    if (!ok) {
        memset(out, 0, sizeof *out);
        out->track_id = LIB_ID_NONE;
        return CORE_ENOTFOUND;
    }
    // Library entries carry no path in the queue: look it up (outside the player lock).
    if (!out->path[0] && out->track_id != LIB_ID_NONE && p->cfg.library) {
        lib_track_t *t = core_malloc(sizeof *t);
        if (t && library_get_track(p->cfg.library, out->track_id, t) == CORE_OK) {
            core_strlcpy(out->path, t->path, sizeof out->path);
        }
        core_free(t);
    }
    return CORE_OK;
}

void player_status_lock_update_queue(player_t *p) {
    core_mutex_lock(p->mu);
    p->st.queue_length = p->q.count;
    p->st.queue_index = p->q.count ? p->q.cur : 0;
    p->st.shuffle = p->q.shuffled;
    core_mutex_unlock(p->mu);
}

void player_post_state(player_t *p) { core_event_post_simple(EV_PLAYER_STATE, (int32_t)p->state, 0); }

void player_post_volume(player_t *p) {
    core_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = EV_VOLUME;
    ev.f = p->vol_db;
    core_event_post(&ev);
}

// ------------------------------------------------------------------------------ settings ----
void player_set_chunk_frames(player_t *p, uint32_t frames) {
    if (!p) return;
    if (frames == 0) frames = PLAYER_CHUNK_DEFAULT;
    frames = CORE_CLAMP(frames, PLAYER_CHUNK_MIN, PLAYER_CHUNK_MAX);
    core_mutex_lock(p->mu);
    p->chunk_req = frames;
    core_mutex_unlock(p->mu);
}

void player_test_set_seed(player_t *p, uint32_t seed) {
    if (!p) return;
    core_mutex_lock(p->mu);
    p->rng = seed ? seed : 1;
    core_mutex_unlock(p->mu);
}

void player_test_set_prep_min(player_t *p, uint32_t n) {
    if (!p) return;
    core_mutex_lock(p->mu);
    p->prep_min = n;
    core_mutex_unlock(p->mu);
}

void player_test_set_wall_clock(player_t *p, int64_t (*fn)(void)) {
    if (p) p->wall_clock = fn ? fn : default_wall_clock;
}

int64_t player_wall_now(player_t *p) { return p->wall_clock ? p->wall_clock() : (int64_t)time(NULL); }
