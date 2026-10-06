// SPDX-License-Identifier: Apache-2.0
// Single-producer single-consumer FIFO of 32-bit stereo frames (8 bytes per frame) between
// the audio task (I2S sink write) and the I2S feeder task (i2s_sink.c). The storage comes from
// core_malloc (PSRAM on the device) once, at create. Any capacity works, not only powers of
// two: both positions run modulo 2 * capacity, which tells a full FIFO from an empty one
// without a counter that both sides modify. Only atomic loads and stores are used (no
// read-modify-write), so the struct itself may live in any memory. Pure C: covered by
// test/test_port_i2s.c.
//
// Rules: one producer thread calls ap_i2s_fifo_write / ap_i2s_fifo_space / ap_i2s_fifo_set_limit,
// one consumer thread calls ap_i2s_fifo_peek / ap_i2s_fifo_consume. ap_i2s_fifo_level may be
// called by either. init/free/reset only while the consumer is stopped.
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AP_I2S_FIFO_FRAME_BYTES 8u  // 2 x int32_t (Q31, left-justified in 32-bit I2S slots)
#define AP_I2S_FIFO_MAX_FRAMES (1u << 24)

typedef struct {
    int32_t *buf;          // capacity * 2 samples, interleaved L/R
    uint32_t capacity;     // frames
    uint32_t limit;        // the producer keeps at most this many frames queued (1..capacity)
    _Atomic uint32_t wr;   // write position 0 .. 2 * capacity - 1 (producer)
    _Atomic uint32_t rd;   // read position 0 .. 2 * capacity - 1 (consumer)
} ap_i2s_fifo_t;

// Frames that `ms` milliseconds take at `rate` Hz, rounded up (0 for a zero rate or time).
uint32_t ap_i2s_fifo_frames_for_ms(uint32_t rate, uint32_t ms);

// Returns CORE_OK, CORE_EINVAL (capacity 0 or above AP_I2S_FIFO_MAX_FRAMES) or CORE_ENOMEM.
// The limit starts at the capacity.
int ap_i2s_fifo_init(ap_i2s_fifo_t *f, uint32_t capacity_frames);
void ap_i2s_fifo_free(ap_i2s_fifo_t *f);
// Empty the FIFO (the consumer must be stopped). The limit is kept.
void ap_i2s_fifo_reset(ap_i2s_fifo_t *f);
// Clamped to 1..capacity. Frames already queued above a lower limit stay and play.
void ap_i2s_fifo_set_limit(ap_i2s_fifo_t *f, uint32_t frames);

// Frames queued and not yet consumed.
uint32_t ap_i2s_fifo_level(const ap_i2s_fifo_t *f);

// ---- producer ----
// Frames that can be written now without exceeding the limit.
uint32_t ap_i2s_fifo_space(const ap_i2s_fifo_t *f);
// Copies up to `frames` interleaved stereo frames. Returns frames stored.
uint32_t ap_i2s_fifo_write(ap_i2s_fifo_t *f, const int32_t *pcm, uint32_t frames);

// ---- consumer ----
// Contiguous frames available at the read position (*span points at them); 0 when empty.
// A wrapped FIFO takes two peeks.
uint32_t ap_i2s_fifo_peek(const ap_i2s_fifo_t *f, const int32_t **span);
// Release `frames` frames after the data was used (at most what peek returned).
void ap_i2s_fifo_consume(ap_i2s_fifo_t *f, uint32_t frames);

#ifdef __cplusplus
}
#endif
