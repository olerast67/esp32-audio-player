// SPDX-License-Identifier: Apache-2.0
#include "audio_player/stream.h"

#include <stdio.h>
#include <string.h>

struct core_stream {
    const core_stream_ops_t *ops;
    void *ctx;
};

core_stream_t *core_stream_create(const core_stream_ops_t *ops, void *ctx) {
    if (!ops || !ops->read) return NULL;
    core_stream_t *s = core_malloc_fast(sizeof *s);
    if (!s) {
        if (ops->close) ops->close(ctx);
        return NULL;
    }
    s->ops = ops;
    s->ctx = ctx;
    return s;
}

int32_t core_stream_read(core_stream_t *s, void *buf, size_t len) {
    if (!s || !buf) return CORE_EINVAL;
    if (len == 0) return 0;
    return s->ops->read(s->ctx, buf, len);
}

int core_stream_read_exact(core_stream_t *s, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) {
        int32_t n = core_stream_read(s, p, len);
        if (n < 0) return n;
        if (n == 0) return CORE_EOF;
        p += n;
        len -= (size_t)n;
    }
    return CORE_OK;
}

int core_stream_seek(core_stream_t *s, int64_t offset, int whence) {
    if (!s || !s->ops->seek) return CORE_EUNSUPPORTED;
    return s->ops->seek(s->ctx, offset, whence);
}

int core_stream_skip(core_stream_t *s, int64_t bytes) {
    if (bytes <= 0) return bytes == 0 ? CORE_OK : core_stream_seek(s, bytes, 1);
    if (s->ops->seek) return core_stream_seek(s, bytes, 1);
    uint8_t tmp[256];
    while (bytes > 0) {
        int32_t n = core_stream_read(s, tmp, (size_t)CORE_MIN(bytes, (int64_t)sizeof tmp));
        if (n <= 0) return n < 0 ? n : CORE_EOF;
        bytes -= n;
    }
    return CORE_OK;
}

int64_t core_stream_tell(core_stream_t *s) { return (s && s->ops->tell) ? s->ops->tell(s->ctx) : -1; }
int64_t core_stream_size(core_stream_t *s) { return (s && s->ops->size) ? s->ops->size(s->ctx) : -1; }

void core_stream_close(core_stream_t *s) {
    if (!s) return;
    if (s->ops->close) s->ops->close(s->ctx);
    core_free(s);
}

#define READ_N(name, type, n, expr)                                                                                  \
    int name(core_stream_t *s, type *out) {                                                                          \
        uint8_t b[n];                                                                                                \
        int r = core_stream_read_exact(s, b, n);                                                                     \
        if (r) return r;                                                                                             \
        *out = (type)(expr);                                                                                         \
        return CORE_OK;                                                                                              \
    }

READ_N(core_stream_read_u8, uint8_t, 1, b[0])
READ_N(core_stream_read_le16, uint16_t, 2, b[0] | (uint16_t)b[1] << 8)
READ_N(core_stream_read_le32, uint32_t, 4, b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24)
READ_N(core_stream_read_be16, uint16_t, 2, (uint16_t)b[0] << 8 | b[1])
READ_N(core_stream_read_be24, uint32_t, 3, (uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2])
READ_N(core_stream_read_be32, uint32_t, 4, (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3])

int core_stream_read_le64(core_stream_t *s, uint64_t *out) {
    uint32_t lo, hi;
    int r = core_stream_read_le32(s, &lo);
    if (!r) r = core_stream_read_le32(s, &hi);
    if (!r) *out = (uint64_t)hi << 32 | lo;
    return r;
}

int core_stream_read_be64(core_stream_t *s, uint64_t *out) {
    uint32_t lo, hi;
    int r = core_stream_read_be32(s, &hi);
    if (!r) r = core_stream_read_be32(s, &lo);
    if (!r) *out = (uint64_t)hi << 32 | lo;
    return r;
}

// ------------------------------------------------------------ file stream ----
typedef struct {
    FILE *f;
    uint8_t *buf;
    size_t cap, len, pos;  // buffered window [0, len), read position pos
    int64_t buf_start;     // file offset of buf[0]
    int64_t size;
} file_ctx_t;

static int32_t file_read(void *c, void *dst, size_t len) {
    file_ctx_t *fc = c;
    uint8_t *out = dst;
    size_t done = 0;
    while (done < len) {
        if (fc->pos < fc->len) {
            size_t n = CORE_MIN(len - done, fc->len - fc->pos);
            memcpy(out + done, fc->buf + fc->pos, n);
            fc->pos += n;
            done += n;
            continue;
        }
        // Large reads bypass the buffer.
        if (len - done >= fc->cap) {
            size_t n = fread(out + done, 1, len - done, fc->f);
            fc->buf_start += (int64_t)(fc->len + n);
            fc->len = fc->pos = 0;
            done += n;
            if (n == 0) break;
            continue;
        }
        fc->buf_start += (int64_t)fc->len;
        fc->len = fread(fc->buf, 1, fc->cap, fc->f);
        fc->pos = 0;
        if (fc->len == 0) break;
    }
    if (ferror(fc->f)) {
        // A read error, not the end of the file, even after part of the data: callers
        // that stop at a short read must not take damaged data for a complete file.
        clearerr(fc->f);
        fc->len = fc->pos = 0;
        fseek(fc->f, (long)fc->buf_start, SEEK_SET);  // keep the FILE position in step with tell()
        return CORE_EIO;
    }
    return (int32_t)done;
}

static int64_t file_tell(void *c) {
    file_ctx_t *fc = c;
    return fc->buf_start + (int64_t)fc->pos;
}

static int file_seek(void *c, int64_t off, int whence) {
    file_ctx_t *fc = c;
    int64_t target = off;
    if (whence == 1) target = file_tell(c) + off;
    else if (whence == 2) target = fc->size + off;
    if (target < 0) return CORE_EINVAL;
    // Inside the current buffer: just move the cursor.
    if (target >= fc->buf_start && target <= fc->buf_start + (int64_t)fc->len) {
        fc->pos = (size_t)(target - fc->buf_start);
        return CORE_OK;
    }
    if (fseek(fc->f, (long)target, SEEK_SET) != 0) return CORE_EIO;
    fc->buf_start = target;
    fc->len = fc->pos = 0;
    return CORE_OK;
}

static int64_t file_size(void *c) { return ((file_ctx_t *)c)->size; }

static void file_close(void *c) {
    file_ctx_t *fc = c;
    if (fc->f) fclose(fc->f);
    core_free(fc->buf);
    core_free(fc);
}

static const core_stream_ops_t s_file_ops = {file_read, file_seek, file_tell, file_size, file_close};

core_stream_t *core_stream_open_file(const char *path, size_t buf_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    file_ctx_t *fc = core_calloc(1, sizeof *fc);
    if (!fc) {
        fclose(f);
        return NULL;
    }
    fc->f = f;
    fc->cap = buf_size ? buf_size : 16 * 1024;
    fc->buf = core_malloc(fc->cap);
    if (!fc->buf) {
        file_close(fc);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0) {
        fc->size = (int64_t)ftell(f);
        fseek(f, 0, SEEK_SET);
    } else {
        fc->size = -1;
    }
    return core_stream_create(&s_file_ops, fc);
}

// ---------------------------------------------------------- memory stream ----
typedef struct {
    const uint8_t *data;
    size_t len, pos;
    bool owned;
} mem_ctx_t;

static int32_t mem_read(void *c, void *dst, size_t len) {
    mem_ctx_t *m = c;
    size_t n = CORE_MIN(len, m->len - m->pos);
    memcpy(dst, m->data + m->pos, n);
    m->pos += n;
    return (int32_t)n;
}

static int mem_seek(void *c, int64_t off, int whence) {
    mem_ctx_t *m = c;
    int64_t t = whence == 0 ? off : whence == 1 ? (int64_t)m->pos + off : (int64_t)m->len + off;
    if (t < 0 || t > (int64_t)m->len) return CORE_EINVAL;
    m->pos = (size_t)t;
    return CORE_OK;
}

static int64_t mem_tell(void *c) { return (int64_t)((mem_ctx_t *)c)->pos; }
static int64_t mem_size(void *c) { return (int64_t)((mem_ctx_t *)c)->len; }

static void mem_close(void *c) {
    mem_ctx_t *m = c;
    if (m->owned) core_free((void *)m->data);
    core_free(m);
}

static const core_stream_ops_t s_mem_ops = {mem_read, mem_seek, mem_tell, mem_size, mem_close};

core_stream_t *core_stream_open_memory(const void *data, size_t len, bool take_ownership) {
    mem_ctx_t *m = core_malloc_fast(sizeof *m);
    if (!m) return NULL;
    m->data = data;
    m->len = len;
    m->pos = 0;
    m->owned = take_ownership;
    return core_stream_create(&s_mem_ops, m);
}
