// SPDX-License-Identifier: Apache-2.0
// Private declarations of the player module (not a public contract).
//
// Files:
//   player.c         create/destroy, command ring, queries, status snapshot
//   player_queue.c   compact queue storage, shuffle order, queue edits
//   player_engine.c  player_run_once(): commands, decoding, gapless, output
//   player_meta.c    item -> path + metadata (library, tags, CUE titles)
//   player_state.c   resume state (queue.m3u8, player.ini) and the scrobbler log
//
// Library ids: track ids belong to one snapshot of the library index (a rescan renumbers
// them). The queue records the generation of its ids (pq_t.lib_gen); after a rescan the
// audio thread translates them into a copy of items[] in slices and swaps it in, and every
// reader that resolves an id outside the loop translates it first (player_lib_id()).
//
// Locking: p->mu protects the command ring, the queue and the status snapshot. Only
// the audio thread (player_run_once) changes the queue, always under p->mu and only
// for short edits. Edits that are O(n) on a long queue (remove, insert, shuffle) are
// prepared by the commanding thread on a private copy (pq_prep_t, copied in slices) and
// swapped in by the audio thread in O(1); only when the queue changed in between does the
// audio thread edit in place. Decoding, file I/O and sink writes run without the lock.
// Events are never posted while p->mu is held (the event sink may call back into the player).
#pragma once

#include <stdio.h>

#include "audio_player/events.h"
#include "audio_player/player.h"
#include "audio_player/playlist.h"
#include "audio_player/resampler.h"
#include "audio_player/stream.h"
#include "audio_player/tags.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYER_TAG "player"

#define PLAYER_CMD_RING 64             // pending commands (repeated volume/seek are merged)
#define PLAYER_CHUNK_DEFAULT 1024u
#define PLAYER_CHUNK_MIN 64u
#define PLAYER_CHUNK_MAX 8192u
#define PLAYER_WRITE_TIMEOUT_MS 20u    // longest sink write; bounds the command latency
#define PLAYER_PREFETCH_MS 2000u       // open the next decoder when less than this is left
#define PLAYER_PREV_RESTART_MS 3000u   // "previous" restarts the track after this much
#define PLAYER_MAX_FAILURES 10u        // consecutive unplayable items before stopping
#define PLAYER_POSITION_EVENT_MS 250u  // EV_POSITION about 4 times per second
#define PLAYER_FADE_MS 30000u          // sleep timer fade-out length
#define PLAYER_FADE_FLOOR_DB (-60.0f)  // level reached at the end of the fade
#define PLAYER_TAIL_FRAMES 512u        // zeros pushed through resampler/DSP to flush their delay
#define PLAYER_REMAP_SLICE 512u        // queue entries whose library ids are translated per iteration
// Samples in dec_buf / rs_buf for n frames: the DSP may append its delay line (DSP_DRAIN_MAX).
#define PLAYER_BUF_SAMPLES(n) (((size_t)(n) + DSP_DRAIN_MAX) * AUDIO_MAX_CHANNELS)
#define PLAYER_SCROBBLE_MAX 200u       // entries kept in RAM while the clock is not set
#define PLAYER_SCROBBLE_LINE 448u
#define PLAYER_CLOCK_VALID 1600000000  // unix time: the RTC is set (2020-09-13)

#include "audio_player/version.h"
#ifndef PLAYER_VERSION_STRING
#define PLAYER_VERSION_STRING AUDIO_PLAYER_VERSION_STRING  // the scrobble log names this version
#endif

// ---------------------------------------------------------------------------------- queue ----
#define PQ_NO_PATH 0xFFFFFFFFu

// One queue entry, 16 bytes. Library tracks keep only their id (the path comes from the
// index); other entries keep their path in the string pool.
typedef struct {
    lib_id_t track_id;
    uint32_t path;          // pool offset, PQ_NO_PATH when resolved through the library
    uint32_t start_ms, end_ms;
} pq_item_t;

typedef struct pq_prep pq_prep_t;

// Items prepared by a command caller outside the lock. The queue takes the arrays over
// (replace) or copies them (insert).
typedef struct {
    pq_item_t *items;
    uint32_t count;
    char *pool;
    uint32_t pool_len;
    uint32_t *order;        // play order (a permutation of 0..count-1), always allocated
    uint32_t seed;          // PCMD_PLAY: seed of the shuffle already applied to order
    uint32_t lib_gen;       // library generation of the track ids
    pq_prep_t *prep;        // PCMD_ENQUEUE: the insert prepared on a copy of the queue, or NULL
} pq_batch_t;

typedef struct {
    pq_item_t *items;       // insertion ("original") order
    uint32_t *order;        // play = display order: order[pos] is an index into items
    uint32_t count, cap;
    char *pool;
    uint32_t pool_len, pool_cap, pool_dead;
    uint32_t cur;           // current play position (0 when empty)
    bool shuffled;
    uint32_t seed;          // seed of the current shuffle order
    uint32_t gen;           // changes on every edit of items or order
    uint32_t lib_gen;       // library generation of the track ids in items[]
} pq_t;

// Builds a batch. keep_lib_paths = false drops the paths of library items (the library
// resolves them). Items without id and path are skipped. Returns NULL when nothing is left
// or on allocation failure.
pq_batch_t *pq_batch_from_items(const queue_item_t *items, uint32_t n, bool keep_lib_paths);
pq_batch_t *pq_batch_from_ids(const lib_id_t *ids, uint32_t n);
// Empty batch with room for n items and pool_cap bytes of paths (restore).
pq_batch_t *pq_batch_new(uint32_t n, uint32_t pool_cap);
// Appends one entry to a batch made by pq_batch_new(). Returns false when it is full.
bool pq_batch_push(pq_batch_t *b, uint32_t pool_cap, lib_id_t id, const char *path, uint32_t start_ms,
                   uint32_t end_ms);
void pq_batch_free(pq_batch_t *b);

void pq_init(pq_t *q);
void pq_free(pq_t *q);
// Replace the queue with the batch (takes its arrays; the batch struct is freed).
// shuffle: new random order with items[start] first. keep_order: use b->order as is.
void pq_replace(pq_t *q, pq_batch_t *b, uint32_t start, bool shuffle, bool keep_order, uint32_t seed);
// Insert after the current item (play_next) or append. Returns the number of entries
// added (fewer than requested when the queue is full or memory runs out). The batch
// stays owned by the caller; its ids must be of the queue's library generation (or the
// queue empty).
uint32_t pq_insert(pq_t *q, const pq_batch_t *b, bool play_next);
void pq_remove(pq_t *q, uint32_t pos);
void pq_clear(pq_t *q);
// Shuffle on: current item first, the rest in a new random order. Off: original order.
void pq_set_shuffle(pq_t *q, bool on, uint32_t seed);

// A queue edit prepared by the commanding thread: the same edit applied to a copy of the
// queue. Worth it from PQ_PREP_MIN entries (below, the in-place edit is cheap anyway).
#define PQ_PREP_MIN 256u
typedef enum { PQ_PREP_REMOVE = 1, PQ_PREP_INSERT, PQ_PREP_SHUFFLE } pq_prep_kind_t;
struct pq_prep {
    pq_t q;                 // the edited copy
    uint32_t gen;           // generation of the live queue it was copied from
    uint32_t cur;           // live q.cur at the copy
    uint32_t pos;           // PQ_PREP_REMOVE: the removed position
    uint32_t added;         // PQ_PREP_INSERT: entries added
    bool play_next;         // PQ_PREP_INSERT: inserted after the current entry
    uint8_t kind;
};
void pq_prep_free(pq_prep_t *pr);
// Swaps the prepared copy in (the caller holds the player lock): O(1) plus the adjustment for
// a current entry that moved meanwhile. Returns false when the live queue changed since the
// copy (the caller then edits in place). *old receives the replaced queue: pq_free() it
// after unlocking.
bool pq_prep_apply(pq_t *q, pq_prep_t *pr, pq_t *old);
const pq_item_t *pq_at(const pq_t *q, uint32_t pos);
const char *pq_path(const pq_t *q, const pq_item_t *it);  // NULL if none
void pq_get(const pq_t *q, uint32_t pos, queue_item_t *out);
// Position after pos. wrap: continue at the start after the end. Returns false at the end.
bool pq_step(const pq_t *q, uint32_t pos, int dir, bool wrap, uint32_t *out);
uint32_t pq_rand(uint32_t *state);                // xorshift32, state never 0
void pq_shuffle_order(uint32_t *order, uint32_t n, uint32_t first, uint32_t seed);

// ------------------------------------------------------------------------------- commands ----
typedef enum {
    PCMD_NONE = 0,
    PCMD_PLAY,            // ptr = batch, u = start, flag = shuffle
    PCMD_ENQUEUE,         // ptr = batch, flag = play_next
    PCMD_REMOVE,          // u = index, ptr = pq_prep_t or NULL
    PCMD_JUMP,            // u = index
    PCMD_CLEAR,
    PCMD_TOGGLE,
    PCMD_PAUSE,
    PCMD_RESUME,
    PCMD_STOP,
    PCMD_NEXT,
    PCMD_PREV,
    PCMD_SEEK,            // u = ms
    PCMD_SEEK_REL,        // i = delta ms
    PCMD_VOLUME,          // f = dB
    PCMD_VOLUME_STEP,     // i = steps
    PCMD_SHUFFLE,         // flag, u = seed, ptr = pq_prep_t or NULL
    PCMD_REPEAT,          // u = mode
    PCMD_DSP,             // payload in p->dsp_pending
    PCMD_GAPLESS,         // flag
    PCMD_SLEEP,           // u = minutes
    PCMD_VOLUME_LIMIT,    // f = dB
    PCMD_SINK,            // ptr = sink
    PCMD_RESTORE,         // ptr = restore data
} pcmd_type_t;

typedef struct {
    uint8_t type;
    bool flag;
    uint32_t u;
    int32_t i;
    float f;
    void *ptr;
} pcmd_t;

// Data of player_restore_state() for the audio thread.
typedef struct {
    pq_batch_t *batch;      // may be NULL (no queue saved)
    uint32_t index;         // play position
    uint32_t position_ms;
    float volume_db;
    bool has_volume;
    bool shuffle;
    uint32_t seed;
    repeat_mode_t repeat;
    bool autoplay;
} prestore_t;

// ------------------------------------------------------------------------------- engine ----
typedef struct {
    char path[CORE_PATH_MAX];
    char title[TAG_TEXT_MAX];
    char artist[TAG_TEXT_MAX];
    char album[TAG_TEXT_MAX];
    uint16_t year, track_no;
    lib_id_t track_id, album_id;
    uint32_t lib_gen;       // library generation of track_id and album_id
    uint64_t cover_offset;
    uint32_t cover_size;
    uint32_t duration_ms;   // of the queue entry (CUE range or whole file), 0 if unknown
    float rg_track_db, rg_track_peak, rg_album_db, rg_album_peak;
} pmeta_t;

typedef struct {
    bool open;
    decoder_t *dec;         // NULL for a "shared" next slot (same file, CUE)
    decoder_info_t info;
    pmeta_t meta;
    uint32_t start_ms, end_ms;       // queue entry range
    uint64_t start_frame, end_frame; // end_frame 0 = until the end of the file
    uint64_t pos;                    // decoder position, absolute frames in the file
    uint64_t skip;                   // frames still to discard (non-seekable start)
    uint64_t frames_out;             // frames decoded for this entry
    uint64_t played_out;             // sink frames written for this entry (scrobbling)
    int64_t start_unix;              // wall clock at the first written frame (0 unknown)
    uint32_t start_mono_ms;          // core_now_ms() at the first written frame
    bool started;                    // first frame written
    // next slot only
    bool shared;                     // continues the decoder of the current entry
    uint32_t qpos, qgen;             // queue position and generation it was prepared for
    bool album_ctx;
} ptrack_t;

typedef enum { DRAIN_NONE = 0, DRAIN_REOPEN, DRAIN_STOP } pdrain_t;

typedef struct {
    char line[PLAYER_SCROBBLE_LINE];  // "artist\talbum\ttitle\ttrack\tlength\trating\t"
    int64_t unix_ts;                  // 0: compute from mono_ms once the clock is valid
    uint32_t mono_ms;
} pscrobble_t;

struct player {
    player_config_t cfg;
    char state_dir[CORE_PATH_MAX];
    core_mutex_t *mu;
    core_mutex_t *file_mu;           // serialises player_save_state / player_restore_state
    // Last queue.m3u8 written by this player (file_mu): an unchanged queue is not rewritten.
    bool saved_q_valid;              // the file holds every entry of queue generation saved_q_gen
    uint32_t saved_q_gen, saved_q_count;

    // ---- shared (p->mu) ----
    pcmd_t ring[PLAYER_CMD_RING];
    uint32_t ring_head, ring_count;
    dsp_config_t dsp_pending;
    uint32_t chunk_req;
    pq_t q;
    player_status_t st;
    uint32_t rng;                    // shuffle seeds
    uint32_t prep_min;               // queues from this length get prepared edits (PQ_PREP_MIN)

    // ---- audio thread ----
    // Translation of the queue's library ids after a rescan: items[] is copied and
    // translated PLAYER_REMAP_SLICE entries per loop iteration, then swapped in.
    struct {
        pq_item_t *items;            // the translated copy (NULL: no translation running)
        lib_id_t *ids;               // scratch for one slice
        uint32_t at, count, qgen, from, to;
    } remap;
    pcmd_t work[PLAYER_CMD_RING];
    player_state_t state;
    audio_sink_t *sink;
    bool sink_open;
    audio_format_t sink_fmt;
    bool out_ready;                  // processing configured for cur and the sink is open
    bool sink_paused;                // we called sink->pause(true) and not yet pause(false)
    bool resume_on_sink;             // PLAYING was interrupted by player_cmd_set_sink(NULL)
    ptrack_t cur, next;
    bool next_tried;                 // prefetch attempted for (next.qpos, next.qgen)
    uint32_t next_tried_gen, next_tried_pos;
    uint32_t pending_start_ms;       // start position for the next open of the current entry
    bool pending_start_valid;

    audio_format_t src_fmt;          // decoded format of cur
    bool upmix, dop;
    uint8_t dop_last_marker;         // DoP marker of the last frame produced (0 after a flush)
    resampler_t *rs;
    uint32_t rs_in_rate, rs_out_rate;
    uint8_t rs_ch;
    dsp_t *dsp;
    dsp_config_t dsp_user;
    uint32_t chunk;
    int32_t *dec_buf;                // PLAYER_BUF_SAMPLES(chunk)
    int32_t *rs_buf;                 // PLAYER_BUF_SAMPLES(rs_cap)
    uint32_t rs_cap;                 // frames the resampler may write
    const int32_t *pend;             // output not yet accepted by the sink
    uint32_t pend_frames;

    // position accounting (sink frames)
    uint64_t w;                      // frames accepted since the sink was opened or flushed
    uint64_t seg_w0;                 // w at the first frame of cur
    uint64_t seg_base;               // track frame (relative to the entry start) at seg_w0
    bool sw_pending;                 // previous entry still audible: status not switched yet
    uint32_t prev_qpos, prev_qgen;   // queue position (and generation) of that previous entry
    uint64_t prev_w0, prev_base;
    uint32_t prev_rate_in;           // source rate of the previous entry
    uint32_t prev_duration_ms;
    uint32_t last_pos_ms, last_pos_event_ms;
    uint32_t last_level;
    bool level_seen;                 // the sink reported a non-zero buffer level at least once

    pdrain_t drain;
    uint32_t drain_start_ms, drain_change_ms, drain_level;
    bool reopen_ready;               // drained: output_setup() may reopen the sink now

    // volume
    float vol_db, vol_max_db, vol_step;
    float fade_db;
    bool hw_vol;
    float hw_applied_db;
    uint32_t hw_applied_ms;
    bool hw_applied_valid;

    // timers and counters
    bool gapless;
    repeat_mode_t repeat;
    uint32_t sleep_deadline_ms;      // core_now_ms() value; 0 = off
    bool sleep_on;
    uint32_t sleep_last_posted;
    uint32_t fail_count;
    uint32_t sink_errors;
    uint32_t underruns;
    bool shown_bypass;               // dsp_is_bypass() when the status was last updated
    bool busy;                       // last iteration did work without writing
    bool blocked;                    // last write was refused (sink full)

    // metadata scratch (audio thread; too big for the task stack)
    lib_track_t lt;
    track_tags_t tags;
    playlist_t *cue_pl;              // CUE sheet cached for cue_for
    char cue_for[CORE_PATH_MAX];     // audio file the cache was looked up for
    bool cue_checked;

    // scrobbler
    pscrobble_t *scrobbles;          // PLAYER_SCROBBLE_MAX entries, allocated on first use
    uint32_t scrobble_count;
    uint32_t scrobble_check_ms;
    int64_t (*wall_clock)(void);
};

// player.c
void player_status_lock_update_queue(player_t *p);  // queue_index/length in st (takes p->mu)
// A library track id of generation gen, in the current library snapshot (unchanged when it
// cannot be translated). Takes the library lock, not p->mu.
lib_id_t player_lib_id(player_t *p, lib_id_t id, uint32_t gen);
void player_cmd_free_payload(pcmd_t *c);           // batches, prepared edits, restore data
void player_post_state(player_t *p);
void player_post_volume(player_t *p);

// player_meta.c
// qi->track_id must be of the current library generation (queue_copy() translates it).
int player_meta_open(player_t *p, ptrack_t *t, const pq_item_t *qi, const char *qpath, uint32_t qpos);
void player_meta_cue_title(player_t *p, ptrack_t *t);
bool player_meta_album_ctx(player_t *p, uint32_t qpos, const pmeta_t *m);
void player_meta_title_fallback(pmeta_t *m);
void player_meta_free(player_t *p);

// player_state.c
void player_scrobble_track(player_t *p, ptrack_t *t, bool completed);
void player_scrobble_poll(player_t *p);      // flush deferred entries once the clock is valid
void player_scrobble_flush(player_t *p);
void player_scrobble_free(player_t *p);
int64_t player_wall_now(player_t *p);
void player_apply_restore(player_t *p, prestore_t *r);  // engine side, in player_engine.c

// player_engine.c
void player_engine_apply(player_t *p, const pcmd_t *c);
void player_engine_close(player_t *p);
int player_engine_set_chunk(player_t *p, uint32_t frames);

// ---------------------------------------------------------------- test hooks ----
// Seed of the next shuffle (deterministic tests). Takes effect for later shuffles.
void player_test_set_seed(player_t *p, uint32_t seed);
// Queue length from which edits are prepared outside the audio thread (UINT32_MAX: never).
void player_test_set_prep_min(player_t *p, uint32_t n);
// Wall clock used for scrobble timestamps (default: time(NULL)).
void player_test_set_wall_clock(player_t *p, int64_t (*fn)(void));

#ifdef __cplusplus
}
#endif
