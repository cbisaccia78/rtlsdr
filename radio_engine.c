#include "radio_engine.h"

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
#define ADC_CENTER 127.4f
#define ADC_SCALE 128.0f
#define SPECTRUM_FLOOR_DB -120.0f

typedef struct RadioEngine {
    GMutex lock;
    GThread *thread;
    vDSP_DFT_Setup dft_setup;
    rtlsdr_dev_t *dev;
    uint32_t device_index;
    uint32_t device_count;
    uint32_t center_freq_hz;
    uint32_t sample_rate_hz;
    uint64_t total_samples;
    float last_i;
    float last_q;
    float window[RADIO_SPECTRUM_BINS];
    float fft_real_in[RADIO_SPECTRUM_BINS];
    float fft_imag_in[RADIO_SPECTRUM_BINS];
    float fft_real_out[RADIO_SPECTRUM_BINS];
    float fft_imag_out[RADIO_SPECTRUM_BINS];
    float spectrum_db[RADIO_SPECTRUM_BINS];
    bool running;
    bool spectrum_ready;
    bool stop_requested;
    char status[160];
} RadioEngine;

static void copy_message(char *destination, size_t destination_size, const char *message) {
    if (!destination || destination_size == 0) {
        return;
    }

    snprintf(destination, destination_size, "%s", message);
}

static void set_status_locked(RadioEngine *engine, const char *format, ...) {
    va_list args;

    va_start(args, format);
    vsnprintf(engine->status, sizeof(engine->status), format, args);
    va_end(args);
}

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx) {
    RadioEngine *engine = ctx;
    uint32_t num_samples = len / 2;
    float complex first_sample = 0.0f;
    float spectrum_db[RADIO_SPECTRUM_BINS];
    bool spectrum_ready = false;

    if (num_samples > 0) {
        float i_float = ((float)buf[0] - ADC_CENTER) / ADC_SCALE;
        float q_float = ((float)buf[1] - ADC_CENTER) / ADC_SCALE;
        first_sample = i_float + q_float * I;
    }

    if (num_samples >= RADIO_SPECTRUM_BINS) {
        for (uint32_t index = 0; index < RADIO_SPECTRUM_BINS; index++) {
            float i_float = ((float)buf[2 * index] - ADC_CENTER) / ADC_SCALE;
            float q_float = ((float)buf[2 * index + 1] - ADC_CENTER) / ADC_SCALE;
            float window = engine->window[index];

            engine->fft_real_in[index] = i_float * window;
            engine->fft_imag_in[index] = q_float * window;
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
            float power = (real * real) + (imag * imag);
            float db = 10.0f * log10f(power + 1.0e-12f);

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
        memcpy(engine->spectrum_db, spectrum_db, sizeof(spectrum_db));
        engine->spectrum_ready = true;
    }
    g_mutex_unlock(&engine->lock);
}

static gpointer radio_engine_thread(gpointer user_data) {
    RadioEngine *engine = user_data;
    rtlsdr_dev_t *dev = NULL;
    uint32_t device_index;
    uint32_t center_freq_hz;
    uint32_t sample_rate_hz;
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
    set_status_locked(engine, "Streaming %.3f MHz at %.3f MS/s.", center_freq_hz / 1000000.0, sample_rate_hz / 1000000.0);
    g_mutex_unlock(&engine->lock);

    result = rtlsdr_read_async(dev, rtlsdr_callback, engine, 0, 0);

    g_mutex_lock(&engine->lock);
    engine->running = false;
    engine->dev = NULL;
    engine->thread = NULL;
    if (engine->stop_requested) {
        set_status_locked(engine, "Stopped.");
    } else if (result < 0) {
        set_status_locked(engine, "Streaming stopped with RTL-SDR error %d.", result);
    } else {
        set_status_locked(engine, "Streaming ended.");
    }
    g_mutex_unlock(&engine->lock);

    rtlsdr_close(dev);
    return NULL;
}

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
    for (uint32_t index = 0; index < RADIO_SPECTRUM_BINS; index++) {
        engine->spectrum_db[index] = SPECTRUM_FLOOR_DB;
    }
    engine->center_freq_hz = DEFAULT_CENTER_FREQ;
    engine->sample_rate_hz = DEFAULT_SAMPLE_RATE;
    engine->device_count = rtlsdr_get_device_count();
    snprintf(engine->status, sizeof(engine->status), "Ready.");
    return engine;
}

void radio_engine_free(RadioEngine *engine) {
    if (!engine) {
        return;
    }

    radio_engine_stop(engine);
    if (engine->dft_setup) {
        vDSP_DFT_DestroySetup(engine->dft_setup);
    }
    g_mutex_clear(&engine->lock);
    free(engine);
}

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

bool radio_engine_set_sample_rate(RadioEngine *engine, uint32_t sample_rate_hz, char *error_message, size_t error_message_size) {
    if (!engine) {
        copy_message(error_message, error_message_size, "Engine is not initialized.");
        return false;
    }

    g_mutex_lock(&engine->lock);
    engine->sample_rate_hz = sample_rate_hz;
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
    engine->last_i = 0.0f;
    engine->last_q = 0.0f;
    engine->spectrum_ready = false;
    for (uint32_t index = 0; index < RADIO_SPECTRUM_BINS; index++) {
        engine->spectrum_db[index] = SPECTRUM_FLOOR_DB;
    }
    engine->stop_requested = false;
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

void radio_engine_get_snapshot(RadioEngine *engine, RadioEngineSnapshot *snapshot) {
    if (!engine || !snapshot) {
        return;
    }

    g_mutex_lock(&engine->lock);
    snapshot->running = engine->running;
    snapshot->spectrum_ready = engine->spectrum_ready;
    snapshot->device_count = engine->device_count;
    snapshot->center_freq_hz = engine->center_freq_hz;
    snapshot->sample_rate_hz = engine->sample_rate_hz;
    snapshot->total_samples = engine->total_samples;
    snapshot->last_i = engine->last_i;
    snapshot->last_q = engine->last_q;
    memcpy(snapshot->spectrum_db, engine->spectrum_db, sizeof(snapshot->spectrum_db));
    snprintf(snapshot->status, sizeof(snapshot->status), "%s", engine->status);
    g_mutex_unlock(&engine->lock);
}
