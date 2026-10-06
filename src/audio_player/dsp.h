// SPDX-License-Identifier: Apache-2.0
/// \file
/// Sound processing chain, float32 inside, Q31 in and out:
///   ReplayGain -> preamp -> parametric EQ (<=10 bands, RBJ biquads, AutoEQ import)
///   -> crossfeed (Bauer-style, bs2b-like) -> software volume -> limiter -> dither.
/// When every stage is off and volume is unity, dsp_process() leaves samples
/// untouched (bit-perfect path) and dsp_is_bypass() returns true.
#pragma once

#include "audio_player/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_MAX_BANDS 10
#define DSP_PRESET_NAME_MAX 48

typedef enum { PEQ_PEAK = 0, PEQ_LOW_SHELF, PEQ_HIGH_SHELF, PEQ_LOW_PASS, PEQ_HIGH_PASS } peq_type_t;

typedef struct {
    peq_type_t type;
    float freq_hz;
    float gain_db;  ///< ignored for pass filters
    float q;
    bool enabled;
} peq_band_t;

typedef struct {
    char name[DSP_PRESET_NAME_MAX];
    float preamp_db;           ///< usually negative to avoid clipping
    uint8_t band_count;
    peq_band_t bands[DSP_MAX_BANDS];
} eq_preset_t;

/// Parse AutoEQ "ParametricEQ.txt" (lines like
/// "Preamp: -6.3 dB" and "Filter 1: ON PK Fc 105 Hz Gain -2.9 dB Q 0.70",
/// types PK, LSC, HSC, LS, HS, LP, HP). Returns CORE_OK or CORE_ECORRUPT.
/// Extra bands beyond DSP_MAX_BANDS are dropped with a warning.
int eq_preset_parse_autoeq(const char *text, eq_preset_t *out);
/// Write the same text format (so presets round-trip). Returns bytes written or <0.
int eq_preset_format_autoeq(const eq_preset_t *p, char *out, size_t out_size);
/// Built-in presets: "Flat", "Bass", "Treble", "Vocal", "Loudness". Index 0 is Flat.
uint32_t eq_builtin_count(void);
const eq_preset_t *eq_builtin_get(uint32_t index);

typedef enum { RG_OFF = 0, RG_TRACK, RG_ALBUM, RG_AUTO /* album gain when playing an album in order */ } rg_mode_t;

typedef struct {
    bool eq_enabled;
    eq_preset_t eq;
    rg_mode_t rg_mode;
    float rg_preamp_db;        ///< added to RG gain, default 0
    float rg_fallback_db;      ///< gain for files without RG tags, default 0
    bool rg_prevent_clip;      ///< limit gain by peak
    bool crossfeed_enabled;
    uint16_t crossfeed_fcut_hz;  ///< 300..2000, default 700
    float crossfeed_level_db;    ///< 1..15, default 4.5
    bool sw_volume;            ///< true when the sink has no hardware volume
    float volume_db;           ///< used only if sw_volume; <= 0
    bool limiter_enabled;      ///< soft limiter when gains can exceed 0 dBFS
    uint8_t out_bits;          ///< sink resolution for dither: 16, 24 or 32 (32 = no dither)
} dsp_config_t;

void dsp_config_defaults(dsp_config_t *c);

/// Active processing flags (for a "signal path" display).
enum {
    DSP_FLAG_RG = 1u << 0,
    DSP_FLAG_EQ = 1u << 1,
    DSP_FLAG_CROSSFEED = 1u << 2,
    DSP_FLAG_SW_VOLUME = 1u << 3,
    DSP_FLAG_LIMITER = 1u << 4,
    DSP_FLAG_DITHER = 1u << 5,
    DSP_FLAG_RESAMPLE = 1u << 6,  ///< set by the player, not by dsp
};

typedef struct dsp dsp_t;
dsp_t *dsp_create(void);
void dsp_destroy(dsp_t *d);
/// (Re)configure for a format. Keeps filter state when only gains change.
void dsp_configure(dsp_t *d, const dsp_config_t *cfg, uint32_t sample_rate, uint8_t channels);
/// ReplayGain values of the current track (NAN when absent). Call on track change.
void dsp_set_replaygain(dsp_t *d, float track_gain_db, float track_peak, float album_gain_db, float album_peak,
                        bool album_context);
void dsp_process(dsp_t *d, int32_t *pcm, uint32_t frames);  ///< in place
/// Most frames dsp_process_out() adds to a chunk: the limiter delay line (about 1 ms).
#define DSP_DRAIN_MAX 128u
/// dsp_process() with room for more output. While active the chain delays the audio by the
/// limiter lookahead. When it goes to bypass (a gain ramp ended at unity, or dsp_configure()
/// switched every stage off), that delayed audio is played out instead of dropped: after
/// the chunk when the switch happened inside this call, before it when it happened earlier.
/// Later chunks pass bit-exact. pcm holds `frames` frames and has room for cap_frames;
/// returns the frames now in pcm. frames may be 0 (only the delayed audio, e.g. at the end of
/// the stream). Without room for DSP_DRAIN_MAX more frames the delayed audio is dropped, as
/// dsp_process() does.
uint32_t dsp_process_out(dsp_t *d, int32_t *pcm, uint32_t frames, uint32_t cap_frames);
void dsp_reset(dsp_t *d);                                   ///< clear filter state (seek, track change)
bool dsp_is_bypass(const dsp_t *d);
uint32_t dsp_active_flags(const dsp_t *d);

// ---------------------------------------------------------------------------
/// Low-level building blocks, exported for tests and for the Bluetooth path.
// ---------------------------------------------------------------------------
typedef struct {
    float b0, b1, b2, a1, a2;  ///< normalized (a0 = 1)
} biquad_coef_t;

/// RBJ Audio EQ Cookbook designs.
void biquad_design(biquad_coef_t *c, peq_type_t type, float fs, float f0, float gain_db, float q);
/// Magnitude response in dB at frequency f (for EQ curve drawing and tests).
float biquad_response_db(const biquad_coef_t *c, float fs, float f);

/// Q31 <-> float
void pcm_q31_to_f32(const int32_t *in, float *out, size_t samples);
void pcm_f32_to_q31(const float *in, int32_t *out, size_t samples);  ///< saturating

/// Reduce to 16 bits with TPDF dither (state = LCG seed, keep per stream).
void pcm_q31_to_s16_dither(const int32_t *in, int16_t *out, size_t samples, uint32_t *state);
/// Upmix mono Q31 to stereo. in == out is allowed (the buffer must hold 2*frames samples).
void pcm_mono_to_stereo(const int32_t *in, int32_t *out, size_t frames);

float db_to_lin(float db);
float lin_to_db(float lin);

#ifdef __cplusplus
}
#endif
