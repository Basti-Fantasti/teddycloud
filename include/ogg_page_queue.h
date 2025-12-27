/**
 * @file ogg_page_queue.h
 * @brief Thread-safe OGG page queue for asynchronous file writing
 *
 * Buffers OGG pages for asynchronous writing to decouple OGG generation
 * from disk I/O. Preserves page order and tracks block boundaries for
 * TAF file format compliance.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "os_port.h"
#include "error.h"
#include "ogg/ogg.h"

/**
 * @brief Maximum OGG page size (header + body)
 * OGG spec allows up to 65307 bytes, but typical pages are much smaller.
 * We use a larger buffer to accommodate any valid OGG page.
 */
#define OGG_PAGE_MAX_SIZE (OGG_HEADER_LENGTH + 255 * 255)

/**
 * @brief OGG header length (from toniefile.h)
 */
#ifndef OGG_HEADER_LENGTH
#define OGG_HEADER_LENGTH 27
#endif

/**
 * @brief Number of pages in the queue ring buffer
 * 32 pages provides good buffering for I/O latency spikes
 */
#define OGG_PAGE_QUEUE_SIZE 32

/**
 * @brief TAF block size for alignment tracking
 */
#define OGG_PAGE_BLOCK_SIZE 4096

/**
 * @brief OGG page entry containing page data and metadata
 */
typedef struct
{
    uint8_t *header;           /* OGG page header (dynamically allocated) */
    size_t header_len;         /* Header length in bytes */
    uint8_t *body;             /* OGG page body (dynamically allocated) */
    size_t body_len;           /* Body length in bytes */
    size_t file_pos;           /* File position at time of queuing */
    size_t block_num;          /* TAF block number at time of queuing */
} ogg_page_entry_t;

/**
 * @brief Thread-safe OGG page queue
 */
typedef struct
{
    ogg_page_entry_t entries[OGG_PAGE_QUEUE_SIZE];  /* Ring buffer */
    volatile size_t head;                            /* Write index (producer) */
    volatile size_t tail;                            /* Read index (consumer) */
    volatile size_t count;                           /* Current entry count */

    OsMutex mutex;                                   /* Protects queue state */
    OsEvent not_empty;                               /* Signaled when entries available */
    OsEvent not_full;                                /* Signaled when space available */

    volatile bool_t closed;                          /* Queue closed for new writes */
    volatile bool_t finished;                        /* Producer finished, drain remaining */
} ogg_page_queue_t;

/**
 * @brief Create and initialize an OGG page queue
 * @return Pointer to new queue, or NULL on failure
 */
ogg_page_queue_t *ogg_page_queue_create(void);

/**
 * @brief Destroy an OGG page queue and free resources
 * @param queue Queue to destroy
 */
void ogg_page_queue_destroy(ogg_page_queue_t *queue);

/**
 * @brief Push an OGG page onto the queue (producer)
 *
 * Copies the OGG page data. Blocks if queue is full until space
 * becomes available or queue is closed.
 *
 * @param queue Target queue
 * @param og OGG page to copy (header and body)
 * @param file_pos Current file position before write
 * @param block_num Current TAF block number
 * @return NO_ERROR on success, ERROR_ABORTED if queue closed
 */
error_t ogg_page_queue_push(ogg_page_queue_t *queue,
                            const ogg_page *og,
                            size_t file_pos,
                            size_t block_num);

/**
 * @brief Pop an OGG page from the queue (consumer)
 *
 * Blocks if queue is empty until an entry becomes available.
 * Caller must free the returned header and body pointers.
 *
 * @param queue Source queue
 * @param entry Output entry (data ownership transferred to caller)
 * @return NO_ERROR on success, ERROR_END_OF_STREAM if queue closed and empty
 */
error_t ogg_page_queue_pop(ogg_page_queue_t *queue,
                           ogg_page_entry_t *entry);

/**
 * @brief Check if queue is empty (non-blocking)
 * @param queue Queue to check
 * @return true if empty
 */
bool_t ogg_page_queue_is_empty(ogg_page_queue_t *queue);

/**
 * @brief Check if queue is full (non-blocking)
 * @param queue Queue to check
 * @return true if full
 */
bool_t ogg_page_queue_is_full(ogg_page_queue_t *queue);

/**
 * @brief Get current entry count (non-blocking)
 * @param queue Queue to check
 * @return Number of entries in queue
 */
size_t ogg_page_queue_count(ogg_page_queue_t *queue);

/**
 * @brief Signal that producer is finished (no more pages coming)
 *
 * After finishing, the consumer can drain remaining entries.
 * Pop will return ERROR_END_OF_STREAM once queue is empty.
 *
 * @param queue Queue to finish
 */
void ogg_page_queue_finish(ogg_page_queue_t *queue);

/**
 * @brief Close the queue and abort operations
 *
 * After closing, push operations will fail immediately and pop
 * operations will return ERROR_ABORTED.
 *
 * @param queue Queue to close
 */
void ogg_page_queue_close(ogg_page_queue_t *queue);

/**
 * @brief Reset queue to initial state (for reuse)
 *
 * Frees any remaining entries and resets indices.
 *
 * @param queue Queue to reset
 */
void ogg_page_queue_reset(ogg_page_queue_t *queue);

/**
 * @brief Free an OGG page entry's allocated memory
 *
 * Helper function to free header and body pointers after pop.
 *
 * @param entry Entry to free
 */
void ogg_page_entry_free(ogg_page_entry_t *entry);
