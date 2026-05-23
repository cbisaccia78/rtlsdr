#ifndef RADIO_ENGINE_H
#define RADIO_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct RadioEngine RadioEngine;

#define RADIO_SPECTRUM_BINS 1024U

typedef struct {
    bool running;
    bool spectrum_ready;
    uint32_t device_count;
    uint32_t center_freq_hz;
    uint32_t sample_rate_hz;
    uint64_t total_samples;
    float last_i;
    float last_q;
    float spectrum_db[RADIO_SPECTRUM_BINS];
    char status[160];
} RadioEngineSnapshot;

RadioEngine *radio_engine_new(void);
void radio_engine_free(RadioEngine *engine);

bool radio_engine_set_center_freq(RadioEngine *engine, uint32_t center_freq_hz, char *error_message, size_t error_message_size);
bool radio_engine_set_sample_rate(RadioEngine *engine, uint32_t sample_rate_hz, char *error_message, size_t error_message_size);
bool radio_engine_start(RadioEngine *engine, uint32_t device_index, char *error_message, size_t error_message_size);
void radio_engine_stop(RadioEngine *engine);
void radio_engine_get_snapshot(RadioEngine *engine, RadioEngineSnapshot *snapshot);

#endif
