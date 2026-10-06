// SPDX-License-Identifier: Apache-2.0
#include "audio_player/base.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------- errors ----
const char *core_err_name(int err) {
    switch (err) {
    case CORE_OK: return "ok";
    case CORE_ERR: return "error";
    case CORE_EOF: return "end of stream";
    case CORE_ENOMEM: return "out of memory";
    case CORE_EINVAL: return "invalid argument";
    case CORE_EIO: return "i/o error";
    case CORE_EUNSUPPORTED: return "unsupported";
    case CORE_EAGAIN: return "try again";
    case CORE_ENOTFOUND: return "not found";
    case CORE_ECORRUPT: return "corrupt data";
    default: return "unknown error";
    }
}

// ---------------------------------------------------------------- memory ----
static void *std_alloc(size_t size) { return malloc(size); }
static void *std_realloc(void *p, size_t size) { return realloc(p, size); }
static void std_free(void *p) { free(p); }

static core_allocator_t s_alloc = {std_alloc, std_alloc, std_realloc, std_free};

void core_set_allocator(const core_allocator_t *a) {
    if (a && a->alloc_large && a->alloc_fast && a->realloc_large && a->free) {
        s_alloc = *a;
    } else {
        s_alloc.alloc_large = std_alloc;
        s_alloc.alloc_fast = std_alloc;
        s_alloc.realloc_large = std_realloc;
        s_alloc.free = std_free;
    }
}

void *core_malloc(size_t size) { return s_alloc.alloc_large(size ? size : 1); }
void *core_malloc_fast(size_t size) { return s_alloc.alloc_fast(size ? size : 1); }

void *core_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
    void *p = core_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void *core_realloc(void *ptr, size_t size) {
    if (size == 0) {
        core_free(ptr);
        return NULL;
    }
    return s_alloc.realloc_large(ptr, size);
}

void core_free(void *ptr) {
    if (ptr) s_alloc.free(ptr);
}

char *core_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = core_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

// --------------------------------------------------------------- logging ----
static void std_log_sink(core_log_level_t level, const char *tag, const char *msg) {
    static const char lv[] = {'?', 'E', 'W', 'I', 'D'};
    fprintf(stderr, "%c (%s) %s\n", lv[level <= CORE_LOG_DEBUG ? level : 0], tag, msg);
}

static core_log_sink_t s_log_sink = std_log_sink;
static core_log_level_t s_log_max = CORE_LOG_INFO;

void core_set_log_sink(core_log_sink_t sink, core_log_level_t max_level) {
    s_log_sink = sink ? sink : std_log_sink;
    s_log_max = max_level;
}

void core_log(core_log_level_t level, const char *tag, const char *fmt, ...) {
    if (level > s_log_max) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s_log_sink(level, tag ? tag : "core", buf);
}

// ----------------------------------------------------------------- locks ----
static core_lock_ops_t s_lock_ops;  // all NULL = no-op
static int s_dummy_mutex;

void core_set_lock_ops(const core_lock_ops_t *ops) {
    if (ops) {
        s_lock_ops = *ops;
    } else {
        memset(&s_lock_ops, 0, sizeof s_lock_ops);
    }
}

core_mutex_t *core_mutex_create(void) {
    if (s_lock_ops.create) return (core_mutex_t *)s_lock_ops.create();
    return (core_mutex_t *)&s_dummy_mutex;
}

void core_mutex_lock(core_mutex_t *m) {
    if (m && s_lock_ops.lock) s_lock_ops.lock(m);
}

void core_mutex_unlock(core_mutex_t *m) {
    if (m && s_lock_ops.unlock) s_lock_ops.unlock(m);
}

void core_mutex_destroy(core_mutex_t *m) {
    if (m && s_lock_ops.destroy && m != (core_mutex_t *)&s_dummy_mutex) s_lock_ops.destroy(m);
}

// ------------------------------------------------------------------ time ----
static uint32_t std_clock(void) { return (uint32_t)((uint64_t)clock() * 1000u / CLOCKS_PER_SEC); }
static core_clock_fn_t s_clock = std_clock;

void core_set_clock(core_clock_fn_t fn) { s_clock = fn ? fn : std_clock; }
uint32_t core_now_ms(void) { return s_clock(); }

// --------------------------------------------------------------- strings ----
size_t core_strlcpy(char *dst, const char *src, size_t size) {
    size_t n = src ? strlen(src) : 0;
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        if (c) memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

size_t core_strlcat(char *dst, const char *src, size_t size) {
    size_t d = strnlen(dst, size);
    if (d == size) return size + (src ? strlen(src) : 0);
    return d + core_strlcpy(dst + d, src, size - d);
}

static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

bool core_str_ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n) return false;
    s += n - m;
    for (size_t i = 0; i < m; i++) {
        if (ascii_lower((unsigned char)s[i]) != ascii_lower((unsigned char)suffix[i])) return false;
    }
    return true;
}

const char *core_path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

const char *core_path_ext(const char *path) {
    const char *base = core_path_basename(path);
    const char *dot = strrchr(base, '.');
    return (dot && dot != base) ? dot + 1 : "";
}

void core_path_dirname(const char *path, char *out, size_t out_size) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        core_strlcpy(out, ".", out_size);
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n == 0) n = 1;  // "/file" -> "/"
    if (n >= out_size) n = out_size - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

int core_path_join(char *out, size_t out_size, const char *dir, const char *name) {
    size_t dl = strlen(dir);
    bool need_slash = dl > 0 && dir[dl - 1] != '/';
    int n = snprintf(out, out_size, "%s%s%s", dir, need_slash ? "/" : "", name);
    return (n < 0 || (size_t)n >= out_size) ? CORE_EINVAL : CORE_OK;
}
