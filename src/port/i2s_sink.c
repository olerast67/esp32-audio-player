// SPDX-License-Identifier: Apache-2.0
// I2S DAC sink (see audio_player/i2s_sink.h).
//
// Buffering: write() copies into a FIFO and returns; a feeder task (priority above the audio
// task) moves the FIFO into the I2S DMA with i2s_channel_write(). The feeder holds io_lock
// around every DMA write; every control operation (open, pause, flush, close) first stops it
// (feed_on = false, take io_lock), so the I2S channel only changes while nobody writes to it.
// If the FIFO cannot be allocated the sink writes straight into the DMA.
//
// Pause: the feeder stops (the FIFO keeps its audio), the DMA plays out, the DAC is muted and
// the I2S channel is disabled, which releases the driver's power-management lock. Resume
// enables the channel (DMA preloaded with silence), waits for the DAC to lock and only then
// unmutes. Mute always comes before the clocks stop and the clocks before the unmute.
//
// DoP: wherever the DAC would hear PCM silence (silence preloaded at enable, the auto-clear
// after a drained tail, the mute ramp before the clocks stop) a DoP stream gets DoP silence
// (ap_dop_silence, continuing the marker sequence of the last frame written), so the DAC
// never leaves DSD mode with a click.
#ifdef ESP_PLATFORM

#include <string.h>

#include "audio_player/base.h"
#include "audio_player/i2s_sink.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "port/i2s_fifo.h"
#include "port/i2s_plan.h"
#include "soc/soc_caps.h"

static const char *TAG = "i2s_sink";

#define DMA_DESC_NUM 4
#define DMA_FRAME_NUM 480
#define DMA_TOTAL_FRAMES (DMA_DESC_NUM * DMA_FRAME_NUM)
#define MUTE_SETTLE_MS 20    // DAC soft-mute ramp before the audio is cut
#define RELOCK_MS 5          // silence plays this long after the clocks start before the unmute
#define PAUSED_WRITE_SLEEP_MS 10
#define DEFAULT_MAX_RATE 192000u
#define DEFAULT_FIFO_MS 200u
#define FIFO_MIN_MS 40u              // smallest FIFO tried when memory is short
#define FEED_STACK 2560              // i2s_channel_write only; the feeder never logs
#define FEED_CHUNK_FRAMES DMA_FRAME_NUM  // one DMA buffer per write: io_lock is held briefly
#define FEED_WRITE_TIMEOUT_MS 100    // the DMA frees a buffer every <= 60 ms while running
#define FEED_QUIT_WAIT_MS 1000
#define RATE_DRAIN_SLACK_MS 200      // bound of the FIFO drain before a rate change, beyond its length
#define DOP_CHUNK_FRAMES 64          // even: every chunk of the silence pattern starts on the same marker

typedef struct {
    audio_sink_t sink;
    audio_i2s_config_t cfg;
    uint32_t max_rate;
    i2s_chan_handle_t tx;
    volatile bool enabled;  // I2S channel running
    bool suspended;         // paused with the channel disabled
    bool paused;
    bool muted;
    bool gpio_ready;
    audio_format_t fmt;
    ap_i2s_clock_plan_t plan;

    // DMA level estimate: frames queued in DMA ahead of the DAC (lvl_frames at lvl_us, of
    // which the first lvl_zeros are the silence preloaded at enable). The DMA drains at exactly
    // the sample rate, so a leaky bucket is accurate enough for buffered_frames() and for
    // waiting until the tail has played. Written by the feeder, read by the audio task.
    portMUX_TYPE lvl_mux;
    int64_t lvl_us;
    uint32_t lvl_frames;
    uint32_t lvl_zeros;

    // FIFO and feeder (fifo.buf == NULL: no FIFO, write() goes straight to the DMA).
    ap_i2s_fifo_t fifo;
    uint32_t fifo_ms;
    TaskHandle_t feeder;
    SemaphoreHandle_t io_lock;      // feeder: around each DMA write; control ops: while they run
    SemaphoreHandle_t space;        // the feeder freed FIFO space
    SemaphoreHandle_t feeder_done;  // the feeder task ended
    volatile bool feed_on;          // the feeder may write (channel enabled, not paused)
    volatile bool feeder_quit;
    volatile uint32_t feed_errors;  // failed DMA writes (logged by the audio task)
    uint32_t feed_errors_logged;
    uint32_t dop_phase;             // DoP marker of the next frame into the DMA (0: 0x05, 1: 0xFA)
} i2s_out_t;

static const uint8_t k_zeros[512];
// DOP_CHUNK_FRAMES + 1 frames of DoP silence starting with marker 0x05: a chunk taken from
// frame `phase` starts with the right marker (filled at the first create).
static int32_t s_dop_silence[(DOP_CHUNK_FRAMES + 1) * 2];

// ------------------------------------------------------------------ helpers ----

static void sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) + 1); }

// Frames still in the DMA at this moment (silence included) and how many of them are silence.
static uint32_t dma_level(i2s_out_t *w, uint32_t *zeros_left) {
    uint32_t total = 0, zeros = 0;
    uint32_t rate = w->plan.sample_rate;
    taskENTER_CRITICAL(&w->lvl_mux);
    if (w->enabled && rate) {
        int64_t dt = esp_timer_get_time() - w->lvl_us;
        if (dt < 0) dt = 0;
        uint64_t played = (uint64_t)dt * rate / 1000000u;
        total = played >= w->lvl_frames ? 0 : w->lvl_frames - (uint32_t)played;
        zeros = played >= w->lvl_zeros ? 0 : w->lvl_zeros - (uint32_t)played;
    }
    taskEXIT_CRITICAL(&w->lvl_mux);
    if (zeros > total) zeros = total;
    if (zeros_left) *zeros_left = zeros;
    return total;
}

// Real audio in the DMA (the preloaded silence is not queued audio).
static uint32_t dma_audio_level(i2s_out_t *w) {
    uint32_t zeros = 0;
    uint32_t total = dma_level(w, &zeros);
    return total - zeros;
}

static void dma_level_set(i2s_out_t *w, uint32_t frames, uint32_t zeros) {
    if (frames > DMA_TOTAL_FRAMES) frames = DMA_TOTAL_FRAMES;
    if (zeros > frames) zeros = frames;
    taskENTER_CRITICAL(&w->lvl_mux);
    w->lvl_frames = frames;
    w->lvl_zeros = zeros;
    w->lvl_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&w->lvl_mux);
}

static void dma_level_add(i2s_out_t *w, uint32_t frames) {
    uint32_t zeros = 0;
    uint32_t total = dma_level(w, &zeros);
    dma_level_set(w, total + frames, zeros);
}

// Wait until everything in the DMA has played (bounded: 1920 frames).
static void drain_dma(i2s_out_t *w) {
    uint32_t lvl = dma_level(w, NULL);
    if (!lvl || !w->plan.sample_rate) return;
    sleep_ms((uint32_t)((uint64_t)lvl * 1000u / w->plan.sample_rate) + 2);
}

static void gpio_out(int pin, int level) {
    if (pin >= 0) gpio_set_level((gpio_num_t)pin, level);
}

static void gpio_setup(i2s_out_t *w) {
    if (w->gpio_ready) return;
    uint64_t mask = 0;
    if (w->cfg.mute_gpio >= 0) mask |= 1ULL << w->cfg.mute_gpio;
    if (w->cfg.amp_enable_gpio >= 0) mask |= 1ULL << w->cfg.amp_enable_gpio;
    if (mask) {
        gpio_config_t io = {
            .pin_bit_mask = mask,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&io) != ESP_OK) ESP_LOGW(TAG, "GPIO setup failed");
    }
    gpio_out(w->cfg.mute_gpio, w->cfg.mute_active_level ? 1 : 0);  // muted
    gpio_out(w->cfg.amp_enable_gpio, 0);
    w->gpio_ready = true;
}

static void dac_mute(i2s_out_t *w, bool mute, bool settle) {
    if (w->muted == mute) return;
    int active = w->cfg.mute_active_level ? 1 : 0;
    gpio_out(w->cfg.mute_gpio, mute ? active : !active);
    w->muted = mute;
    if (mute && settle && w->cfg.mute_gpio >= 0) sleep_ms(MUTE_SETTLE_MS);
}

static i2s_std_clk_config_t clk_config(const ap_i2s_clock_plan_t *p) {
    i2s_std_clk_config_t c = I2S_STD_CLK_DEFAULT_CONFIG(p->sample_rate);
#if SOC_I2S_SUPPORTS_APLL
    // APLL gives exact 44.1 and 48 kHz families. It must stay owned by this channel alone,
    // otherwise the driver cannot retune it between rates.
    c.clk_src = I2S_CLK_SRC_APLL;
#endif
    c.mclk_multiple = (i2s_mclk_multiple_t)p->mclk_multiple;
    return c;
}

// Next piece of silence for the DMA: zeros, or DoP silence that continues the marker
// sequence. At most max_frames; *frames receives the length.
static const void *silence_chunk(i2s_out_t *w, uint32_t max_frames, uint32_t *frames) {
    if (!w->fmt.dop) {
        uint32_t n = (uint32_t)(sizeof k_zeros / AP_I2S_FRAME_BYTES);
        *frames = max_frames < n ? max_frames : n;
        return k_zeros;
    }
    *frames = max_frames < DOP_CHUNK_FRAMES ? max_frames : DOP_CHUNK_FRAMES;
    return &s_dop_silence[w->dop_phase * 2u];
}

// The DMA took `frames` frames of silence_chunk(): an odd count flips the DoP marker.
static void silence_taken(i2s_out_t *w, uint32_t frames) { w->dop_phase ^= frames & 1u; }

// Fill every DMA buffer with silence (the channel must be disabled). After enable the DAC
// first hears this silence, then the new audio: no stale samples, no click.
static void preload_silence(i2s_out_t *w) {
    uint32_t total = DMA_TOTAL_FRAMES, done = 0;
    while (done < total) {
        uint32_t n;
        const void *src = silence_chunk(w, total - done, &n);
        size_t got = 0;
        if (i2s_channel_preload_data(w->tx, src, (size_t)n * AP_I2S_FRAME_BYTES, &got) != ESP_OK || got == 0) break;
        uint32_t frames = (uint32_t)(got / AP_I2S_FRAME_BYTES);
        silence_taken(w, frames);
        done += frames;
        if (frames < n) break;  // every DMA buffer is full
    }
}

// DoP only: queue `frames` frames of DoP silence behind what the DMA holds (blocks while it
// is full, so this also waits for the audio in front of it to play). The channel runs and
// the feeder is stopped.
static void dop_pad(i2s_out_t *w, uint32_t frames) {
    if (!w->fmt.dop || !w->enabled || !w->plan.sample_rate) return;
    uint32_t timeout_ms = (uint32_t)((uint64_t)(frames + DMA_TOTAL_FRAMES) * 1000u / w->plan.sample_rate) + 100u;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint32_t done = 0;
    while (done < frames && esp_timer_get_time() < deadline) {
        uint32_t n;
        const void *src = silence_chunk(w, frames - done, &n);
        size_t written = 0;
        esp_err_t err = i2s_channel_write(w->tx, src, (size_t)n * AP_I2S_FRAME_BYTES, &written, timeout_ms);
        uint32_t got = (uint32_t)(written / AP_I2S_FRAME_BYTES);
        if (got) {
            silence_taken(w, got);
            dma_level_add(w, got);
            done += got;
        }
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) break;
    }
}

// Mute before the clocks stop. A DoP stream keeps hearing DoP silence through the whole mute
// ramp (PCM zeros from the auto-clear would end DSD mode with a click first).
static void mute_for_stop(i2s_out_t *w) {
    if (!w->fmt.dop || !w->enabled) {
        dac_mute(w, true, true);
        return;
    }
    dac_mute(w, true, false);
    // When the write returns, at most one DMA of silence is still queued: the rest played.
    dop_pad(w, (uint32_t)((uint64_t)w->plan.sample_rate * MUTE_SETTLE_MS / 1000u) + DMA_TOTAL_FRAMES);
}

// Let the audio in the DMA play out (pause, clock change). DoP: with DoP silence behind it.
static void play_out(i2s_out_t *w) {
    if (w->fmt.dop && w->enabled) {
        dop_pad(w, DMA_TOTAL_FRAMES);
    } else {
        drain_dma(w);
    }
}

static int channel_enable(i2s_out_t *w) {
    preload_silence(w);
    esp_err_t err = i2s_channel_enable(w->tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S enable: %s", esp_err_to_name(err));
        return CORE_EIO;
    }
    w->enabled = true;
    dma_level_set(w, DMA_TOTAL_FRAMES, DMA_TOTAL_FRAMES);
    return CORE_OK;
}

// Stops the clocks. The DAC must be muted already.
static void channel_disable(i2s_out_t *w) {
    if (w->enabled) {
        esp_err_t err = i2s_channel_disable(w->tx);
        if (err != ESP_OK) ESP_LOGW(TAG, "I2S disable: %s", esp_err_to_name(err));
    }
    w->enabled = false;
    dma_level_set(w, 0, 0);
}

static int channel_create(i2s_out_t *w, const ap_i2s_clock_plan_t *plan) {
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(w->cfg.port, I2S_ROLE_MASTER);
    cc.dma_desc_num = DMA_DESC_NUM;
    cc.dma_frame_num = DMA_FRAME_NUM;
    cc.auto_clear = true;  // silence, not repeated audio, when the writer falls behind
    esp_err_t err = i2s_new_channel(&cc, &w->tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel: %s", esp_err_to_name(err));
        w->tx = NULL;
        return CORE_EIO;
    }
    i2s_std_config_t sc = {
        .clk_cfg = clk_config(plan),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = w->cfg.mclk_gpio >= 0 ? (gpio_num_t)w->cfg.mclk_gpio : I2S_GPIO_UNUSED,
                .bclk = (gpio_num_t)w->cfg.bck_gpio,
                .ws = (gpio_num_t)w->cfg.ws_gpio,
                .dout = (gpio_num_t)w->cfg.dout_gpio,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
            },
    };
    sc.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    err = i2s_channel_init_std_mode(w->tx, &sc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init at %u Hz: %s", (unsigned)plan->sample_rate, esp_err_to_name(err));
        i2s_del_channel(w->tx);
        w->tx = NULL;
        return CORE_EIO;
    }
    w->plan = *plan;
    ESP_LOGI(TAG, "%u Hz, MCLK %u x fs", (unsigned)plan->sample_rate, (unsigned)plan->mclk_multiple);
    return CORE_OK;
}

static void channel_destroy(i2s_out_t *w) {
    if (!w->tx) return;
    channel_disable(w);
    i2s_del_channel(w->tx);
    w->tx = NULL;
}

// Clocks on (silence first), DAC locked on them, still muted.
static int output_up(i2s_out_t *w) {
    int rc = channel_enable(w);
    if (rc) return rc;
    sleep_ms(RELOCK_MS);  // the DAC PLL locks on BCK while the silence plays
    return CORE_OK;
}

// Mute, clocks off.
static void output_down(i2s_out_t *w) {
    if (!w->enabled) return;
    mute_for_stop(w);
    channel_disable(w);
}

// ---------------------------------------------------------------- feeder ----

static void feeder_task(void *arg) {
    i2s_out_t *w = (i2s_out_t *)arg;
    while (!w->feeder_quit) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // no timeout: nothing wakes the CPU while paused
        while (!w->feeder_quit) {
            xSemaphoreTake(w->io_lock, portMAX_DELAY);
            const int32_t *span = NULL;
            uint32_t n = (w->feed_on && w->enabled) ? ap_i2s_fifo_peek(&w->fifo, &span) : 0;
            if (!n) {
                xSemaphoreGive(w->io_lock);
                break;
            }
            if (n > FEED_CHUNK_FRAMES) n = FEED_CHUNK_FRAMES;
            size_t written = 0;
            esp_err_t err =
                i2s_channel_write(w->tx, span, (size_t)n * AP_I2S_FRAME_BYTES, &written, FEED_WRITE_TIMEOUT_MS);
            uint32_t done = (uint32_t)(written / AP_I2S_FRAME_BYTES);
            if (done) {
                if (w->fmt.dop) w->dop_phase = ap_dop_phase_after(span[(size_t)(done - 1) * 2u]);
                ap_i2s_fifo_consume(&w->fifo, done);
                dma_level_add(w, done);
            }
            xSemaphoreGive(w->io_lock);
            if (done) xSemaphoreGive(w->space);
            if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
                w->feed_errors++;
                vTaskDelay(pdMS_TO_TICKS(10));  // do not spin on a broken channel
                break;
            }
        }
    }
    xSemaphoreGive(w->feeder_done);
    vTaskDelete(NULL);
}

// Stop the feeder and keep it out of the I2S channel until feed_release().
static void feed_stop(i2s_out_t *w) {
    if (!w->feeder) return;
    w->feed_on = false;
    xSemaphoreTake(w->io_lock, portMAX_DELAY);  // waits for a DMA write in progress (<= 1 buffer)
}

// The feeder runs while the channel is enabled and the sink is not paused.
static void feed_release(i2s_out_t *w) {
    if (!w->feeder) return;
    w->feed_on = w->enabled && !w->paused;
    xSemaphoreGive(w->io_lock);
    if (w->feed_on) xTaskNotifyGive(w->feeder);
}

// Let the FIFO play out through the running feeder (before a clock change). Bounded.
static void fifo_drain(i2s_out_t *w) {
    if (!w->feeder || !w->feed_on || !w->plan.sample_rate) return;
    uint32_t lvl = ap_i2s_fifo_level(&w->fifo);
    if (!lvl) return;
    int64_t deadline = esp_timer_get_time() + (int64_t)((uint64_t)lvl * 1000000u / w->plan.sample_rate) +
                       RATE_DRAIN_SLACK_MS * 1000;
    xTaskNotifyGive(w->feeder);
    while (ap_i2s_fifo_level(&w->fifo) && esp_timer_get_time() < deadline) xSemaphoreTake(w->space, pdMS_TO_TICKS(20));
}

static void log_feed_errors(i2s_out_t *w) {
    uint32_t e = w->feed_errors;
    if (e == w->feed_errors_logged) return;
    w->feed_errors_logged = e;
    ESP_LOGE(TAG, "I2S write failed (%u times)", (unsigned)e);
}

// ------------------------------------------------------------------ sink ops ----

static int op_negotiate(audio_sink_t *s, const audio_format_t *src, audio_format_t *out) {
    i2s_out_t *w = (i2s_out_t *)s->ctx;
    return ap_i2s_negotiate(w->max_rate, w->cfg.mclk_multiple, w->cfg.accept_dop, src, out);
}

// dop: the new stream is DoP (the silence preloaded at enable must be of its kind).
static int first_start(i2s_out_t *w, const ap_i2s_clock_plan_t *plan, bool dop) {
    gpio_setup(w);
    w->fmt.dop = dop;
    int rc = channel_create(w, plan);
    if (rc) return rc;
    if (w->paused) {
        w->suspended = true;  // resume brings the output up
    } else {
        rc = output_up(w);
        if (rc) {
            channel_destroy(w);
            return rc;
        }
    }
    gpio_out(w->cfg.amp_enable_gpio, 1);
    return CORE_OK;
}

// The feeder is stopped (op_open); the FIFO was drained through it before.
static int rate_change(i2s_out_t *w, const ap_i2s_clock_plan_t *plan, bool dop) {
    uint32_t left = ap_i2s_fifo_level(&w->fifo);
    if (left) ESP_LOGW(TAG, "rate change drops %u queued frames", (unsigned)left);
    ap_i2s_fifo_reset(&w->fifo);
    // The tail in the DMA plays (auto-clear sends silence after it), then mute, clocks off.
    play_out(w);
    output_down(w);
    w->fmt.dop = dop;  // from here on the silence is that of the new stream
    i2s_std_clk_config_t clk = clk_config(plan);
    esp_err_t err = i2s_channel_reconfig_std_clock(w->tx, &clk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "clock switch to %u Hz: %s", (unsigned)plan->sample_rate, esp_err_to_name(err));
        w->suspended = false;
        return CORE_EIO;
    }
    w->plan = *plan;
    ESP_LOGI(TAG, "%u Hz, MCLK %u x fs", (unsigned)plan->sample_rate, (unsigned)plan->mclk_multiple);
    if (w->paused) {
        w->suspended = true;
        return CORE_OK;
    }
    w->suspended = false;
    return output_up(w);
}

static int op_open(audio_sink_t *s, const audio_format_t *fmt) {
    i2s_out_t *w = (i2s_out_t *)s->ctx;
    if (!fmt) return CORE_EINVAL;
    if (fmt->channels != 2 || (fmt->dop && !w->cfg.accept_dop)) return CORE_EUNSUPPORTED;
    if (!ap_i2s_rate_supported(w->max_rate, w->cfg.mclk_multiple, fmt->sample_rate)) return CORE_EUNSUPPORTED;
    ap_i2s_clock_plan_t plan;
    int rc = ap_i2s_plan_clock(fmt->sample_rate, w->cfg.mclk_multiple, &plan);
    if (rc) return rc;

    bool up = w->tx && (w->enabled || w->suspended);
    // Same clocks and the same kind of stream: continue gapless. PCM <-> DoP at the same rate
    // still goes through mute and silence of the new kind.
    bool same = up && w->plan.sample_rate == plan.sample_rate && w->fmt.dop == fmt->dop;
    // A clock change: what is queued plays at the old rate first (the player normally drained
    // it already), through the feeder, which still runs.
    if (!same && w->tx) fifo_drain(w);
    feed_stop(w);
    if (same) {
        w->fmt = *fmt;  // same clocks (bit depth may differ, the slot is 32 bits anyway)
    } else {
        rc = w->tx ? rate_change(w, &plan, fmt->dop) : first_start(w, &plan, fmt->dop);
        if (rc) {
            ESP_LOGE(TAG, "open %u Hz failed: %s", (unsigned)fmt->sample_rate, core_err_name(rc));
            feed_release(w);
            return rc;
        }
        w->fmt = *fmt;
    }
    if (w->fifo.buf) ap_i2s_fifo_set_limit(&w->fifo, ap_i2s_fifo_frames_for_ms(plan.sample_rate, w->fifo_ms));
    if (!w->paused && w->enabled) dac_mute(w, false, false);
    feed_release(w);
    return CORE_OK;
}

// Without a FIFO: straight into the DMA (blocks while it is full).
static int32_t write_direct(i2s_out_t *w, const int32_t *pcm, uint32_t frames, uint32_t timeout_ms) {
    // Q31 left-justified in 32-bit slots is exactly what the DAC takes: no conversion.
    size_t written = 0;
    esp_err_t err = i2s_channel_write(w->tx, pcm, (size_t)frames * AP_I2S_FRAME_BYTES, &written, timeout_ms);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "I2S write: %s", esp_err_to_name(err));
        return CORE_EIO;
    }
    uint32_t done = (uint32_t)(written / AP_I2S_FRAME_BYTES);
    if (done) {
        if (w->fmt.dop) w->dop_phase = ap_dop_phase_after(pcm[(size_t)(done - 1) * 2u]);
        dma_level_add(w, done);
    }
    return (int32_t)done;
}

static int32_t op_write(audio_sink_t *s, const int32_t *pcm, uint32_t frames, uint32_t timeout_ms) {
    i2s_out_t *w = (i2s_out_t *)s->ctx;
    if (!w->tx || (!w->enabled && !w->suspended)) return CORE_EINVAL;
    if (!frames) return 0;
    if (!pcm) return CORE_EINVAL;
    if (w->paused) {
        sleep_ms(timeout_ms < PAUSED_WRITE_SLEEP_MS ? timeout_ms : PAUSED_WRITE_SLEEP_MS);
        return 0;
    }
    if (!w->fifo.buf) return write_direct(w, pcm, frames, timeout_ms);
    log_feed_errors(w);
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint32_t done = 0;
    for (;;) {
        uint32_t n = ap_i2s_fifo_write(&w->fifo, pcm + (size_t)done * 2u, frames - done);
        if (n) {
            done += n;
            xTaskNotifyGive(w->feeder);  // higher priority: it refills the DMA before we go on
        }
        if (done >= frames) break;
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) break;
        // FIFO full: wait until the feeder moved a DMA buffer, in slices so the deadline holds.
        uint32_t wait_ms = (uint32_t)(left_us / 1000);
        if (wait_ms > 20) wait_ms = 20;
        xSemaphoreTake(w->space, pdMS_TO_TICKS(wait_ms) + 1);
    }
    return (int32_t)done;
}

static void op_pause(audio_sink_t *s, bool paused) {
    i2s_out_t *w = (i2s_out_t *)s->ctx;
    if (paused == w->paused) return;
    feed_stop(w);
    w->paused = paused;
    if (paused) {
        if (w->enabled) {
            // Nothing is dropped: the FIFO keeps its audio, what sits in the DMA plays out,
            // then mute and clocks off.
            play_out(w);
            output_down(w);
            w->suspended = true;
        }
    } else if (w->tx && !w->enabled) {
        w->suspended = false;
        if (output_up(w) == CORE_OK) dac_mute(w, false, false);  // clocks and lock first, then unmute
    } else if (w->enabled) {
        dac_mute(w, false, false);
    }
    feed_release(w);
}

static void op_flush(audio_sink_t *s) {
    i2s_out_t *w = (i2s_out_t *)s->ctx;
    if (!w->tx) return;
    feed_stop(w);
    ap_i2s_fifo_reset(&w->fifo);
    if (w->enabled) {
        bool was_muted = w->muted;
        mute_for_stop(w);  // the queued audio plays during the ramp (DoP: then DoP silence)
        // Disable, overwrite every DMA buffer with silence, enable: the queued audio is gone.
        if (i2s_channel_disable(w->tx) == ESP_OK) {
            w->enabled = false;
            if (channel_enable(w) == CORE_OK) sleep_ms(RELOCK_MS);
        }
        if (w->enabled && !was_muted && !w->paused) dac_mute(w, false, false);
    }
    feed_release(w);
}

static int op_set_volume_db(audio_sink_t *s, float db) {
    (void)s;
    (void)db;
    return -1;  // no hardware volume: the player uses software volume
}

static uint32_t op_buffered_frames(audio_sink_t *s) {
    i2s_out_t *w = (i2s_out_t *)s->ctx;
    return ap_i2s_fifo_level(&w->fifo) + dma_audio_level(w);
}

static void op_describe(audio_sink_t *s, char *out, size_t out_size) {
    i2s_out_t *w = (i2s_out_t *)s->ctx;
    ap_i2s_describe(w->cfg.dac_name, (w->enabled || w->suspended) ? &w->fmt : NULL, out, out_size);
}

static void op_close(audio_sink_t *s) {
    i2s_out_t *w = (i2s_out_t *)s->ctx;
    feed_stop(w);
    ap_i2s_fifo_reset(&w->fifo);
    if (w->tx) {
        output_down(w);
        gpio_out(w->cfg.amp_enable_gpio, 0);
        channel_destroy(w);
    }
    w->paused = false;
    w->suspended = false;
    memset(&w->fmt, 0, sizeof w->fmt);
    memset(&w->plan, 0, sizeof w->plan);
    feed_release(w);
}

// ------------------------------------------------------------------ public ----

// FIFO for fifo_ms at the highest rate, smaller when memory is short, none as the last resort.
// Without PSRAM it would come from internal RAM, which decoders, Bluetooth and Wi-Fi need: then
// it is sized for 100 ms at 48 kHz at most (38 KB); higher rates get a proportionally shorter
// FIFO through the limit set at open().
static void fifo_create(i2s_out_t *w) {
    w->fifo_ms = w->cfg.fifo_ms ? w->cfg.fifo_ms : DEFAULT_FIFO_MS;
    uint32_t size_rate = w->max_rate;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
        if (size_rate > 48000u) size_rate = 48000u;
        if (w->fifo_ms > 100u) w->fifo_ms = 100u;
    }
    for (uint32_t ms = w->fifo_ms; ms >= FIFO_MIN_MS; ms /= 2) {
        uint32_t frames = ap_i2s_fifo_frames_for_ms(size_rate, ms);
        if (ap_i2s_fifo_init(&w->fifo, frames) == CORE_OK) {
            if (ms < w->fifo_ms)
                ESP_LOGW(TAG, "memory short: %u ms output FIFO instead of %u", (unsigned)ms, (unsigned)w->fifo_ms);
            w->fifo_ms = ms;
            return;
        }
    }
    ESP_LOGE(TAG, "no memory for the output FIFO: writing straight to the I2S DMA");
    w->fifo_ms = 0;
}

static bool feeder_start(i2s_out_t *w) {
    w->io_lock = xSemaphoreCreateMutex();
    w->space = xSemaphoreCreateBinary();
    w->feeder_done = xSemaphoreCreateBinary();
    if (!w->io_lock || !w->space || !w->feeder_done) return false;
    BaseType_t core = tskNO_AFFINITY;
    if (w->cfg.feeder_core >= 0 && w->cfg.feeder_core < portNUM_PROCESSORS) core = w->cfg.feeder_core;
    UBaseType_t prio = w->cfg.feeder_priority ? w->cfg.feeder_priority : 19;
    // Internal RAM stack: the feeder may run while the flash cache is off.
    if (xTaskCreatePinnedToCore(feeder_task, "i2s_feed", FEED_STACK, w, prio, &w->feeder, core) != pdPASS) {
        w->feeder = NULL;
        return false;
    }
    return true;
}

static void feeder_stop(i2s_out_t *w) {
    if (w->feeder) {
        feed_stop(w);
        w->feeder_quit = true;
        xSemaphoreGive(w->io_lock);
        xTaskNotifyGive(w->feeder);
        if (xSemaphoreTake(w->feeder_done, pdMS_TO_TICKS(FEED_QUIT_WAIT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "feeder task did not stop");
            vTaskDelete(w->feeder);  // blocked on its notification or io_lock: safe to delete
        }
        w->feeder = NULL;
    }
    if (w->io_lock) vSemaphoreDelete(w->io_lock);
    if (w->space) vSemaphoreDelete(w->space);
    if (w->feeder_done) vSemaphoreDelete(w->feeder_done);
    w->io_lock = w->space = w->feeder_done = NULL;
}

audio_sink_t *audio_i2s_sink_create(const audio_i2s_config_t *cfg) {
    if (!cfg || cfg->bck_gpio < 0 || cfg->ws_gpio < 0 || cfg->dout_gpio < 0) {
        ESP_LOGE(TAG, "BCK, WS and DOUT pins are required");
        return NULL;
    }
    // Internal RAM: the feeder and the audio task share it (spinlock, flags).
    i2s_out_t *w = (i2s_out_t *)heap_caps_calloc(1, sizeof *w, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!w) return NULL;
    if (!s_dop_silence[0]) {
        uint32_t phase = 0;
        ap_dop_silence(s_dop_silence, DOP_CHUNK_FRAMES + 1, &phase);
    }
    w->cfg = *cfg;
    w->max_rate = ap_i2s_max_rate(cfg->max_sample_rate ? cfg->max_sample_rate : DEFAULT_MAX_RATE, cfg->mclk_multiple);
    if (!w->max_rate) {
        ESP_LOGE(TAG, "no usable sample rate (max_sample_rate %u, mclk_multiple %u)", (unsigned)cfg->max_sample_rate,
                 (unsigned)cfg->mclk_multiple);
        heap_caps_free(w);
        return NULL;
    }
    w->muted = true;
    portMUX_INITIALIZE(&w->lvl_mux);
    fifo_create(w);
    if (w->fifo.buf && !feeder_start(w)) {
        ESP_LOGE(TAG, "cannot start the I2S feeder: writing straight to the I2S DMA");
        feeder_stop(w);
        ap_i2s_fifo_free(&w->fifo);
        w->fifo_ms = 0;
    }
    gpio_setup(w);

    audio_sink_t *s = &w->sink;
    s->name = "i2s";
    s->caps = SINK_CAP_BITPERFECT | SINK_CAP_RATE_SWITCH | (cfg->accept_dop ? SINK_CAP_DOP : 0);
    s->ctx = w;
    s->negotiate = op_negotiate;
    s->open = op_open;
    s->write = op_write;
    s->pause = op_pause;
    s->flush = op_flush;
    s->set_volume_db = op_set_volume_db;
    s->buffered_frames = op_buffered_frames;
    s->describe = op_describe;
    s->close = op_close;
    return s;
}

void audio_i2s_sink_destroy(audio_sink_t *sink) {
    if (!sink) return;
    i2s_out_t *w = (i2s_out_t *)sink->ctx;
    op_close(sink);
    feeder_stop(w);
    ap_i2s_fifo_free(&w->fifo);
    heap_caps_free(w);
}

#endif  // ESP_PLATFORM
