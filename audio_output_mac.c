#include "audio_output_mac.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_OUTPUT_BUFFER_COUNT 3

typedef struct AudioOutputMac {
    AudioBuffer *buffer;
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[AUDIO_OUTPUT_BUFFER_COUNT];
    uint32_t sample_rate_hz;
    size_t frames_per_buffer;
    bool running;
    bool initialized;
} AudioOutputMac;

static void copy_message(char *destination, size_t destination_size, const char *message) {
    if (!destination || destination_size == 0) {
        return;
    }

    snprintf(destination, destination_size, "%s", message);
}

static void audio_queue_callback(void *user_data, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    AudioOutputMac *output = user_data;
    size_t requested_frames = output->frames_per_buffer;
    size_t popped = audio_buffer_pop(output->buffer, (float *)buffer->mAudioData, requested_frames);
    float *samples = (float *)buffer->mAudioData;

    for (size_t index = popped; index < requested_frames; index++) {
        samples[index] = 0.0f;
    }

    buffer->mAudioDataByteSize = (UInt32)(requested_frames * sizeof(float));
    AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
}

static bool audio_output_mac_initialize(AudioOutputMac *output, char *error_message, size_t error_message_size) {
    AudioStreamBasicDescription format = {0};
    OSStatus status;

    if (!output) {
        copy_message(error_message, error_message_size, "Audio output is not initialized.");
        return false;
    }

    if (output->initialized) {
        return true;
    }

    format.mSampleRate = output->sample_rate_hz;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 8U * sizeof(float);

    status = AudioQueueNewOutput(&format, audio_queue_callback, output, CFRunLoopGetMain(), kCFRunLoopCommonModes, 0, &output->queue);
    if (status != noErr) {
        copy_message(error_message, error_message_size, "Failed to create the macOS audio queue.");
        return false;
    }

    for (size_t index = 0; index < AUDIO_OUTPUT_BUFFER_COUNT; index++) {
        status = AudioQueueAllocateBuffer(output->queue, (UInt32)(output->frames_per_buffer * sizeof(float)), &output->buffers[index]);
        if (status != noErr) {
            copy_message(error_message, error_message_size, "Failed to allocate an audio queue buffer.");
            return false;
        }

        memset(output->buffers[index]->mAudioData, 0, output->frames_per_buffer * sizeof(float));
        output->buffers[index]->mAudioDataByteSize = (UInt32)(output->frames_per_buffer * sizeof(float));
    }

    output->initialized = true;
    return true;
}

AudioOutputMac *audio_output_mac_create(AudioBuffer *buffer, uint32_t sample_rate_hz, size_t frames_per_buffer, char *error_message, size_t error_message_size) {
    AudioOutputMac *output;

    if (!buffer || sample_rate_hz == 0 || frames_per_buffer == 0) {
        copy_message(error_message, error_message_size, "Invalid audio output configuration.");
        return NULL;
    }

    output = calloc(1, sizeof(*output));
    if (!output) {
        copy_message(error_message, error_message_size, "Failed to allocate the audio output state.");
        return NULL;
    }

    output->buffer = buffer;
    output->sample_rate_hz = sample_rate_hz;
    output->frames_per_buffer = frames_per_buffer;
    return output;
}

void audio_output_mac_free(AudioOutputMac *output) {
    if (!output) {
        return;
    }

    audio_output_mac_stop(output);
    if (output->queue) {
        AudioQueueDispose(output->queue, true);
    }
    free(output);
}

bool audio_output_mac_start(AudioOutputMac *output, char *error_message, size_t error_message_size) {
    OSStatus status;

    if (!audio_output_mac_initialize(output, error_message, error_message_size)) {
        return false;
    }

    if (output->running) {
        return true;
    }

    for (size_t index = 0; index < AUDIO_OUTPUT_BUFFER_COUNT; index++) {
        audio_queue_callback(output, output->queue, output->buffers[index]);
    }

    status = AudioQueueStart(output->queue, NULL);
    if (status != noErr) {
        copy_message(error_message, error_message_size, "Failed to start the macOS audio queue.");
        return false;
    }

    output->running = true;
    return true;
}

void audio_output_mac_stop(AudioOutputMac *output) {
    if (!output || !output->queue || !output->running) {
        return;
    }

    AudioQueueStop(output->queue, true);
    output->running = false;
}

bool audio_output_mac_is_running(AudioOutputMac *output) {
    if (!output) {
        return false;
    }

    return output->running;
}
