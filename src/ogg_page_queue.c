/**
 * @file ogg_page_queue.c
 * @brief Thread-safe OGG page queue implementation
 */

#include "ogg_page_queue.h"
#include "os_port.h"
#include "debug.h"

/* Timeout for blocking operations (ms) */
#define QUEUE_WAIT_TIMEOUT_MS 100

ogg_page_queue_t *ogg_page_queue_create(void)
{
    ogg_page_queue_t *queue = osAllocMem(sizeof(ogg_page_queue_t));
    if (queue == NULL)
    {
        TRACE_ERROR("Failed to allocate OGG page queue\r\n");
        return NULL;
    }

    osMemset(queue, 0, sizeof(ogg_page_queue_t));

    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->closed = FALSE;
    queue->finished = FALSE;

    /* Initialize all entry pointers to NULL */
    for (size_t i = 0; i < OGG_PAGE_QUEUE_SIZE; i++)
    {
        queue->entries[i].header = NULL;
        queue->entries[i].body = NULL;
        queue->entries[i].header_len = 0;
        queue->entries[i].body_len = 0;
        queue->entries[i].file_pos = 0;
        queue->entries[i].block_num = 0;
    }

    if (!osCreateMutex(&queue->mutex))
    {
        TRACE_ERROR("Failed to create OGG queue mutex\r\n");
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

    TRACE_DEBUG("OGG page queue created (size=%d)\r\n", OGG_PAGE_QUEUE_SIZE);

    return queue;
}

void ogg_page_queue_destroy(ogg_page_queue_t *queue)
{
    if (queue == NULL)
    {
        return;
    }

    /* Free any remaining entries */
    for (size_t i = 0; i < OGG_PAGE_QUEUE_SIZE; i++)
    {
        ogg_page_entry_free(&queue->entries[i]);
    }

    osDeleteEvent(&queue->not_full);
    osDeleteEvent(&queue->not_empty);
    osDeleteMutex(&queue->mutex);
    osFreeMem(queue);

    TRACE_DEBUG("OGG page queue destroyed\r\n");
}

error_t ogg_page_queue_push(ogg_page_queue_t *queue,
                            const ogg_page *og,
                            size_t file_pos,
                            size_t block_num)
{
    if (queue == NULL || og == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    if (og->header_len <= 0 || og->header == NULL)
    {
        TRACE_ERROR("Invalid OGG page header\r\n");
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
        if (queue->count < OGG_PAGE_QUEUE_SIZE)
        {
            ogg_page_entry_t *entry = &queue->entries[queue->head];

            /* Free any existing data (shouldn't happen in normal operation) */
            ogg_page_entry_free(entry);

            /* Allocate and copy header */
            entry->header = osAllocMem(og->header_len);
            if (entry->header == NULL)
            {
                osReleaseMutex(&queue->mutex);
                TRACE_ERROR("Failed to allocate OGG header copy\r\n");
                return ERROR_OUT_OF_MEMORY;
            }
            osMemcpy(entry->header, og->header, og->header_len);
            entry->header_len = og->header_len;

            /* Allocate and copy body if present */
            if (og->body_len > 0 && og->body != NULL)
            {
                entry->body = osAllocMem(og->body_len);
                if (entry->body == NULL)
                {
                    osFreeMem(entry->header);
                    entry->header = NULL;
                    entry->header_len = 0;
                    osReleaseMutex(&queue->mutex);
                    TRACE_ERROR("Failed to allocate OGG body copy\r\n");
                    return ERROR_OUT_OF_MEMORY;
                }
                osMemcpy(entry->body, og->body, og->body_len);
                entry->body_len = og->body_len;
            }
            else
            {
                entry->body = NULL;
                entry->body_len = 0;
            }

            /* Store metadata */
            entry->file_pos = file_pos;
            entry->block_num = block_num;

            /* Advance head pointer */
            queue->head = (queue->head + 1) % OGG_PAGE_QUEUE_SIZE;
            queue->count++;

            /* Signal that queue is not empty */
            osSetEvent(&queue->not_empty);

            /* If queue is now full, reset not_full event */
            if (queue->count >= OGG_PAGE_QUEUE_SIZE)
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

error_t ogg_page_queue_pop(ogg_page_queue_t *queue,
                           ogg_page_entry_t *entry)
{
    if (queue == NULL || entry == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    /* Initialize output entry */
    entry->header = NULL;
    entry->body = NULL;
    entry->header_len = 0;
    entry->body_len = 0;
    entry->file_pos = 0;
    entry->block_num = 0;

    /* Wait for data in queue */
    while (TRUE)
    {
        osAcquireMutex(&queue->mutex);

        /* Check if aborted */
        if (queue->closed)
        {
            osReleaseMutex(&queue->mutex);
            return ERROR_ABORTED;
        }

        /* Check if there's data */
        if (queue->count > 0)
        {
            ogg_page_entry_t *src = &queue->entries[queue->tail];

            /* Transfer ownership of data to caller */
            entry->header = src->header;
            entry->header_len = src->header_len;
            entry->body = src->body;
            entry->body_len = src->body_len;
            entry->file_pos = src->file_pos;
            entry->block_num = src->block_num;

            /* Clear source entry (ownership transferred) */
            src->header = NULL;
            src->body = NULL;
            src->header_len = 0;
            src->body_len = 0;
            src->file_pos = 0;
            src->block_num = 0;

            /* Advance tail pointer */
            queue->tail = (queue->tail + 1) % OGG_PAGE_QUEUE_SIZE;
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

        /* Queue is empty - check if finished */
        if (queue->finished)
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

bool_t ogg_page_queue_is_empty(ogg_page_queue_t *queue)
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

bool_t ogg_page_queue_is_full(ogg_page_queue_t *queue)
{
    if (queue == NULL)
    {
        return FALSE;
    }

    osAcquireMutex(&queue->mutex);
    bool_t full = (queue->count >= OGG_PAGE_QUEUE_SIZE);
    osReleaseMutex(&queue->mutex);

    return full;
}

size_t ogg_page_queue_count(ogg_page_queue_t *queue)
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

void ogg_page_queue_finish(ogg_page_queue_t *queue)
{
    if (queue == NULL)
    {
        return;
    }

    osAcquireMutex(&queue->mutex);
    queue->finished = TRUE;

    /* Wake up any waiting consumer */
    osSetEvent(&queue->not_empty);
    osReleaseMutex(&queue->mutex);

    TRACE_DEBUG("OGG page queue finished\r\n");
}

void ogg_page_queue_close(ogg_page_queue_t *queue)
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

    TRACE_DEBUG("OGG page queue closed\r\n");
}

void ogg_page_queue_reset(ogg_page_queue_t *queue)
{
    if (queue == NULL)
    {
        return;
    }

    osAcquireMutex(&queue->mutex);

    /* Free any remaining entries */
    for (size_t i = 0; i < OGG_PAGE_QUEUE_SIZE; i++)
    {
        ogg_page_entry_free(&queue->entries[i]);
    }

    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->closed = FALSE;
    queue->finished = FALSE;

    /* Reset events to initial state */
    osResetEvent(&queue->not_empty);
    osSetEvent(&queue->not_full);

    osReleaseMutex(&queue->mutex);

    TRACE_DEBUG("OGG page queue reset\r\n");
}

void ogg_page_entry_free(ogg_page_entry_t *entry)
{
    if (entry == NULL)
    {
        return;
    }

    if (entry->header != NULL)
    {
        osFreeMem(entry->header);
        entry->header = NULL;
    }

    if (entry->body != NULL)
    {
        osFreeMem(entry->body);
        entry->body = NULL;
    }

    entry->header_len = 0;
    entry->body_len = 0;
    entry->file_pos = 0;
    entry->block_num = 0;
}
