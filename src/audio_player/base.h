// SPDX-License-Identifier: Apache-2.0
/// \file
/// Base definitions shared by every core module: error codes, memory, logging, locks.
/// The core is portable C17 without ESP-IDF headers: it builds for ESP32 and for the host PC.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
/// Errors. Functions return 0 (CORE_OK) or a negative code.
// ---------------------------------------------------------------------------
typedef enum {
    CORE_OK = 0,
    CORE_ERR = -1,           ///< generic failure
    CORE_EOF = -2,           ///< end of stream
    CORE_ENOMEM = -3,
    CORE_EINVAL = -4,
    CORE_EIO = -5,
    CORE_EUNSUPPORTED = -6,  ///< format or feature not supported
    CORE_EAGAIN = -7,        ///< try again later (non-blocking)
    CORE_ENOTFOUND = -8,
    CORE_ECORRUPT = -9,      ///< malformed data
} core_err_t;

const char *core_err_name(int err);

#define CORE_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CORE_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CORE_MAX(a, b) ((a) > (b) ? (a) : (b))
#define CORE_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

/// Maximum path length used by the core (bytes, UTF-8, including terminator).
#define CORE_PATH_MAX 256

// ---------------------------------------------------------------------------
/// Memory. On ESP32 the platform routes "large" allocations to PSRAM and "fast"
/// allocations to internal RAM. On the host both map to malloc.
// ---------------------------------------------------------------------------
typedef struct {
    void *(*alloc_large)(size_t size);             ///< big buffers: decoders, readahead, caches (PSRAM)
    void *(*alloc_fast)(size_t size);              ///< small hot buffers (internal RAM)
    void *(*realloc_large)(void *ptr, size_t size);  ///< grows memory from either allocator
    void (*free)(void *ptr);                       ///< frees memory from either allocator
} core_allocator_t;

void core_set_allocator(const core_allocator_t *a);  ///< NULL restores stdlib defaults
void *core_malloc(size_t size);                     ///< large (PSRAM on device)
void *core_calloc(size_t n, size_t size);           ///< large, zeroed
void *core_malloc_fast(size_t size);                ///< fast (internal RAM on device)
void *core_realloc(void *ptr, size_t size);         ///< large
void core_free(void *ptr);
char *core_strdup(const char *s);

// ---------------------------------------------------------------------------
/// Logging. The platform installs a sink; default prints to stderr.
// ---------------------------------------------------------------------------
typedef enum { CORE_LOG_ERROR = 1, CORE_LOG_WARN, CORE_LOG_INFO, CORE_LOG_DEBUG } core_log_level_t;
typedef void (*core_log_sink_t)(core_log_level_t level, const char *tag, const char *msg);
void core_set_log_sink(core_log_sink_t sink, core_log_level_t max_level);
void core_log(core_log_level_t level, const char *tag, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
#define CORE_LOGE(tag, ...) core_log(CORE_LOG_ERROR, tag, __VA_ARGS__)
#define CORE_LOGW(tag, ...) core_log(CORE_LOG_WARN, tag, __VA_ARGS__)
#define CORE_LOGI(tag, ...) core_log(CORE_LOG_INFO, tag, __VA_ARGS__)
#define CORE_LOGD(tag, ...) core_log(CORE_LOG_DEBUG, tag, __VA_ARGS__)

// ---------------------------------------------------------------------------
/// Locks. The core never creates threads. Modules that can be called from
/// several threads (player commands, library queries) protect shared state with
/// these ops. Default: no-op (single-threaded host tests).
// ---------------------------------------------------------------------------
typedef struct {
    void *(*create)(void);
    void (*lock)(void *m);
    void (*unlock)(void *m);
    void (*destroy)(void *m);
} core_lock_ops_t;

void core_set_lock_ops(const core_lock_ops_t *ops);  ///< NULL restores no-op
typedef struct core_mutex core_mutex_t;
core_mutex_t *core_mutex_create(void);
void core_mutex_lock(core_mutex_t *m);
void core_mutex_unlock(core_mutex_t *m);
void core_mutex_destroy(core_mutex_t *m);

// ---------------------------------------------------------------------------
/// Time. Monotonic milliseconds, installed by the platform (default: clock()).
// ---------------------------------------------------------------------------
typedef uint32_t (*core_clock_fn_t)(void);
void core_set_clock(core_clock_fn_t fn);
uint32_t core_now_ms(void);

// ---------------------------------------------------------------------------
/// Small string helpers (UTF-8 safe where noted).
// ---------------------------------------------------------------------------
size_t core_strlcpy(char *dst, const char *src, size_t size);  ///< always terminates
size_t core_strlcat(char *dst, const char *src, size_t size);
bool core_str_ends_with_ci(const char *s, const char *suffix);  ///< ASCII case-insensitive
const char *core_path_ext(const char *path);                     ///< pointer after last '.', "" if none
const char *core_path_basename(const char *path);
void core_path_dirname(const char *path, char *out, size_t out_size);
int core_path_join(char *out, size_t out_size, const char *dir, const char *name);

#ifdef __cplusplus
}
#endif
