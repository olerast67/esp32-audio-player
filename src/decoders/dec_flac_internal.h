// SPDX-License-Identifier: Apache-2.0
// FLAC backend extras that are not part of the public decoder contract (used by golden tests).
#pragma once

#include "audio_player/decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

// Copies the MD5 signature of the unencoded audio from STREAMINFO (native and Ogg FLAC).
// Returns false if d is not a FLAC decoder or the encoder did not store an MD5 (all zeros).
// The MD5 covers all samples interleaved, signed little-endian, packed to the source bit depth.
bool decoder_flac_md5(const decoder_t *d, uint8_t md5[16]);

#ifdef __cplusplus
}
#endif
