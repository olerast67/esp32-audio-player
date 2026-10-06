// SPDX-License-Identifier: Apache-2.0
// Private helpers shared by the sources of the library module (tags, index, folders,
// playlists). Not a public contract: only files in src/library include it.
#pragma once

#include <stdio.h>

#include "audio_player/base.h"
#include "audio_player/collate.h"
#include "audio_player/stream.h"
#include "audio_player/tags.h"
#include "audio_player/text.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Hashes and checksums
// ---------------------------------------------------------------------------
#define LIB_FNV32_INIT 0x811C9DC5u
#define LIB_FNV64_INIT 0xCBF29CE484222325ull

uint32_t lib_crc32(uint32_t crc, const void *data, size_t len);  // start with crc = 0
uint32_t lib_fnv1a32(uint32_t h, const void *data, size_t len);
uint64_t lib_fnv1a64(uint64_t h, const void *data, size_t len);
uint32_t lib_path_hash(const char *path);    // 32-bit FNV-1a of the UTF-8 bytes
uint64_t lib_path_hash64(const char *path);  // 64-bit FNV-1a

// ---------------------------------------------------------------------------
// Portable file system layer (POSIX dirent/stat on ESP-IDF and Linux, mingw on
// Windows). The core never includes ESP-IDF headers, so this is plain libc.
// ---------------------------------------------------------------------------
typedef struct {
    bool is_dir;
    uint64_t size;
    uint32_t mtime;  // seconds, 0 if unknown
} lfs_stat_t;

typedef enum { LFS_TYPE_UNKNOWN = 0, LFS_TYPE_FILE, LFS_TYPE_DIR } lfs_type_t;

typedef struct {
    const char *name;  // valid until the next lfs_readdir()
    lfs_type_t type;   // from d_type when the platform has it, else UNKNOWN
} lfs_dirent_t;

typedef struct lfs_dir lfs_dir_t;

int lfs_stat(const char *path, lfs_stat_t *st);
bool lfs_exists(const char *path);
lfs_dir_t *lfs_opendir(const char *path);
bool lfs_readdir(lfs_dir_t *d, lfs_dirent_t *e);  // skips "." and ".."
bool lfs_dir_failed(const lfs_dir_t *d);          // the last lfs_readdir() ended on an error
void lfs_closedir(lfs_dir_t *d);
int lfs_mkdir(const char *path);   // CORE_OK if it already exists
int lfs_mkdirs(const char *path);  // creates missing parents too
int lfs_remove(const char *path);  // CORE_OK if missing
int lfs_rmdir(const char *path);   // empty directory; CORE_OK if missing
// Replace `to` with `from`: rename, or remove + rename where rename cannot overwrite (FAT, Windows).
int lfs_replace(const char *from, const char *to);
// Flush stdio buffers and ask the OS to write the file to the medium.
int lfs_fsync(FILE *f);
// Folders the scanner and the browser never enter: hidden (".x"), "System Volume Information", "$RECYCLE.BIN".
bool lfs_is_skipped_dir(const char *name);

// Write a whole file atomically: data goes to "<path>.tmp", is synced, then replaces path.
int lfs_write_atomic(const char *path, const void *data, size_t len);

// ---------------------------------------------------------------------------
// Collation internals (collate.c)
// ---------------------------------------------------------------------------
// Primary sort key as bytes: memcmp order of two keys (shorter first on a common
// prefix) equals the primary order of collate_cmp(). Returns the full key length
// even when it exceeds max (only max bytes are written).
size_t collate_key(const char *s, const collate_opts_t *opts, uint8_t *out, size_t max);
// Primary comparison only (no tie-break on the raw bytes).
int collate_cmp_primary(const char *a, const char *b, const collate_opts_t *opts);
uint32_t collate_hash_primary(const char *s, const collate_opts_t *opts);
// Order of jump letters consistent with the sort order: '#' < 'A'..'Z' < 'А'..'Я' < others.
uint32_t collate_letter_rank(uint32_t letter);
int collate_key_cmp(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);

// ---------------------------------------------------------------------------
// Tag helpers shared by the container parsers (tags*.c)
// ---------------------------------------------------------------------------
#define TAG_RAW_MAX 1024  // bytes of one text value read from a file (longer values are cut)

// Byte source used by the Vorbis-comment parser: a bounded region of a stream or an
// Ogg packet spread over pages.
typedef struct tag_src tag_src_t;
struct tag_src {
    int32_t (*read)(tag_src_t *src, void *buf, size_t len);  // bytes read, 0 at end, <0 error
    int (*skip)(tag_src_t *src, uint64_t len);                // CORE_OK or error
};

// Stream region [start, start+len) as a tag source.
typedef struct {
    tag_src_t base;
    core_stream_t *s;
    uint64_t left;
} tag_region_t;
void tag_region_init(tag_region_t *r, core_stream_t *s, uint64_t len);  // from the current position

// Set a text field from raw bytes in the given encoding: converts to UTF-8, trims,
// truncates to the field size. Does nothing if overwrite is false and dst is set.
void tagf_set(char *dst, size_t cap, const uint8_t *src, size_t len, text_encoding_t enc, bool overwrite);
void tagf_set_utf8(char *dst, size_t cap, const char *src, bool overwrite);
// Length of p[0..n) without a UTF-8 sequence cut off at the end (use on values that
// were truncated to a buffer size, so a valid UTF-8 value is not taken for CP1251).
size_t tagf_trim_partial_utf8(const uint8_t *p, size_t n);
// "3/12" -> 3 and 12 (either may stay unchanged when missing).
void tagf_parse_pair(const char *s, uint16_t *no, uint16_t *total, bool overwrite);
uint16_t tagf_parse_year(const char *s);  // first 4-digit group, 0 if none
float tagf_parse_float(const char *s, bool *ok);  // "-6.54 dB", "0.98", locale independent, float only
// ID3v1 genre index -> name (NULL if out of range).
const char *tags_id3v1_genre(unsigned idx);
// Normalize an ID3 TCON value in place: "(13)" / "13" / "(13)Pop" / "(RX)" -> names.
void tags_normalize_genre(char *genre, size_t cap);
// Apply one "KEY=VALUE" pair (Vorbis comment, APEv2, MP4 freeform, RIFF INFO mapping).
// Keys are case-insensitive. overwrite=false only fills empty fields.
void tags_apply_kv(track_tags_t *t, const char *key, const char *value, bool overwrite);
bool tags_is_audiobook_genre(const char *genre);

// Container parsers. `start` is the absolute offset of the container in the stream.
int tags_parse_id3v2(core_stream_t *s, int64_t start, track_tags_t *t, uint32_t *total_size);
int tags_parse_id3v1(core_stream_t *s, int64_t file_size, track_tags_t *t);   // CORE_ENOTFOUND if absent
// APEv2 before an optional ID3v1 at the end; *tag_start receives the tag start (or end offset).
int tags_parse_apev2(core_stream_t *s, int64_t end, track_tags_t *t, int64_t *tag_start);
int tags_parse_mpeg(core_stream_t *s, int64_t start, int64_t end, track_tags_t *t);  // MP3 / ADTS
int tags_parse_flac(core_stream_t *s, int64_t start, track_tags_t *t);
int tags_parse_ogg(core_stream_t *s, int64_t start, int64_t file_size, track_tags_t *t);
int tags_parse_vorbis_comment(tag_src_t *src, track_tags_t *t);
int tags_parse_wav(core_stream_t *s, int64_t start, track_tags_t *t);
int tags_parse_aiff(core_stream_t *s, int64_t start, track_tags_t *t);
int tags_parse_dsf(core_stream_t *s, int64_t start, track_tags_t *t);
int tags_parse_dff(core_stream_t *s, int64_t start, track_tags_t *t);
int tags_parse_wavpack(core_stream_t *s, int64_t start, track_tags_t *t);
int tags_parse_ape(core_stream_t *s, int64_t start, track_tags_t *t);
int tags_parse_mp4(core_stream_t *s, int64_t start, int64_t file_size, track_tags_t *t);

// Image type from the first bytes: 1 JPEG, 2 PNG, 0 unknown.
uint8_t tags_image_type(const uint8_t *head, size_t len);

static inline uint32_t tags_rd_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline uint32_t tags_rd_le32(const uint8_t *p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
static inline uint16_t tags_rd_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint16_t tags_rd_le16(const uint8_t *p) { return (uint16_t)(p[1] << 8 | p[0]); }
static inline uint64_t tags_rd_le64(const uint8_t *p) {
    return (uint64_t)tags_rd_le32(p + 4) << 32 | tags_rd_le32(p);
}
static inline uint64_t tags_rd_be64(const uint8_t *p) {
    return (uint64_t)tags_rd_be32(p) << 32 | tags_rd_be32(p + 4);
}

// Read exactly len bytes at an absolute offset.
int tags_read_at(core_stream_t *s, int64_t off, void *buf, size_t len);

#ifdef __cplusplus
}
#endif
