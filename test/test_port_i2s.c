// SPDX-License-Identifier: Apache-2.0
// Host tests for the pure-C parts of the ESP32 I2S output: rate planning and negotiation,
// the status string, DoP silence, and the SPSC frame FIFO between the audio and feeder tasks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/i2s_fifo.h"
#include "port/i2s_plan.h"
#include "test.h"

static audio_format_t fmt(uint32_t rate, uint8_t ch, uint8_t bits) {
    audio_format_t f = {rate, ch, bits, false};
    return f;
}

static uint32_t neg_rate(uint32_t max_rate, uint32_t multiple, uint32_t src_rate) {
    audio_format_t src = fmt(src_rate, 2, 16), out;
    if (ap_i2s_negotiate(max_rate, multiple, false, &src, &out) != CORE_OK) return 0;
    return out.sample_rate;
}

TEST(clock_plans) {
    for (uint32_t i = 0; i < ap_i2s_rate_count; i++) {
        uint32_t r = ap_i2s_rates[i];
        ap_i2s_clock_plan_t p;
        CHECK_EQ_INT(ap_i2s_plan_clock(r, 0, &p), CORE_OK);
        CHECK_EQ_INT(p.sample_rate, r);
        CHECK_EQ_INT(p.mclk_multiple, r > 96000 ? 128 : 256);
        CHECK_EQ_INT(p.mclk_hz, r * p.mclk_multiple);
        CHECK(p.bclk_div >= AP_I2S_MIN_BCLK_DIV);
        CHECK_EQ_INT(p.mclk_multiple, p.bclk_div * AP_I2S_BCK_PER_FRAME);
    }
    ap_i2s_clock_plan_t p;
    CHECK_EQ_INT(ap_i2s_plan_clock(44100, 384, &p), CORE_OK);  // 6 x 64 fs
    CHECK_EQ_INT(p.bclk_div, 6);
    CHECK_EQ_INT(ap_i2s_plan_clock(44100, 64, &p), CORE_EUNSUPPORTED);   // BCK = MCLK: divider 1
    CHECK_EQ_INT(ap_i2s_plan_clock(44100, 200, &p), CORE_EUNSUPPORTED);  // not a multiple of 64 fs
    CHECK_EQ_INT(ap_i2s_plan_clock(44000, 0, &p), CORE_EUNSUPPORTED);
    CHECK_EQ_INT(ap_i2s_plan_clock(44100, 0, NULL), CORE_EINVAL);
    CHECK_EQ_INT(ap_i2s_max_rate(0, 0), 192000);
    CHECK_EQ_INT(ap_i2s_max_rate(96000, 0), 96000);
    CHECK_EQ_INT(ap_i2s_max_rate(50000, 0), 48000);
    CHECK(ap_i2s_rate_supported(0, 0, 22050));
    CHECK(!ap_i2s_rate_supported(48000, 0, 96000));
}

TEST(negotiate_rates) {
    CHECK_EQ_INT(neg_rate(0, 0, 44100), 44100);
    CHECK_EQ_INT(neg_rate(0, 0, 96000), 96000);
    CHECK_EQ_INT(neg_rate(0, 0, 192000), 192000);
    CHECK_EQ_INT(neg_rate(0, 0, 22050), 22050);  // low rates play natively
    CHECK_EQ_INT(neg_rate(0, 0, 352800), 176400);  // hi-res above the limit: same family
    CHECK_EQ_INT(neg_rate(0, 0, 384000), 192000);
    CHECK_EQ_INT(neg_rate(96000, 0, 192000), 96000);
    CHECK_EQ_INT(neg_rate(96000, 0, 176400), 88200);
    CHECK_EQ_INT(neg_rate(48000, 0, 96000), 48000);
    CHECK_EQ_INT(neg_rate(0, 0, 24000), 32000);  // not in the table: nearest above, same family
    CHECK_EQ_INT(neg_rate(0, 0, 64000), 96000);
    CHECK_EQ_INT(neg_rate(0, 0, 37800), 44100);  // other family: nearest above
    CHECK_EQ_INT(neg_rate(0, 0, 500000), 192000);
    CHECK_EQ_INT(neg_rate(0, 384, 192000), 192000);  // 384 fs works at every rate (div 6)
}

TEST(negotiate_format_fields) {
    audio_format_t out, src = fmt(48000, 1, 24);
    CHECK_EQ_INT(ap_i2s_negotiate(0, 0, false, &src, &out), CORE_OK);
    CHECK_EQ_INT(out.channels, 2);  // mono is upmixed by the player
    CHECK_EQ_INT(out.bits, 24);
    CHECK(!out.dop);
    src = fmt(44100, 2, 8);
    ap_i2s_negotiate(0, 0, false, &src, &out);
    CHECK_EQ_INT(out.bits, 16);
    src = fmt(44100, 2, 32);
    ap_i2s_negotiate(0, 0, false, &src, &out);
    CHECK_EQ_INT(out.bits, 32);
    // DoP: refused unless enabled, then passed unchanged at its own rate.
    src = fmt(176400, 2, 24);
    src.dop = true;
    CHECK_EQ_INT(ap_i2s_negotiate(0, 0, false, &src, &out), CORE_EUNSUPPORTED);
    CHECK_EQ_INT(ap_i2s_negotiate(0, 0, true, &src, &out), CORE_OK);
    CHECK(out.dop);
    CHECK_EQ_INT(out.sample_rate, 176400);
    CHECK_EQ_INT(ap_i2s_negotiate(96000, 0, true, &src, &out), CORE_EUNSUPPORTED);  // never resampled
    src = fmt(0, 2, 16);
    CHECK_EQ_INT(ap_i2s_negotiate(0, 0, false, &src, &out), CORE_EINVAL);
    src = fmt(44100, 3, 16);
    CHECK_EQ_INT(ap_i2s_negotiate(0, 0, false, &src, &out), CORE_EINVAL);
    CHECK_EQ_INT(ap_i2s_negotiate(0, 0, false, NULL, &out), CORE_EINVAL);
}

TEST(describe_strings) {
    char buf[40];
    audio_format_t f = fmt(96000, 2, 24);
    ap_i2s_describe("PCM5102A", &f, buf, sizeof buf);
    CHECK_STR(buf, "PCM5102A 24/96");
    f = fmt(44100, 2, 16);
    ap_i2s_describe(NULL, &f, buf, sizeof buf);
    CHECK_STR(buf, "I2S 16/44.1");
    ap_i2s_describe("UDA1334A", NULL, buf, sizeof buf);
    CHECK_STR(buf, "UDA1334A");
    f = fmt(176400, 2, 24);
    f.dop = true;
    ap_i2s_describe("ES9038Q2M", &f, buf, sizeof buf);
    CHECK_STR(buf, "ES9038Q2M DoP 176.4");
    ap_i2s_format_khz(22050, buf, sizeof buf);
    CHECK_STR(buf, "22.05");
    ap_i2s_format_khz(11025, buf, sizeof buf);
    CHECK_STR(buf, "11.025");
    ap_i2s_format_khz(8000, buf, sizeof buf);
    CHECK_STR(buf, "8");
    char tiny[6];
    f = fmt(44100, 2, 16);
    ap_i2s_describe("PCM5102A", &f, tiny, sizeof tiny);  // truncates, stays terminated
    CHECK_EQ_INT(strlen(tiny), 5);
}

static void wfifo_seq(int32_t *pcm, uint32_t frames, uint32_t first) {
    for (uint32_t i = 0; i < frames; i++) {
        pcm[2 * i] = (int32_t)(first + i);
        pcm[2 * i + 1] = -(int32_t)(first + i);
    }
}

TEST(dop_silence) {
    // The DSD idle pattern under alternating markers, the same word in both channels, in the
    // layout of the core's DoP frames (marker << 24 | first byte << 16 | second byte << 8).
    int32_t f[9 * 2];
    uint32_t phase = 0;
    ap_dop_silence(f, 9, &phase);
    CHECK_EQ_INT(phase, 1);  // 9 frames: the next one carries 0xFA
    for (int i = 0; i < 9; i++) {
        uint32_t want = (i & 1) ? 0xFA696900u : 0x05696900u;
        CHECK_EQ_INT((uint32_t)f[2 * i], want);
        CHECK_EQ_INT(f[2 * i], f[2 * i + 1]);
    }
    // Continued in pieces of any length, the markers keep alternating across the joins.
    int32_t g[64 * 2];
    uint32_t marker_prev = 0xFAu, joins_ok = 1;
    phase = 0;
    for (uint32_t piece = 1; piece <= 11; piece++) {
        ap_dop_silence(g, piece, &phase);
        for (uint32_t i = 0; i < piece; i++) {
            uint32_t m = (uint32_t)g[2 * i] >> 24;
            if (m == marker_prev || (m != 0x05u && m != 0xFAu)) joins_ok = 0;
            marker_prev = m;
        }
    }
    CHECK(joins_ok);
    // The phase after a frame of the stream: the silence continues its marker sequence.
    CHECK_EQ_INT(ap_dop_phase_after((int32_t)0x05123400u), 1);
    CHECK_EQ_INT(ap_dop_phase_after((int32_t)0xFA123400u), 0);
    CHECK_EQ_INT(ap_dop_phase_after(0), 0);
    phase = ap_dop_phase_after((int32_t)0x05ABCD00u);
    ap_dop_silence(f, 1, &phase);
    CHECK_EQ_INT((uint32_t)f[0] >> 24, 0xFAu);
    ap_dop_silence(NULL, 4, &phase);  // nothing written, phase unchanged
    CHECK_EQ_INT(phase, 0);
}

TEST(fifo_sizes) {
    CHECK_EQ_INT(ap_i2s_fifo_frames_for_ms(44100, 200), 8820);
    CHECK_EQ_INT(ap_i2s_fifo_frames_for_ms(192000, 200), 38400);
    CHECK_EQ_INT(ap_i2s_fifo_frames_for_ms(44100, 1), 45);  // rounded up
    CHECK_EQ_INT(ap_i2s_fifo_frames_for_ms(0, 200), 0);
    ap_i2s_fifo_t f;
    CHECK_EQ_INT(ap_i2s_fifo_init(&f, 0), CORE_EINVAL);
    CHECK_EQ_INT(ap_i2s_fifo_init(&f, AP_I2S_FIFO_MAX_FRAMES + 1), CORE_EINVAL);
    CHECK_EQ_INT(ap_i2s_fifo_init(NULL, 16), CORE_EINVAL);
    CHECK_EQ_INT(ap_i2s_fifo_init(&f, 1000), CORE_OK);  // not a power of two
    CHECK_EQ_INT(f.capacity, 1000);
    CHECK_EQ_INT(ap_i2s_fifo_space(&f), 1000);
    CHECK_EQ_INT(ap_i2s_fifo_level(&f), 0);
    ap_i2s_fifo_set_limit(&f, 0);
    CHECK_EQ_INT(f.limit, 1);
    ap_i2s_fifo_set_limit(&f, 5000);
    CHECK_EQ_INT(f.limit, 1000);
    ap_i2s_fifo_free(&f);
    CHECK(f.buf == NULL);
    CHECK_EQ_INT(ap_i2s_fifo_level(&f), 0);
    CHECK_EQ_INT(ap_i2s_fifo_space(&f), 0);
    const int32_t *span = (const int32_t *)&f;
    CHECK_EQ_INT(ap_i2s_fifo_peek(&f, &span), 0);
    CHECK(span == NULL);
}

TEST(fifo_full_empty_and_wrap) {
    ap_i2s_fifo_t f;
    CHECK_EQ_INT(ap_i2s_fifo_init(&f, 10), CORE_OK);
    int32_t in[2 * 32], expect = 0;
    wfifo_seq(in, 32, 0);
    CHECK_EQ_INT(ap_i2s_fifo_write(&f, in, 32), 10);  // full at the capacity
    CHECK_EQ_INT(ap_i2s_fifo_level(&f), 10);
    CHECK_EQ_INT(ap_i2s_fifo_space(&f), 0);
    CHECK_EQ_INT(ap_i2s_fifo_write(&f, in, 1), 0);
    const int32_t *span;
    CHECK_EQ_INT(ap_i2s_fifo_peek(&f, &span), 10);
    CHECK_EQ_INT(span[0], 0);
    CHECK_EQ_INT(span[19], -9);
    ap_i2s_fifo_consume(&f, 7);
    CHECK_EQ_INT(ap_i2s_fifo_level(&f), 3);
    expect = 7;
    // Several laps: positions run over 2 x capacity and wrap many times.
    uint32_t next_in = 10;
    for (int lap = 0; lap < 50; lap++) {
        int32_t chunk[2 * 7];
        wfifo_seq(chunk, 7, next_in);
        uint32_t n = ap_i2s_fifo_write(&f, chunk, 7);
        CHECK_EQ_INT(n, 7);
        next_in += n;
        uint32_t got = 0;
        while (got < 7) {
            uint32_t run = ap_i2s_fifo_peek(&f, &span);
            CHECK(run > 0);
            if (!run) break;
            if (run > 7 - got) run = 7 - got;
            for (uint32_t i = 0; i < run; i++) {
                CHECK_EQ_INT(span[2 * i], expect);
                CHECK_EQ_INT(span[2 * i + 1], -expect);
                expect++;
            }
            ap_i2s_fifo_consume(&f, run);
            got += run;
        }
        CHECK_EQ_INT(ap_i2s_fifo_level(&f), 3);
    }
    // Consuming more than is queued stops at empty.
    ap_i2s_fifo_consume(&f, 100);
    CHECK_EQ_INT(ap_i2s_fifo_level(&f), 0);
    CHECK_EQ_INT(ap_i2s_fifo_peek(&f, &span), 0);
    ap_i2s_fifo_free(&f);
}

TEST(fifo_limit_and_reset) {
    ap_i2s_fifo_t f;
    CHECK_EQ_INT(ap_i2s_fifo_init(&f, 100), CORE_OK);
    int32_t in[2 * 100];
    wfifo_seq(in, 100, 1);
    ap_i2s_fifo_set_limit(&f, 40);  // 200 ms at a lower rate
    CHECK_EQ_INT(ap_i2s_fifo_space(&f), 40);
    CHECK_EQ_INT(ap_i2s_fifo_write(&f, in, 100), 40);
    CHECK_EQ_INT(ap_i2s_fifo_space(&f), 0);
    ap_i2s_fifo_set_limit(&f, 30);  // lower than queued: nothing is lost, no space
    CHECK_EQ_INT(ap_i2s_fifo_level(&f), 40);
    CHECK_EQ_INT(ap_i2s_fifo_space(&f), 0);
    ap_i2s_fifo_consume(&f, 15);
    CHECK_EQ_INT(ap_i2s_fifo_space(&f), 5);
    ap_i2s_fifo_reset(&f);
    CHECK_EQ_INT(ap_i2s_fifo_level(&f), 0);
    CHECK_EQ_INT(f.limit, 30);  // kept
    CHECK_EQ_INT(ap_i2s_fifo_write(&f, in, 3), 3);
    const int32_t *span;
    CHECK_EQ_INT(ap_i2s_fifo_peek(&f, &span), 3);
    CHECK_EQ_INT(span[0], 1);
    CHECK_EQ_INT(ap_i2s_fifo_write(NULL, in, 3), 0);
    CHECK_EQ_INT(ap_i2s_fifo_write(&f, NULL, 3), 0);
    ap_i2s_fifo_free(&f);
}

TEST(fifo_random_sizes_keep_order) {
    ap_i2s_fifo_t f;
    CHECK_EQ_INT(ap_i2s_fifo_init(&f, 1234), CORE_OK);
    ap_i2s_fifo_set_limit(&f, 999);
    int32_t chunk[2 * 600];
    uint32_t rng = 12345, next_in = 0, expect = 0, errors = 0;
    for (int step = 0; step < 20000; step++) {
        rng = rng * 1103515245u + 12345u;
        uint32_t want = (rng >> 8) % 600u + 1u;
        if ((rng >> 4) & 1u) {
            wfifo_seq(chunk, want, next_in);
            next_in += ap_i2s_fifo_write(&f, chunk, want);
        } else {
            const int32_t *span;
            uint32_t run = ap_i2s_fifo_peek(&f, &span);
            if (run > want) run = want;
            for (uint32_t i = 0; i < run; i++) {
                if (span[2 * i] != (int32_t)expect || span[2 * i + 1] != -(int32_t)expect) errors++;
                expect++;
            }
            ap_i2s_fifo_consume(&f, run);
        }
        if (ap_i2s_fifo_level(&f) != next_in - expect) errors++;
        if (ap_i2s_fifo_level(&f) > f.limit) errors++;
    }
    CHECK_EQ_INT(errors, 0);
    CHECK(next_in > 100000);
    ap_i2s_fifo_free(&f);
}

TEST_MAIN(RUN(clock_plans) RUN(negotiate_rates) RUN(negotiate_format_fields) RUN(describe_strings) RUN(dop_silence)
              RUN(fifo_sizes) RUN(fifo_full_empty_and_wrap) RUN(fifo_limit_and_reset)
                  RUN(fifo_random_sizes_keep_order))
