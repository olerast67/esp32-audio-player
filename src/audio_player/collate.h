// SPDX-License-Identifier: Apache-2.0
/// \file
/// Compact collation for Russian + English (+ Latin with diacritics folded):
/// case-insensitive, "ё" == "е", digits before letters, Latin before Cyrillic,
/// optional skipping of leading articles ("The ", "A ").
#pragma once

#include "audio_player/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ignore_articles;  ///< "The Beatles" sorts as "Beatles"
} collate_opts_t;

int collate_cmp(const char *a, const char *b, const collate_opts_t *opts);

/// Primary sort key of one code point (folded, lowercased). 0 for ignorable chars.
uint32_t collate_fold(uint32_t cp);

/// Uppercase first letter used for the "jump to letter" overlay: 'A'..'Z', 'А'..'Я',
/// '#' for digits and symbols. Returns a code point.
uint32_t collate_index_letter(const char *s, const collate_opts_t *opts);

#ifdef __cplusplus
}
#endif
