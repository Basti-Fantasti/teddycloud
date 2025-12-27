/**
 * @file toniefile_writer.c
 * @brief Async writer thread implementation for TAF encoding pipeline
 */

#include "toniefile_writer.h"
#include "toniefile.h"
#include "debug.h"
#include "error.h"

/* Task parameters for writer thread */
#define WRITER_TASK_PRIORITY 0
#define WRITER_TASK_STACK_SIZE (8 * 1024)

writer_ctx_t *writer_ctx_create(FsFile *file,
                                 ogg_page_queue_t *queue,
                                 Sha1Context *sha1,
                                 volatile size_t *file_pos,
                                 volatile size_t *audio_length,
                                 volatile size_t *taf_block_num)
{
    if (file == NULL || queue == NULL || sha1 == NULL)
    {
        TRACE_ERROR("Invalid parameters for writer_ctx_create\r\n");
        return NULL;
    }

    if (file_pos == NULL || audio_length == NULL || taf_block_num == NULL)
    {
        TRACE_ERROR("Invalid counter pointers for writer_ctx_create\r\n");
        return NULL;
    }

    writer_ctx_t *ctx = osAllocMem(sizeof(writer_ctx_t));
    if (ctx == NULL)
    {
        TRACE_ERROR("Failed to allocate writer context\r\n");
        return NULL;
    }

    osMemset(ctx, 0, sizeof(writer_ctx_t));

    ctx->file = file;
    ctx->queue = queue;
    ctx->sha1 = sha1;
    ctx->file_pos = file_pos;
    ctx->audio_length = audio_length;
    ctx->taf_block_num = taf_block_num;

    ctx->running = FALSE;
    ctx->quit = FALSE;
    ctx->stop_requested = FALSE;
    ctx->error = NO_ERROR;
    ctx->pages_written = 0;
    ctx->bytes_written = 0;

    ctx->task_params.priority = WRITER_TASK_PRIORITY;
    ctx->task_params.stackSize = WRITER_TASK_STACK_SIZE;

    TRACE_DEBUG("Writer context created\r\n");

    return ctx;
}

void writer_ctx_destroy(writer_ctx_t *ctx)
{
    if (ctx == NULL)
    {
        return;
    }

    /* Stop thread if running */
    if (ctx->running)
    {
        writer_stop(ctx);
    }

    osFreeMem(ctx);
    TRACE_DEBUG("Writer context destroyed\r\n");
}

error_t writer_start(writer_ctx_t *ctx)
{
    if (ctx == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    if (ctx->running)
    {
        TRACE_WARNING("Writer already running\r\n");
        return ERROR_ALREADY_RUNNING;
    }

    /* Reset state */
    ctx->running = FALSE;
    ctx->quit = FALSE;
    ctx->stop_requested = FALSE;
    ctx->error = NO_ERROR;
    ctx->pages_written = 0;
    ctx->bytes_written = 0;

    /* Create writer task */
    ctx->task_id = osCreateTask("writer", &writer_thread_task, ctx, &ctx->task_params);

    /* Wait for thread to signal it's running */
    while (!ctx->running && ctx->error == NO_ERROR && !ctx->quit)
    {
        osDelayTask(10);
    }

    if (ctx->error != NO_ERROR)
    {
        TRACE_ERROR("Writer thread failed to start: %s\r\n", error2text(ctx->error));
        return ctx->error;
    }

    TRACE_INFO("Writer thread started\r\n");
    return NO_ERROR;
}

void writer_stop(writer_ctx_t *ctx)
{
    if (ctx == NULL)
    {
        return;
    }

    if (!ctx->running && ctx->quit)
    {
        return;
    }

    TRACE_DEBUG("Stopping writer thread...\r\n");

    /* Signal thread to stop */
    ctx->stop_requested = TRUE;

    /* Close the queue to unblock any waiting pop operations */
    if (ctx->queue != NULL)
    {
        ogg_page_queue_close(ctx->queue);
    }

    /* Wait for thread to exit */
    while (!ctx->quit)
    {
        osDelayTask(10);
    }

    TRACE_DEBUG("Writer thread stopped\r\n");
}

error_t writer_wait(writer_ctx_t *ctx)
{
    if (ctx == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    /* Wait for thread to exit naturally */
    while (!ctx->quit)
    {
        osDelayTask(10);
    }

    return ctx->error;
}

bool_t writer_is_running(writer_ctx_t *ctx)
{
    if (ctx == NULL)
    {
        return FALSE;
    }
    return ctx->running;
}

error_t writer_get_error(writer_ctx_t *ctx)
{
    if (ctx == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }
    return ctx->error;
}

size_t writer_get_pages_written(writer_ctx_t *ctx)
{
    if (ctx == NULL)
    {
        return 0;
    }
    return ctx->pages_written;
}

size_t writer_get_bytes_written(writer_ctx_t *ctx)
{
    if (ctx == NULL)
    {
        return 0;
    }
    return ctx->bytes_written;
}

/**
 * @brief Writer thread main function
 *
 * Consumes OGG pages from queue and writes to file.
 * Updates SHA1 hash and tracks block boundaries.
 */
void writer_thread_task(void *param)
{
    writer_ctx_t *ctx = (writer_ctx_t *)param;
    ogg_page_entry_t entry;
    error_t error = NO_ERROR;

    if (ctx == NULL)
    {
        TRACE_ERROR("Writer thread: NULL context\r\n");
        return;
    }

    TRACE_INFO("Writer thread starting\r\n");

    /* Signal that we're running */
    ctx->running = TRUE;

    /* Main write loop */
    while (!ctx->stop_requested)
    {
        /* Pop page from queue */
        error = ogg_page_queue_pop(ctx->queue, &entry);

        if (error == ERROR_END_OF_STREAM)
        {
            /* Queue finished and drained - normal completion */
            TRACE_DEBUG("Writer: queue drained, exiting\r\n");
            error = NO_ERROR;
            break;
        }

        if (error == ERROR_ABORTED)
        {
            /* Queue was closed - stop requested */
            TRACE_DEBUG("Writer: queue closed, exiting\r\n");
            error = NO_ERROR;
            break;
        }

        if (error != NO_ERROR)
        {
            TRACE_ERROR("Writer: queue pop failed: %s\r\n", error2text(error));
            ctx->error = error;
            break;
        }

        /* Write header to file */
        if (entry.header != NULL && entry.header_len > 0)
        {
            if (fsWriteFile(ctx->file, entry.header, entry.header_len) != NO_ERROR)
            {
                TRACE_ERROR("Writer: failed to write page header\r\n");
                ctx->error = ERROR_WRITE_FAILED;
                ogg_page_entry_free(&entry);
                break;
            }

            /* Update SHA1 hash */
            sha1Update(ctx->sha1, entry.header, entry.header_len);
        }

        /* Write body to file */
        if (entry.body != NULL && entry.body_len > 0)
        {
            if (fsWriteFile(ctx->file, entry.body, entry.body_len) != NO_ERROR)
            {
                TRACE_ERROR("Writer: failed to write page body\r\n");
                ctx->error = ERROR_WRITE_FAILED;
                ogg_page_entry_free(&entry);
                break;
            }

            /* Update SHA1 hash */
            sha1Update(ctx->sha1, entry.body, entry.body_len);
        }

        /* Update counters */
        size_t page_size = entry.header_len + entry.body_len;
        size_t prev_pos = *(ctx->file_pos);

        *(ctx->file_pos) += page_size;
        *(ctx->audio_length) += page_size;
        ctx->bytes_written += page_size;
        ctx->pages_written++;

        /* Check for block boundary crossing */
        if ((prev_pos / TONIEFILE_FRAME_SIZE) != (*(ctx->file_pos) / TONIEFILE_FRAME_SIZE))
        {
            (*(ctx->taf_block_num))++;

            /* Verify block alignment */
            if (*(ctx->file_pos) % TONIEFILE_FRAME_SIZE)
            {
                TRACE_ERROR("Writer: block alignment mismatch at 0x%08" PRIX32 "\r\n",
                           (uint32_t)*(ctx->file_pos));
                ctx->error = ERROR_FAILURE;
                ogg_page_entry_free(&entry);
                break;
            }
        }

        /* Free the entry data */
        ogg_page_entry_free(&entry);
    }

    ctx->running = FALSE;
    ctx->quit = TRUE;

    if (ctx->error == NO_ERROR)
    {
        TRACE_INFO("Writer thread completed: %" PRIuSIZE " pages, %" PRIuSIZE " bytes\r\n",
                   ctx->pages_written, ctx->bytes_written);
    }
    else
    {
        TRACE_ERROR("Writer thread exiting with error: %s\r\n", error2text(ctx->error));
    }

    osDeleteTask((OsTaskId)OS_SELF_TASK_ID);
}
