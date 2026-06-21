/**
 * @file filetrx.h
 * @brief Victor 9000 File Transfer Library
 *
 * Bidirectional file transfer between PC and Victor 9000 over serial.
 * Built on the packet protocol layer for reliable communication.
 *
 * Features:
 * - Bidirectional transfers (PC<->Victor)
 * - CRC-32 file integrity verification
 * - Optional RLE compression
 * - Progress callbacks
 * - Error recovery with retries
 * - Directory listing
 */

#ifndef FILETRX_H
#define FILETRX_H

#include <stdint.h>
#include "packet.h"
#include "ftx_protocol.h"
#include "ftx_dosfile.h"

/*===========================================================================
 * Configuration
 *===========================================================================*/

/* Default timeout for operations (ms) */
#define FTX_DEFAULT_TIMEOUT     5000

/* Default retry count */
#define FTX_DEFAULT_RETRIES     5

/*===========================================================================
 * Transfer State
 *===========================================================================*/

/**
 * Transfer statistics.
 */
typedef struct {
    uint32_t bytes_transferred; /* Bytes transferred so far */
    uint32_t total_bytes;       /* Total file size */
    uint16_t chunks_done;       /* Chunks completed */
    uint16_t total_chunks;      /* Total chunks */
    uint16_t errors;            /* Error count */
    uint16_t retries;           /* Retry count */
    uint32_t start_ticks;       /* Start time (DOS ticks) */
    uint32_t elapsed_ticks;     /* Elapsed time (DOS ticks) */
} ftx_stats_t;

/**
 * Transfer state structure.
 */
typedef struct {
    /* Packet protocol state */
    pkt_state_t *pkt;           /* Underlying packet protocol */
    uint8_t port;               /* Serial port */

    /* Current transfer info */
    char filename[FTX_MAX_FILENAME];
    uint32_t file_size;         /* Original file size */
    uint32_t file_crc;          /* File CRC-32 */
    uint16_t file_date;         /* DOS date */
    uint16_t file_time;         /* DOS time */
    uint8_t file_attr;          /* DOS attributes */
    uint8_t compression;        /* Compression mode */
    uint8_t direction;          /* Transfer direction */

    /* File handle (when open) */
    ftx_file_t file_handle;

    /* Transfer state */
    uint16_t current_chunk;     /* Current chunk number */
    uint16_t total_chunks;      /* Total chunks in transfer */
    int last_error;             /* Last error code */

    /* Statistics */
    ftx_stats_t stats;

    /* Data buffers */
    uint8_t chunk_buf[FTX_CHUNK_SIZE];      /* Chunk data buffer */
    uint8_t decomp_buf[FTX_CHUNK_SIZE];     /* Decompression buffer */
    uint8_t pkt_buf[FTX_MAX_PAYLOAD];       /* Packet buffer */

    /* Disk I/O block buffer: the receive path accumulates chunk payloads here
     * and flushes one big DOS write per block; the send path reads one big DOS
     * block here and serves chunks from it. disk_buf_len is the valid byte
     * count; disk_buf_pos is the read cursor (send path only). */
    uint8_t disk_buf[FTX_DISK_BLOCK];
    uint16_t disk_buf_len;
    uint16_t disk_buf_pos;
} ftx_state_t;

/*===========================================================================
 * Progress Callback
 *===========================================================================*/

/**
 * Progress callback function type.
 * Called during transfer with current statistics.
 *
 * @param state Transfer state
 * @param stats Current statistics
 * @return 0 to continue, non-zero to abort
 */
typedef int (*ftx_progress_fn)(ftx_state_t *state, const ftx_stats_t *stats);

/*===========================================================================
 * Initialization
 *===========================================================================*/

/**
 * Initialize file transfer layer.
 * @param state Transfer state structure to initialize
 * @param pkt Initialized packet protocol state
 * @return FTX_OK on success, negative error code on failure
 */
int ftx_init(ftx_state_t *state, pkt_state_t *pkt);

/**
 * Shutdown file transfer layer.
 * Closes any open files and cleans up state.
 * @param state Transfer state
 */
void ftx_shutdown(ftx_state_t *state);

/*===========================================================================
 * File Transfer - Receive
 *===========================================================================*/

/**
 * Receive a file from PC.
 * Waits for PC to initiate transfer, then receives file data.
 *
 * @param state Transfer state
 * @param dst_path Destination path on Victor (NULL to use sent filename)
 * @param overwrite Overwrite if file exists
 * @param progress Progress callback (may be NULL)
 * @return FTX_OK on success, negative error code on failure
 */
int ftx_receive_file(ftx_state_t *state, const char *dst_path,
                     int overwrite, ftx_progress_fn progress);

/**
 * Wait for incoming transfer request.
 * Blocks until PC sends START packet.
 *
 * @param state Transfer state
 * @param timeout Timeout in seconds (0 = infinite)
 * @return FTX_OK if request received, negative error on timeout/error
 */
int ftx_wait_request(ftx_state_t *state, uint16_t timeout);

/**
 * Parse a START packet already received into state->pkt_buf, populating the
 * transfer state (file metadata + fresh stats). Used by the server dispatch,
 * which reads the START itself to decide direction before acting on it.
 * @return FTX_OK on success, negative error code on failure
 */
int ftx_parse_start(ftx_state_t *state);

/**
 * Receive a file when the START has already been parsed into state (see
 * ftx_parse_start). Creates the file, sends READY, receives the data, verifies.
 * @param state Transfer state (START already applied)
 * @param dst_path Destination path (NULL to use the sent filename)
 * @param overwrite Overwrite if file exists
 * @param progress Progress callback (may be NULL)
 * @return FTX_OK on success, negative error code on failure
 */
int ftx_recv_after_start(ftx_state_t *state, const char *dst_path,
                         int overwrite, ftx_progress_fn progress);

/*===========================================================================
 * File Transfer - Send
 *===========================================================================*/

/**
 * Send a file to PC.
 * Victor initiates transfer by sending START packet.
 *
 * @param state Transfer state
 * @param src_path Source file path on Victor
 * @param compression Compression mode (FTX_COMP_*)
 * @param progress Progress callback (may be NULL)
 * @return FTX_OK on success, negative error code on failure
 */
int ftx_send_file(ftx_state_t *state, const char *src_path,
                  uint8_t compression, ftx_progress_fn progress);

/*===========================================================================
 * Directory Listing
 *===========================================================================*/

/**
 * Handle directory listing request from PC.
 * Called when PC sends LIST command. Receives the LIST packet then responds.
 *
 * @param state Transfer state
 * @return FTX_OK on success, negative error code on failure
 */
int ftx_handle_list_request(ftx_state_t *state);

/**
 * Respond to a LIST request already received into state->pkt_buf. Used by the
 * server dispatch, which reads the request itself.
 * @return FTX_OK on success, negative error code on failure
 */
int ftx_list_respond(ftx_state_t *state);

/* Outcomes of ftx_serve_one(). */
#define FTX_SERVE_IDLE  1   /* nothing arrived before the receive timed out */
#define FTX_SERVE_DONE  2   /* handled one request (transfer or listing) */
#define FTX_SERVE_QUIT  3   /* a QUIT command was received */

/**
 * Handle at most one server request, then return. Dispatches
 * START(PC->Victor) to receive, START(Victor->PC) to send the requested file,
 * LIST to a directory listing, and QUIT to FTX_SERVE_QUIT. Blocks only for one
 * receive timeout when idle, so the caller can poll the keyboard between calls.
 *
 * @param state Initialized transfer state
 * @param progress Progress callback (may be NULL)
 * @return one of FTX_SERVE_*
 */
int ftx_serve_one(ftx_state_t *state, ftx_progress_fn progress);

/**
 * Convenience loop: call ftx_serve_one() until a QUIT arrives.
 *
 * @param state Initialized transfer state
 * @param progress Progress callback (may be NULL)
 * @return FTX_OK when a QUIT was received
 */
int ftx_serve(ftx_state_t *state, ftx_progress_fn progress);

/**
 * Request directory listing from PC.
 * Victor requests listing, receives response.
 *
 * @param state Transfer state
 * @param path Directory path to list
 * @param callback Called for each entry (return 0 to continue)
 * @param user_data User data passed to callback
 * @return FTX_OK on success, negative error code on failure
 */
typedef int (*ftx_dir_callback)(const ftx_dir_entry_t *entry, void *user_data);

int ftx_list_dir(ftx_state_t *state, const char *path,
                 ftx_dir_callback callback, void *user_data);

/*===========================================================================
 * Control Functions
 *===========================================================================*/

/**
 * Abort current transfer.
 * Sends ABORT packet and cleans up.
 *
 * @param state Transfer state
 */
void ftx_abort(ftx_state_t *state);

/**
 * Get last error code.
 * @param state Transfer state
 * @return Last error code
 */
int ftx_get_error(ftx_state_t *state);

/**
 * Get error message for error code.
 * @param error Error code
 * @return Error message string
 */
const char *ftx_error_msg(int error);

/*===========================================================================
 * Statistics Functions
 *===========================================================================*/

/**
 * Get transfer statistics.
 * @param state Transfer state
 * @return Pointer to statistics structure
 */
const ftx_stats_t *ftx_get_stats(ftx_state_t *state);

/**
 * Get bytes transferred.
 * @param state Transfer state
 * @return Bytes transferred so far
 */
uint32_t ftx_get_bytes_transferred(ftx_state_t *state);

/**
 * Get transfer speed in bytes per second.
 * @param state Transfer state
 * @return Transfer speed (0 if not enough data)
 */
uint16_t ftx_get_bytes_per_second(ftx_state_t *state);

/**
 * Get completion percentage.
 * @param state Transfer state
 * @return Percentage complete (0-100)
 */
uint8_t ftx_get_percent_complete(ftx_state_t *state);

/*===========================================================================
 * Internal Functions (for ftx_send.c and ftx_recv.c)
 *===========================================================================*/

/* Send a protocol packet */
int ftx_send_packet(ftx_state_t *state, const uint8_t *data, uint16_t len);

/* Receive a protocol packet */
int ftx_recv_packet(ftx_state_t *state, uint8_t *data, uint16_t maxlen);

/* Receive a protocol packet with the ACK deferred (software flow control).
 * Pair with ftx_ack_data() once the chunk has been processed/flushed. */
int ftx_recv_data(ftx_state_t *state, uint8_t *data, uint16_t maxlen);

/* Send the ACK that ftx_recv_data() withheld. */
void ftx_ack_data(ftx_state_t *state);

/* Update statistics */
void ftx_update_stats(ftx_state_t *state, uint16_t bytes);

/* Get DOS tick count */
uint32_t ftx_get_ticks(void);

#endif /* FILETRX_H */
