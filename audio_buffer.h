#ifndef AUDIO_BUFFER_H
#define AUDIO_BUFFER_H

#include <stddef.h>

typedef struct AudioBuffer AudioBuffer;

AudioBuffer *audio_buffer_create(size_t capacity);
void audio_buffer_free(AudioBuffer *buffer);
void audio_buffer_reset(AudioBuffer *buffer);
size_t audio_buffer_push(AudioBuffer *buffer, const float *samples, size_t sample_count);
size_t audio_buffer_pop(AudioBuffer *buffer, float *samples, size_t sample_count);
size_t audio_buffer_size(AudioBuffer *buffer);
size_t audio_buffer_capacity(AudioBuffer *buffer);

#endif
