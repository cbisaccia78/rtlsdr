#ifndef RADIO_ENGINE_H
#define RADIO_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque radio controller that owns device state, worker thread, and FFT buffers. */
typedef struct RadioEngine RadioEngine;

typedef enum {
    RADIO_DEMOD_MODE_OFF = 0,
    RADIO_DEMOD_MODE_FM,
    RADIO_DEMOD_MODE_AM
} RadioDemodMode;

/* Number of FFT bins exposed to the GTK spectrum display. */
#define RADIO_SPECTRUM_BINS 1024U

/*
 * Snapshot copied out of the engine under lock for safe UI consumption.
 *
 * running: true while the async RTL-SDR read loop is active.
 * spectrum_ready: true after at least one FFT frame has been produced.
 * demod_mode: user-selected audio demodulation mode.
 * device_count: number of RTL-SDR devices visible when the snapshot was taken.
 * center_freq_hz: currently configured tuner center frequency.
 * sample_rate_hz: currently configured sample rate.
 * total_samples: cumulative complex IQ samples observed since the last start.
 * last_i / last_q: first normalized IQ sample seen in the latest callback.
 * spectrum_db: FFT magnitudes in dB, shifted so 0 Hz is centered in the array.
 * status: latest human-readable engine status string.
 */
typedef struct {
    bool running;
    bool spectrum_ready;
    RadioDemodMode demod_mode;
    uint32_t device_count;
    uint32_t center_freq_hz;
    uint32_t sample_rate_hz;
    uint64_t total_samples;
    float last_i;
    float last_q;
    float spectrum_db[RADIO_SPECTRUM_BINS];
    char status[160];
} RadioEngineSnapshot;

/*
 * Allocate and initialize a new radio engine.
 *
 * The engine starts in an idle state with default frequency/sample-rate values,
 * preallocated FFT resources, and a device count snapshot. The caller owns the
 * returned pointer and must release it with radio_engine_free().
 *
 * Returns NULL if memory allocation or FFT setup creation fails.
 */
RadioEngine *radio_engine_new(void);

/*
 * Stop any running stream, release FFT/device resources, and free the engine.
 *
 * Passing NULL is allowed and has no effect.
 */
void radio_engine_free(RadioEngine *engine);

/*
 * Update the center frequency in Hz.
 *
 * If the engine is currently streaming, this attempts to push the change to the
 * open tuner immediately. On success, error_message receives a short status
 * string when a destination buffer is supplied.
 *
 * Returns true on success, false on validation or tuner update failure.
 */
bool radio_engine_set_center_freq(RadioEngine *engine, uint32_t center_freq_hz, char *error_message, size_t error_message_size);

/*
 * Update the sample rate in Hz.
 *
 * The value is always stored in the engine configuration. If streaming is not
 * active, the new rate becomes the next active sample rate immediately. If the
 * stream is active, the function returns false and leaves a message explaining
 * that a stop/start cycle is required before the hardware setting changes.
 */
bool radio_engine_set_sample_rate(RadioEngine *engine, uint32_t sample_rate_hz, char *error_message, size_t error_message_size);

/*
 * Update the selected demodulation mode.
 *
 * The first implementation only stores the user's selection so the UI and the
 * engine agree on which demodulator should be activated later.
 */
bool radio_engine_set_demod_mode(RadioEngine *engine, RadioDemodMode demod_mode, char *error_message, size_t error_message_size);

/*
 * Start the RTL-SDR worker thread for the selected device index.
 *
 * The worker thread opens the device, applies the current configuration,
 * enables automatic gain control, and enters the async callback loop.
 *
 * Returns true after the worker thread is successfully created. Hardware setup
 * failures that occur inside the worker are reflected later through the status
 * string visible via radio_engine_get_snapshot().
 */
bool radio_engine_start(RadioEngine *engine, uint32_t device_index, char *error_message, size_t error_message_size);

/*
 * Request shutdown of the async read loop and wait for the worker thread.
 *
 * Passing NULL is allowed and has no effect.
 */
void radio_engine_stop(RadioEngine *engine);

/*
 * Copy the current engine state into a UI-safe snapshot.
 *
 * The snapshot is taken under the engine mutex so the caller sees a consistent
 * view of status, counters, and the most recent FFT frame.
 */
void radio_engine_get_snapshot(RadioEngine *engine, RadioEngineSnapshot *snapshot);

#endif
