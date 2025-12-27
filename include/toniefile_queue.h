/**
 * @file toniefile_queue.h
 * @brief Thread-safe audio frame queue for TAF encoding pipeline
 *
 * Provides a ring buffer queue for passing decoded audio frames between
 * decoder and encoder threads. Designed for single-producer single-consumer
 * (SPSC) usage pattern with blocking operations.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "os_port.h"
#include "error.h"

/**
 * @brief Maximum samples per frame (stereo interleaved)
 * Matches the decode buffer size in ffmpeg_stream(): 2 * 4096 = 8192
 */
#define AUDIO_FRAME_MAX_SAMPLES (2 * 4096)

/**
 * @brief Number of frames in the queue ring buffer
 * 16 frames * 16KB = 256KB total buffer
 */
#define AUDIO_FRAME_QUEUE_SIZE 16

/**
 * @brief Flags for audio frame metadata
 */
typedef enum
{
    AUDIO_FRAME_FLAG_NONE = 0,
    AUDIO_FRAME_FLAG_END_OF_STREAM = (1 << 0),  /* Last frame from current source */
    AUDIO_FRAME_FLAG_NEW_CHAPTER = (1 << 1),    /* Start of new chapter/track */
    AUDIO_FRAME_FLAG_ABORT = (1 << 2)           /* Signal to abort processing */
} audio_frame_flags_t;

/**
 * @brief Audio frame containing decoded samples
 */
typedef struct
{
    int16_t samples[AUDIO_FRAME_MAX_SAMPLES];  /* Interleaved stereo samples */
    size_t sample_count;                        /* Number of int16_t values (not sample pairs) */
    uint32_t flags;                             /* Combination of audio_frame_flags_t */
} audio_frame_t;

/**
 * @brief Thread-safe audio frame queue
 */
typedef struct
{
    audio_frame_t frames[AUDIO_FRAME_QUEUE_SIZE];  /* Ring buffer */
    volatile size_t head;                          /* Write index (producer) */
    volatile size_t tail;                          /* Read index (consumer) */
    volatile size_t count;                         /* Current frame count */

    OsMutex mutex;                                 /* Protects queue state */
    OsEvent not_empty;                             /* Signaled when frames available */
    OsEvent not_full;                              /* Signaled when space available */

    volatile bool_t closed;                        /* Queue closed for new writes */
} audio_frame_queue_t;

/**
 * @brief Create and initialize an audio frame queue
 * @return Pointer to new queue, or NULL on failure
 */
audio_frame_queue_t *audio_frame_queue_create(void);

/**
 * @brief Destroy an audio frame queue and free resources
 * @param queue Queue to destroy
 */
void audio_frame_queue_destroy(audio_frame_queue_t *queue);

/**
 * @brief Push a frame onto the queue (producer)
 *
 * Blocks if queue is full until space becomes available or queue is closed.
 *
 * @param queue Target queue
 * @param samples Sample data to copy (interleaved stereo)
 * @param sample_count Number of int16_t values
 * @param flags Frame flags (EOS, new chapter, etc.)
 * @return NO_ERROR on success, ERROR_ABORTED if queue closed
 */
error_t audio_frame_queue_push(audio_frame_queue_t *queue,
                               const int16_t *samples,
                               size_t sample_count,
                               uint32_t flags);

/**
 * @brief Pop a frame from the queue (consumer)
 *
 * Blocks if queue is empty until a frame becomes available.
 *
 * @param queue Source queue
 * @param frame Output frame (data copied here)
 * @return NO_ERROR on success, ERROR_END_OF_STREAM if queue closed and empty
 */
error_t audio_frame_queue_pop(audio_frame_queue_t *queue,
                              audio_frame_t *frame);

/**
 * @brief Check if queue is empty (non-blocking)
 * @param queue Queue to check
 * @return true if empty
 */
bool_t audio_frame_queue_is_empty(audio_frame_queue_t *queue);

/**
 * @brief Check if queue is full (non-blocking)
 * @param queue Queue to check
 * @return true if full
 */
bool_t audio_frame_queue_is_full(audio_frame_queue_t *queue);

/**
 * @brief Get current frame count (non-blocking)
 * @param queue Queue to check
 * @return Number of frames in queue
 */
size_t audio_frame_queue_count(audio_frame_queue_t *queue);

/**
 * @brief Close the queue (producer signals completion)
 *
 * After closing, push operations will fail and pop operations
 * will return ERROR_END_OF_STREAM once queue is drained.
 *
 * @param queue Queue to close
 */
void audio_frame_queue_close(audio_frame_queue_t *queue);

/**
 * @brief Reset queue to initial state (for reuse)
 * @param queue Queue to reset
 */
void audio_frame_queue_reset(audio_frame_queue_t *queue);
