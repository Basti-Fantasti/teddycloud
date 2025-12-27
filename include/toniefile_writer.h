/**
 * @file toniefile_writer.h
 * @brief Async writer thread for TAF encoding pipeline
 *
 * Provides asynchronous file I/O for OGG pages. The writer thread consumes
 * pages from the OGG page queue and writes them to disk, allowing the encoder
 * to continue without blocking on I/O.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "os_port.h"
#include "error.h"
#include "ogg_page_queue.h"
#include "hash/sha1.h"
#include "fs_ext.h"

/**
 * @brief Async writer thread context
 */
typedef struct
{
    /* I/O resources */
    FsFile *file;                                 /* Output file handle */
    ogg_page_queue_t *queue;                      /* Input page queue */

    /* SHA1 hash context (shared with toniefile) */
    Sha1Context *sha1;                            /* SHA1 context for hash updates */

    /* Block tracking (shared with toniefile) */
    volatile size_t *file_pos;                    /* Current file position */
    volatile size_t *audio_length;                /* Total audio length written */
    volatile size_t *taf_block_num;               /* TAF block number */

    /* Thread control */
    volatile bool_t running;                      /* Thread is running */
    volatile bool_t quit;                         /* Thread has exited */
    volatile bool_t stop_requested;               /* Stop signal from caller */
    volatile error_t error;                       /* Error from thread */

    /* Statistics */
    volatile size_t pages_written;                /* Count of pages written */
    volatile size_t bytes_written;                /* Total bytes written */

    /* Task management */
    OsTaskId task_id;                             /* Thread task ID */
    OsTaskParameters task_params;                 /* Task parameters */
} writer_ctx_t;

/**
 * @brief Create a writer context
 *
 * @param file Output file handle (must be open for writing)
 * @param queue Input OGG page queue
 * @param sha1 SHA1 context for hash updates
 * @param file_pos Pointer to file position counter
 * @param audio_length Pointer to audio length counter
 * @param taf_block_num Pointer to TAF block number counter
 * @return Pointer to new context, or NULL on failure
 */
writer_ctx_t *writer_ctx_create(FsFile *file,
                                 ogg_page_queue_t *queue,
                                 Sha1Context *sha1,
                                 volatile size_t *file_pos,
                                 volatile size_t *audio_length,
                                 volatile size_t *taf_block_num);

/**
 * @brief Destroy a writer context
 *
 * Stops the writer thread if running and frees resources.
 * Does NOT close the file handle (caller's responsibility).
 *
 * @param ctx Context to destroy
 */
void writer_ctx_destroy(writer_ctx_t *ctx);

/**
 * @brief Start the writer thread
 *
 * Launches the writer thread which will:
 * - Pop pages from the queue
 * - Write page data to file
 * - Update SHA1 hash
 * - Track block boundaries
 *
 * @param ctx Writer context
 * @return NO_ERROR on success, error code on failure
 */
error_t writer_start(writer_ctx_t *ctx);

/**
 * @brief Stop the writer thread and wait for completion
 *
 * Signals the queue to finish and waits for the writer to drain
 * all remaining pages before exiting.
 *
 * @param ctx Writer context
 */
void writer_stop(writer_ctx_t *ctx);

/**
 * @brief Wait for writer to complete (blocking)
 *
 * Waits for the writer thread to finish processing all pages.
 * Use after calling ogg_page_queue_finish() on the queue.
 *
 * @param ctx Writer context
 * @return Writer's final error status
 */
error_t writer_wait(writer_ctx_t *ctx);

/**
 * @brief Check if writer is running
 *
 * @param ctx Writer context
 * @return true if writer thread is running
 */
bool_t writer_is_running(writer_ctx_t *ctx);

/**
 * @brief Get writer error status
 *
 * @param ctx Writer context
 * @return Last error from writer thread
 */
error_t writer_get_error(writer_ctx_t *ctx);

/**
 * @brief Get number of pages written
 *
 * @param ctx Writer context
 * @return Count of pages written
 */
size_t writer_get_pages_written(writer_ctx_t *ctx);

/**
 * @brief Get total bytes written
 *
 * @param ctx Writer context
 * @return Total bytes written to file
 */
size_t writer_get_bytes_written(writer_ctx_t *ctx);

/**
 * @brief Writer thread entry point (internal)
 *
 * @param param Pointer to writer_ctx_t
 */
void writer_thread_task(void *param);
