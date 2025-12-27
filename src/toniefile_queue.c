/**
 * @file toniefile_queue.c
 * @brief Thread-safe audio frame queue implementation
 */

#include "toniefile_queue.h"
#include "os_port.h"
#include "debug.h"

/* Timeout for blocking operations (ms) */
#define QUEUE_WAIT_TIMEOUT_MS 100

audio_frame_queue_t *audio_frame_queue_create(void)
{
    audio_frame_queue_t *queue = osAllocMem(sizeof(audio_frame_queue_t));
    if (queue == NULL)
    {
        TRACE_ERROR("Failed to allocate audio frame queue\r\n");
        return NULL;
    }

    osMemset(queue, 0, sizeof(audio_frame_queue_t));

    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->closed = FALSE;

    if (!osCreateMutex(&queue->mutex))
    {
        TRACE_ERROR("Failed to create queue mutex\r\n");
        osFreeMem(queue);
        return NULL;
    }

    if (!osCreateEvent(&queue->not_empty))
    {
        TRACE_ERROR("Failed to create not_empty event\r\n");
        osDeleteMutex(&queue->mutex);
        osFreeMem(queue);
        return NULL;
    }

    if (!osCreateEvent(&queue->not_full))
    {
        TRACE_ERROR("Failed to create not_full event\r\n");
        osDeleteEvent(&queue->not_empty);
        osDeleteMutex(&queue->mutex);
        osFreeMem(queue);
        return NULL;
    }

    /* Initially not_full is signaled (queue has space) */
    osSetEvent(&queue->not_full);

    TRACE_DEBUG("Audio frame queue created (size=%d, frame_size=%d bytes)\r\n",
                AUDIO_FRAME_QUEUE_SIZE, (int)sizeof(audio_frame_t));

    return queue;
}

void audio_frame_queue_destroy(audio_frame_queue_t *queue)
{
    if (queue == NULL)
    {
        return;
    }

    osDeleteEvent(&queue->not_full);
    osDeleteEvent(&queue->not_empty);
    osDeleteMutex(&queue->mutex);
    osFreeMem(queue);

    TRACE_DEBUG("Audio frame queue destroyed\r\n");
}

error_t audio_frame_queue_push(audio_frame_queue_t *queue,
                               const int16_t *samples,
                               size_t sample_count,
                               uint32_t flags)
{
    if (queue == NULL || samples == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    if (sample_count > AUDIO_FRAME_MAX_SAMPLES)
    {
        TRACE_ERROR("Sample count %" PRIuSIZE " exceeds max %d\r\n",
                    sample_count, AUDIO_FRAME_MAX_SAMPLES);
        return ERROR_INVALID_PARAMETER;
    }

    /* Wait for space in queue */
    while (TRUE)
    {
        osAcquireMutex(&queue->mutex);

        /* Check if queue was closed */
        if (queue->closed)
        {
            osReleaseMutex(&queue->mutex);
            return ERROR_ABORTED;
        }

        /* Check if there's space */
        if (queue->count < AUDIO_FRAME_QUEUE_SIZE)
        {
            /* Copy frame data */
            audio_frame_t *frame = &queue->frames[queue->head];
            osMemcpy(frame->samples, samples, sample_count * sizeof(int16_t));
            frame->sample_count = sample_count;
            frame->flags = flags;

            /* Advance head pointer */
            queue->head = (queue->head + 1) % AUDIO_FRAME_QUEUE_SIZE;
            queue->count++;

            /* Signal that queue is not empty */
            osSetEvent(&queue->not_empty);

            /* If queue is now full, reset not_full event */
            if (queue->count >= AUDIO_FRAME_QUEUE_SIZE)
            {
                osResetEvent(&queue->not_full);
            }

            osReleaseMutex(&queue->mutex);
            return NO_ERROR;
        }

        osReleaseMutex(&queue->mutex);

        /* Wait for space with timeout */
        if (!osWaitForEvent(&queue->not_full, QUEUE_WAIT_TIMEOUT_MS))
        {
            /* Timeout - check again (handles spurious wakeups and closed state) */
            continue;
        }
    }
}

error_t audio_frame_queue_pop(audio_frame_queue_t *queue,
                              audio_frame_t *frame)
{
    if (queue == NULL || frame == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    /* Wait for data in queue */
    while (TRUE)
    {
        osAcquireMutex(&queue->mutex);

        /* Check if there's data */
        if (queue->count > 0)
        {
            /* Copy frame data */
            audio_frame_t *src = &queue->frames[queue->tail];
            osMemcpy(frame->samples, src->samples, src->sample_count * sizeof(int16_t));
            frame->sample_count = src->sample_count;
            frame->flags = src->flags;

            /* Advance tail pointer */
            queue->tail = (queue->tail + 1) % AUDIO_FRAME_QUEUE_SIZE;
            queue->count--;

            /* Signal that queue is not full */
            osSetEvent(&queue->not_full);

            /* If queue is now empty, reset not_empty event */
            if (queue->count == 0)
            {
                osResetEvent(&queue->not_empty);
            }

            osReleaseMutex(&queue->mutex);
            return NO_ERROR;
        }

        /* Queue is empty - check if closed */
        if (queue->closed)
        {
            osReleaseMutex(&queue->mutex);
            return ERROR_END_OF_STREAM;
        }

        osReleaseMutex(&queue->mutex);

        /* Wait for data with timeout */
        if (!osWaitForEvent(&queue->not_empty, QUEUE_WAIT_TIMEOUT_MS))
        {
            /* Timeout - check again */
            continue;
        }
    }
}

bool_t audio_frame_queue_is_empty(audio_frame_queue_t *queue)
{
    if (queue == NULL)
    {
        return TRUE;
    }

    osAcquireMutex(&queue->mutex);
    bool_t empty = (queue->count == 0);
    osReleaseMutex(&queue->mutex);

    return empty;
}

bool_t audio_frame_queue_is_full(audio_frame_queue_t *queue)
{
    if (queue == NULL)
    {
        return FALSE;
    }

    osAcquireMutex(&queue->mutex);
    bool_t full = (queue->count >= AUDIO_FRAME_QUEUE_SIZE);
    osReleaseMutex(&queue->mutex);

    return full;
}

size_t audio_frame_queue_count(audio_frame_queue_t *queue)
{
    if (queue == NULL)
    {
        return 0;
    }

    osAcquireMutex(&queue->mutex);
    size_t count = queue->count;
    osReleaseMutex(&queue->mutex);

    return count;
}

void audio_frame_queue_close(audio_frame_queue_t *queue)
{
    if (queue == NULL)
    {
        return;
    }

    osAcquireMutex(&queue->mutex);
    queue->closed = TRUE;

    /* Wake up any waiting threads */
    osSetEvent(&queue->not_empty);
    osSetEvent(&queue->not_full);
    osReleaseMutex(&queue->mutex);

    TRACE_DEBUG("Audio frame queue closed\r\n");
}

void audio_frame_queue_reset(audio_frame_queue_t *queue)
{
    if (queue == NULL)
    {
        return;
    }

    osAcquireMutex(&queue->mutex);

    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->closed = FALSE;

    /* Reset events to initial state */
    osResetEvent(&queue->not_empty);
    osSetEvent(&queue->not_full);

    osReleaseMutex(&queue->mutex);

    TRACE_DEBUG("Audio frame queue reset\r\n");
}
