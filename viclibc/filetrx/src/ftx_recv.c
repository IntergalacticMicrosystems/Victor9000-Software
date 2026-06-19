/**
 * @file ftx_recv.c
 * @brief File Transfer - Receive Implementation
 *
 * Handles receiving files from PC to Victor.
 */

#include <string.h>
#include <stdio.h>
#include "filetrx.h"
#include "ftx_crc32.h"
#include "ftx_compress.h"

/* While waiting for a DATA chunk, a transient packet-layer error (a timeout
 * during the sender's retransmit of a dropped chunk, or a CRC error the
 * packet layer already NAKed for a corrupted chunk) is recoverable: the
 * sender retransmits. Keep re-receiving up to this many times before giving
 * up, so one lost/garbled chunk does not fail the whole transfer. Bounded so
 * a genuinely dead sender still aborts. */
#define FTX_RECV_ERR_RETRIES   40

/**
 * Parse a START packet already sitting in state->pkt_buf into the transfer
 * state (file metadata + fresh stats). Public: the server reads the START
 * itself (to inspect direction) before acting on it.
 */
int ftx_parse_start(ftx_state_t *state)
{
    ftx_start_t *start;

    /* Check command type */
    if (state->pkt_buf[0] != FTX_CMD_START) {
        state->last_error = FTX_ERR_PROTOCOL;
        return FTX_ERR_PROTOCOL;
    }

    /* Parse START packet */
    start = (ftx_start_t *)state->pkt_buf;

    /* Copy file info to state */
    state->direction = start->direction;
    state->compression = start->compression;
    state->file_size = start->file_size;
    state->file_crc = start->file_crc;
    state->file_date = start->file_date;
    state->file_time = start->file_time;
    state->file_attr = start->file_attr;

    /* Copy filename */
    if (start->name_len >= FTX_MAX_FILENAME) {
        state->last_error = FTX_ERR_PROTOCOL;
        return FTX_ERR_PROTOCOL;
    }
    memcpy(state->filename, start->filename, start->name_len);
    state->filename[start->name_len] = '\0';

    /* Calculate total chunks */
    state->total_chunks = FTX_CHUNKS_NEEDED(state->file_size);
    state->current_chunk = 0;

    /* Initialize statistics */
    state->stats.total_bytes = state->file_size;
    state->stats.total_chunks = state->total_chunks;
    state->stats.bytes_transferred = 0;
    state->stats.chunks_done = 0;
    state->stats.errors = 0;
    state->stats.retries = 0;
    state->stats.start_ticks = ftx_get_ticks();

    return FTX_OK;
}

/**
 * Wait for and receive START packet from PC, then parse it.
 */
static int recv_start_packet(ftx_state_t *state)
{
    int16_t len;

    /* Wait for packet */
    len = ftx_recv_packet(state, state->pkt_buf, FTX_MAX_PAYLOAD);
    if (len < 0) {
        state->last_error = FTX_ERR_TIMEOUT;
        return FTX_ERR_TIMEOUT;
    }

    return ftx_parse_start(state);
}

/**
 * Send READY response to PC.
 */
static int send_ready(ftx_state_t *state, uint8_t status)
{
    ftx_ready_t ready;

    ready.cmd = FTX_CMD_READY;
    ready.status = status;

    return ftx_send_packet(state, (uint8_t *)&ready, sizeof(ready));
}

/**
 * Receive and process DATA packet.
 *
 * Handles out-of-order chunks gracefully:
 * - If chunk_num < expected: duplicate from retry, ignore and wait for correct chunk
 * - If chunk_num > expected: missed chunk, request resend
 */
static int recv_data_packet(ftx_state_t *state, uint16_t expected_chunk)
{
    int16_t len;
    ftx_data_t *data;
    uint16_t write_len;
    int16_t written;
    uint8_t *write_buf;
    uint32_t decomp_len;
    ftx_resend_t resend;
    int resend_count;
    int recv_err_count;

    resend_count = 0;
    recv_err_count = 0;

retry_recv:
    /* Receive packet */
    len = ftx_recv_packet(state, state->pkt_buf, FTX_MAX_PAYLOAD);
    if (len < 0) {
        state->stats.errors++;
        /* Transient error - the sender will retransmit (dropped chunk after
         * its ACK timeout, or corrupted chunk after our packet-layer NAK).
         * Keep waiting for the retransmit instead of failing the transfer. */
        if (++recv_err_count < FTX_RECV_ERR_RETRIES) {
            goto retry_recv;
        }
        return (int)len;
    }

    /* Check for ABORT */
    if (state->pkt_buf[0] == FTX_CMD_ABORT) {
        state->last_error = FTX_ERR_ABORT;
        return FTX_ERR_ABORT;
    }

    /* Check for END (premature or expected) */
    if (state->pkt_buf[0] == FTX_CMD_END) {
        return 1;  /* Signal end of transfer */
    }

    /* Must be DATA packet */
    if (state->pkt_buf[0] != FTX_CMD_DATA) {
        state->stats.errors++;
        state->last_error = FTX_ERR_PROTOCOL;
        return FTX_ERR_PROTOCOL;
    }

    data = (ftx_data_t *)state->pkt_buf;

    /* Check chunk number */
    if (data->chunk_num != expected_chunk) {
        if (data->chunk_num < expected_chunk) {
            /* Duplicate chunk from retry - PC didn't receive our ACK.
             * The packet layer already ACKed this, just wait for correct chunk. */
            resend_count++;
            if (resend_count < 10) {
                goto retry_recv;
            }
        } else {
            /* Missed chunk(s) - request resend */
            resend.cmd = FTX_CMD_RESEND;
            resend.chunk_num = expected_chunk;
            ftx_send_packet(state, (uint8_t *)&resend, sizeof(resend));
            state->stats.retries++;
            resend_count++;
            if (resend_count < 10) {
                goto retry_recv;
            }
        }

        /* Too many out-of-order packets, give up */
        state->last_error = FTX_ERR_PROTOCOL;
        return FTX_ERR_PROTOCOL;
    }

    /* Decompress if needed - check per-chunk flag */
    if ((data->flags & FTX_DATA_FLAG_COMPRESSED) != 0) {
        decomp_len = rle_decompress(data->data, data->chunk_size,
                                    state->decomp_buf, FTX_CHUNK_SIZE);
        if (decomp_len == 0) {
            state->stats.errors++;
            state->last_error = FTX_ERR_COMPRESS;
            return FTX_ERR_COMPRESS;
        }
        write_buf = state->decomp_buf;
        write_len = (uint16_t)decomp_len;
    } else {
        write_buf = data->data;
        write_len = data->chunk_size;
    }

    /* Write to file */
    written = ftx_file_write(state->file_handle, write_buf, write_len);
    if (written != (int16_t)write_len) {
        state->stats.errors++;
        state->last_error = FTX_ERR_FILE;
        return FTX_ERR_FILE;
    }

    /* Update statistics */
    ftx_update_stats(state, write_len);
    state->current_chunk++;

    return FTX_OK;
}

/**
 * Process END packet and verify transfer.
 */
static int process_end_packet(ftx_state_t *state)
{
    ftx_end_t *end;

    end = (ftx_end_t *)state->pkt_buf;

    /* Verify total chunks */
    if (end->total_chunks != state->current_chunk) {
        state->last_error = FTX_ERR_PROTOCOL;
        return FTX_ERR_PROTOCOL;
    }

    /* Verify CRC matches what we computed while writing */
    /* Note: CRC verification happens after file is closed and re-read,
       or we compute it incrementally during write */

    return (end->status == FTX_STATUS_OK) ? FTX_OK : FTX_ERR_PROTOCOL;
}

/*===========================================================================
 * Public Functions
 *===========================================================================*/

int ftx_wait_request(ftx_state_t *state, uint16_t timeout)
{
    uint32_t start_ticks;
    uint32_t timeout_ticks;
    int result;

    /* Convert timeout to ticks (18.2 ticks per second) */
    timeout_ticks = (uint32_t)timeout * 182UL / 10UL;
    start_ticks = ftx_get_ticks();

    while (1) {
        /* Try to receive START packet */
        result = recv_start_packet(state);
        if (result == FTX_OK) {
            return FTX_OK;
        }

        /* Check timeout */
        if (timeout > 0) {
            if ((ftx_get_ticks() - start_ticks) > timeout_ticks) {
                state->last_error = FTX_ERR_TIMEOUT;
                return FTX_ERR_TIMEOUT;
            }
        }
    }
}

int ftx_receive_file(ftx_state_t *state, const char *dst_path,
                     int overwrite, ftx_progress_fn progress)
{
    int result;

    /* Wait for START packet */
    result = recv_start_packet(state);
    if (result != FTX_OK) {
        return result;
    }

    return ftx_recv_after_start(state, dst_path, overwrite, progress);
}

int ftx_recv_after_start(ftx_state_t *state, const char *dst_path,
                         int overwrite, ftx_progress_fn progress)
{
    int result;
    const char *filename;
    uint32_t computed_crc;
    uint16_t chunk;
    int retry_count;

    /* Determine destination filename */
    filename = (dst_path != (const char *)0) ? dst_path : state->filename;

    /* Check if file exists */
    if (!overwrite && ftx_file_exists(filename)) {
        /* Send error response */
        send_ready(state, FTX_ERR_EXISTS);
        state->last_error = FTX_ERR_EXISTS;
        return FTX_ERR_EXISTS;
    }

    /* Check disk space */
    /* Note: Could check ftx_disk_free() here */

    /* Create file */
    state->file_handle = ftx_file_create(filename, FTX_ATTR_NORMAL);
    if (state->file_handle == FTX_FILE_INVALID) {
        send_ready(state, FTX_ERR_FILE);
        state->last_error = FTX_ERR_FILE;
        return FTX_ERR_FILE;
    }

    /* Send READY response */
    result = send_ready(state, 0);
    if (result != FTX_OK) {
        ftx_file_close(state->file_handle);
        ftx_file_delete(filename);
        return result;
    }

    /* Initialize CRC for verification */
    computed_crc = FTX_CRC32_INIT;

    /* Receive data chunks */
    for (chunk = 0; chunk < state->total_chunks; chunk++) {
        retry_count = 0;

        while (retry_count < FTX_MAX_RETRIES) {
            result = recv_data_packet(state, chunk);

            if (result == FTX_OK) {
                /* Success - ACK is handled by packet layer */
                break;
            } else if (result == 1) {
                /* Received END packet early */
                goto end_received;
            } else if (result == FTX_ERR_ABORT) {
                /* Transfer aborted by PC */
                goto abort_transfer;
            } else {
                /* Error - retry */
                retry_count++;
                state->stats.retries++;
            }
        }

        if (retry_count >= FTX_MAX_RETRIES) {
            /* Max retries exceeded */
            goto abort_transfer;
        }

        /* Update progress */
        state->stats.elapsed_ticks = ftx_get_ticks() - state->stats.start_ticks;

        /* Call progress callback */
        if (progress != (ftx_progress_fn)0) {
            if (progress(state, &state->stats) != 0) {
                /* User requested abort */
                ftx_abort(state);
                goto abort_transfer;
            }
        }
    }

    /* Wait for END packet, ignoring any DATA retries */
    retry_count = 0;
    while (retry_count < 10) {
        result = ftx_recv_packet(state, state->pkt_buf, FTX_MAX_PAYLOAD);
        if (result < 0) {
            goto abort_transfer;
        }

        if (state->pkt_buf[0] == FTX_CMD_END) {
            break;
        }

        if (state->pkt_buf[0] == FTX_CMD_DATA) {
            /* DATA retry from PC - packet layer already ACKed, ignore and wait for END */
            retry_count++;
            continue;
        }

        /* Unexpected packet type */
        goto abort_transfer;
    }

    if (retry_count >= 10) {
        goto abort_transfer;
    }

end_received:
    /* Process END packet */
    result = process_end_packet(state);

    /* Close file and set date/time */
    ftx_file_set_datetime(state->file_handle, state->file_date, state->file_time);
    ftx_file_close(state->file_handle);
    state->file_handle = FTX_FILE_INVALID;

    if (result != FTX_OK) {
        /* Verification failed - delete file */
        ftx_file_delete(filename);
        return result;
    }

    /* TODO: Verify file CRC by re-reading file */

    return FTX_OK;

abort_transfer:
    /* Clean up on error */
    if (state->file_handle != FTX_FILE_INVALID) {
        ftx_file_close(state->file_handle);
        state->file_handle = FTX_FILE_INVALID;
    }
    ftx_file_delete(filename);
    state->last_error = FTX_ERR_ABORT;
    return FTX_ERR_ABORT;
}
