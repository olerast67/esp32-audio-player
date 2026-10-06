// SPDX-License-Identifier: Apache-2.0
// Play the /music folder of the SD card to Bluetooth headphones: FLAC, MP3, Vorbis or WAV at
// any rate, resampled to 44.1 kHz, gapless, with SBC-XQ when the headphones and the ESP-IDF
// version allow it. The buttons on the headphones control the player.
//
// Needs the esp32-a2dp-xq library (Library Manager, or github.com/olerast67/esp32-a2dp-xq).
// Board: a classic ESP32, preferably with PSRAM (WROVER). Tools > Partition Scheme: "Huge APP".
// SD card (SPI): MOSI 23, MISO 19, SCK 18, CS 5.
#include <FS.h>
#include <SD.h>
#include <a2dp_xq.h>
#include <audio_player.h>
#include <audio_player/a2dp_sink.h>

const int SD_CS = 5;
const char *HEADPHONES = "";  // "aa:bb:cc:dd:ee:ff", or empty to take the first device found

player_t *player;
volatile bool connectRequested = false;

void onBluetooth(const a2dp_xq_event_t *ev, void *) {
  if (ev->type == A2DP_XQ_EVENT_DEVICE_FOUND && !connectRequested) {
    Serial.printf("connecting to %s\n", ev->device.name);
    connectRequested = true;
    a2dp_xq_connect(ev->device.addr);
  } else if (ev->type == A2DP_XQ_EVENT_STATE && ev->state == A2DP_XQ_STATE_IDLE && !connectRequested) {
    a2dp_xq_scan(true);
  } else if (ev->type == A2DP_XQ_EVENT_CODEC) {
    char codec[32];
    a2dp_xq_describe(codec, sizeof codec);
    Serial.printf("Bluetooth: %s\n", codec);
  }
}

void onPlayer(const core_event_t *ev, void *) {
  if (ev->type == EV_TRACK_CHANGED) {
    player_status_t st;
    player_get_status(player, &st);
    Serial.printf("> %s - %s (%s %u/%.1f kHz)\n", st.artist, st.title, codec_name(st.codec), st.src_fmt.bits,
                  st.src_fmt.sample_rate / 1000.0);
  }
}

void setup() {
  Serial.begin(115200);
  if (!SD.begin(SD_CS, SPI, 20000000)) {
    Serial.println("no SD card");
    return;
  }
  SD.mkdir("/.player");

  audio_player_esp32_config_t cfg = AUDIO_PLAYER_ESP32_CONFIG_DEFAULT();
  cfg.state_dir = "/sd/.player";
  cfg.on_event = onPlayer;
  player = audio_player_start(&cfg);
  if (!player) {
    Serial.println("player start failed");
    return;
  }

  a2dp_xq_config_t bt = A2DP_XQ_CONFIG_DEFAULT();
  bt.device_name = "ESP32 player";
  bt.last_device = HEADPHONES[0] ? HEADPHONES : nullptr;
  bt.state_dir = "/sd/.player";  // per-headphone SBC-XQ results
  bt.event_cb = onBluetooth;
  bt.remote_cb = audio_player_a2dp_remote;
  bt.remote_user = player;
  if (a2dp_xq_init(&bt) != ESP_OK) {
    Serial.println("Bluetooth init failed");
    return;
  }
  if (HEADPHONES[0]) {
    connectRequested = true;
    a2dp_xq_connect(HEADPHONES);
  }
  player_cmd_set_sink(player, audio_player_a2dp_sink());
  if (player_restore_state(player, true) != CORE_OK) {
    player_cmd_play_folder(player, "/sd/music", 0, false);
  }
}

void loop() {
  static uint32_t lastSave;
  if (player && millis() - lastSave > 30000) {
    lastSave = millis();
    player_save_state(player);
  }
  delay(100);
}
