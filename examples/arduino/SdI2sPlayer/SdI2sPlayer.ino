// SPDX-License-Identifier: Apache-2.0
// Play every audio file in /music on the SD card to an I2S DAC, gapless, and continue where it
// stopped after a reset.
//
//   SD card (SPI):  MOSI 23, MISO 19, SCK 18, CS 5
//   DAC (I2S):      BCK 26, LRCK/WS 25, DIN 22   (PCM5102A: SCK to GND, XSMT to 3V3)
//
// Serial commands: space = pause/resume, n = next, p = previous, + / - = volume.
// A board with PSRAM (WROVER, ESP32-S3 with PSRAM) is recommended for hi-res files.
#include <FS.h>
#include <SD.h>
#include <audio_player.h>

const int SD_CS = 5;
const int I2S_BCK = 26, I2S_WS = 25, I2S_DOUT = 22;

player_t *player;

void onEvent(const core_event_t *ev, void *) {
  if (ev->type == EV_TRACK_CHANGED) {
    player_status_t st;
    player_get_status(player, &st);
    Serial.printf("> %s - %s [%s, %s]\n", st.artist, st.title, codec_name(st.codec), st.sink_detail);
  } else if (ev->type == EV_PLAYBACK_ERROR) {
    Serial.printf("cannot play %s: %s\n", ev->text, core_err_name(ev->a));
  }
}

void setup() {
  Serial.begin(115200);
  if (!SD.begin(SD_CS)) {  // mounted at /sd
    Serial.println("no SD card");
    return;
  }
  SD.mkdir("/.player");

  audio_i2s_config_t i2s = AUDIO_I2S_CONFIG_DEFAULT();
  i2s.bck_gpio = I2S_BCK;
  i2s.ws_gpio = I2S_WS;
  i2s.dout_gpio = I2S_DOUT;
  i2s.dac_name = "PCM5102A";

  audio_player_esp32_config_t cfg = AUDIO_PLAYER_ESP32_CONFIG_DEFAULT();
  cfg.sink = audio_i2s_sink_create(&i2s);
  cfg.state_dir = "/sd/.player";  // queue, position and volume survive a reset
  cfg.on_event = onEvent;
  player = audio_player_start(&cfg);

  if (player_restore_state(player, true) != CORE_OK) {
    player_cmd_play_folder(player, "/sd/music", 0, false);
  }
}

void loop() {
  if (!player) return;
  switch (Serial.read()) {
    case ' ': player_cmd_toggle_pause(player); break;
    case 'n': player_cmd_next(player); break;
    case 'p': player_cmd_prev(player); break;
    case '+': player_cmd_volume_step(player, 2); break;
    case '-': player_cmd_volume_step(player, -2); break;
  }
  static uint32_t lastSave;
  if (millis() - lastSave > 30000) {  // a power cut loses at most 30 s
    lastSave = millis();
    player_save_state(player);
  }
  delay(20);
}
