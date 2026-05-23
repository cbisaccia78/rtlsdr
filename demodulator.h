#ifndef DEMODULATOR_H
#define DEMODULATOR_H

#include <complex.h>
#include <stddef.h>

/*
 * FM demodulation state. The previous IQ sample is preserved so phase
 * differentiation remains continuous across adjacent blocks.
 */
typedef struct {
    float complex previous_sample;
    int has_previous_sample;
} FmDemodState;

/* One-pole audio low-pass state used to smooth crude demodulated output. */
typedef struct {
    float previous_output;
    int has_previous_output;
} AudioLowPassState;

/* FM deemphasis state modeled as a first-order IIR at audio sample rate. */
typedef struct {
    float previous_input;
    float previous_output;
    int has_previous_sample;
} FmDeemphasisState;

/* Reset the FM demodulator state before processing a new stream. */
void demodulator_reset_fm(FmDemodState *state);

/* Reset the state used by the simple audio low-pass filter. */
void demodulator_reset_lowpass(AudioLowPassState *state);

/* Reset the FM deemphasis state before starting a new audio stream. */
void demodulator_reset_fm_deemphasis(FmDeemphasisState *state);

/*
 * Convert a block of complex IQ samples into FM-demodulated audio.
 *
 * Returns the number of audio samples written, which matches iq_count.
 */
size_t demodulator_demodulate_fm(
    FmDemodState *state,
    const float complex *iq_samples,
    size_t iq_count,
    float *audio_out);

/*
 * Convert a block of complex IQ samples into an AM envelope.
 *
 * Returns the number of audio samples written, which matches iq_count.
 */
size_t demodulator_demodulate_am(
    const float complex *iq_samples,
    size_t iq_count,
    float *audio_out);

/* Remove the mean value from a block of audio samples in place. */
void demodulator_remove_dc(float *samples, size_t sample_count);

/* Normalize a block of audio samples to the range [-1, 1] if possible. */
void demodulator_normalize_audio(float *samples, size_t sample_count);

/* Apply a simple one-pole low-pass filter in place. */
void demodulator_apply_lowpass(AudioLowPassState *state, float *samples, size_t sample_count, float alpha);

/* Apply a first-order FM deemphasis curve in place. */
void demodulator_apply_fm_deemphasis(
    FmDeemphasisState *state,
    float *samples,
    size_t sample_count,
    float sample_rate_hz,
    float time_constant_us);

#endif
