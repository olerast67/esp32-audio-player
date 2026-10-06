// SPDX-License-Identifier: Apache-2.0
/// \file
/// I2S DAC output for the player (ESP-IDF i2s_std driver, master, Philips format, 32-bit slots).
///
/// Works with DACs that take plain I2S without a control bus: PCM5100/5101/5102A, PCM5122 in
/// hardware mode, UDA1334A, ES9023, CS4344 (with MCLK), MAX98357A and similar.
///
///  - The DAC follows the source rate (8 kHz .. 192 kHz, capped by max_sample_rate); other
///    rates are resampled by the player to the nearest rate of the same family.
///  - Q31 samples go into 32-bit slots unchanged: 16, 24 and 32-bit sources reach the DAC
///    bit-exact while the DSP is bypassed (SINK_CAP_BITPERFECT).
///  - write() copies into a FIFO (PSRAM when available, fifo_ms long) that a feeder task moves
///    into the I2S DMA, so file opens and tag reads at track boundaries never cause dropouts.
///  - pause() lets the DMA play out, mutes (mute_gpio) and stops the clocks; resume starts the
///    clocks, waits for the DAC to lock on them and only then unmutes. Rate changes between
///    tracks follow the same order: no pops by design.
///  - DoP: with accept_dop the DSD files of the player (DSF/DFF as DoP at 176.4 kHz) go to the
///    DAC as they are, and every pause or flush sends DoP silence instead of PCM zeros so the DAC
///    stays in DSD mode. Only for DACs that detect DoP (ES9018/ES9038, AK4490 boards...).
///  - No hardware volume: the player applies its software volume with TPDF dither.
#pragma once

#include "audio_player/sink.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int port;                  ///< I2S controller number (0 or 1)
    int bck_gpio;              ///< bit clock (BCK, SCK on some boards)
    int ws_gpio;               ///< word select (LRCK, LCK)
    int dout_gpio;             ///< data to the DAC (DIN)
    int mclk_gpio;             ///< master clock to the DAC, -1 when the DAC derives it from BCK
    uint16_t mclk_multiple;    ///< MCLK / sample rate; 0 = 256 (128 above 96 kHz)
    int mute_gpio;             ///< DAC soft-mute input (PCM5102A XSMT), -1 = none
    uint8_t mute_active_level; ///< level of mute_gpio while muted (PCM5102A XSMT: 0)
    int amp_enable_gpio;       ///< amplifier enable, high while the output is open, -1 = none
    uint32_t max_sample_rate;  ///< highest rate the DAC takes, 0 = 192000
    bool accept_dop;           ///< pass DoP (DSD) frames to the DAC
    uint16_t fifo_ms;          ///< FIFO in front of the DMA at the highest rate, 0 = 200
    int8_t feeder_core;        ///< core of the feeder task, -1 = any
    uint8_t feeder_priority;   ///< FreeRTOS priority, above the audio task
    const char *dac_name;      ///< shown by describe(), NULL = "I2S"
} audio_i2s_config_t;

#define AUDIO_I2S_CONFIG_DEFAULT()                                                                             \
    {                                                                                                          \
        .port = 0, .bck_gpio = -1, .ws_gpio = -1, .dout_gpio = -1, .mclk_gpio = -1, .mclk_multiple = 0,       \
        .mute_gpio = -1, .mute_active_level = 0, .amp_enable_gpio = -1, .max_sample_rate = 0,                  \
        .accept_dop = false, .fifo_ms = 200, .feeder_core = 1, .feeder_priority = 19, .dac_name = NULL,       \
    }

/// Create the sink. The I2S channel is allocated at the first open() and released by close().
/// NULL when the pins are not set or memory runs out.
audio_sink_t *audio_i2s_sink_create(const audio_i2s_config_t *cfg);
/// Stops the feeder task and frees the sink. Call only after the player released it
/// (player_cmd_set_sink() to another sink, or audio_player_stop()).
void audio_i2s_sink_destroy(audio_sink_t *sink);

#ifdef __cplusplus
}
#endif
