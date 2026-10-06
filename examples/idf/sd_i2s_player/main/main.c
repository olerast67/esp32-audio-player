// SPDX-License-Identifier: Apache-2.0
// Minimal player: every audio file in /sdcard/music, gapless, to an I2S DAC (PCM5102A wiring).
//
//   SD card (SPI):  MOSI 23, MISO 19, SCK 18, CS 5
//   DAC (I2S):      BCK 26, LRCK/WS 25, DIN 22   (PCM5102A: SCK to GND, XSMT to 3V3)
//
// Change the pins below for your board. The player keeps its resume state in /sdcard/.player.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "audio_player.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#define PIN_SD_MOSI 23
#define PIN_SD_MISO 19
#define PIN_SD_SCK 18
#define PIN_SD_CS 5
#define PIN_I2S_BCK 26
#define PIN_I2S_WS 25
#define PIN_I2S_DOUT 22

#define MOUNT "/sdcard"
#define MUSIC_DIR MOUNT "/music"
#define STATE_DIR MOUNT "/.player"

static const char *TAG = "example";

static bool mount_sd(void) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    if (spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA) != ESP_OK) return false;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = host.slot;
    esp_vfs_fat_sdmmc_mount_config_t mount = {.format_if_mount_failed = false, .max_files = 8};
    sdmmc_card_t *card;
    return esp_vfs_fat_sdspi_mount(MOUNT, &host, &slot, &mount, &card) == ESP_OK;
}

static void on_event(const core_event_t *ev, void *user) {
    player_t **pp = (player_t **)user;
    if (ev->type == EV_TRACK_CHANGED && *pp) {
        player_status_t st;
        player_get_status(*pp, &st);
        ESP_LOGI(TAG, "now playing: %s - %s [%s, %s]", st.artist, st.title, codec_name(st.codec), st.sink_detail);
    } else if (ev->type == EV_PLAYBACK_ERROR) {
        ESP_LOGW(TAG, "cannot play %s: %s", ev->text, core_err_name(ev->a));
    }
}

void app_main(void) {
    if (!mount_sd()) {
        ESP_LOGE(TAG, "no SD card");
        return;
    }
    mkdir(STATE_DIR, 0775);

    audio_i2s_config_t i2s = AUDIO_I2S_CONFIG_DEFAULT();
    i2s.bck_gpio = PIN_I2S_BCK;
    i2s.ws_gpio = PIN_I2S_WS;
    i2s.dout_gpio = PIN_I2S_DOUT;
    i2s.max_sample_rate = 192000;  // PCM5102A: up to 384 kHz; the player resamples above the cap
    i2s.dac_name = "PCM5102A";

    static player_t *player;
    audio_player_esp32_config_t cfg = AUDIO_PLAYER_ESP32_CONFIG_DEFAULT();
    cfg.sink = audio_i2s_sink_create(&i2s);
    cfg.state_dir = STATE_DIR;
    cfg.on_event = on_event;
    cfg.on_event_user = &player;
    player = audio_player_start(&cfg);
    if (!player) return;

    // Continue the last session, or start the music folder from the top.
    if (player_restore_state(player, true) != CORE_OK) {
        int n = player_cmd_play_folder(player, MUSIC_DIR, 0, false);
        ESP_LOGI(TAG, "%s: %d files", MUSIC_DIR, n);
    }

    // Save the position every 30 s so a power cut loses little.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        player_save_state(player);
    }
}
