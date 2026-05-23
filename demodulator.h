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

/* Reset the FM demodulator state before processing a new stream. */
void demodulator_reset_fm(FmDemodState *state);

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

#endif
