// SPDX-License-Identifier: Apache-2.0
// Headphone correction with an AutoEQ preset: copy the "ParametricEQ.txt" of your headphones
// from https://github.com/jaakkopasanen/AutoEq (results/<source>/<headphones>/) to the SD
// card as /eq.txt. The player loads it into its 10-band parametric EQ with the preamp, and adds
// crossfeed for a more natural stereo image on headphones.
//
// Serial commands: e = EQ on/off, c = crossfeed on/off, 0..5 = built-in presets.
// Wiring as in the SdI2sPlayer example.
#include <FS.h>
#include <SD.h>
#include <audio_player.h>

const int SD_CS = 5;
player_t *player;
dsp_config_t dsp;

bool loadAutoEq(const char *path, eq_preset_t *out) {
  File f = SD.open(path);
  if (!f) return false;
  String text = f.readString();
  f.close();
  return eq_preset_parse_autoeq(text.c_str(), out) == CORE_OK;
}

void printEq() {
  Serial.printf("EQ %s: \"%s\", preamp %.1f dB, %u bands; crossfeed %s\n", dsp.eq_enabled ? "on" : "off",
                dsp.eq.name, dsp.eq.preamp_db, dsp.eq.band_count, dsp.crossfeed_enabled ? "on" : "off");
  static const char *types[] = {"PK", "LS", "HS", "LP", "HP"};
  for (int i = 0; i < dsp.eq.band_count; i++) {
    const peq_band_t &b = dsp.eq.bands[i];
    Serial.printf("  %s %7.0f Hz %+5.1f dB Q %.2f\n", types[b.type], b.freq_hz, b.gain_db, b.q);
  }
}

void setup() {
  Serial.begin(115200);
  if (!SD.begin(SD_CS, SPI, 20000000)) {
    Serial.println("no SD card");
    return;
  }
  audio_i2s_config_t i2s = AUDIO_I2S_CONFIG_DEFAULT();
  i2s.bck_gpio = 26;
  i2s.ws_gpio = 25;
  i2s.dout_gpio = 22;
  audio_player_esp32_config_t cfg = AUDIO_PLAYER_ESP32_CONFIG_DEFAULT();
  cfg.sink = audio_i2s_sink_create(&i2s);
  player = audio_player_start(&cfg);
  if (!player) {
    Serial.println("player start failed");
    return;
  }

  dsp_config_defaults(&dsp);
  if (loadAutoEq("/eq.txt", &dsp.eq)) {
    strncpy(dsp.eq.name, "AutoEQ /eq.txt", sizeof dsp.eq.name - 1);
  } else {
    Serial.println("no /eq.txt, using the built-in \"Bass\" preset");
    dsp.eq = *eq_builtin_get(1);
  }
  dsp.eq_enabled = true;
  dsp.crossfeed_enabled = true;
  dsp.limiter_enabled = true;  // EQ boosts can exceed full scale
  player_cmd_set_dsp(player, &dsp);
  printEq();
  player_cmd_play_folder(player, "/sd/music", 0, false);
}

void loop() {
  if (!player) return;
  int c = Serial.read();
  if (c < 0) {
    delay(20);
    return;
  }
  if (c == 'e') dsp.eq_enabled = !dsp.eq_enabled;
  if (c == 'c') dsp.crossfeed_enabled = !dsp.crossfeed_enabled;
  if (c >= '0' && c <= '9' && (uint32_t)(c - '0') < eq_builtin_count()) dsp.eq = *eq_builtin_get(c - '0');
  player_cmd_set_dsp(player, &dsp);
  printEq();
}
