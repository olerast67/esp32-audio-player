// SPDX-License-Identifier: Apache-2.0
// SPSC FIFO of 32-bit stereo frames (see i2s_fifo.h).
#include "port/i2s_fifo.h"

#include <string.h>

#include "audio_player/base.h"

#define LOAD_RELAXED(p) atomic_load_explicit((p), memory_order_relaxed)
#define LOAD_ACQUIRE(p) atomic_load_explicit((p), memory_order_acquire)
#define STORE_RELAXED(p, v) atomic_store_explicit((p), (v), memory_order_relaxed)
#define STORE_RELEASE(p, v) atomic_store_explicit((p), (v), memory_order_release)

uint32_t ap_i2s_fifo_frames_for_ms(uint32_t rate, uint32_t ms) {
    uint64_t f = ((uint64_t)rate * ms + 999u) / 1000u;
    return f > UINT32_MAX ? UINT32_MAX : (uint32_t)f;
}

// Positions run over 0 .. 2 * capacity - 1: equal = empty, capacity apart = full.
static uint32_t used_of(const ap_i2s_fifo_t *f, uint32_t w, uint32_t r) {
    return w >= r ? w - r : w + 2u * f->capacity - r;
}

static uint32_t index_of(const ap_i2s_fifo_t *f, uint32_t pos) { return pos < f->capacity ? pos : pos - f->capacity; }

static uint32_t advance(const ap_i2s_fifo_t *f, uint32_t pos, uint32_t n) {
    uint32_t p = pos + n;  // pos < 2 * capacity, n <= capacity: no overflow below 2^24 frames
    return p >= 2u * f->capacity ? p - 2u * f->capacity : p;
}

int ap_i2s_fifo_init(ap_i2s_fifo_t *f, uint32_t capacity_frames) {
    if (!f) return CORE_EINVAL;
    memset(f, 0, sizeof *f);
    if (!capacity_frames || capacity_frames > AP_I2S_FIFO_MAX_FRAMES) return CORE_EINVAL;
    f->buf = (int32_t *)core_malloc((size_t)capacity_frames * AP_I2S_FIFO_FRAME_BYTES);
    if (!f->buf) return CORE_ENOMEM;
    f->capacity = capacity_frames;
    f->limit = capacity_frames;
    ap_i2s_fifo_reset(f);
    return CORE_OK;
}

void ap_i2s_fifo_free(ap_i2s_fifo_t *f) {
    if (!f) return;
    core_free(f->buf);
    f->buf = NULL;
    f->capacity = f->limit = 0;
    STORE_RELAXED(&f->wr, 0);
    STORE_RELAXED(&f->rd, 0);
}

void ap_i2s_fifo_reset(ap_i2s_fifo_t *f) {
    if (!f) return;
    STORE_RELAXED(&f->wr, 0);
    STORE_RELAXED(&f->rd, 0);
    atomic_thread_fence(memory_order_seq_cst);
}

void ap_i2s_fifo_set_limit(ap_i2s_fifo_t *f, uint32_t frames) {
    if (!f || !f->capacity) return;
    if (frames < 1) frames = 1;
    if (frames > f->capacity) frames = f->capacity;
    f->limit = frames;
}

uint32_t ap_i2s_fifo_level(const ap_i2s_fifo_t *f) {
    if (!f || !f->buf) return 0;
    uint32_t w = LOAD_ACQUIRE((_Atomic uint32_t *)&f->wr);
    uint32_t r = LOAD_ACQUIRE((_Atomic uint32_t *)&f->rd);
    return used_of(f, w, r);
}

uint32_t ap_i2s_fifo_space(const ap_i2s_fifo_t *f) {
    if (!f || !f->buf) return 0;
    uint32_t w = LOAD_RELAXED((_Atomic uint32_t *)&f->wr);
    uint32_t r = LOAD_ACQUIRE((_Atomic uint32_t *)&f->rd);
    uint32_t used = used_of(f, w, r);
    return used >= f->limit ? 0 : f->limit - used;
}

uint32_t ap_i2s_fifo_write(ap_i2s_fifo_t *f, const int32_t *pcm, uint32_t frames) {
    if (!f || !f->buf || !pcm || !frames) return 0;
    uint32_t n = ap_i2s_fifo_space(f);
    if (frames < n) n = frames;
    if (!n) return 0;
    uint32_t w = LOAD_RELAXED(&f->wr);
    uint32_t idx = index_of(f, w);
    uint32_t first = f->capacity - idx;
    if (first > n) first = n;
    memcpy(f->buf + (size_t)idx * 2u, pcm, (size_t)first * AP_I2S_FIFO_FRAME_BYTES);
    if (n > first) memcpy(f->buf, pcm + (size_t)first * 2u, (size_t)(n - first) * AP_I2S_FIFO_FRAME_BYTES);
    STORE_RELEASE(&f->wr, advance(f, w, n));
    return n;
}

uint32_t ap_i2s_fifo_peek(const ap_i2s_fifo_t *f, const int32_t **span) {
    if (span) *span = NULL;
    if (!f || !f->buf) return 0;
    uint32_t w = LOAD_ACQUIRE((_Atomic uint32_t *)&f->wr);
    uint32_t r = LOAD_RELAXED((_Atomic uint32_t *)&f->rd);
    uint32_t avail = used_of(f, w, r);
    if (!avail) return 0;
    uint32_t idx = index_of(f, r);
    uint32_t run = f->capacity - idx;
    if (run > avail) run = avail;
    if (span) *span = f->buf + (size_t)idx * 2u;
    return run;
}

void ap_i2s_fifo_consume(ap_i2s_fifo_t *f, uint32_t frames) {
    if (!f || !f->buf || !frames) return;
    uint32_t w = LOAD_ACQUIRE(&f->wr);
    uint32_t r = LOAD_RELAXED(&f->rd);
    uint32_t avail = used_of(f, w, r);
    if (frames > avail) frames = avail;
    STORE_RELEASE(&f->rd, advance(f, r, frames));
}
