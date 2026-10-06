// SPDX-License-Identifier: Apache-2.0
// The audio thread side of the player: commands, decoding, processing, output.
//
// One player_run_once() call:
//   1. applies all queued commands;
//   2. advances the sleep timer (fade-out over the last 30 s, then pause);
//   3. while PLAYING: writes pending output first (a partial sink write is never dropped),
//      otherwise decodes one chunk of the current entry, upmixes mono, resamples when the
//      sink rate differs, runs the DSP at the sink rate and writes to the sink;
//   4. updates the status snapshot and posts EV_POSITION about 4 times per second.
//
// Track changes. When less than ~2 s of the current entry is left, the next entry is
// opened ahead (its decoder, tags and CUE title). At the end of the entry the prepared one
// takes over; if the sink format stays the same the frames simply continue (gapless: no
// flush, DSP and resampler state carry over). Otherwise the resampler/DSP tail is pushed
// out, the sink drains, and it is reopened in the new format. Consecutive CUE tracks of
// one file continue on the same decoder. The status follows what is audible: it switches
// once the previous entry's tail has left the sink buffer.
#include <math.h>
#include <string.h>

#include "player/player_priv.h"

#define TAG PLAYER_TAG

#define DRAIN_STUCK_MS 500u      // sink level unchanged this long: stop waiting for it
#define SINK_ERROR_LIMIT 50u     // consecutive failed writes before pausing
#define HW_FADE_STEP_DB 0.5f     // hardware volume steps during the sleep fade
#define HW_FADE_MIN_MS 100u

static void status_track(player_t *p);
static void status_output(player_t *p);

// ------------------------------------------------------------------------------ helpers ----
static void post_error(int err, const char *path) {
    core_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = EV_PLAYBACK_ERROR;
    ev.a = err;
    const char *base = path ? core_path_basename(path) : "";
    size_t n = strlen(base);
    if (n >= sizeof ev.text) {
        n = sizeof ev.text - 1;
        while (n > 0 && ((unsigned char)base[n] & 0xC0) == 0x80) n--;  // whole UTF-8 characters
    }
    memcpy(ev.text, base, n);
    ev.text[n] = 0;
    core_event_post(&ev);
}

static void post_position(player_t *p, uint32_t ms) {
    p->last_pos_event_ms = ms;
    core_event_post_simple(EV_POSITION, (int32_t)ms, 0);
}

static void post_queue(player_t *p) {
    player_status_lock_update_queue(p);
    core_event_post_simple(EV_QUEUE_CHANGED, (int32_t)p->q.count, 0);
}

static void set_state(player_t *p, player_state_t s) {
    if (p->state == s) return;
    p->state = s;
    core_mutex_lock(p->mu);
    p->st.state = s;
    core_mutex_unlock(p->mu);
    player_post_state(p);
}

static uint64_t entry_len(const ptrack_t *t) {
    uint64_t end = t->end_frame ? t->end_frame : t->info.total_frames;
    return end > t->start_frame ? end - t->start_frame : 0;
}

// Position relative to the entry start, counting frames still to be skipped as done.
static uint64_t entry_rel(const ptrack_t *t) {
    uint64_t eff = t->pos + t->skip;
    return eff > t->start_frame ? eff - t->start_frame : 0;
}

static void track_close(ptrack_t *t) {
    if (t->dec) decoder_close(t->dec);
    t->dec = NULL;
    t->open = false;
    t->shared = false;
}

static void next_close(player_t *p) {
    track_close(&p->next);
    p->next_tried = false;
}

static bool sink_has(const player_t *p) { return p->sink != NULL; }

static uint32_t sink_level(player_t *p) {
    if (!p->sink || !p->sink_open || !p->sink->buffered_frames) return 0;
    return p->sink->buffered_frames(p->sink);
}

// Frames the listener has heard since the sink was opened or flushed.
static uint64_t played_frames(player_t *p) {
    uint64_t lvl = sink_level(p);
    return p->w > lvl ? p->w - lvl : 0;
}

static uint64_t seg_frame(uint64_t base, uint64_t w0, uint64_t played, uint32_t in_rate, uint32_t out_rate) {
    uint64_t d = played > w0 ? played - w0 : 0;
    if (in_rate && out_rate && in_rate != out_rate) d = d * in_rate / out_rate;
    return base + d;
}

// Audible frame of the current entry, relative to its start (source rate).
static uint64_t audible_rel(player_t *p) {
    if (!p->cur.open) return 0;
    if (!p->sink_open) return entry_rel(&p->cur);
    uint64_t played = played_frames(p);
    if (p->sw_pending && played < p->seg_w0) return p->seg_base;
    uint64_t f = seg_frame(p->seg_base, p->seg_w0, played, p->cur.info.fmt.sample_rate, p->sink_fmt.sample_rate);
    uint64_t dec = entry_rel(&p->cur);
    return f < dec ? f : dec;  // never ahead of the decoder (bad buffered_frames())
}

static uint32_t audible_ms(player_t *p) {
    if (!p->cur.open) return p->pending_start_valid ? p->pending_start_ms : 0;
    uint32_t ms = audio_frames_to_ms(audible_rel(p), p->cur.info.fmt.sample_rate);
    uint32_t dur = p->cur.meta.duration_ms;
    return (dur && ms > dur) ? dur : ms;
}

// The sink lost everything queued (open, flush): positions restart from the decoder.
static void rebase(player_t *p) {
    p->w = 0;
    p->seg_w0 = 0;
    p->seg_base = p->cur.open ? entry_rel(&p->cur) : 0;
    p->last_level = 0;
    p->dop_last_marker = 0;  // nothing queued: the next DoP frame may take either marker
}

// ------------------------------------------------------------------------------- volume ----
// Resolution the DSP rounds to (with TPDF dither below 32 bits). The negotiated bits only
// describe the format: a sink that dithers itself (Bluetooth) gets Q31 without a second
// dither, and a bit-perfect sink carries the whole word to the DAC (32-bit I2S slots), so a
// 16-bit source is not re-quantised to 16 bits there when the DSP is active.
static uint8_t dsp_out_bits(const player_t *p) {
    uint32_t caps = p->sink ? p->sink->caps : 0;
    if (caps & SINK_CAP_DITHERS) return 32;
    uint8_t bits = p->sink_fmt.bits ? p->sink_fmt.bits : 32;
    if ((caps & SINK_CAP_BITPERFECT) && bits < 24) bits = 24;
    return bits;
}

static void dsp_apply(player_t *p) {
    if (!p->sink_open || p->dop) return;
    dsp_config_t c = p->dsp_user;
    c.out_bits = dsp_out_bits(p);
    c.sw_volume = !p->hw_vol;
    float v = p->vol_db + p->fade_db;
    c.volume_db = p->hw_vol ? 0.0f : (v < 0.0f ? v : 0.0f);
    if (!p->hw_vol && v <= PLAYER_VOLUME_MIN_DB + PLAYER_FADE_FLOOR_DB) c.volume_db = -INFINITY;
    dsp_configure(p->dsp, &c, p->sink_fmt.sample_rate, p->sink_fmt.channels);
}

static void volume_apply(player_t *p, bool force_hw) {
    audio_sink_t *s = p->sink;
    bool hw = s && (s->caps & SINK_CAP_HW_VOLUME) && s->set_volume_db;
    if (hw) {
        float v = p->vol_db + p->fade_db;
        if (force_hw || !p->hw_applied_valid || fabsf(v - p->hw_applied_db) > 0.001f) {
            int r = s->set_volume_db(s, v);
            if (r == CORE_ERR) {
                hw = false;  // "-1 if unsupported": software volume instead
            } else {
                if (r < 0) CORE_LOGW(TAG, "sink volume: %s", core_err_name(r));
                p->hw_applied_db = v;
                p->hw_applied_valid = true;
                p->hw_applied_ms = core_now_ms();
            }
        }
    }
    p->hw_vol = hw;
    dsp_apply(p);
}

static void set_volume(player_t *p, float db, bool always_post) {
    if (isnan(db)) return;
    float v = CORE_CLAMP(db, PLAYER_VOLUME_MIN_DB, p->vol_max_db);
    bool changed = v != p->vol_db;
    p->vol_db = v;
    if (changed) volume_apply(p, false);
    core_mutex_lock(p->mu);
    p->st.volume_db = v;
    p->st.volume_max_db = p->vol_max_db;
    core_mutex_unlock(p->mu);
    if (changed || always_post) player_post_volume(p);
    if (changed) status_output(p);
}

// ----------------------------------------------------------------------------- output ----
// Wraps sink->negotiate and checks the answer.
static int negotiate(player_t *p, const audio_format_t *src, audio_format_t *out) {
    audio_sink_t *s = p->sink;
    *out = *src;
    if (!src->dop && src->channels == 1) out->channels = 2;
    if (s->negotiate) {
        int r = s->negotiate(s, src, out);
        if (r != CORE_OK) return src->dop ? CORE_EUNSUPPORTED : (r < 0 ? r : CORE_EUNSUPPORTED);
    }
    if (!out->sample_rate || out->channels < 1 || out->channels > AUDIO_MAX_CHANNELS) return CORE_EUNSUPPORTED;
    out->bits = out->bits <= 16 ? 16 : out->bits <= 24 ? 24 : 32;
    if (out->channels != src->channels && !(src->channels == 1 && out->channels == 2)) return CORE_EUNSUPPORTED;
    if (src->dop) {
        // DoP must reach the DAC untouched: same rate, no resampling, no processing.
        if (!out->dop || out->sample_rate != src->sample_rate) return CORE_EUNSUPPORTED;
    } else if (out->dop) {
        return CORE_EUNSUPPORTED;
    }
    return CORE_OK;
}

// Upmix, resampler and buffers for src -> out. Keeps the resampler when nothing changed.
static int chain_setup(player_t *p, const audio_format_t *src, const audio_format_t *out) {
    p->src_fmt = *src;
    p->dop = src->dop;
    p->upmix = src->channels == 1 && out->channels == 2;
    bool need_rs = !src->dop && src->sample_rate != out->sample_rate;
    if (!need_rs) {
        resampler_destroy(p->rs);
        p->rs = NULL;
        return CORE_OK;
    }
    if (p->rs && p->rs_in_rate == src->sample_rate && p->rs_out_rate == out->sample_rate && p->rs_ch == out->channels)
        return CORE_OK;
    resampler_destroy(p->rs);
    p->rs = resampler_create(src->sample_rate, out->sample_rate, out->channels, RESAMPLER_GOOD);
    if (!p->rs) return CORE_EUNSUPPORTED;
    p->rs_in_rate = src->sample_rate;
    p->rs_out_rate = out->sample_rate;
    p->rs_ch = out->channels;
    uint32_t cap = resampler_max_output(p->rs, p->chunk) + 1;
    if (cap > p->rs_cap || !p->rs_buf) {
        int32_t *nb = core_malloc(PLAYER_BUF_SAMPLES(cap) * sizeof(int32_t));
        if (!nb) {
            resampler_destroy(p->rs);
            p->rs = NULL;
            return CORE_ENOMEM;
        }
        core_free(p->rs_buf);
        p->rs_buf = nb;
        p->rs_cap = cap;
    }
    return CORE_OK;
}

// DoP markers must alternate from frame to frame across entries too. Decoders start every
// file with 0x05, so a gapless DSD album whose previous file ended on 0x05 would send two in a
// row (DACs then drop out of DoP for a moment). A chunk that would repeat the last marker gets
// both markers swapped (0x05 <-> 0xFA); later chunks of the entry then follow that phase.
static void dop_keep_phase(player_t *p, int32_t *buf, uint32_t frames) {
    if (!frames) return;
    const uint32_t ch = p->sink_fmt.channels ? p->sink_fmt.channels : 1;  // after the upmix
    uint8_t first = (uint8_t)((uint32_t)buf[0] >> 24);
    if ((first == 0x05 || first == 0xFA) && first == p->dop_last_marker) {
        for (size_t i = 0; i < (size_t)frames * ch; i++) buf[i] = (int32_t)((uint32_t)buf[i] ^ 0xFF000000u);
    }
    p->dop_last_marker = (uint8_t)((uint32_t)buf[(size_t)(frames - 1) * ch] >> 24);
}

// Runs n decoded frames (in dec_buf) through upmix, resampler and DSP into p->pend. The
// DSP may add up to DSP_DRAIN_MAX frames (its delay line after a switch to bypass): both
// buffers have that room.
static void process(player_t *p, uint32_t n) {
    int32_t *buf = p->dec_buf;
    if (p->upmix) pcm_mono_to_stereo(buf, buf, n);
    int32_t *out = buf;
    uint32_t frames = n, cap = p->chunk + DSP_DRAIN_MAX;
    if (!p->dop) {
        if (p->rs) {
            uint32_t consumed = 0;
            frames = resampler_process(p->rs, buf, n, &consumed, p->rs_buf, p->rs_cap);
            if (consumed < n) CORE_LOGW(TAG, "resampler kept %lu frames", (unsigned long)(n - consumed));
            out = p->rs_buf;
            cap = p->rs_cap + DSP_DRAIN_MAX;
        }
        frames = dsp_process_out(p->dsp, out, frames, cap);
    } else {
        dop_keep_phase(p, out, frames);
    }
    p->pend = out;
    p->pend_frames = frames;
}

// Pushes silence through the resampler and the DSP so their delay lines (the last
// fraction of a millisecond of audio) reach the sink before it drains or closes. A DSP
// that already went to bypass may still hold its delay line: process(p, 0) plays it out.
static void process_tail(player_t *p) {
    if (p->dop) return;
    uint32_t n = 0;
    if (p->rs || !dsp_is_bypass(p->dsp)) {
        n = CORE_MIN(PLAYER_TAIL_FRAMES, p->chunk);
        memset(p->dec_buf, 0, (size_t)n * AUDIO_MAX_CHANNELS * sizeof(int32_t));
    }
    process(p, n);
}

static void begin_drain(player_t *p, pdrain_t d) {
    p->drain = d;
    p->drain_start_ms = p->drain_change_ms = core_now_ms();
    p->drain_level = UINT32_MAX;
}

// flush: drop what is queued (user stop); false after a completed drain.
static void sink_close(player_t *p, bool flush) {
    if (p->sink && p->sink_open) {
        if (flush && p->sink->flush) p->sink->flush(p->sink);
        if (p->sink->close) p->sink->close(p->sink);
    }
    p->sink_open = false;
    p->out_ready = false;
    p->pend_frames = 0;
    p->drain = DRAIN_NONE;
    p->reopen_ready = false;
    rebase(p);
}

// Drops everything queued in the sink and the processing (seek, skip).
static void flush_output(player_t *p) {
    if (p->sink && p->sink_open && p->sink->flush) p->sink->flush(p->sink);
    p->pend_frames = 0;
    // Nothing is left to play out. A pending end-of-queue stop is dropped as well: the flush
    // comes from a new selection (play, jump, previous) that must not be stopped by it.
    p->drain = DRAIN_NONE;
    p->reopen_ready = false;
    dsp_reset(p->dsp);
    resampler_reset(p->rs);
    rebase(p);
    if (p->sw_pending) {
        // The previous entry's tail is gone: the status shows the current one right away.
        p->sw_pending = false;
        if (p->cur.open) {
            status_track(p);
            core_event_post_simple(EV_TRACK_CHANGED, (int32_t)p->q.cur, 0);
        }
    }
}

static int sink_open(player_t *p, const audio_format_t *fmt) {
    audio_sink_t *s = p->sink;
    int r = s->open ? s->open(s, fmt) : CORE_OK;
    if (r != CORE_OK && p->sink_open) {
        // Could not reconfigure in place: close and start again.
        if (s->close) s->close(s);
        p->sink_open = false;
        r = s->open ? s->open(s, fmt) : CORE_OK;
    }
    if (r != CORE_OK) {
        p->sink_open = false;
        return r;
    }
    bool was_open = p->sink_open;
    p->sink_open = true;
    p->sink_fmt = *fmt;
    if (!was_open) {
        p->hw_applied_valid = false;
        // A sink paused before it was closed may keep that state: we only open to play.
        if (p->sink_paused && s->pause) s->pause(s, false);
        p->sink_paused = false;
    }
    rebase(p);
    return CORE_OK;
}

// ------------------------------------------------------------------------------ status ----
static void status_track(player_t *p) {
    const ptrack_t *t = &p->cur;
    core_mutex_lock(p->mu);
    player_status_t *st = &p->st;
    if (t->open) {
        const pmeta_t *m = &t->meta;
        core_strlcpy(st->path, m->path, sizeof st->path);
        core_strlcpy(st->title, m->title, sizeof st->title);
        core_strlcpy(st->artist, m->artist, sizeof st->artist);
        core_strlcpy(st->album, m->album, sizeof st->album);
        st->year = m->year;
        st->track_no = m->track_no;
        st->track_id = m->track_id;
        st->cover_offset = m->cover_offset;
        st->cover_size = m->cover_size;
        st->duration_ms = m->duration_ms;
        st->codec = t->info.codec;
        st->bitrate_kbps = t->info.bitrate_kbps;
        st->src_fmt = t->info.fmt;
    } else {
        st->path[0] = st->title[0] = st->artist[0] = st->album[0] = 0;
        st->year = st->track_no = 0;
        st->track_id = LIB_ID_NONE;
        st->cover_offset = 0;
        st->cover_size = 0;
        st->duration_ms = 0;
        st->codec = CODEC_UNKNOWN;
        st->bitrate_kbps = 0;
        memset(&st->src_fmt, 0, sizeof st->src_fmt);
    }
    st->queue_index = p->q.count ? p->q.cur : 0;
    st->queue_length = p->q.count;
    core_mutex_unlock(p->mu);
}

static void status_output(player_t *p) {
    bool bp = false;
    uint32_t flags = 0;
    audio_format_t out;
    memset(&out, 0, sizeof out);
    char detail[sizeof p->st.sink_detail] = "";
    if (p->sink_open && p->out_ready) {
        out = p->sink_fmt;
        // Only a sink that passes samples unchanged can be bit-perfect (not Bluetooth: it
        // re-dithers and encodes lossily).
        const bool sink_bp = p->sink && (p->sink->caps & SINK_CAP_BITPERFECT);
        if (p->dop) {
            bp = sink_bp;
        } else {
            flags = dsp_active_flags(p->dsp);
            if (p->rs) flags |= DSP_FLAG_RESAMPLE;
            const audio_format_t *s = &p->src_fmt;
            bool fmt_ok = s->sample_rate == out.sample_rate && s->bits == out.bits && s->dop == out.dop &&
                          (s->channels == out.channels || (s->channels == 1 && out.channels == 2));
            bool vol_ok = p->hw_vol || p->vol_db + p->fade_db >= 0.0f;
            bp = sink_bp && dsp_is_bypass(p->dsp) && !p->rs && vol_ok && fmt_ok;
        }
    }
    if (p->sink && p->sink->describe) p->sink->describe(p->sink, detail, sizeof detail);
    p->shown_bypass = dsp_is_bypass(p->dsp);
    core_mutex_lock(p->mu);
    p->st.out_fmt = out;
    p->st.bitperfect = bp;
    p->st.dsp_flags = flags;
    core_strlcpy(p->st.sink_name, p->sink && p->sink->name ? p->sink->name : "", sizeof p->st.sink_name);
    core_strlcpy(p->st.sink_detail, detail, sizeof p->st.sink_detail);
    core_mutex_unlock(p->mu);
}

// Dynamic fields, the deferred track switch and EV_POSITION.
static void status_tick(player_t *p) {
    uint32_t pos_ms = 0;
    bool switched = false;
    if (p->sw_pending) {
        uint64_t played = played_frames(p);
        if (p->cur.open && (played >= p->seg_w0 || !p->sink_open)) {
            p->sw_pending = false;
            switched = true;
        } else {
            uint64_t f = seg_frame(p->prev_base, p->prev_w0, played, p->prev_rate_in, p->sink_fmt.sample_rate);
            pos_ms = audio_frames_to_ms(f, p->prev_rate_in);
            if (p->prev_duration_ms && pos_ms > p->prev_duration_ms) pos_ms = p->prev_duration_ms;
        }
    }
    if (switched) status_track(p);
    if (!p->sw_pending) pos_ms = audible_ms(p);
    // A finished gain ramp can take the DSP into bypass without a configuration change.
    if (p->out_ready && !p->dop && dsp_is_bypass(p->dsp) != p->shown_bypass) status_output(p);
    uint32_t sleep_left = 0;
    if (p->sleep_on) {
        int32_t left = (int32_t)(p->sleep_deadline_ms - core_now_ms());
        sleep_left = left > 0 ? ((uint32_t)left + 999u) / 1000u : 0;
    }
    core_mutex_lock(p->mu);
    player_status_t *st = &p->st;
    st->state = p->state;
    st->position_ms = pos_ms;
    st->volume_db = p->vol_db;
    st->volume_max_db = p->vol_max_db;
    st->repeat = p->repeat;
    st->shuffle = p->q.shuffled;
    st->sleep_left_s = sleep_left;
    st->underruns = p->underruns;
    st->queue_length = p->q.count;
    if (!p->sw_pending) st->queue_index = p->q.count ? p->q.cur : 0;
    uint32_t qi = st->queue_index;
    core_mutex_unlock(p->mu);
    p->last_pos_ms = pos_ms;
    if (switched) core_event_post_simple(EV_TRACK_CHANGED, (int32_t)qi, 0);
    if (p->state == PLAYER_PLAYING &&
        (switched || pos_ms >= p->last_pos_event_ms + PLAYER_POSITION_EVENT_MS || pos_ms < p->last_pos_event_ms)) {
        post_position(p, pos_ms);
    }
}

// ------------------------------------------------------------------------------ queue ----
typedef struct {
    pq_item_t it;
    char path[CORE_PATH_MAX];
    bool has_path;
    uint32_t pos, gen, count;
} qcopy_t;

// The entry's library id comes out in the current library generation.
static bool queue_copy(player_t *p, uint32_t pos, qcopy_t *out) {
    core_mutex_lock(p->mu);
    const pq_item_t *it = pq_at(&p->q, pos);
    const uint32_t lib_gen = p->q.lib_gen;
    if (it) {
        out->it = *it;
        const char *path = pq_path(&p->q, it);
        out->has_path = path != NULL;
        core_strlcpy(out->path, path ? path : "", sizeof out->path);
        out->pos = pos;
        out->gen = p->q.gen;
        out->count = p->q.count;
    }
    core_mutex_unlock(p->mu);
    if (it) out->it.track_id = player_lib_id(p, out->it.track_id, lib_gen);
    return it != NULL;
}

// ------------------------------------------------------------------ library ids ----
static void remap_abort(player_t *p) {
    core_free(p->remap.items);
    core_free(p->remap.ids);
    memset(&p->remap, 0, sizeof p->remap);
}

// The metadata of an open entry follows a rescan too (status track id, album context).
static void meta_follow(player_t *p, pmeta_t *m) {
    library_t *lib = p->cfg.library;
    const uint32_t now = library_generation(lib);
    if (!lib || m->lib_gen == now) return;
    uint32_t g = m->lib_gen;
    lib_id_t id = m->track_id;
    m->lib_gen = now;
    if (id == LIB_ID_NONE || !library_translate_ids(lib, &g, &id, 1)) return;  // too old: ids kept
    m->lib_gen = g;
    m->track_id = id;
    m->album_id = LIB_ID_NONE;  // album ids are renumbered as well
    if (id != LIB_ID_NONE && library_get_track(lib, id, &p->lt) == CORE_OK) m->album_id = p->lt.album_id;
}

// Library ids in the queue belong to one index snapshot. After a rescan they are translated
// into a copy of items[], PLAYER_REMAP_SLICE entries per call, and the copy is swapped in at
// the end: a long queue never holds the audio loop. An edit of the queue meanwhile starts
// the translation again; a second rescan before the end leaves the ids as they are.
static void remap_step(player_t *p) {
    library_t *lib = p->cfg.library;
    if (!lib) return;
    if (p->cur.open && p->cur.meta.lib_gen != library_generation(lib)) {
        meta_follow(p, &p->cur.meta);
        core_mutex_lock(p->mu);
        if (!p->sw_pending) p->st.track_id = p->cur.meta.track_id;  // else at the switch
        core_mutex_unlock(p->mu);
    }
    if (p->next.open) meta_follow(p, &p->next.meta);
    if (p->remap.items && (p->q.gen != p->remap.qgen || p->q.lib_gen != p->remap.from)) remap_abort(p);
    if (!p->remap.items) {
        const uint32_t now = library_generation(lib);
        if (now == p->q.lib_gen) return;
        if (p->q.count == 0) {
            core_mutex_lock(p->mu);
            p->q.lib_gen = now;
            core_mutex_unlock(p->mu);
            return;
        }
        p->remap.items = core_malloc((size_t)p->q.cap * sizeof *p->remap.items);
        p->remap.ids = core_malloc(PLAYER_REMAP_SLICE * sizeof *p->remap.ids);
        if (!p->remap.items || !p->remap.ids) {
            remap_abort(p);  // memory is short: try again at the next iteration
            return;
        }
        p->remap.count = p->q.count;
        p->remap.qgen = p->q.gen;
        p->remap.from = p->q.lib_gen;
    }
    // Only this thread edits the queue: it reads items[] without the lock.
    const uint32_t at = p->remap.at, n = CORE_MIN(PLAYER_REMAP_SLICE, p->remap.count - at);
    pq_item_t *dst = p->remap.items + at;
    memcpy(dst, p->q.items + at, (size_t)n * sizeof *dst);
    for (uint32_t i = 0; i < n; i++) p->remap.ids[i] = dst[i].track_id;
    uint32_t g = p->remap.from;
    const bool ok = library_translate_ids(lib, &g, p->remap.ids, n) && (at == 0 || g == p->remap.to);
    if (!ok) {
        CORE_LOGW(TAG, "queue: library ids of an older index kept (two rescans in a row)");
        remap_abort(p);
        core_mutex_lock(p->mu);
        p->q.lib_gen = library_generation(lib);
        core_mutex_unlock(p->mu);
        return;
    }
    for (uint32_t i = 0; i < n; i++) dst[i].track_id = p->remap.ids[i];
    p->remap.to = g;
    p->remap.at = at + n;
    if (p->remap.at < p->remap.count) return;
    core_mutex_lock(p->mu);
    pq_item_t *old = p->q.items;
    p->q.items = p->remap.items;
    p->q.lib_gen = p->remap.to;
    core_mutex_unlock(p->mu);
    p->remap.items = NULL;
    core_free(old);
    remap_abort(p);
}

// Before ids of a batch join the queue: the queue's own ids are current (a translation that
// is running is finished now, which is O(n) but rare) and the batch's are brought to the
// same generation.
static void lib_align(player_t *p, pq_batch_t *b) {
    library_t *lib = p->cfg.library;
    if (!lib || !b) return;
    for (int guard = 0; guard < 2 && p->q.count && (p->remap.items || p->q.lib_gen != library_generation(lib));
         guard++) {
        do remap_step(p);
        while (p->remap.items);
    }
    if (p->q.count == 0 || b->lib_gen == p->q.lib_gen) return;
    lib_id_t *ids = core_malloc((size_t)b->count * sizeof *ids);
    if (!ids) return;
    for (uint32_t i = 0; i < b->count; i++) ids[i] = b->items[i].track_id;
    uint32_t g = b->lib_gen;
    if (library_translate_ids(lib, &g, ids, b->count) && g == p->q.lib_gen) {
        for (uint32_t i = 0; i < b->count; i++) b->items[i].track_id = ids[i];
        b->lib_gen = g;
    }
    core_free(ids);
}

static void set_cur_pos(player_t *p, uint32_t pos) {
    core_mutex_lock(p->mu);
    if (pos < p->q.count) p->q.cur = pos;
    core_mutex_unlock(p->mu);
}

// While sw_pending the listener still hears the previous entry (prev_qpos), and the status
// shows it: commands that depend on "the current track" refer to that one. False when the
// queue changed since (the position would name another entry).
static bool audible_is_prev(const player_t *p) {
    return p->sw_pending && p->prev_qgen == p->q.gen && p->prev_qpos < p->q.count;
}

// Natural successor of pos: REPEAT_ONE handled by the caller.
static bool next_pos(player_t *p, uint32_t pos, uint32_t *out) {
    return pq_step(&p->q, pos, +1, p->repeat == REPEAT_ALL, out);
}

// Scrobble and close the current entry.
static void finish_current(player_t *p, bool completed) {
    if (p->cur.open && p->cur.started) player_scrobble_track(p, &p->cur, completed);
    if (p->next.shared) next_close(p);  // it continues the decoder being closed
    track_close(&p->cur);
    p->out_ready = false;
}

static void stop_playback(player_t *p) {
    finish_current(p, false);
    next_close(p);
    sink_close(p, true);
    p->sw_pending = false;
    p->pending_start_valid = false;
    p->resume_on_sink = false;
    set_state(p, PLAYER_STOPPED);
    status_track(p);
    status_output(p);
}

// End of the queue: let the sink play out, then stop and rewind to the first entry.
static void end_of_queue(player_t *p) {
    process_tail(p);
    begin_drain(p, DRAIN_STOP);
}

static void finish_stop(player_t *p) {
    next_close(p);
    sink_close(p, false);
    p->sw_pending = false;
    set_cur_pos(p, 0);
    set_state(p, PLAYER_STOPPED);
    status_track(p);
    status_output(p);
    player_scrobble_flush(p);
}

// An entry could not be played: report it and move on (or stop).
static void entry_failed(player_t *p, const char *path, int err) {
    CORE_LOGW(TAG, "%s: %s", path ? path : "?", core_err_name(err));
    post_error(err, path);
    track_close(&p->cur);
    p->out_ready = false;
    p->fail_count++;
    uint32_t npos = 0;
    bool has = pq_step(&p->q, p->q.cur, +1, p->repeat != REPEAT_OFF, &npos);
    if (p->fail_count >= PLAYER_MAX_FAILURES || p->fail_count >= p->q.count) {
        CORE_LOGW(TAG, "%lu unplayable entries in a row, stopping", (unsigned long)p->fail_count);
        stop_playback(p);
        p->fail_count = 0;
        return;
    }
    if (!has) {
        if (p->state == PLAYER_PLAYING && p->sink_open && p->w) {
            end_of_queue(p);
        } else {
            finish_stop(p);
        }
        return;
    }
    set_cur_pos(p, npos);
    player_status_lock_update_queue(p);
}

// --------------------------------------------------------------------------- opening ----
// Opens the current queue entry into p->cur (decoder, metadata). Reuses a prepared next
// slot when it matches. On failure the error is posted and the queue moves on.
static bool open_current(player_t *p) {
    qcopy_t qc;
    if (!queue_copy(p, p->q.cur, &qc)) {
        stop_playback(p);
        return false;
    }
    if (p->next.open && !p->next.shared && p->next.qpos == qc.pos && p->next.qgen == qc.gen) {
        track_close(&p->cur);
        p->cur = p->next;
        memset(&p->next, 0, sizeof p->next);
        p->next_tried = false;
    } else {
        track_close(&p->cur);
        // A slot prepared for another position (the user jumped away during the last seconds
        // of an entry) would block the prefetch for this one and hold a decoder and a ring.
        if (p->next.open) next_close(p);
        int err = player_meta_open(p, &p->cur, &qc.it, qc.has_path ? qc.path : NULL, qc.pos);
        if (err != CORE_OK) {
            const char *name = qc.has_path ? qc.path : p->cur.meta.path;
            char path[CORE_PATH_MAX];
            core_strlcpy(path, name[0] ? name : "?", sizeof path);
            track_close(&p->cur);
            entry_failed(p, path, err);
            return false;
        }
        p->cur.album_ctx = player_meta_album_ctx(p, qc.pos, &p->cur.meta);
    }
    p->cur.qpos = qc.pos;
    p->cur.qgen = qc.gen;
    p->out_ready = false;
    if (p->pending_start_valid) {
        uint64_t rel = audio_ms_to_frames(p->pending_start_ms, p->cur.info.fmt.sample_rate);
        uint64_t len = entry_len(&p->cur);
        if (len && rel > len) rel = len;
        uint64_t abs = p->cur.start_frame + rel;
        if (rel && decoder_seek(p->cur.dec, abs) == CORE_OK) {
            p->cur.pos = abs;
            p->cur.skip = 0;
        } else if (rel && abs > p->cur.pos) {
            p->cur.skip = abs - p->cur.pos;
        }
        p->pending_start_valid = false;
    }
    p->seg_w0 = p->w;
    p->seg_base = entry_rel(&p->cur);
    if (!p->sw_pending) {
        status_track(p);
        core_event_post_simple(EV_TRACK_CHANGED, (int32_t)qc.pos, 0);
    }
    return true;
}

// Negotiates the output for p->cur, drains/reopens the sink when the format changes and
// configures upmix, resampler and DSP. Returns true when frames can be written.
static bool output_setup(player_t *p) {
    ptrack_t *t = &p->cur;
    audio_format_t src = t->info.fmt, out;
    int err = negotiate(p, &src, &out);
    if (err != CORE_OK) {
        if (src.dop) CORE_LOGW(TAG, "%s: DoP not accepted by the %s output", t->meta.path, p->sink->name);
        char path[CORE_PATH_MAX];
        core_strlcpy(path, t->meta.path, sizeof path);
        entry_failed(p, path, src.dop ? CORE_EUNSUPPORTED : err);
        return false;
    }
    if (p->sink_open && !audio_format_equal(&out, &p->sink_fmt)) {
        if (p->w && p->drain != DRAIN_REOPEN && !p->reopen_ready) {
            // Let the previous format play out first (its resampler/DSP tail included).
            process_tail(p);
            begin_drain(p, DRAIN_REOPEN);
            return false;
        }
        p->reopen_ready = false;
        err = sink_open(p, &out);
        if (err == CORE_OK) {
            // The old tail is gone from the processing too.
            dsp_reset(p->dsp);
            resampler_reset(p->rs);
        }
    } else if (!p->sink_open) {
        err = sink_open(p, &out);
        dsp_reset(p->dsp);
    }
    if (err != CORE_OK) {
        CORE_LOGE(TAG, "%s output: cannot open %lu Hz / %u bit: %s", p->sink->name ? p->sink->name : "sink",
                  (unsigned long)out.sample_rate, (unsigned)out.bits, core_err_name(err));
        post_error(CORE_EIO, p->sink->name ? p->sink->name : "output");
        p->resume_on_sink = false;
        set_state(p, PLAYER_PAUSED);
        return false;
    }
    err = chain_setup(p, &src, &out);
    if (err != CORE_OK) {
        char path[CORE_PATH_MAX];
        core_strlcpy(path, t->meta.path, sizeof path);
        entry_failed(p, path, err == CORE_ENOMEM ? err : CORE_EUNSUPPORTED);
        return false;
    }
    if (!p->dop) {
        volume_apply(p, !p->hw_applied_valid);
        const pmeta_t *m = &t->meta;
        dsp_set_replaygain(p->dsp, m->rg_track_db, m->rg_track_peak, m->rg_album_db, m->rg_album_peak, t->album_ctx);
    } else {
        volume_apply(p, !p->hw_applied_valid);
        if (!p->hw_vol) CORE_LOGW(TAG, "DoP on an output without hardware volume plays at full level");
    }
    p->out_ready = true;
    status_output(p);
    return true;
}

// Prepares the entry after the current one (gapless). Failures are reported when the
// entry is reached.
static void maybe_prefetch(player_t *p) {
    ptrack_t *t = &p->cur;
    if (!p->gapless || p->next.open || !t->open || p->repeat == REPEAT_ONE) return;
    uint64_t len = entry_len(t);
    if (!len) return;  // unknown length: open at the end
    uint64_t rel = entry_rel(t);
    uint64_t lead = (uint64_t)t->info.fmt.sample_rate * PLAYER_PREFETCH_MS / 1000u;
    if (rel + lead < len) return;
    uint32_t npos;
    if (!next_pos(p, p->q.cur, &npos)) return;
    if (p->next_tried && p->next_tried_gen == p->q.gen && p->next_tried_pos == npos) return;
    p->next_tried = true;
    p->next_tried_gen = p->q.gen;
    p->next_tried_pos = npos;
    qcopy_t qc;
    if (!queue_copy(p, npos, &qc)) return;
    ptrack_t *n = &p->next;
    bool cue_cur = t->start_ms || t->end_ms;
    bool cue_next = qc.it.start_ms || qc.it.end_ms;
    if (cue_cur && cue_next && qc.it.track_id == LIB_ID_NONE && qc.has_path && t->meta.track_id == LIB_ID_NONE &&
        strcmp(qc.path, t->meta.path) == 0) {
        // Next CUE track of the same image: keep decoding the same file.
        memset(n, 0, sizeof *n);
        n->info = t->info;
        n->meta = t->meta;
        n->meta.title[0] = 0;
        const uint32_t rate = t->info.fmt.sample_rate;
        n->start_ms = qc.it.start_ms;
        n->end_ms = qc.it.end_ms > qc.it.start_ms ? qc.it.end_ms : 0;
        n->start_frame = audio_ms_to_frames(n->start_ms, rate);
        n->end_frame = n->end_ms ? audio_ms_to_frames(n->end_ms, rate) : 0;
        uint64_t total = t->info.total_frames;
        if (total && n->end_frame >= total) n->end_frame = 0;
        if (n->end_frame) {
            n->meta.duration_ms = n->end_ms - n->start_ms;
        } else if (total > n->start_frame) {
            n->meta.duration_ms = audio_frames_to_ms(total - n->start_frame, rate);
        } else {
            n->meta.duration_ms = 0;
        }
        player_meta_cue_title(p, n);
        player_meta_title_fallback(&n->meta);
        n->shared = true;
        n->open = true;
    } else {
        int err = player_meta_open(p, n, &qc.it, qc.has_path ? qc.path : NULL, npos);
        if (err != CORE_OK) {
            track_close(n);
            memset(n, 0, sizeof *n);
            return;
        }
    }
    n->qpos = npos;
    n->qgen = qc.gen;
    n->album_ctx = player_meta_album_ctx(p, npos, &n->meta);
}

// ----------------------------------------------------------------------------- decoding ----
// Decodes one chunk of the current entry into p->pend. Returns 1 when data was decoded
// (the output may still be empty: resampler warm-up, skipping), 0 at the end of the entry,
// or a negative error.
static int produce(player_t *p) {
    ptrack_t *t = &p->cur;
    uint32_t want = p->chunk;
    if (t->skip) {
        for (int i = 0; i < 16 && t->skip; i++) {
            uint32_t n = (uint32_t)CORE_MIN((uint64_t)want, t->skip);
            int32_t r = decoder_read(t->dec, p->dec_buf, n);
            if (r <= 0) return r;
            t->pos += (uint64_t)r;
            t->skip -= CORE_MIN((uint64_t)r, t->skip);
        }
        p->busy = true;
        return 1;
    }
    if (t->end_frame) {
        if (t->pos >= t->end_frame) return 0;
        uint64_t left = t->end_frame - t->pos;
        if (left < want) want = (uint32_t)left;
    }
    int32_t n = decoder_read(t->dec, p->dec_buf, want);
    if (n <= 0) return n;
    if ((uint32_t)n > want) n = (int32_t)want;
    t->pos += (uint64_t)n;
    t->frames_out += (uint64_t)n;
    process(p, (uint32_t)n);
    return 1;
}

// Starts a new audible segment for p->cur at the current write position.
static void new_segment(player_t *p, uint32_t prev_rate, uint32_t prev_duration) {
    p->prev_w0 = p->seg_w0;
    p->prev_base = p->seg_base;
    p->prev_rate_in = prev_rate;
    p->prev_duration_ms = prev_duration;
    p->seg_w0 = p->w;
    p->seg_base = p->cur.open ? entry_rel(&p->cur) : 0;
    p->sw_pending = p->sink_open && p->w > 0;
}

// The current entry ended (err = 0) or failed while decoding (err < 0).
static void entry_end(player_t *p, int err) {
    ptrack_t *t = &p->cur;
    if (err < 0) CORE_LOGW(TAG, "%s: decoding stopped: %s", t->meta.path, core_err_name(err));
    if (t->frames_out == 0) {
        // Nothing decoded (broken or empty file, CUE range past the end): unplayable. This
        // also keeps a repeat-all queue of empty files from spinning forever.
        char path[CORE_PATH_MAX];
        core_strlcpy(path, t->meta.path, sizeof path);
        if (p->next.shared) next_close(p);
        entry_failed(p, path, err < 0 ? err : CORE_ECORRUPT);
        return;
    }
    const uint32_t rate = t->info.fmt.sample_rate, dur = t->meta.duration_ms;
    if (p->repeat == REPEAT_ONE && t->frames_out > 0) {
        p->prev_qpos = p->q.cur;
        p->prev_qgen = p->q.gen;
        if (t->started) player_scrobble_track(p, t, true);
        uint64_t prev_w0 = p->seg_w0, prev_base = p->seg_base;
        if (decoder_seek(t->dec, t->start_frame) == CORE_OK) {
            t->pos = t->start_frame;
            t->frames_out = 0;
            t->played_out = 0;
            t->started = false;
            p->prev_w0 = prev_w0;
            p->prev_base = prev_base;
            p->prev_rate_in = rate;
            p->prev_duration_ms = dur;
            p->seg_w0 = p->w;
            p->seg_base = 0;
            p->sw_pending = p->sink_open && p->w > 0;
            if (!p->sw_pending) {
                status_track(p);
                core_event_post_simple(EV_TRACK_CHANGED, (int32_t)p->q.cur, 0);
            }
            return;
        }
        // Not seekable: reopen it.
        track_close(t);
        p->out_ready = false;
        new_segment(p, rate, dur);
        return;
    }
    uint32_t npos;
    bool has = next_pos(p, p->q.cur, &npos);
    p->prev_qpos = p->q.cur;  // stays audible until its tail has left the sink
    p->prev_qgen = p->q.gen;
    if (t->started) player_scrobble_track(p, t, err == 0);
    if (!has) {
        if (p->next.shared) next_close(p);
        track_close(t);
        p->out_ready = false;
        new_segment(p, rate, dur);
        end_of_queue(p);
        return;
    }
    set_cur_pos(p, npos);
    ptrack_t *n = &p->next;
    if (n->open && n->qpos == npos && n->qgen == p->q.gen) {
        if (n->shared) {
            decoder_t *dec = t->dec;
            uint64_t pos = t->pos;
            t->dec = NULL;
            *t = *n;
            t->dec = dec;
            t->pos = pos;
            t->shared = false;
            memset(n, 0, sizeof *n);
            if (t->pos != t->start_frame) {
                if (decoder_seek(t->dec, t->start_frame) == CORE_OK) {
                    t->pos = t->start_frame;
                } else if (t->start_frame > t->pos) {
                    t->skip = t->start_frame - t->pos;
                }
            }
            // Same file, same format: the output stays as it is.
        } else {
            track_close(t);
            *t = *n;
            memset(n, 0, sizeof *n);
            p->out_ready = false;  // output_setup() keeps the sink when the format matches
        }
        p->next_tried = false;
        new_segment(p, rate, dur);
        if (!p->sw_pending) {
            status_track(p);
            core_event_post_simple(EV_TRACK_CHANGED, (int32_t)npos, 0);
        }
        if (p->out_ready) {
            const pmeta_t *m = &t->meta;
            dsp_set_replaygain(p->dsp, m->rg_track_db, m->rg_track_peak, m->rg_album_db, m->rg_album_peak,
                               t->album_ctx);
        }
        return;
    }
    // Nothing prepared (gapless off, unknown length, or the queue changed): open now.
    next_close(p);
    track_close(t);
    p->out_ready = false;
    new_segment(p, rate, dur);
}

// ------------------------------------------------------------------------------ writing ----
static int32_t write_pending(player_t *p) {
    audio_sink_t *s = p->sink;
    uint32_t lvl = sink_level(p);
    // The buffer ran dry since the last write. Sinks that never report a level are skipped.
    if (lvl) p->level_seen = true;
    if (p->level_seen && p->w > 0 && lvl == 0 && p->last_level > 0) p->underruns++;
    int32_t r = s->write ? s->write(s, p->pend, p->pend_frames, PLAYER_WRITE_TIMEOUT_MS) : (int32_t)p->pend_frames;
    if (r < 0) {
        if (++p->sink_errors >= SINK_ERROR_LIMIT) {
            CORE_LOGE(TAG, "%s output keeps failing (%s), pausing", s->name ? s->name : "sink", core_err_name(r));
            post_error(CORE_EIO, s->name ? s->name : "output");
            p->sink_errors = 0;
            set_state(p, PLAYER_PAUSED);
            if (s->pause) s->pause(s, true);
            p->sink_paused = true;
        }
        p->blocked = true;
        return 0;
    }
    p->sink_errors = 0;
    if (r == 0) {
        p->blocked = true;
        p->last_level = lvl;
        return 0;
    }
    if ((uint32_t)r > p->pend_frames) r = (int32_t)p->pend_frames;
    ptrack_t *t = &p->cur;
    if (t->open && !t->started) {
        t->started = true;
        t->start_mono_ms = core_now_ms();
        int64_t now = player_wall_now(p);
        t->start_unix = now > PLAYER_CLOCK_VALID ? now : 0;
        p->fail_count = 0;
    }
    if (t->open) t->played_out += (uint64_t)r;
    p->w += (uint64_t)r;
    p->pend += (size_t)r * p->sink_fmt.channels;
    p->pend_frames -= (uint32_t)r;
    p->last_level = lvl + (uint32_t)r;
    return r;
}

// Returns true when the sink has played everything written so far.
static bool drain_step(player_t *p) {
    uint32_t now = core_now_ms();
    uint32_t lvl = sink_level(p);
    if (lvl != p->drain_level) {
        p->drain_level = lvl;
        p->drain_change_ms = now;
    }
    if (lvl > 0 && now - p->drain_change_ms < DRAIN_STUCK_MS) {
        p->blocked = true;
        return false;
    }
    pdrain_t d = p->drain;
    p->drain = DRAIN_NONE;
    if (d == DRAIN_STOP) {
        finish_stop(p);
        return false;
    }
    p->reopen_ready = true;
    return true;
}

static int32_t play_step(player_t *p) {
    if (!sink_has(p)) {
        p->resume_on_sink = true;
        set_state(p, PLAYER_PAUSED);
        return 0;
    }
    if (p->pend_frames) return write_pending(p);
    if (p->drain != DRAIN_NONE && !drain_step(p)) return 0;
    for (int guard = 0; guard < 4; guard++) {
        if (p->state != PLAYER_PLAYING) return 0;
        if (!p->cur.open && !open_current(p)) {
            p->busy = true;
            return 0;
        }
        if (!p->out_ready && !output_setup(p)) {
            p->busy = p->drain == DRAIN_NONE && p->state == PLAYER_PLAYING;
            if (p->pend_frames) return write_pending(p);
            return 0;
        }
        maybe_prefetch(p);
        int r = produce(p);
        if (r > 0) {
            if (p->pend_frames) return write_pending(p);
            continue;  // resampler warm-up or skipping: decode more
        }
        entry_end(p, r);
        if (p->pend_frames) return write_pending(p);  // tail before a drain
        if (p->drain != DRAIN_NONE) return 0;
    }
    p->busy = true;
    return 0;
}

// While paused (e.g. after restore) the current entry is opened so the status shows it.
static void prepare_paused(player_t *p) {
    if (p->cur.open || p->q.count == 0) return;
    open_current(p);
}

// --------------------------------------------------------------------------- commands ----
static void select_entry(player_t *p, uint32_t pos, player_state_t state) {
    finish_current(p, false);
    set_cur_pos(p, pos);
    flush_output(p);
    p->pending_start_valid = false;
    p->fail_count = 0;
    if (state == PLAYER_PLAYING && !sink_has(p)) {
        p->resume_on_sink = true;
        state = PLAYER_PAUSED;
    }
    set_state(p, state);
    player_status_lock_update_queue(p);
    if (state == PLAYER_STOPPED) {
        status_track(p);
        core_event_post_simple(EV_TRACK_CHANGED, (int32_t)p->q.cur, 0);
    }
}

static void seek_to(player_t *p, uint32_t ms) {
    if (!p->cur.open) {
        if (p->q.count == 0) return;
        p->pending_start_ms = ms;
        p->pending_start_valid = true;
        core_mutex_lock(p->mu);
        p->st.position_ms = ms;
        core_mutex_unlock(p->mu);
        post_position(p, ms);
        return;
    }
    ptrack_t *t = &p->cur;
    const uint32_t rate = t->info.fmt.sample_rate;
    uint64_t rel = audio_ms_to_frames(ms, rate);
    uint64_t len = entry_len(t);
    if (len && rel > len) rel = len;  // at the end: the next iteration moves on
    uint64_t abs = t->start_frame + rel;
    int r = decoder_seek(t->dec, abs);
    if (r == CORE_OK) {
        t->pos = abs;
        t->skip = 0;
    } else if (abs >= t->pos) {
        t->skip = abs - t->pos;
    } else {
        CORE_LOGW(TAG, "%s: cannot seek back (%s)", t->meta.path, core_err_name(r));
        return;
    }
    if (p->next.shared) next_close(p);
    flush_output(p);
    p->seg_base = rel;
    uint32_t pos_ms = audio_frames_to_ms(rel, rate);
    core_mutex_lock(p->mu);
    p->st.position_ms = pos_ms;
    core_mutex_unlock(p->mu);
    post_position(p, pos_ms);
}

static void cmd_play(player_t *p, pq_batch_t *b, uint32_t start, bool shuffle) {
    finish_current(p, false);
    next_close(p);
    // The commanding thread already shuffled the batch (player.c): taking it is O(1).
    core_mutex_lock(p->mu);
    pq_replace(&p->q, b, start, shuffle, true, b ? b->seed : 0);
    core_mutex_unlock(p->mu);
    post_queue(p);
    if (p->q.count == 0) {
        stop_playback(p);
        return;
    }
    select_entry(p, p->q.cur, PLAYER_PLAYING);
}

static void cmd_enqueue(player_t *p, pq_batch_t *b, bool play_next) {
    lib_align(p, b);
    pq_t old;
    core_mutex_lock(p->mu);
    // Prepared on a copy of the queue: swapped in. Else (short queue, or it changed since the
    // command) inserted in place.
    const bool swapped = b->prep && b->prep->play_next == play_next && pq_prep_apply(&p->q, b->prep, &old);
    uint32_t added = swapped ? b->prep->added : pq_insert(&p->q, b, play_next);
    core_mutex_unlock(p->mu);
    if (swapped) pq_free(&old);
    if (added < b->count) CORE_LOGW(TAG, "queue full: %lu of %lu added", (unsigned long)added, (unsigned long)b->count);
    if (added) post_queue(p);
}

static void cmd_remove(player_t *p, uint32_t pos, pq_prep_t *pr) {
    if (pos >= p->q.count) return;
    bool is_cur = pos == p->q.cur;
    bool was_last = pos + 1 == p->q.count;
    if (is_cur) finish_current(p, false);
    pq_t old;
    core_mutex_lock(p->mu);
    const bool swapped = pr && pr->pos == pos && pq_prep_apply(&p->q, pr, &old);
    if (!swapped) pq_remove(&p->q, pos);
    core_mutex_unlock(p->mu);
    if (swapped) pq_free(&old);
    post_queue(p);
    if (p->q.count == 0) {
        stop_playback(p);
        status_track(p);
        return;
    }
    if (!is_cur) return;
    player_state_t st = p->state;
    if (was_last) {
        // The last entry was playing: continue at the start only with repeat all.
        set_cur_pos(p, 0);
        if (p->repeat != REPEAT_ALL) st = PLAYER_STOPPED;
    }
    if (st == PLAYER_STOPPED) {
        stop_playback(p);
        status_track(p);
        return;
    }
    select_entry(p, p->q.cur, st);
}

// Starts pos at ms (a reopen, e.g. seeking back into the entry that is still audible).
static void select_at(player_t *p, uint32_t pos, uint32_t ms) {
    select_entry(p, pos, p->state);
    if (!ms) return;
    p->pending_start_ms = ms;
    p->pending_start_valid = true;
    core_mutex_lock(p->mu);
    p->st.position_ms = ms;
    core_mutex_unlock(p->mu);
    post_position(p, ms);
}

static void cmd_next(player_t *p) {
    if (p->q.count == 0) return;
    if (audible_is_prev(p) && p->cur.open && p->prev_qpos != p->q.cur) {
        // The next entry of what the listener hears is the one already decoding: start it
        // now (its first frames are dropped with the previous tail). Not for repeat-one,
        // where both are the same entry.
        select_entry(p, p->q.cur, p->state);
        return;
    }
    uint32_t npos;
    if (!pq_step(&p->q, p->q.cur, +1, p->repeat != REPEAT_OFF, &npos)) return;
    select_entry(p, npos, p->state);
}

static void cmd_prev(player_t *p) {
    if (p->q.count == 0) return;
    if (audible_is_prev(p)) {
        // Restart it after 3 s, else go to the entry before it (restart when there is none).
        uint32_t npos = p->prev_qpos;
        if (p->last_pos_ms <= PLAYER_PREV_RESTART_MS) pq_step(&p->q, p->prev_qpos, -1, p->repeat == REPEAT_ALL, &npos);
        select_entry(p, npos, p->state);
        return;
    }
    if (p->cur.open && audible_ms(p) > PLAYER_PREV_RESTART_MS) {
        seek_to(p, 0);
        return;
    }
    uint32_t npos;
    if (!pq_step(&p->q, p->q.cur, -1, p->repeat == REPEAT_ALL, &npos)) {
        if (p->cur.open) seek_to(p, 0);
        return;
    }
    select_entry(p, npos, p->state);
}

// Absolute seek. While the previous entry is still audible the position refers to it (the
// progress bar shows it): inside it the entry is reopened there, past its end the seek
// continues into the current one.
static void cmd_seek(player_t *p, int64_t ms) {
    if (ms < 0) ms = 0;
    if (ms > UINT32_MAX) ms = UINT32_MAX;
    if (audible_is_prev(p)) {
        const uint32_t dur = p->prev_duration_ms;
        if (dur && ms >= dur) {
            seek_to(p, (uint32_t)(ms - dur));
        } else {
            select_at(p, p->prev_qpos, (uint32_t)ms);
        }
        return;
    }
    seek_to(p, (uint32_t)ms);
}

static void cmd_seek_rel(player_t *p, int32_t delta) {
    if (p->q.count == 0) return;
    const uint32_t from = audible_is_prev(p) ? p->last_pos_ms : audible_ms(p);
    cmd_seek(p, (int64_t)from + delta);
}

static void do_pause(player_t *p) {
    if (p->state != PLAYER_PLAYING) return;
    if (p->sink_open && p->sink && p->sink->pause) {
        p->sink->pause(p->sink, true);
        p->sink_paused = true;
    }
    p->last_level = 0;  // the sink plays out while paused: not an underrun
    set_state(p, PLAYER_PAUSED);
    player_scrobble_flush(p);
}

static void do_resume(player_t *p) {
    if (p->q.count == 0) return;
    if (!sink_has(p)) {
        p->resume_on_sink = true;
        if (p->state == PLAYER_STOPPED) set_state(p, PLAYER_PAUSED);
        return;
    }
    if (p->state == PLAYER_PAUSED) {
        if (p->sink_open && p->sink->pause) p->sink->pause(p->sink, false);
        p->sink_paused = false;
        p->last_level = 0;
        if (p->drain != DRAIN_NONE) p->drain_change_ms = core_now_ms();
    } else if (p->state == PLAYER_STOPPED) {
        p->fail_count = 0;
    }
    set_state(p, PLAYER_PLAYING);
}

static void cmd_shuffle(player_t *p, bool on, uint32_t seed, pq_prep_t *pr) {
    pq_t old;
    bool swapped = false;
    core_mutex_lock(p->mu);
    bool change = p->q.shuffled != on;
    if (change) {
        swapped = pr && pr->q.shuffled == on && pq_prep_apply(&p->q, pr, &old);
        if (!swapped) pq_set_shuffle(&p->q, on, seed);
    }
    core_mutex_unlock(p->mu);
    if (swapped) pq_free(&old);
    if (!change) return;
    if (p->cur.open) p->cur.qpos = p->q.cur;
    post_queue(p);
    player_post_state(p);
}

static void cmd_sleep(player_t *p, uint32_t minutes) {
    bool was = p->sleep_on;
    if (minutes == 0) {
        p->sleep_on = false;
        if (p->fade_db != 0.0f) {
            p->fade_db = 0.0f;
            volume_apply(p, false);
            status_output(p);
        }
        if (was) core_event_post_simple(EV_SLEEP_TIMER, -1, 0);
        return;
    }
    p->sleep_on = true;
    p->sleep_deadline_ms = core_now_ms() + minutes * 60000u;
    p->sleep_last_posted = minutes * 60u;
    if (p->fade_db != 0.0f) {
        p->fade_db = 0.0f;
        volume_apply(p, false);
    }
    core_event_post_simple(EV_SLEEP_TIMER, (int32_t)(minutes * 60u), 0);
}

static void cmd_sink(player_t *p, audio_sink_t *s) {
    if (s == p->sink) return;
    // The queue ended and its tail was playing out: the old output took it along, so the
    // stop completes now (instead of reopening the last entry on the new output).
    const bool stop_pending = p->drain == DRAIN_STOP;
    bool had = p->cur.open;
    uint64_t rel = had ? audible_rel(p) : 0;
    if (p->sink && p->sink_open) {
        if (p->sink->flush) p->sink->flush(p->sink);
        if (p->sink->close) p->sink->close(p->sink);
    }
    p->sink = s;
    p->sink_open = false;
    p->out_ready = false;
    p->pend_frames = 0;
    p->drain = DRAIN_NONE;
    p->reopen_ready = false;
    p->hw_applied_valid = false;
    p->hw_vol = false;
    p->sink_paused = false;
    p->level_seen = false;
    bool was_pending = p->sw_pending;
    p->sw_pending = false;
    if (had) {
        // Continue from what was audible on the old output (its queued audio is lost).
        ptrack_t *t = &p->cur;
        uint64_t abs = t->start_frame + rel;
        if (abs != t->pos) {
            if (decoder_seek(t->dec, abs) == CORE_OK) {
                t->pos = abs;
                t->skip = 0;
            } else if (abs > t->pos) {
                t->skip = abs - t->pos;
            }
        }
        if (p->next.shared) next_close(p);
        dsp_reset(p->dsp);
        resampler_reset(p->rs);
        if (was_pending) {
            status_track(p);
            core_event_post_simple(EV_TRACK_CHANGED, (int32_t)p->q.cur, 0);
        }
    }
    rebase(p);
    p->seg_base = rel;
    if (stop_pending) {
        p->resume_on_sink = false;
        finish_stop(p);
    } else if (!s) {
        if (p->state == PLAYER_PLAYING) {
            p->resume_on_sink = true;
            set_state(p, PLAYER_PAUSED);
        }
    } else if (p->resume_on_sink) {
        p->resume_on_sink = false;
        if (p->state == PLAYER_PAUSED && p->q.count) set_state(p, PLAYER_PLAYING);
    }
    status_output(p);
}

void player_apply_restore(player_t *p, prestore_t *r) {
    finish_current(p, false);
    next_close(p);
    flush_output(p);
    core_mutex_lock(p->mu);
    if (r->seed) p->rng = r->seed;
    if (r->batch) {
        pq_replace(&p->q, r->batch, r->index, r->shuffle, true, r->seed);
        r->batch = NULL;
    } else {
        pq_clear(&p->q);
    }
    core_mutex_unlock(p->mu);
    p->repeat = r->repeat;
    if (r->has_volume) set_volume(p, r->volume_db, true);
    p->pending_start_ms = r->position_ms;
    p->pending_start_valid = r->position_ms > 0;
    p->fail_count = 0;
    post_queue(p);
    if (p->q.count == 0) {
        stop_playback(p);
        return;
    }
    player_state_t st = r->autoplay ? PLAYER_PLAYING : PLAYER_PAUSED;
    if (st == PLAYER_PLAYING && !sink_has(p)) {
        st = PLAYER_PAUSED;
        p->resume_on_sink = true;
    }
    if (p->state == st) {
        player_post_state(p);  // repeat/shuffle may have changed
    } else {
        set_state(p, st);
    }
}

void player_engine_apply(player_t *p, const pcmd_t *c) {
    switch ((pcmd_type_t)c->type) {
    case PCMD_PLAY:
        cmd_play(p, c->ptr, c->u, c->flag);
        break;
    case PCMD_ENQUEUE:
        cmd_enqueue(p, c->ptr, c->flag);
        pq_batch_free(c->ptr);
        break;
    case PCMD_REMOVE:
        cmd_remove(p, c->u, c->ptr);
        pq_prep_free(c->ptr);
        break;
    case PCMD_JUMP:
        if (c->u < p->q.count) select_entry(p, c->u, PLAYER_PLAYING);
        break;
    case PCMD_CLEAR:
        stop_playback(p);
        core_mutex_lock(p->mu);
        pq_clear(&p->q);
        core_mutex_unlock(p->mu);
        status_track(p);
        post_queue(p);
        break;
    case PCMD_TOGGLE:
        if (p->state == PLAYER_PLAYING) {
            do_pause(p);
        } else if (!sink_has(p) && p->state == PLAYER_PAUSED && p->resume_on_sink) {
            p->resume_on_sink = false;  // paused without output and set to resume: now it stays paused
        } else {
            do_resume(p);
        }
        break;
    case PCMD_PAUSE:
        do_pause(p);
        p->resume_on_sink = false;  // also while waiting for an output (mode switch, Wi-Fi mode)
        break;
    case PCMD_RESUME:
        do_resume(p);
        break;
    case PCMD_STOP:
        if (p->state != PLAYER_STOPPED || p->cur.open) stop_playback(p);
        break;
    case PCMD_NEXT:
        cmd_next(p);
        break;
    case PCMD_PREV:
        cmd_prev(p);
        break;
    case PCMD_SEEK:
        if (p->q.count) cmd_seek(p, c->u);
        break;
    case PCMD_SEEK_REL:
        cmd_seek_rel(p, c->i);
        break;
    case PCMD_VOLUME:
        set_volume(p, c->f, true);
        break;
    case PCMD_VOLUME_STEP:
        set_volume(p, p->vol_db + (float)c->i * p->vol_step, true);
        break;
    case PCMD_SHUFFLE:
        cmd_shuffle(p, c->flag, c->u, c->ptr);
        pq_prep_free(c->ptr);
        break;
    case PCMD_REPEAT:
        if ((repeat_mode_t)c->u != p->repeat) {
            p->repeat = (repeat_mode_t)c->u;
            next_close(p);
        }
        core_mutex_lock(p->mu);
        p->st.repeat = p->repeat;
        core_mutex_unlock(p->mu);
        player_post_state(p);
        break;
    case PCMD_DSP: {
        core_mutex_lock(p->mu);
        p->dsp_user = p->dsp_pending;
        core_mutex_unlock(p->mu);
        dsp_apply(p);
        if (p->cur.open && p->out_ready && !p->dop) {
            p->cur.album_ctx = player_meta_album_ctx(p, p->q.cur, &p->cur.meta);
            const pmeta_t *m = &p->cur.meta;
            dsp_set_replaygain(p->dsp, m->rg_track_db, m->rg_track_peak, m->rg_album_db, m->rg_album_peak,
                               p->cur.album_ctx);
        }
        status_output(p);
        break;
    }
    case PCMD_GAPLESS:
        p->gapless = c->flag;
        if (!p->gapless && !p->next.shared) next_close(p);
        break;
    case PCMD_SLEEP:
        cmd_sleep(p, c->u);
        break;
    case PCMD_VOLUME_LIMIT: {
        float lim = CORE_CLAMP(c->f, PLAYER_VOLUME_MIN_DB, 0.0f);
        p->vol_max_db = lim;
        if (p->vol_db > lim) {
            set_volume(p, lim, true);
        } else {
            core_mutex_lock(p->mu);
            p->st.volume_max_db = lim;
            core_mutex_unlock(p->mu);
            player_post_volume(p);
        }
        break;
    }
    case PCMD_SINK:
        cmd_sink(p, c->ptr);
        break;
    case PCMD_RESTORE: {
        prestore_t *r = c->ptr;
        if (r) {
            player_apply_restore(p, r);
            pq_batch_free(r->batch);
            core_free(r);
        }
        break;
    }
    default:
        break;
    }
}

// ------------------------------------------------------------------------ sleep timer ----
static void sleep_tick(player_t *p) {
    if (!p->sleep_on) return;
    uint32_t now = core_now_ms();
    int32_t left = (int32_t)(p->sleep_deadline_ms - now);
    if (left <= 0) {
        p->sleep_on = false;
        do_pause(p);
        p->resume_on_sink = false;  // a sleep timer that fired without an output keeps it paused
        p->fade_db = 0.0f;
        volume_apply(p, false);
        status_output(p);
        core_event_post_simple(EV_SLEEP_TIMER, 0, 0);
        return;
    }
    float fade = 0.0f;
    if ((uint32_t)left < PLAYER_FADE_MS) fade = PLAYER_FADE_FLOOR_DB * (1.0f - (float)left / (float)PLAYER_FADE_MS);
    if (fade != p->fade_db) {
        bool hw = p->sink && (p->sink->caps & SINK_CAP_HW_VOLUME);
        // Hardware volume: coarser steps (each one is a bus transfer).
        if (!hw || fabsf(fade - p->fade_db) >= HW_FADE_STEP_DB || now - p->hw_applied_ms >= HW_FADE_MIN_MS * 5u) {
            bool edge = (fade == 0.0f) != (p->fade_db == 0.0f);
            p->fade_db = fade;
            volume_apply(p, false);
            if (edge) status_output(p);  // bit-perfect flag
        }
    }
    uint32_t left_s = ((uint32_t)left + 999u) / 1000u;
    if (left_s != p->sleep_last_posted && (left_s % 60u == 0 || (left_s <= 30u && left_s % 5u == 0))) {
        p->sleep_last_posted = left_s;
        core_event_post_simple(EV_SLEEP_TIMER, (int32_t)left_s, 0);
    }
}

// ---------------------------------------------------------------------------- run loop ----
int player_engine_set_chunk(player_t *p, uint32_t frames) {
    if (frames == p->chunk) return CORE_OK;
    int32_t *nb = core_malloc(PLAYER_BUF_SAMPLES(frames) * sizeof(int32_t));
    if (!nb) return CORE_ENOMEM;
    uint32_t rs_cap = p->rs ? resampler_max_output(p->rs, frames) + 1 : 0;
    int32_t *nrs = NULL;
    if (rs_cap > p->rs_cap) {
        nrs = core_malloc(PLAYER_BUF_SAMPLES(rs_cap) * sizeof(int32_t));
        if (!nrs) {
            core_free(nb);
            return CORE_ENOMEM;
        }
    }
    // Pending output may point into the old buffers: keep it valid.
    if (p->pend_frames) {
        size_t n = (size_t)p->pend_frames * p->sink_fmt.channels;
        int32_t *dst = nb;
        if (n > PLAYER_BUF_SAMPLES(frames)) {
            core_free(nb);
            core_free(nrs);
            return CORE_EAGAIN;  // try again once the pending frames are written
        }
        memcpy(dst, p->pend, n * sizeof(int32_t));
        p->pend = dst;
    }
    core_free(p->dec_buf);
    p->dec_buf = nb;
    p->chunk = frames;
    if (nrs) {
        core_free(p->rs_buf);
        p->rs_buf = nrs;
        p->rs_cap = rs_cap;
    }
    return CORE_OK;
}

void player_engine_close(player_t *p) {
    remap_abort(p);
    if (p->cur.open && p->cur.started) player_scrobble_track(p, &p->cur, false);  // playing at shutdown
    track_close(&p->cur);
    track_close(&p->next);
    if (p->sink && p->sink_open) {
        if (p->sink->flush) p->sink->flush(p->sink);
        if (p->sink->close) p->sink->close(p->sink);
    }
    p->sink_open = false;
}

int32_t player_run_once(player_t *p) {
    if (!p) return 0;
    p->busy = false;
    p->blocked = false;
    // Chunk size change and queued commands.
    core_mutex_lock(p->mu);
    uint32_t chunk = p->chunk_req;
    p->chunk_req = 0;
    uint32_t n = p->ring_count;
    for (uint32_t i = 0; i < n; i++) p->work[i] = p->ring[(p->ring_head + i) % PLAYER_CMD_RING];
    p->ring_head = 0;
    p->ring_count = 0;
    core_mutex_unlock(p->mu);
    if (chunk && player_engine_set_chunk(p, chunk) == CORE_EAGAIN) {
        core_mutex_lock(p->mu);
        if (!p->chunk_req) p->chunk_req = chunk;
        core_mutex_unlock(p->mu);
    }
    for (uint32_t i = 0; i < n; i++) player_engine_apply(p, &p->work[i]);
    if (p->next.open && p->next.qgen != p->q.gen) next_close(p);  // the queue changed under it
    remap_step(p);  // library ids after a rescan

    sleep_tick(p);
    player_scrobble_poll(p);
    int32_t written = 0;
    if (p->state == PLAYER_PLAYING) {
        written = play_step(p);
    } else if (p->state == PLAYER_PAUSED) {
        prepare_paused(p);
    }
    status_tick(p);
    return written;
}

uint32_t player_wait_hint_ms(player_t *p) {
    if (!p) return 100;
    if (p->busy) return 0;
    if (p->state == PLAYER_PLAYING && p->sink) {
        if (p->drain != DRAIN_NONE) {
            uint32_t rate = p->sink_fmt.sample_rate ? p->sink_fmt.sample_rate : 44100u;
            uint32_t ms = (uint32_t)((uint64_t)sink_level(p) * 1000u / rate);
            return CORE_CLAMP(ms, 1u, 20u);
        }
        if (p->blocked || p->pend_frames) return 2;
        return 0;
    }
    if (p->sleep_on) return 100;
    return p->cfg.wake ? 1000 : 50;
}
