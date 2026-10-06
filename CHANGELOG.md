# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-10-06

First public release. Not yet tested on ESP32 hardware: the host tests pass and the examples build
with ESP-IDF 5.3, 5.5, 6.0, 6.1 and the Arduino core 3.1.3 and newer.

### Added

- Decoders: FLAC and Ogg FLAC (dr_flac), MP3 with Xing, Info, VBRI and LAME gapless data
  (minimp3), Ogg Vorbis (stb_vorbis), WAV, RF64 and BW64, AIFF and AIFF-C, DSF and DFF as DoP.
- Player engine: queue of up to 20000 entries, shuffle, repeat, sample-exact gapless transitions,
  CUE sheets, seeking, sleep timer, volume limit, resume files, Rockbox scrobble log.
- DSP chain: ReplayGain, 10-band parametric EQ with AutoEQ import and export, crossfeed, software
  volume, limiter, TPDF dither, resampler for fixed-rate outputs.
- Tag reading (ID3v1, ID3v2.2-2.4, APEv2, Vorbis comments, RIFF, MP4) with CP1251 detection,
  folder browsing, M3U and M3U8 playlists, music library index on the SD card.
- ESP32 port: PSRAM-aware allocator, logging, locks and clock hooks, audio task
  (`audio_player_start`), I2S DAC output (`audio_i2s_sink_create`) for 8-192 kHz with optional DoP,
  Bluetooth output through esp32-a2dp-xq (`audio_player/a2dp_sink.h`).
- `player_cmd_play_folder()` to queue the audio files of one folder.
- Examples for ESP-IDF and Arduino, the host command-line player `player_cli`.

[Unreleased]: https://github.com/olerast67/esp32-audio-player/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/olerast67/esp32-audio-player/releases/tag/v0.1.0
