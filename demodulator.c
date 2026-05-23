#include "demodulator.h"

#include <math.h>

void demodulator_reset_fm(FmDemodState *state) {
    if (!state) {
        return;
    }

    state->previous_sample = 0.0f + 0.0f * I;
    state->has_previous_sample = 0;
}

void demodulator_reset_lowpass(AudioLowPassState *state) {
    if (!state) {
        return;
    }

    state->previous_output = 0.0f;
    state->has_previous_output = 0;
}

void demodulator_reset_highpass(AudioHighPassState *state) {
    if (!state) {
        return;
    }

    state->previous_input = 0.0f;
    state->previous_output = 0.0f;
    state->has_previous_sample = 0;
}

void demodulator_reset_fm_deemphasis(FmDeemphasisState *state) {
    if (!state) {
        return;
    }

    state->previous_input = 0.0f;
    state->previous_output = 0.0f;
    state->has_previous_sample = 0;
}

void demodulator_reset_am_agc(AmAgcState *state) {
    if (!state) {
        return;
    }

    state->gain = 1.0f;
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

void demodulator_apply_lowpass(AudioLowPassState *state, float *samples, size_t sample_count, float alpha) {
    float previous_output;

    if (!state || !samples || sample_count == 0) {
        return;
    }

    if (alpha <= 0.0f) {
        alpha = 0.0f;
    }
    if (alpha >= 1.0f) {
        alpha = 1.0f;
    }

    previous_output = state->has_previous_output ? state->previous_output : samples[0];
    for (size_t index = 0; index < sample_count; index++) {
        previous_output += alpha * (samples[index] - previous_output);
        samples[index] = previous_output;
    }

    state->previous_output = previous_output;
    state->has_previous_output = 1;
}

void demodulator_apply_highpass(AudioHighPassState *state, float *samples, size_t sample_count, float alpha) {
    float previous_input;
    float previous_output;

    if (!state || !samples || sample_count == 0) {
        return;
    }

    if (alpha <= 0.0f) {
        alpha = 0.0f;
    }
    if (alpha >= 1.0f) {
        alpha = 1.0f;
    }

    previous_input = state->has_previous_sample ? state->previous_input : samples[0];
    previous_output = state->has_previous_sample ? state->previous_output : 0.0f;

    for (size_t index = 0; index < sample_count; index++) {
        float current_input = samples[index];
        float current_output = alpha * (previous_output + current_input - previous_input);

        samples[index] = current_output;
        previous_input = current_input;
        previous_output = current_output;
    }

    state->previous_input = previous_input;
    state->previous_output = previous_output;
    state->has_previous_sample = 1;
}

void demodulator_apply_fm_deemphasis(
    FmDeemphasisState *state,
    float *samples,
    size_t sample_count,
    float sample_rate_hz,
    float time_constant_us) {
    float sample_interval_seconds;
    float time_constant_seconds;
    float alpha;
    float previous_input;
    float previous_output;

    if (!state || !samples || sample_count == 0 || sample_rate_hz <= 0.0f || time_constant_us <= 0.0f) {
        return;
    }

    sample_interval_seconds = 1.0f / sample_rate_hz;
    time_constant_seconds = time_constant_us * 1.0e-6f;
    alpha = expf(-sample_interval_seconds / time_constant_seconds);
    previous_input = state->has_previous_sample ? state->previous_input : samples[0];
    previous_output = state->has_previous_sample ? state->previous_output : samples[0];

    for (size_t index = 0; index < sample_count; index++) {
        float current_input = samples[index];
        float current_output = alpha * previous_output + (1.0f - alpha) * current_input;

        samples[index] = current_output;
        previous_input = current_input;
        previous_output = current_output;
    }

    state->previous_input = previous_input;
    state->previous_output = previous_output;
    state->has_previous_sample = 1;
}

void demodulator_apply_am_agc(
    AmAgcState *state,
    float *samples,
    size_t sample_count,
    float target_level,
    float attack_alpha,
    float release_alpha) {
    if (!state || !samples || sample_count == 0) {
        return;
    }

    if (target_level <= 0.0f) {
        target_level = 0.2f;
    }

    for (size_t index = 0; index < sample_count; index++) {
        float magnitude = fabsf(samples[index]);
        float desired_gain;
        float alpha;

        if (magnitude < 1.0e-4f) {
            desired_gain = state->gain;
        } else {
            desired_gain = target_level / magnitude;
        }

        alpha = desired_gain < state->gain ? attack_alpha : release_alpha;
        if (alpha < 0.0f) {
            alpha = 0.0f;
        }
        if (alpha > 1.0f) {
            alpha = 1.0f;
        }

        state->gain += alpha * (desired_gain - state->gain);
        samples[index] *= state->gain;
    }
}
