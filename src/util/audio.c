// SPDX-License-Identifier: Apache-2.0
#include "audio_player/audio.h"

rate_family_t audio_rate_family(uint32_t sample_rate) {
    if (sample_rate == 0) return RATE_FAMILY_OTHER;
    if (sample_rate % 11025 == 0) return RATE_FAMILY_44K1;
    if (sample_rate % 8000 == 0) return RATE_FAMILY_48K;
    return RATE_FAMILY_OTHER;
}

uint64_t audio_ms_to_frames(uint32_t ms, uint32_t sample_rate) { return (uint64_t)ms * sample_rate / 1000u; }

uint32_t audio_frames_to_ms(uint64_t frames, uint32_t sample_rate) {
    if (!sample_rate) return 0;
    return (uint32_t)(frames * 1000u / sample_rate);
}
