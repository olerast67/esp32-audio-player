// SPDX-License-Identifier: Apache-2.0
// Play /sdcard/music to Bluetooth headphones through esp32-a2dp-xq: every format resampled to
// 44.1 kHz, gapless, SBC-XQ when the headphones accept it (ESP-IDF 6.0+), headphone buttons
// control the player, the session resumes after a reset.
//
//   SD card (SPI): MOSI 23, MISO 19, SCK 18, CS 5
//
// Put the headphones into pairing mode before the first boot; later boots reconnect.
#include <sys/stat.h>

#include "a2dp_xq.h"
#include "audio_player.h"
#include "audio_player/a2dp_sink.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

#define PIN_SD_MOSI 23
#define PIN_SD_MISO 19
#define PIN_SD_SCK 18
#define PIN_SD_CS 5
#define HEADPHONES_ADDR ""  // "aa:bb:cc:dd:ee:ff" to skip the scan

#define MOUNT "/sdcard"
#define STATE_DIR MOUNT "/.player"

static const char *TAG = "example";
static player_t *s_player;
static volatile bool s_connect_requested;

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
    host.max_freq_khz = 20000;  // hi-res FLAC needs more than the 4 MHz probing clock
    esp_vfs_fat_sdmmc_mount_config_t mount = {.format_if_mount_failed = false, .max_files = 8};
    sdmmc_card_t *card;
    return esp_vfs_fat_sdspi_mount(MOUNT, &host, &slot, &mount, &card) == ESP_OK;
}

static void on_bluetooth(const a2dp_xq_event_t *ev, void *user) {
    (void)user;
    if (ev->type == A2DP_XQ_EVENT_DEVICE_FOUND && !s_connect_requested) {
        ESP_LOGI(TAG, "connecting to %s", ev->device.name);
        s_connect_requested = true;
        a2dp_xq_connect(ev->device.addr);
    } else if (ev->type == A2DP_XQ_EVENT_STATE && ev->state == A2DP_XQ_STATE_IDLE && !s_connect_requested) {
        a2dp_xq_scan(true);
    } else if (ev->type == A2DP_XQ_EVENT_CODEC) {
        char codec[32];
        a2dp_xq_describe(codec, sizeof codec);
        ESP_LOGI(TAG, "Bluetooth: %s", codec);
    }
}

static void on_player(const core_event_t *ev, void *user) {
    (void)user;
    if (ev->type == EV_TRACK_CHANGED && s_player) {
        player_status_t st;
        player_get_status(s_player, &st);
        ESP_LOGI(TAG, "now playing: %s - %s (%s %u/%u)", st.artist, st.title, codec_name(st.codec), st.src_fmt.bits,
                 (unsigned)st.src_fmt.sample_rate);
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();  // Bluetooth bonding keys
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    if (!mount_sd()) {
        ESP_LOGE(TAG, "no SD card");
        return;
    }
    mkdir(STATE_DIR, 0775);

    audio_player_esp32_config_t cfg = AUDIO_PLAYER_ESP32_CONFIG_DEFAULT();
    cfg.state_dir = STATE_DIR;
    cfg.on_event = on_player;
    s_player = audio_player_start(&cfg);
    if (!s_player) return;

    a2dp_xq_config_t bt = A2DP_XQ_CONFIG_DEFAULT();
    bt.device_name = "ESP32 player";
    bt.last_device = HEADPHONES_ADDR[0] ? HEADPHONES_ADDR : NULL;
    bt.state_dir = STATE_DIR;
    bt.event_cb = on_bluetooth;
    bt.remote_cb = audio_player_a2dp_remote;
    bt.remote_user = s_player;
    ESP_ERROR_CHECK(a2dp_xq_init(&bt));
    if (HEADPHONES_ADDR[0]) {
        s_connect_requested = true;
        a2dp_xq_connect(HEADPHONES_ADDR);
    }
    player_cmd_set_sink(s_player, audio_player_a2dp_sink());

    if (player_restore_state(s_player, true) != CORE_OK) player_cmd_play_folder(s_player, MOUNT "/music", 0, false);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        player_save_state(s_player);
    }
}
