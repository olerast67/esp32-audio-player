// SPDX-License-Identifier: Apache-2.0
// I2S output planning: which sample rates the DAC output takes, how the I2S clocks are set for
// each rate, the format the sink negotiates, and the DoP silence pattern. Pure C (no ESP-IDF),
// covered by test/test_port_i2s.c.
#pragma once

#include "audio_player/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

// The I2S slot is always 32 bits: Q31 samples go to the DAC unchanged (16/24-bit sources are
// left-justified inside), so 16, 24 and 32-bit sources reach the DAC bit-exact.
#define AP_I2S_SLOT_BITS 32u
#define AP_I2S_FRAME_BYTES 8u  // 2 channels x 32 bits
#define AP_I2S_BCK_PER_FRAME (2u * AP_I2S_SLOT_BITS)
// The ESP32 I2S master needs MCLK / BCK >= 2.
#define AP_I2S_MIN_BCLK_DIV 2u

typedef struct {
    uint32_t sample_rate;
    uint32_t mclk_multiple;  // i2s_std_clk_config_t.mclk_multiple
    uint32_t mclk_hz;        // sample_rate * mclk_multiple
    uint32_t bclk_div;       // MCLK / BCK
} ap_i2s_clock_plan_t;

// Every rate the output can run (ascending). max_rate may cap it.
extern const uint32_t ap_i2s_rates[];
extern const uint32_t ap_i2s_rate_count;

// mclk_multiple 0 = automatic: 256 fs up to 96 kHz, 128 fs above (keeps the clock in range).
// Otherwise it must be a multiple of 64 fs giving MCLK / BCK >= 2. CORE_EUNSUPPORTED for a
// rate that is not in ap_i2s_rates or a multiple that does not fit.
int ap_i2s_plan_clock(uint32_t rate, uint32_t mclk_multiple, ap_i2s_clock_plan_t *out);

// max_rate 0 = no cap.
bool ap_i2s_rate_supported(uint32_t max_rate, uint32_t mclk_multiple, uint32_t rate);
uint32_t ap_i2s_max_rate(uint32_t max_rate, uint32_t mclk_multiple);

// sink.negotiate(): keep the source rate when the output takes it, otherwise pick the closest
// rate of the same family (the player resamples). Always 2 channels; bits are passed through
// (clamped to 16..32) because every width travels in a 32-bit slot. DoP passes unchanged when
// accept_dop is set and its rate is supported, otherwise CORE_EUNSUPPORTED.
int ap_i2s_negotiate(uint32_t max_rate, uint32_t mclk_multiple, bool accept_dop, const audio_format_t *src,
                     audio_format_t *out);

// "PCM5102A 24/96", "I2S DoP 176.4". fmt may be NULL (name only).
void ap_i2s_describe(const char *dac_name, const audio_format_t *fmt, char *out, size_t out_size);
// "44.1", "96", "22.05": kHz without trailing zeros.
void ap_i2s_format_khz(uint32_t rate, char *out, size_t out_size);

// DoP silence: the DSD idle pattern 0x69 in both payload bytes under the DoP marker, which
// alternates 0x05 / 0xFA from frame to frame. In Q31 slots: marker << 24 | 0x69 << 16 |
// 0x69 << 8, the same word in both channels. A DoP DAC stays in DSD mode on it; PCM zeros
// would switch it to PCM and back, which clicks.
#define AP_DOP_MARKER_A 0x05u
#define AP_DOP_MARKER_B 0xFAu
#define AP_DSD_IDLE 0x69u
// Stereo frames of DoP silence. *phase is the marker of the next frame (0: 0x05, 1: 0xFA)
// and is advanced past the n frames written.
void ap_dop_silence(int32_t *frames, uint32_t n, uint32_t *phase);
// Phase (as above) of the frame after a DoP frame whose left word is `word`.
uint32_t ap_dop_phase_after(int32_t word);

#ifdef __cplusplus
}
#endif
