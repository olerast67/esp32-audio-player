// SPDX-License-Identifier: Apache-2.0
/// \file
/// esp32-audio-player: one include for the whole library.
///
/// Portable core (also builds on a PC):
///   audio_player/player.h    queue, gapless playback, DSP, resume, scrobble log
///   audio_player/decoder.h   FLAC, MP3, Ogg Vorbis, WAV, AIFF, DSF/DFF
///   audio_player/dsp.h       ReplayGain, parametric EQ with AutoEQ import, crossfeed, limiter
///   audio_player/library.h   music library index on the SD card
///   audio_player/playlist.h  M3U/M3U8 and CUE
///   audio_player/tags.h      ID3, Vorbis comments, MP4, RIFF, APE tags
/// ESP32 only:
///   audio_player/esp32.h     runtime glue and the audio task
///   audio_player/i2s_sink.h  I2S DAC output
///   audio_player/a2dp_sink.h Bluetooth output through esp32-a2dp-xq (include it yourself)
#pragma once

#include "audio_player/version.h"
#include "audio_player/base.h"
#include "audio_player/audio.h"
#include "audio_player/events.h"
#include "audio_player/stream.h"
#include "audio_player/decoder.h"
#include "audio_player/dsp.h"
#include "audio_player/resampler.h"
#include "audio_player/tags.h"
#include "audio_player/text.h"
#include "audio_player/collate.h"
#include "audio_player/fsbrowse.h"
#include "audio_player/playlist.h"
#include "audio_player/library.h"
#include "audio_player/sink.h"
#include "audio_player/player.h"

#if defined(ESP_PLATFORM)
#include "audio_player/esp32.h"
#include "audio_player/i2s_sink.h"
#endif
