// SPDX-License-Identifier: Apache-2.0
/// \file
/// Byte streams used by decoders and tag parsers. A stream is either a plain file
/// (stdio FILE*, works on ESP-IDF VFS and on the host), a memory block, or a
/// platform-provided implementation (for example the readahead buffer in PSRAM).
#pragma once

#include "audio_player/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct core_stream core_stream_t;

typedef struct {
    /// Read up to len bytes. Returns bytes read (0 at EOF) or a negative core_err_t.
    int32_t (*read)(void *ctx, void *buf, size_t len);
    /// whence: 0 = SEEK_SET, 1 = SEEK_CUR, 2 = SEEK_END. Returns CORE_OK or error.
    int (*seek)(void *ctx, int64_t offset, int whence);
    int64_t (*tell)(void *ctx);
    int64_t (*size)(void *ctx);  ///< total size in bytes, -1 if unknown
    void (*close)(void *ctx);    ///< releases ctx
} core_stream_ops_t;

/// Wrap custom ops. The stream owns ctx and calls ops->close on core_stream_close.
core_stream_t *core_stream_create(const core_stream_ops_t *ops, void *ctx);

/// Open a file with an internal read buffer of buf_size bytes (0 = default 16 KiB).
core_stream_t *core_stream_open_file(const char *path, size_t buf_size);

/// Read-only view of memory. If take_ownership, core_free(data) on close.
core_stream_t *core_stream_open_memory(const void *data, size_t len, bool take_ownership);

int32_t core_stream_read(core_stream_t *s, void *buf, size_t len);
/// Reads exactly len bytes or fails with CORE_EOF / CORE_EIO.
int core_stream_read_exact(core_stream_t *s, void *buf, size_t len);
int core_stream_seek(core_stream_t *s, int64_t offset, int whence);
int core_stream_skip(core_stream_t *s, int64_t bytes);
int64_t core_stream_tell(core_stream_t *s);
int64_t core_stream_size(core_stream_t *s);
void core_stream_close(core_stream_t *s);

/// Little/big-endian readers. Return CORE_OK or error; value written to *out.
int core_stream_read_u8(core_stream_t *s, uint8_t *out);
int core_stream_read_le16(core_stream_t *s, uint16_t *out);
int core_stream_read_le32(core_stream_t *s, uint32_t *out);
int core_stream_read_le64(core_stream_t *s, uint64_t *out);
int core_stream_read_be16(core_stream_t *s, uint16_t *out);
int core_stream_read_be24(core_stream_t *s, uint32_t *out);
int core_stream_read_be32(core_stream_t *s, uint32_t *out);
int core_stream_read_be64(core_stream_t *s, uint64_t *out);

#ifdef __cplusplus
}
#endif
