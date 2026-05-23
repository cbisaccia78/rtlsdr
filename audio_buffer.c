#include "audio_buffer.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

typedef struct AudioBuffer {
    GMutex lock;
    float *samples;
    size_t capacity;
    size_t read_index;
    size_t write_index;
    size_t size;
} AudioBuffer;

AudioBuffer *audio_buffer_create(size_t capacity) {
    AudioBuffer *buffer;

    if (capacity == 0) {
        return NULL;
    }

    buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        return NULL;
    }

    buffer->samples = calloc(capacity, sizeof(float));
    if (!buffer->samples) {
        free(buffer);
        return NULL;
    }

    g_mutex_init(&buffer->lock);
    buffer->capacity = capacity;
    return buffer;
}

void audio_buffer_free(AudioBuffer *buffer) {
    if (!buffer) {
        return;
    }

    g_mutex_clear(&buffer->lock);
    free(buffer->samples);
    free(buffer);
}

void audio_buffer_reset(AudioBuffer *buffer) {
    if (!buffer) {
        return;
    }

    g_mutex_lock(&buffer->lock);
    buffer->read_index = 0;
    buffer->write_index = 0;
    buffer->size = 0;
    memset(buffer->samples, 0, buffer->capacity * sizeof(float));
    g_mutex_unlock(&buffer->lock);
}

size_t audio_buffer_push(AudioBuffer *buffer, const float *samples, size_t sample_count) {
    size_t pushed = 0;

    if (!buffer || !samples || sample_count == 0) {
        return 0;
    }

    g_mutex_lock(&buffer->lock);
    while (pushed < sample_count && buffer->size < buffer->capacity) {
        buffer->samples[buffer->write_index] = samples[pushed];
        buffer->write_index = (buffer->write_index + 1) % buffer->capacity;
        buffer->size++;
        pushed++;
    }
    g_mutex_unlock(&buffer->lock);

    return pushed;
}

size_t audio_buffer_pop(AudioBuffer *buffer, float *samples, size_t sample_count) {
    size_t popped = 0;

    if (!buffer || !samples || sample_count == 0) {
        return 0;
    }

    g_mutex_lock(&buffer->lock);
    while (popped < sample_count && buffer->size > 0) {
        samples[popped] = buffer->samples[buffer->read_index];
        buffer->read_index = (buffer->read_index + 1) % buffer->capacity;
        buffer->size--;
        popped++;
    }
    g_mutex_unlock(&buffer->lock);

    return popped;
}

size_t audio_buffer_size(AudioBuffer *buffer) {
    size_t size;

    if (!buffer) {
        return 0;
    }

    g_mutex_lock(&buffer->lock);
    size = buffer->size;
    g_mutex_unlock(&buffer->lock);
    return size;
}

size_t audio_buffer_capacity(AudioBuffer *buffer) {
    if (!buffer) {
        return 0;
    }

    return buffer->capacity;
}
