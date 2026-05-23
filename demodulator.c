#include "demodulator.h"

#include <math.h>

void demodulator_reset_fm(FmDemodState *state) {
    if (!state) {
        return;
    }

    state->previous_sample = 0.0f + 0.0f * I;
    state->has_previous_sample = 0;
}

size_t demodulator_demodulate_fm(
    FmDemodState *state,
    const float complex *iq_samples,
    size_t iq_count,
    float *audio_out) {
    if (!state || !iq_samples || !audio_out) {
        return 0;
    }

    for (size_t index = 0; index < iq_count; index++) {
        float complex current = iq_samples[index];

        if (!state->has_previous_sample) {
            audio_out[index] = 0.0f;
            state->previous_sample = current;
            state->has_previous_sample = 1;
            continue;
        }

        audio_out[index] = cargf(current * conjf(state->previous_sample));
        state->previous_sample = current;
    }

    return iq_count;
}

size_t demodulator_demodulate_am(
    const float complex *iq_samples,
    size_t iq_count,
    float *audio_out) {
    if (!iq_samples || !audio_out) {
        return 0;
    }

    for (size_t index = 0; index < iq_count; index++) {
        audio_out[index] = cabsf(iq_samples[index]);
    }

    return iq_count;
}

void demodulator_remove_dc(float *samples, size_t sample_count) {
    float mean = 0.0f;

    if (!samples || sample_count == 0) {
        return;
    }

    for (size_t index = 0; index < sample_count; index++) {
        mean += samples[index];
    }

    mean /= (float)sample_count;

    for (size_t index = 0; index < sample_count; index++) {
        samples[index] -= mean;
    }
}

void demodulator_normalize_audio(float *samples, size_t sample_count) {
    float peak = 0.0f;

    if (!samples || sample_count == 0) {
        return;
    }

    for (size_t index = 0; index < sample_count; index++) {
        float magnitude = fabsf(samples[index]);

        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    if (peak <= 0.0f) {
        return;
    }

    for (size_t index = 0; index < sample_count; index++) {
        samples[index] /= peak;
    }
}
