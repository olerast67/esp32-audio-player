// SPDX-License-Identifier: Apache-2.0
/// \file
/// Playback engine: queue, gapless transitions, DSP, output negotiation, resume.
///
/// Threading model. The player never creates threads. Any thread may call the
/// player_cmd_* functions; they only enqueue a command (thread-safe, non-blocking).
/// One audio thread calls player_run_once() in a loop: it applies commands,
/// decodes, processes and writes to the sink (the sink write paces the loop).
/// On the host, tests call player_run_once() directly with a memory sink.
///
/// Gapless: when the current track has less than ~2 s left, the next decoder is
/// opened; if its format matches, frames continue without a gap. If the sample
/// rate family changes the sink is reopened (short mute). Precisely: when the negotiated
/// sink format (rate, bits, channels, DoP) of the next track is identical, frames continue
/// without a flush and the DSP/resampler state carries over; otherwise the old tail is
/// played out, the sink drains and is reopened with sink->open() (close + open when the
/// sink cannot reconfigure). Consecutive CUE tracks of one file share one decoder.
///
/// Signal path per chunk (default 1024 frames): decoder -> mono upmix -> resampler (only
/// when the sink rate differs, e.g. Bluetooth) -> DSP at the sink rate (dither last) ->
/// sink. DoP bypasses the resampler and the DSP. The status snapshot and EV_TRACK_CHANGED
/// follow what is audible: after a gapless switch they change once the tail of the
/// previous track has left the sink buffer (buffered_frames()).
///
/// Events: EV_PLAYER_STATE (a = state; also after repeat/shuffle changes), EV_TRACK_CHANGED
/// (a = queue index), EV_POSITION (a = ms, about 4 per second while playing and after a
/// seek), EV_VOLUME (f = dB), EV_QUEUE_CHANGED (a = queue length), EV_PLAYBACK_ERROR
/// (a = core_err_t, text = file name; CORE_EUNSUPPORTED for DoP on a sink without DoP),
/// EV_SLEEP_TIMER (a = seconds left, 0 = fired and paused, -1 = cancelled).
#pragma once

#include "audio_player/decoder.h"
#include "audio_player/dsp.h"
#include "audio_player/library.h"
#include "audio_player/sink.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PLAYER_STOPPED = 0, PLAYER_PLAYING, PLAYER_PAUSED } player_state_t;
typedef enum { REPEAT_OFF = 0, REPEAT_ALL, REPEAT_ONE } repeat_mode_t;

/// Queue capacity. Internally entries are stored compactly (library id, or a path in
/// a string pool in PSRAM), so a 20 000-track queue costs well under 1 MB.
#define PLAYER_QUEUE_MAX 20000

/// Lowest volume the player sets (volume steps stop here). The upper end is the user
/// limit (volume_max_db), never above 0 dB.
#define PLAYER_VOLUME_MIN_DB (-96.0f)

/// One queue entry. Either a library track (track_id) or a plain path (folders,
/// playlists). start_ms/end_ms describe CUE virtual tracks (0/0 = whole file).
/// For library tracks the path may be empty: the player resolves it through the library.
typedef struct {
    lib_id_t track_id;         ///< LIB_ID_NONE for files outside the library
    char path[CORE_PATH_MAX];
    uint32_t start_ms;
    uint32_t end_ms;
} queue_item_t;

typedef struct {
    player_state_t state;
    uint32_t queue_index;      ///< valid when queue_length > 0
    uint32_t queue_length;
    /// current track
    char path[CORE_PATH_MAX];
    char title[TAG_TEXT_MAX];  ///< falls back to the file name
    char artist[TAG_TEXT_MAX];
    char album[TAG_TEXT_MAX];
    uint16_t year;
    uint16_t track_no;
    lib_id_t track_id;
    uint64_t cover_offset;     ///< embedded cover in the file (0 = none)
    uint32_t cover_size;
    uint32_t position_ms;
    uint32_t duration_ms;      ///< 0 if unknown
    codec_id_t codec;
    uint32_t bitrate_kbps;
    audio_format_t src_fmt;    ///< decoded format
    audio_format_t out_fmt;    ///< format sent to the sink
    bool bitperfect;           ///< samples reach the DAC unchanged (SINK_CAP_BITPERFECT sink, DSP bypass, no resample)
    uint32_t dsp_flags;        ///< DSP_FLAG_* incl. DSP_FLAG_RESAMPLE
    float volume_db;           ///< current volume (hardware or software)
    float volume_max_db;       ///< user limit (hearing protection)
    bool shuffle;
    repeat_mode_t repeat;
    uint32_t sleep_left_s;     ///< 0 = off
    uint32_t underruns;
    char sink_name[16];
    char sink_detail[48];
} player_status_t;

typedef struct {
    audio_sink_t *sink;           ///< may be NULL at start; set later with player_set_sink
    library_t *library;           ///< optional: faster metadata and RG from the index
    /// Open a stream for a path (platform: readahead buffer in PSRAM). NULL = file stream.
    core_stream_t *(*open_stream)(const char *path, void *user);
    void *open_stream_user;
    const char *state_dir;        ///< resume state, queue.m3u8, .scrobbler.log live here
    bool scrobble_log;            ///< write Rockbox-format .scrobbler.log
    float volume_default_db;
    float volume_max_db;
    float volume_step_db;         ///< default 1.0
    /// Optional: called (from the commanding thread, outside any player lock) after each
    /// player_cmd_*, so an idle audio task can block on a semaphore instead of polling.
    void (*wake)(void *user);
    void *wake_user;
} player_config_t;

typedef struct player player_t;

player_t *player_create(const player_config_t *cfg);
void player_destroy(player_t *p);

// ---- commands (any thread) -------------------------------------------------
/// Queue semantics: the display order is the play order. Shuffle is a seeded Fisher-Yates
/// order with the current item first; switching it off restores the original order, and
/// neither direction interrupts the current track. play_next inserts after the current
/// item, enqueue into an empty queue does not start playback. At the end of the queue
/// (repeat off) the player stops and rewinds to the first item. next/prev keep the state
/// (paused stays paused with the new item prepared). Repeated volume and seek commands are
/// merged. Entries without id and path are ignored.
/// Replace the queue and start playing items[start]. shuffle=true shuffles the
/// queue but keeps items[start] first.
void player_cmd_play_items(player_t *p, const queue_item_t *items, uint32_t n, uint32_t start, bool shuffle);
void player_cmd_play_tracks(player_t *p, const lib_id_t *ids, uint32_t n, uint32_t start, bool shuffle);
/// Play the audio files of one folder (not its subfolders) in the order fs_list_dir() shows
/// them, starting at index `start`. Returns the number of files queued, CORE_ENOTFOUND when the
/// folder has none, or another negative core_err_t. At most PLAYER_FOLDER_MAX entries are read.
#define PLAYER_FOLDER_MAX 2000
int player_cmd_play_folder(player_t *p, const char *dir, uint32_t start, bool shuffle);
void player_cmd_enqueue(player_t *p, const queue_item_t *items, uint32_t n, bool play_next);
void player_cmd_queue_remove(player_t *p, uint32_t index);
void player_cmd_queue_jump(player_t *p, uint32_t index);
void player_cmd_queue_clear(player_t *p);
void player_cmd_toggle_pause(player_t *p);
void player_cmd_pause(player_t *p);
void player_cmd_resume(player_t *p);         ///< from STOPPED with a non-empty queue: starts the current item
void player_cmd_stop(player_t *p);
void player_cmd_next(player_t *p);
void player_cmd_prev(player_t *p);           ///< restart the track if more than 3 s played
void player_cmd_seek_ms(player_t *p, uint32_t ms);
void player_cmd_seek_relative_ms(player_t *p, int32_t delta_ms);
void player_cmd_set_volume_db(player_t *p, float db);
void player_cmd_volume_step(player_t *p, int steps);
void player_cmd_set_shuffle(player_t *p, bool on);
void player_cmd_set_repeat(player_t *p, repeat_mode_t mode);
/// sw_volume, volume_db and out_bits are ignored: the player sets them from its volume, the
/// sink capabilities (SINK_CAP_HW_VOLUME; SINK_CAP_DITHERS and SINK_CAP_BITPERFECT for the
/// dither resolution, see sink.h) and the negotiated sink format.
void player_cmd_set_dsp(player_t *p, const dsp_config_t *cfg);
void player_cmd_set_gapless(player_t *p, bool on);
void player_cmd_set_sleep_timer(player_t *p, uint32_t minutes);  ///< 0 = off; fades out over 30 s
void player_cmd_set_volume_limit(player_t *p, float max_db);
/// Switch output (mode change). The player stops writing to the old sink, closes it
/// and continues on the new one from the same position. NULL = no output (paused).
void player_cmd_set_sink(player_t *p, audio_sink_t *sink);

// ---- queries (any thread, copies under lock) ---------------------------------
void player_get_status(player_t *p, player_status_t *out);
uint32_t player_queue_count(player_t *p);
/// Display order = play order (the shuffled order while shuffle is on). Library entries
/// get their path filled in from the library. CORE_ENOTFOUND past the end.
int player_queue_get(player_t *p, uint32_t index, queue_item_t *out);  ///< display order

// ---- persistence -------------------------------------------------------------
/// Audiobook positions are NOT stored by the player: the application calls library_save_position()
/// (on pause, chapter change and every 30 s) and seeks on "Continue".
/// Save queue (queue.m3u8), index, position, volume, shuffle/repeat to state_dir.
/// Any thread; the queue is copied in small slices, so the audio thread is never held up.
/// queue.m3u8 is a valid UTF-8 playlist in play order with absolute paths; "#EXT-EMP-"
/// comment lines carry the original order, CUE ranges and library ids. Both files are
/// replaced atomically (tmp, remove, rename). player.ini is written on every call; queue.m3u8
/// only when the queue (items or order) changed since this player last wrote it, or the file
/// is missing, so periodic saves while playing cost one small write.
int player_save_state(player_t *p);
/// Load state; starts paused at the saved position unless autoplay. Paths are resolved to
/// library ids where possible. CORE_ENOTFOUND when there is no saved state.
int player_restore_state(player_t *p, bool autoplay);

// ---- audio thread -------------------------------------------------------------
/// One iteration. Returns frames written to the sink, 0 when idle (stopped/paused/no
/// sink): the caller should then wait for a command (player_wait_hint_ms()).
int32_t player_run_once(player_t *p);
/// How long the caller may sleep after player_run_once() returned 0: 0 when there is more
/// work right away, a few ms while the sink is full, longer while idle.
uint32_t player_wait_hint_ms(player_t *p);
/// Frames the audio thread tries to decode per iteration (default 1024).
void player_set_chunk_frames(player_t *p, uint32_t frames);

#ifdef __cplusplus
}
#endif
