// SPDX-License-Identifier: Apache-2.0
/// \file
/// Text encodings and Unicode helpers. All strings inside the core are UTF-8.
#pragma once

#include "audio_player/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TEXT_ENC_LATIN1 = 0,
    TEXT_ENC_UTF16_BOM,   ///< ID3 encoding 1
    TEXT_ENC_UTF16BE,     ///< ID3 encoding 2
    TEXT_ENC_UTF8,        ///< ID3 encoding 3
    TEXT_ENC_CP1251,
    TEXT_ENC_AUTO_LEGACY, ///< single-byte text of unknown code page: detect UTF-8 / CP1251 / Latin-1
} text_encoding_t;

/// Convert to UTF-8. Stops at NUL or len. Always terminates out.
/// Returns bytes written (without terminator). Invalid sequences become U+FFFD.
size_t text_to_utf8(const uint8_t *in, size_t len, text_encoding_t enc, char *out, size_t out_size);

/// Heuristic for 8-bit legacy text: valid UTF-8 -> UTF8; mostly bytes 0xC0..0xFF
/// forming Russian words -> CP1251; otherwise LATIN1.
text_encoding_t text_detect_legacy(const uint8_t *in, size_t len);

bool text_is_valid_utf8(const uint8_t *in, size_t len);

/// Decode one code point; returns bytes consumed (>=1) and stores cp (U+FFFD on error).
size_t utf8_decode(const char *s, uint32_t *cp);
size_t utf8_encode(uint32_t cp, char out[4]);
size_t utf8_strlen(const char *s);  ///< code points
/// Truncate to at most max_bytes without splitting a code point.
void utf8_truncate(char *s, size_t max_bytes);
void text_trim(char *s);  ///< trims ASCII whitespace in place

#ifdef __cplusplus
}
#endif
