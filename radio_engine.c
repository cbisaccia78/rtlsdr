#include "radio_engine.h"

#include "audio_buffer.h"
#include "audio_output_mac.h"
#include "demodulator.h"

#include <Accelerate/Accelerate.h>
#include <complex.h>
#include <glib.h>
#include <math.h>
#include <rtl-sdr.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SAMPLE_RATE 2048000U
#define DEFAULT_CENTER_FREQ 100000000U
#define TUNER_MIN_FREQ_HZ 24000000U
#define MIN_RADIO_SAMPLE_RATE 256000U
#define AUDIO_OUTPUT_BUFFER_FRAMES 1024U
#define AUDIO_OUTPUT_BUFFER_CAPACITY 16384U
#define AUDIO_OUTPUT_SAMPLE_RATE 48000U
#define TARGET_FM_CHANNEL_SAMPLE_RATE 250000U
#define TARGET_AM_CHANNEL_SAMPLE_RATE 96000U
#define MAX_AUDIO_SAMPLES_PER_BLOCK ((RADIO_SPECTRUM_BINS * AUDIO_OUTPUT_SAMPLE_RATE / MIN_RADIO_SAMPLE_RATE) + 8U)
#define FM_AUDIO_LOWPASS_ALPHA 0.22f
#define AM_AUDIO_LOWPASS_ALPHA 0.06f
#define FM_DEEMPHASIS_TIME_CONSTANT_US 75.0f
#define AM_AUDIO_TARGET_LEVEL 0.20f
#define AM_AUDIO_AGC_ATTACK_ALPHA 0.20f
#define AM_AUDIO_AGC_RELEASE_ALPHA 0.003f
#define AUDIO_TARGET_PEAK 0.85f
#define AUDIO_GAIN_ATTACK_ALPHA 0.35f
#define AUDIO_GAIN_RELEASE_ALPHA 0.04f
#define ADC_CENTER 127.4f
#define ADC_SCALE 128.0f
#define SPECTRUM_FLOOR_DB -120.0f
#define SPECTRUM_SMOOTHING_ALPHA 0.18f
#define RTLSDR_ASYNC_BUFFER_COUNT 12U
#define RTLSDR_ASYNC_BUFFER_LENGTH 16384U
#define RTLSDR_ERROR_NO_DEVICE -4

/*
 * Internal engine state.
 *
 * lock: guards all mutable state shared between the UI thread, worker thread,
 *     and async RTL-SDR callback.
 * thread: active GLib worker thread while a stream is being started or run.
 * dft_setup: Accelerate FFT plan reused for each spectrum calculation.
 * dev: currently open RTL-SDR device handle, or NULL while idle.
 * device_index: RTL-SDR index selected for the next or current stream.
 * device_count: most recent device enumeration result.
 * center_freq_hz: requested tuner center frequency in Hz.
 * sample_rate_hz: requested sample rate in Hz.
 * audio_sample_rate_hz: output sample rate used for speaker playback.
 * total_samples: cumulative complex IQ samples seen since the current start.
 * audio_samples_generated: cumulative demodulated samples produced for preview.
 * audio_level: RMS-like level of the latest demodulated preview block.
 * last_i: normalized in-phase component of the latest callback's first sample.
 * last_q: normalized quadrature component of the latest callback's first sample.
 * window: precomputed Hann window applied before each FFT.
 * fft_real_in: FFT input buffer for windowed I samples.
 * fft_imag_in: FFT input buffer for windowed Q samples.
 * fft_real_out: FFT output buffer for the real spectrum component.
 * fft_imag_out: FFT output buffer for the imaginary spectrum component.
 * spectrum_db: latest FFT magnitudes in dB, shifted so DC is centered.
 * audio_resample_accumulator: state for crude RF-to-audio decimation.
 * running: true while async streaming is active.
 * spectrum_ready: true after at least one FFT frame has been published.
 * demod_mode: user-selected audio demodulation mode.
 * audio_buffer: PCM ring buffer shared with the macOS audio callback.
 * audio_output: macOS audio output queue wrapper.
 * audio_requested: whether the user has asked to start demodulated audio.
 * audio_active: whether a concrete audio backend is currently active.
 * stop_requested: true after shutdown has been requested by the caller.
 * status: latest human-readable engine status message.
 */
typedef struct RadioEngine {
    GMutex lock;
    GThread *thread;
    vDSP_DFT_Setup dft_setup;
    rtlsdr_dev_t *dev;
    uint32_t device_index;
    uint32_t device_count;
    uint32_t center_freq_hz;
    uint32_t sample_rate_hz;
    uint32_t audio_sample_rate_hz;
    uint32_t channel_sample_rate_hz;
    uint32_t channel_decimation_factor;
    uint64_t total_samples;
    uint64_t audio_samples_generated;
    float audio_level;
    float last_i;
    float last_q;
    float window[RADIO_SPECTRUM_BINS];
    float complex iq_preview[RADIO_SPECTRUM_BINS];
    float demod_audio_preview[RADIO_SPECTRUM_BINS];
    float fft_real_in[RADIO_SPECTRUM_BINS];
    float fft_imag_in[RADIO_SPECTRUM_BINS];
    float fft_real_out[RADIO_SPECTRUM_BINS];
    float fft_imag_out[RADIO_SPECTRUM_BINS];
    float spectrum_power[RADIO_SPECTRUM_BINS];
    float spectrum_db[RADIO_SPECTRUM_BINS];
    float window_sum;
    float audio_gain;
    float complex channel_accumulator;
    uint32_t channel_accumulator_count;
    double audio_resample_accumulator;
    bool running;
    bool spectrum_ready;
    RadioDemodMode demod_mode;
    FmDemodState fm_demod_state;
    AudioLowPassState fm_audio_lowpass_state;
    AudioLowPassState am_audio_lowpass_state;
    FmDeemphasisState fm_deemphasis_state;
    AmAgcState am_agc_state;
    AudioBuffer *audio_buffer;
    AudioOutputMac *audio_output;
    bool audio_requested;
    bool audio_active;
    bool stop_requested;
    char status[160];
} RadioEngine;

/* Safely copy a short status or error message into an optional caller buffer. */
static void copy_message(char *destination, size_t destination_size, const char *message) {
    if (!destination || destination_size == 0) {
        return;
    }

    snprintf(destination, destination_size, "%s", message);
}

/* Format the engine status string while the caller already holds engine->lock. */
static void set_status_locked(RadioEngine *engine, const char *format, ...) {
    va_list args;

    va_start(args, format);
    vsnprintf(engine->status, sizeof(engine->status), format, args);
    va_end(args);
}

/* Reset the published spectrum so the UI does not keep drawing stale data. */
static void clear_spectrum_locked(RadioEngine *engine) {
    engine->spectrum_ready = false;
    for (uint32_t index = 0; index < RADIO_SPECTRUM_BINS; index++) {
        engine->spectrum_power[index] = 0.0f;
        engine->spectrum_db[index] = SPECTRUM_FLOOR_DB;
    }
}

static uint32_t target_channel_sample_rate_hz(RadioEngine *engine) {
    if (!engine) {
        return TARGET_FM_CHANNEL_SAMPLE_RATE;
    }

    return engine->demod_mode == RADIO_DEMOD_MODE_AM ? TARGET_AM_CHANNEL_SAMPLE_RATE : TARGET_FM_CHANNEL_SAMPLE_RATE;
}

static void configure_channelizer(RadioEngine *engine) {
    uint32_t target_sample_rate_hz;
    uint32_t decimation_factor;

    if (!engine) {
        return;
    }

    target_sample_rate_hz = target_channel_sample_rate_hz(engine);
    decimation_factor = (engine->sample_rate_hz + (target_sample_rate_hz / 2U)) / target_sample_rate_hz;
    if (decimation_factor == 0U) {
        decimation_factor = 1U;
    }

    engine->channel_decimation_factor = decimation_factor;
    engine->channel_sample_rate_hz = engine->sample_rate_hz / decimation_factor;
    engine->channel_accumulator = 0.0f + 0.0f * I;
    engine->channel_accumulator_count = 0U;
}

static bool start_audio_backend(RadioEngine *engine, char *error_message, size_t error_message_size) {
    if (!engine) {
        copy_message(error_message, error_message_size, "Engine is not initialized.");
        return false;
    }

    if (!engine->audio_buffer) {
        engine->audio_buffer = audio_buffer_create(AUDIO_OUTPUT_BUFFER_CAPACITY);
        if (!engine->audio_buffer) {
            copy_message(error_message, error_message_size, "Failed to allocate the audio buffer.");
            return false;
        }
    }

    if (!engine->audio_output) {
        engine->audio_output = audio_output_mac_create(
            engine->audio_buffer,
            engine->audio_sample_rate_hz,
            AUDIO_OUTPUT_BUFFER_FRAMES,
            error_message,
            error_message_size);
        if (!engine->audio_output) {
            return false;
        }
    }

    audio_buffer_reset(engine->audio_buffer);
    if (!audio_output_mac_start(engine->audio_output, error_message, error_message_size)) {
        return false;
    }

    g_mutex_lock(&engine->lock);
    engine->audio_gain = 1.0f;
    engine->audio_active = true;
    engine->audio_resample_accumulator = 0.0;
    g_mutex_unlock(&engine->lock);
    return true;
}

static void stop_audio_backend(RadioEngine *engine) {
    if (!engine) {
        return;
    }

    if (engine->audio_output) {
        audio_output_mac_stop(engine->audio_output);
    }
    if (engine->audio_buffer) {
        audio_buffer_reset(engine->audio_buffer);
    }

    g_mutex_lock(&engine->lock);
    engine->audio_active = false;
    engine->audio_level = 0.0f;
    engine->audio_gain = 1.0f;
    engine->audio_resample_accumulator = 0.0;
    demodulator_reset_lowpass(&engine->fm_audio_lowpass_state);
    demodulator_reset_lowpass(&engine->am_audio_lowpass_state);
    demodulator_reset_fm_deemphasis(&engine->fm_deemphasis_state);
    demodulator_reset_am_agc(&engine->am_agc_state);
    configure_channelizer(engine);
    g_mutex_unlock(&engine->lock);
}

static size_t resample_audio_block(
    RadioEngine *engine,
    uint32_t input_sample_rate_hz,
    const float *input,
    size_t input_count,
    float *output,
    size_t output_capacity) {
    size_t output_count = 0;

    if (!engine || !input || !output || input_count == 0 || output_capacity == 0 || input_sample_rate_hz == 0U) {
        return 0;
    }

    for (size_t index = 0; index < input_count; index++) {
        engine->audio_resample_accumulator += (double)engine->audio_sample_rate_hz;
        if (engine->audio_resample_accumulator >= (double)input_sample_rate_hz) {
            if (output_count < output_capacity) {
                output[output_count++] = input[index];
            }
            engine->audio_resample_accumulator -= (double)input_sample_rate_hz;
        }
    }

    return output_count;
}

static size_t channelize_iq_block(
    RadioEngine *engine,
    const float complex *input,
    size_t input_count,
    float complex *output,
    size_t output_capacity) {
    size_t output_count = 0;

    if (!engine || !input || !output || input_count == 0 || output_capacity == 0) {
        return 0;
    }

    for (size_t index = 0; index < input_count; index++) {
        engine->channel_accumulator += input[index];
        engine->channel_accumulator_count += 1U;

        if (engine->channel_accumulator_count >= engine->channel_decimation_factor) {
            if (output_count < output_capacity) {
                output[output_count++] = engine->channel_accumulator / (float)engine->channel_accumulator_count;
            }

            engine->channel_accumulator = 0.0f + 0.0f * I;
            engine->channel_accumulator_count = 0U;
        }
    }

    return output_count;
}

static float compute_audio_level(const float *samples, size_t sample_count);

static float apply_audio_gain_control(RadioEngine *engine, float *samples, size_t sample_count) {
    float peak = 0.0f;
    float target_gain;
    float smoothing_alpha;

    if (!engine || !samples || sample_count == 0) {
        return 0.0f;
    }

    for (size_t index = 0; index < sample_count; index++) {
        float magnitude = fabsf(samples[index]);

        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    if (peak < 1.0e-4f) {
        target_gain = engine->audio_gain;
    } else {
        target_gain = AUDIO_TARGET_PEAK / peak;
    }

    smoothing_alpha = target_gain < engine->audio_gain ? AUDIO_GAIN_ATTACK_ALPHA : AUDIO_GAIN_RELEASE_ALPHA;
    engine->audio_gain += smoothing_alpha * (target_gain - engine->audio_gain);

    for (size_t index = 0; index < sample_count; index++) {
        float scaled = samples[index] * engine->audio_gain;

        if (scaled > 1.0f) {
            scaled = 1.0f;
        } else if (scaled < -1.0f) {
            scaled = -1.0f;
        }

        samples[index] = scaled;
    }

    return compute_audio_level(samples, sample_count);
}

static float compute_audio_level(const float *samples, size_t sample_count) {
    float sum_squares = 0.0f;

    if (!samples || sample_count == 0) {
        return 0.0f;
    }

    for (size_t index = 0; index < sample_count; index++) {
        sum_squares += samples[index] * samples[index];
    }

    return sqrtf(sum_squares / (float)sample_count);
}

/*
 * Async RTL-SDR callback.
 *
 * Each invocation converts the first callback sample into normalized floating
 * point IQ for quick status display, and when enough samples are available it
 * computes a Hann-windowed FFT frame for the spectrum analyzer.
 */
static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx) {
    RadioEngine *engine = ctx;
    uint32_t num_samples = len / 2;
    float complex first_sample = 0.0f;
    float complex iq_block[RADIO_SPECTRUM_BINS];
    float complex channel_iq_block[RADIO_SPECTRUM_BINS];
    float demod_audio_block[RADIO_SPECTRUM_BINS];
    float resampled_audio[MAX_AUDIO_SAMPLES_PER_BLOCK];
    float spectrum_db[RADIO_SPECTRUM_BINS];
    float spectrum_power[RADIO_SPECTRUM_BINS];
    size_t total_resampled_count = 0;
    float audio_level = 0.0f;
    bool audio_active = false;
    bool spectrum_ready = false;

    if (num_samples > 0) {
        float i_float = ((float)buf[0] - ADC_CENTER) / ADC_SCALE;
        float q_float = ((float)buf[1] - ADC_CENTER) / ADC_SCALE;
        first_sample = i_float + q_float * I;
    }

    if (num_samples >= RADIO_SPECTRUM_BINS) {
        float normalization = engine->window_sum * engine->window_sum;

        for (uint32_t chunk_start = 0; chunk_start < num_samples; chunk_start += RADIO_SPECTRUM_BINS) {
            uint32_t chunk_size = num_samples - chunk_start;
            float mean_i = 0.0f;
            float mean_q = 0.0f;

            if (chunk_size > RADIO_SPECTRUM_BINS) {
                chunk_size = RADIO_SPECTRUM_BINS;
            }

            for (uint32_t index = 0; index < chunk_size; index++) {
                uint32_t sample_index = chunk_start + index;

                mean_i += ((float)buf[2 * sample_index] - ADC_CENTER) / ADC_SCALE;
                mean_q += ((float)buf[2 * sample_index + 1] - ADC_CENTER) / ADC_SCALE;
            }

            mean_i /= (float)chunk_size;
            mean_q /= (float)chunk_size;

            for (uint32_t index = 0; index < chunk_size; index++) {
                uint32_t sample_index = chunk_start + index;
                float i_float = (((float)buf[2 * sample_index] - ADC_CENTER) / ADC_SCALE) - mean_i;
                float q_float = (((float)buf[2 * sample_index + 1] - ADC_CENTER) / ADC_SCALE) - mean_q;

                iq_block[index] = i_float + q_float * I;

                if (chunk_start == 0U) {
                    float window = engine->window[index];

                    engine->iq_preview[index] = iq_block[index];
                    engine->fft_real_in[index] = i_float * window;
                    engine->fft_imag_in[index] = q_float * window;
                }
            }

            if (engine->audio_requested && engine->demod_mode != RADIO_DEMOD_MODE_OFF) {
                size_t channel_iq_count = 0;
                size_t audio_count = 0;
                size_t resampled_count = 0;

                channel_iq_count = channelize_iq_block(
                    engine,
                    iq_block,
                    chunk_size,
                    channel_iq_block,
                    sizeof(channel_iq_block) / sizeof(channel_iq_block[0]));

                if (engine->demod_mode == RADIO_DEMOD_MODE_FM) {
                    audio_count = demodulator_demodulate_fm(
                        &engine->fm_demod_state,
                        channel_iq_block,
                        channel_iq_count,
                        demod_audio_block);
                } else if (engine->demod_mode == RADIO_DEMOD_MODE_AM) {
                    audio_count = demodulator_demodulate_am(
                        channel_iq_block,
                        channel_iq_count,
                        demod_audio_block);
                }

                if (audio_count > 0) {
                    demodulator_remove_dc(demod_audio_block, audio_count);
                    resampled_count = resample_audio_block(
                        engine,
                        engine->channel_sample_rate_hz,
                        demod_audio_block,
                        audio_count,
                        resampled_audio,
                        sizeof(resampled_audio) / sizeof(resampled_audio[0]));

                    if (resampled_count > 0) {
                        if (engine->demod_mode == RADIO_DEMOD_MODE_FM) {
                            demodulator_apply_lowpass(&engine->fm_audio_lowpass_state, resampled_audio, resampled_count, FM_AUDIO_LOWPASS_ALPHA);
                            demodulator_apply_fm_deemphasis(
                                &engine->fm_deemphasis_state,
                                resampled_audio,
                                resampled_count,
                                (float)engine->audio_sample_rate_hz,
                                FM_DEEMPHASIS_TIME_CONSTANT_US);
                        } else if (engine->demod_mode == RADIO_DEMOD_MODE_AM) {
                            demodulator_apply_am_agc(
                                &engine->am_agc_state,
                                resampled_audio,
                                resampled_count,
                                AM_AUDIO_TARGET_LEVEL,
                                AM_AUDIO_AGC_ATTACK_ALPHA,
                                AM_AUDIO_AGC_RELEASE_ALPHA);
                            demodulator_apply_lowpass(&engine->am_audio_lowpass_state, resampled_audio, resampled_count, AM_AUDIO_LOWPASS_ALPHA);
                        }

                        audio_level = apply_audio_gain_control(engine, resampled_audio, resampled_count);
                        total_resampled_count += resampled_count;
                    }

                    if (engine->audio_requested && engine->audio_output && audio_output_mac_is_running(engine->audio_output) && resampled_count > 0) {
                        audio_buffer_push(engine->audio_buffer, resampled_audio, resampled_count);
                        audio_active = true;
                    }
                }
            }
        }

        vDSP_DFT_Execute(
            engine->dft_setup,
            engine->fft_real_in,
            engine->fft_imag_in,
            engine->fft_real_out,
            engine->fft_imag_out);

        for (uint32_t index = 0; index < RADIO_SPECTRUM_BINS; index++) {
            uint32_t shifted_index = (index + (RADIO_SPECTRUM_BINS / 2U)) % RADIO_SPECTRUM_BINS;
            float real = engine->fft_real_out[shifted_index];
            float imag = engine->fft_imag_out[shifted_index];
            float power = ((real * real) + (imag * imag)) / normalization;
            float db = 10.0f * log10f(power + 1.0e-12f);

            spectrum_power[index] = power;
            spectrum_db[index] = db < SPECTRUM_FLOOR_DB ? SPECTRUM_FLOOR_DB : db;
        }

        spectrum_ready = true;
    }

    g_mutex_lock(&engine->lock);
    engine->total_samples += num_samples;
    if (num_samples > 0) {
        engine->last_i = crealf(first_sample);
        engine->last_q = cimagf(first_sample);
    }
    if (spectrum_ready) {
        for (uint32_t index = 0; index < RADIO_SPECTRUM_BINS; index++) {
            float averaged_power;

            if (engine->spectrum_ready) {
                averaged_power =
                    ((1.0f - SPECTRUM_SMOOTHING_ALPHA) * engine->spectrum_power[index]) +
                    (SPECTRUM_SMOOTHING_ALPHA * spectrum_power[index]);
            } else {
                averaged_power = spectrum_power[index];
            }

            engine->spectrum_power[index] = averaged_power;
            engine->spectrum_db[index] = fmaxf(10.0f * log10f(averaged_power + 1.0e-12f), SPECTRUM_FLOOR_DB);
        }
        engine->spectrum_ready = true;
    }
    engine->audio_active = engine->audio_requested && audio_active;
    engine->audio_level = audio_level;
    engine->audio_samples_generated += total_resampled_count;
    g_mutex_unlock(&engine->lock);
}

/*
 * Worker thread entry point.
 *
 * This owns the full hardware lifecycle for a running stream: enumerate/open
 * the requested device, apply the current radio settings, start async reading,
 * then publish a final status and close the device when the stream ends.
 */
static gpointer radio_engine_thread(gpointer user_data) {
    RadioEngine *engine = user_data;
    rtlsdr_dev_t *dev = NULL;
    char audio_message[160];
    uint32_t device_index;
    uint32_t center_freq_hz;
    uint32_t sample_rate_hz;
    int direct_sampling_mode;
    int result;

    g_mutex_lock(&engine->lock);
    device_index = engine->device_index;
    center_freq_hz = engine->center_freq_hz;
    sample_rate_hz = engine->sample_rate_hz;
    engine->device_count = rtlsdr_get_device_count();
    if (engine->device_count == 0) {
        set_status_locked(engine, "No RTL-SDR devices found.");
        engine->thread = NULL;
        g_mutex_unlock(&engine->lock);
        return NULL;
    }

    if (device_index >= engine->device_count) {
        set_status_locked(engine, "Device index %u is out of range.", device_index);
        engine->thread = NULL;
        g_mutex_unlock(&engine->lock);
        return NULL;
    }

    g_mutex_unlock(&engine->lock);

    result = rtlsdr_open(&dev, device_index);
    if (result < 0) {
        g_mutex_lock(&engine->lock);
        set_status_locked(engine, "Failed to open device %u.", device_index);
        engine->thread = NULL;
        g_mutex_unlock(&engine->lock);
        return NULL;
    }

    g_mutex_lock(&engine->lock);
    engine->dev = dev;
    g_mutex_unlock(&engine->lock);

    direct_sampling_mode = center_freq_hz < TUNER_MIN_FREQ_HZ ? 2 : 0;
    result = rtlsdr_set_direct_sampling(dev, direct_sampling_mode);
    if (result < 0) {
        g_mutex_lock(&engine->lock);
        set_status_locked(
            engine,
            direct_sampling_mode != 0 ? "Failed to enable direct sampling for %.3f MHz." : "Failed to disable direct sampling.",
            center_freq_hz / 1000000.0);
        engine->dev = NULL;
        engine->thread = NULL;
        g_mutex_unlock(&engine->lock);
        rtlsdr_close(dev);
        return NULL;
    }

    result = rtlsdr_set_sample_rate(dev, sample_rate_hz);
    if (result < 0) {
        g_mutex_lock(&engine->lock);
        set_status_locked(engine, "Failed to set sample rate.");
        engine->dev = NULL;
        engine->thread = NULL;
        g_mutex_unlock(&engine->lock);
        rtlsdr_close(dev);
        return NULL;
    }

    result = rtlsdr_set_center_freq(dev, center_freq_hz);
    if (result < 0) {
        g_mutex_lock(&engine->lock);
        set_status_locked(engine, "Failed to set center frequency.");
        engine->dev = NULL;
        engine->thread = NULL;
        g_mutex_unlock(&engine->lock);
        rtlsdr_close(dev);
        return NULL;
    }

    if (direct_sampling_mode == 0) {
        result = rtlsdr_set_tuner_gain_mode(dev, 0);
        if (result < 0) {
            g_mutex_lock(&engine->lock);
            set_status_locked(engine, "Failed to enable automatic gain control.");
            engine->dev = NULL;
            engine->thread = NULL;
            g_mutex_unlock(&engine->lock);
            rtlsdr_close(dev);
            return NULL;
        }
    }

    result = rtlsdr_reset_buffer(dev);
    if (result < 0) {
        g_mutex_lock(&engine->lock);
        set_status_locked(engine, "Failed to reset the RTL-SDR buffer.");
        engine->dev = NULL;
        engine->thread = NULL;
        g_mutex_unlock(&engine->lock);
        rtlsdr_close(dev);
        return NULL;
    }

    g_mutex_lock(&engine->lock);
    engine->running = true;
    set_status_locked(
        engine,
        direct_sampling_mode != 0 ? "Streaming %.3f MHz at %.3f MS/s using direct sampling." : "Streaming %.3f MHz at %.3f MS/s.",
        center_freq_hz / 1000000.0,
        sample_rate_hz / 1000000.0);
    g_mutex_unlock(&engine->lock);

    if (engine->audio_requested && engine->demod_mode != RADIO_DEMOD_MODE_OFF) {
        if (!start_audio_backend(engine, audio_message, sizeof(audio_message))) {
            g_mutex_lock(&engine->lock);
            set_status_locked(engine, "Streaming without audio: %s", audio_message);
            g_mutex_unlock(&engine->lock);
        }
    }

    result = rtlsdr_read_async(
        dev,
        rtlsdr_callback,
        engine,
        RTLSDR_ASYNC_BUFFER_COUNT,
        RTLSDR_ASYNC_BUFFER_LENGTH);

    stop_audio_backend(engine);

    g_mutex_lock(&engine->lock);
    engine->running = false;
    engine->dev = NULL;
    engine->thread = NULL;
    if (engine->stop_requested) {
        set_status_locked(engine, "Stopped.");
        clear_spectrum_locked(engine);
    } else if (result == RTLSDR_ERROR_NO_DEVICE) {
        set_status_locked(engine, "RTL-SDR device lost. Reconnect it, then press Start.");
        clear_spectrum_locked(engine);
    } else if (result < 0) {
        set_status_locked(engine, "Streaming stopped with RTL-SDR error %d.", result);
        clear_spectrum_locked(engine);
    } else {
        set_status_locked(engine, "Streaming ended.");
    }
    g_mutex_unlock(&engine->lock);

    rtlsdr_close(dev);
    return NULL;
}

/* Create an idle engine with default radio settings and FFT resources. */
RadioEngine *radio_engine_new(void) {
    RadioEngine *engine = calloc(1, sizeof(*engine));

    if (!engine) {
        return NULL;
    }

    g_mutex_init(&engine->lock);
    engine->dft_setup = vDSP_DFT_zop_CreateSetup(NULL, RADIO_SPECTRUM_BINS, vDSP_DFT_FORWARD);
    if (!engine->dft_setup) {
        g_mutex_clear(&engine->lock);
        free(engine);
        return NULL;
    }

    vDSP_hann_window(engine->window, RADIO_SPECTRUM_BINS, vDSP_HANN_NORM);
    engine->window_sum = 0.0f;
    for (uint32_t index = 0; index < RADIO_SPECTRUM_BINS; index++) {
        engine->window_sum += engine->window[index];
    }
    for (uint32_t index = 0; index < RADIO_SPECTRUM_BINS; index++) {
        engine->spectrum_power[index] = 0.0f;
        engine->spectrum_db[index] = SPECTRUM_FLOOR_DB;
    }
    engine->center_freq_hz = DEFAULT_CENTER_FREQ;
    engine->sample_rate_hz = DEFAULT_SAMPLE_RATE;
    engine->audio_sample_rate_hz = AUDIO_OUTPUT_SAMPLE_RATE;
    engine->demod_mode = RADIO_DEMOD_MODE_OFF;
    engine->audio_requested = false;
    engine->audio_active = false;
    engine->audio_samples_generated = 0;
    engine->audio_level = 0.0f;
    engine->audio_gain = 1.0f;
    engine->audio_resample_accumulator = 0.0;
    configure_channelizer(engine);
    demodulator_reset_fm(&engine->fm_demod_state);
    demodulator_reset_lowpass(&engine->fm_audio_lowpass_state);
    demodulator_reset_lowpass(&engine->am_audio_lowpass_state);
    demodulator_reset_fm_deemphasis(&engine->fm_deemphasis_state);
    demodulator_reset_am_agc(&engine->am_agc_state);
    engine->device_count = rtlsdr_get_device_count();
    snprintf(engine->status, sizeof(engine->status), "Ready.");
    return engine;
}

/* Release all engine resources after ensuring the worker has stopped. */
void radio_engine_free(RadioEngine *engine) {
    if (!engine) {
        return;
    }

    radio_engine_stop(engine);
    audio_output_mac_free(engine->audio_output);
    audio_buffer_free(engine->audio_buffer);
    if (engine->dft_setup) {
        vDSP_DFT_DestroySetup(engine->dft_setup);
    }
    g_mutex_clear(&engine->lock);
    free(engine);
}

/* Apply a new center frequency, optionally forwarding it to live hardware. */
bool radio_engine_set_center_freq(RadioEngine *engine, uint32_t center_freq_hz, char *error_message, size_t error_message_size) {
    int result = 0;
    bool running;
    rtlsdr_dev_t *dev;

    if (!engine) {
        copy_message(error_message, error_message_size, "Engine is not initialized.");
        return false;
    }

    g_mutex_lock(&engine->lock);
    engine->center_freq_hz = center_freq_hz;
    running = engine->running;
    dev = engine->dev;
    g_mutex_unlock(&engine->lock);

    if (running && dev) {
        result = rtlsdr_set_center_freq(dev, center_freq_hz);
        if (result < 0) {
            copy_message(error_message, error_message_size, "Failed to change center frequency while streaming.");
            return false;
        }
    }

    copy_message(error_message, error_message_size, "Center frequency updated.");
    return true;
}

/* Store a new sample rate and report whether it can take effect immediately. */
bool radio_engine_set_sample_rate(RadioEngine *engine, uint32_t sample_rate_hz, char *error_message, size_t error_message_size) {
    if (!engine) {
        copy_message(error_message, error_message_size, "Engine is not initialized.");
        return false;
    }

    g_mutex_lock(&engine->lock);
    engine->sample_rate_hz = sample_rate_hz;
    engine->audio_gain = 1.0f;
    engine->audio_resample_accumulator = 0.0;
    configure_channelizer(engine);
    demodulator_reset_am_agc(&engine->am_agc_state);
    if (engine->running) {
        set_status_locked(engine, "Sample rate queued. Stop and restart to apply %.3f MS/s.", sample_rate_hz / 1000000.0);
        g_mutex_unlock(&engine->lock);
        copy_message(error_message, error_message_size, "Stop streaming before the new sample rate takes effect.");
        return false;
    }

    set_status_locked(engine, "Sample rate set to %.3f MS/s.", sample_rate_hz / 1000000.0);
    g_mutex_unlock(&engine->lock);

    copy_message(error_message, error_message_size, "Sample rate updated.");
    return true;
}

/* Store the selected demodulation mode for later audio pipeline activation. */
bool radio_engine_set_demod_mode(RadioEngine *engine, RadioDemodMode demod_mode, char *error_message, size_t error_message_size) {
    if (!engine) {
        copy_message(error_message, error_message_size, "Engine is not initialized.");
        return false;
    }

    g_mutex_lock(&engine->lock);
    engine->demod_mode = demod_mode;
    engine->audio_gain = 1.0f;
    if (demod_mode != RADIO_DEMOD_MODE_FM) {
        demodulator_reset_fm(&engine->fm_demod_state);
        demodulator_reset_fm_deemphasis(&engine->fm_deemphasis_state);
    }
    demodulator_reset_lowpass(&engine->am_audio_lowpass_state);
    demodulator_reset_am_agc(&engine->am_agc_state);
    if (demod_mode != RADIO_DEMOD_MODE_FM) {
        demodulator_reset_lowpass(&engine->fm_audio_lowpass_state);
    }
    configure_channelizer(engine);
    g_mutex_unlock(&engine->lock);

    if (demod_mode == RADIO_DEMOD_MODE_OFF) {
        stop_audio_backend(engine);
    } else if (engine->audio_requested && engine->running) {
        if (!start_audio_backend(engine, error_message, error_message_size)) {
            return false;
        }
    }

    copy_message(error_message, error_message_size, "Demod mode updated.");
    return true;
}

/* Store whether the user wants the future demodulated audio path to run. */
bool radio_engine_set_audio_requested(RadioEngine *engine, bool audio_requested, char *error_message, size_t error_message_size) {
    if (!engine) {
        copy_message(error_message, error_message_size, "Engine is not initialized.");
        return false;
    }

    g_mutex_lock(&engine->lock);
    engine->audio_requested = audio_requested;
    engine->audio_active = false;
    engine->audio_level = 0.0f;
    engine->audio_gain = 1.0f;
    g_mutex_unlock(&engine->lock);

    if (!audio_requested) {
        stop_audio_backend(engine);
    } else if (engine->running && engine->demod_mode != RADIO_DEMOD_MODE_OFF) {
        if (!start_audio_backend(engine, error_message, error_message_size)) {
            return false;
        }
    }

    copy_message(error_message, error_message_size, audio_requested ? "Audio requested." : "Audio disabled.");
    return true;
}

/* Spawn the RTL-SDR worker thread using the engine's current configuration. */
bool radio_engine_start(RadioEngine *engine, uint32_t device_index, char *error_message, size_t error_message_size) {
    GThread *thread;

    if (!engine) {
        copy_message(error_message, error_message_size, "Engine is not initialized.");
        return false;
    }

    g_mutex_lock(&engine->lock);
    if (engine->thread || engine->running) {
        g_mutex_unlock(&engine->lock);
        copy_message(error_message, error_message_size, "Streaming is already active.");
        return false;
    }

    engine->device_index = device_index;
    engine->device_count = rtlsdr_get_device_count();
    engine->total_samples = 0;
    engine->audio_samples_generated = 0;
    engine->audio_level = 0.0f;
    engine->last_i = 0.0f;
    engine->last_q = 0.0f;
    engine->spectrum_ready = false;
    engine->audio_gain = 1.0f;
    engine->audio_resample_accumulator = 0.0;
    configure_channelizer(engine);
    demodulator_reset_lowpass(&engine->fm_audio_lowpass_state);
    demodulator_reset_lowpass(&engine->am_audio_lowpass_state);
    demodulator_reset_fm_deemphasis(&engine->fm_deemphasis_state);
    demodulator_reset_am_agc(&engine->am_agc_state);
    for (uint32_t index = 0; index < RADIO_SPECTRUM_BINS; index++) {
        engine->spectrum_db[index] = SPECTRUM_FLOOR_DB;
    }
    engine->stop_requested = false;
    engine->audio_active = false;
    demodulator_reset_fm(&engine->fm_demod_state);
    set_status_locked(engine, "Starting stream...");
    g_mutex_unlock(&engine->lock);

    thread = g_thread_new("rtlsdr-worker", radio_engine_thread, engine);
    if (!thread) {
        copy_message(error_message, error_message_size, "Failed to create the radio thread.");
        return false;
    }

    g_mutex_lock(&engine->lock);
    engine->thread = thread;
    g_mutex_unlock(&engine->lock);

    copy_message(error_message, error_message_size, "Streaming thread started.");
    return true;
}

/* Request async cancellation and block until the worker thread exits. */
void radio_engine_stop(RadioEngine *engine) {
    GThread *thread;
    rtlsdr_dev_t *dev;

    if (!engine) {
        return;
    }

    g_mutex_lock(&engine->lock);
    thread = engine->thread;
    dev = engine->dev;
    engine->stop_requested = true;
    g_mutex_unlock(&engine->lock);

    if (dev) {
        rtlsdr_cancel_async(dev);
    }

    if (thread) {
        g_thread_join(thread);
    }
}

/* Copy the current engine state into a snapshot for GTK rendering. */
void radio_engine_get_snapshot(RadioEngine *engine, RadioEngineSnapshot *snapshot) {
    if (!engine || !snapshot) {
        return;
    }

    g_mutex_lock(&engine->lock);
    snapshot->running = engine->running;
    snapshot->spectrum_ready = engine->spectrum_ready;
    snapshot->audio_requested = engine->audio_requested;
    snapshot->audio_active = engine->audio_active;
    snapshot->demod_mode = engine->demod_mode;
    snapshot->audio_output_sample_rate_hz = engine->audio_sample_rate_hz;
    snapshot->audio_buffer_fill = engine->audio_buffer ? audio_buffer_size(engine->audio_buffer) : 0U;
    snapshot->audio_buffer_capacity = engine->audio_buffer ? audio_buffer_capacity(engine->audio_buffer) : 0U;
    snapshot->device_count = engine->device_count;
    snapshot->center_freq_hz = engine->center_freq_hz;
    snapshot->sample_rate_hz = engine->sample_rate_hz;
    snapshot->total_samples = engine->total_samples;
    snapshot->audio_samples_generated = engine->audio_samples_generated;
    snapshot->audio_level = engine->audio_level;
    snapshot->last_i = engine->last_i;
    snapshot->last_q = engine->last_q;
    memcpy(snapshot->spectrum_db, engine->spectrum_db, sizeof(snapshot->spectrum_db));
    snprintf(snapshot->status, sizeof(snapshot->status), "%s", engine->status);
    g_mutex_unlock(&engine->lock);
}
