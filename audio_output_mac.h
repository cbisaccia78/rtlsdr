#ifndef AUDIO_OUTPUT_MAC_H
#define AUDIO_OUTPUT_MAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_buffer.h"

typedef struct AudioOutputMac AudioOutputMac;

AudioOutputMac *audio_output_mac_create(AudioBuffer *buffer, uint32_t sample_rate_hz, size_t frames_per_buffer, char *error_message, size_t error_message_size);
void audio_output_mac_free(AudioOutputMac *output);
bool audio_output_mac_start(AudioOutputMac *output, char *error_message, size_t error_message_size);
void audio_output_mac_stop(AudioOutputMac *output);
bool audio_output_mac_is_running(AudioOutputMac *output);

#endif
