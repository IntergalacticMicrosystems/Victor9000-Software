/**
 * @file ftx_send.c
 * @brief File Transfer - Send Implementation
 *
 * Handles sending files from Victor to PC.
 */

#include <string.h>
#include "filetrx.h"
#include "ftx_crc32.h"
#include "ftx_compress.h"

/**
 * Calculate CRC-32 of entire file.
 */
static uint32_t calc_file_crc(ftx_state_t *state)
{
    uint32_t crc = FTX_CRC32_INIT;
    int16_t bytes_read;
    int32_t saved_pos;

    /* Save current position */
    saved_pos = ftx_file_seek(state->file_handle, 0, FTX_SEEK_CUR);

    /* Seek to beginning */
    ftx_file_seek(state->file_handle, 0, FTX_SEEK_SET);

    /* Read and accumulate CRC */
    while (1) {
        bytes_read = ftx_file_read(state->file_handle, state->chunk_buf,
                                   FTX_CHUNK_SIZE);
        if (bytes_read <= 0) {
            break;
        }
        crc = ftx_crc32_update(crc, state->chunk_buf, (uint32_t)bytes_read);
    }

    /* Restore position */
    ftx_file_seek(state->file_handle, saved_pos, FTX_SEEK_SET);

    return crc ^ FTX_CRC32_INIT;
}

/**
 * Extract filename from path.
 */
static const char *get_basename(const char *path)
{
    const char *p = path;
    const char *last = path;

    while (*p) {
        if (*p == '\\' || *p == '/') {
            last = p + 1;
        }
        p++;
    }
    return last;
}

/**
 * Send START packet with file metadata.
 */
static int send_start_packet(ftx_state_t *state, const char *filename)
{
    ftx_start_t start;
    uint8_t name_len;

    memset(&start, 0, sizeof(start));

    start.cmd = FTX_CMD_START;
    start.direction = FTX_DIR_VICTOR_TO_PC;
    start.compression = state->compression;
    start.flags = 0;
    start.file_size = state->file_size;
    start.compressed_size = 0;  /* Calculated per-chunk */
    start.file_date = state->file_date;
    start.file_time = state->file_time;
    start.file_attr = state->file_attr;
    start.file_crc = state->file_crc;

    /* Copy filename (just the base name, not full path) */
    filename = get_basename(filename);
    name_len = 0;
    while (filename[name_len] && name_len < FTX_MAX_FILENAME - 1) {
        start.filename[name_len] = filename[name_len];
        name_len++;
    }
    start.filename[name_len] = '\0';
    start.name_len = name_len;

    /* Send the whole fixed-layout struct so file_crc lands at offset 82, where
     * the receiver reads it. (Truncating after the filename used to drop the
     * CRC entirely, leaving the PC unable to verify a Victor->PC transfer.) */
    return ftx_send_packet(state, (uint8_t *)&start, sizeof(start));
}

/**
 * Wait for READY response from PC.
 */
static int wait_ready_response(ftx_state_t *state)
{
    int16_t len;
    ftx_ready_t *ready;

    len = ftx_recv_packet(state, state->pkt_buf, FTX_MAX_PAYLOAD);
    if (len < 0) {
        state->last_error = FTX_ERR_TIMEOUT;
        return FTX_ERR_TIMEOUT;
    }

    /* Check for error response */
    if (state->pkt_buf[0] == FTX_CMD_ERROR) {
        ftx_error_t *err = (ftx_error_t *)state->pkt_buf;
        state->last_error = err->error_code;
        return err->error_code;
    }

    if (state->pkt_buf[0] != FTX_CMD_READY) {
        state->last_error = FTX_ERR_PROTOCOL;
        return FTX_ERR_PROTOCOL;
    }

    ready = (ftx_ready_t *)state->pkt_buf;
    if (ready->status != 0) {
        state->last_error = ready->status;
        return ready->status;
    }

    return FTX_OK;
}

/**
 * Send a data chunk.
 */
static int send_data_chunk(ftx_state_t *state, uint16_t chunk_num)
{
    ftx_data_t *data;
    int16_t bytes_read;
    uint16_t data_len;
    uint32_t comp_len;
    int result;

    data = (ftx_data_t *)state->pkt_buf;
    data->cmd = FTX_CMD_DATA;
    data->flags = 0;
    data->chunk_num = chunk_num;

    /* Read chunk from file */
    bytes_read = ftx_file_read(state->file_handle, state->chunk_buf,
                               FTX_CHUNK_SIZE);
    if (bytes_read < 0) {
        state->last_error = FTX_ERR_FILE;
        return FTX_ERR_FILE;
    }
    if (bytes_read == 0) {
        /* End of file - shouldn't happen if chunk count is correct */
        state->last_error = FTX_ERR_FILE;
        return FTX_ERR_FILE;
    }

    /* Compress if requested. Mark the chunk so the receiver decompresses it -
     * the per-chunk flag, not just the START's mode, gates decompression. */
    if (state->compression == FTX_COMP_RLE) {
        comp_len = rle_compress(state->chunk_buf, (uint32_t)bytes_read,
                                data->data, FTX_CHUNK_SIZE);
        if (comp_len > 0 && comp_len < (uint32_t)bytes_read) {
            /* Compression saved space */
            data_len = (uint16_t)comp_len;
            data->flags = FTX_DATA_FLAG_COMPRESSED;
        } else {
            /* Compression didn't help - send uncompressed */
            memcpy(data->data, state->chunk_buf, bytes_read);
            data_len = (uint16_t)bytes_read;
        }
    } else {
        /* No compression */
        memcpy(data->data, state->chunk_buf, bytes_read);
        data_len = (uint16_t)bytes_read;
    }

    data->chunk_size = data_len;

    /* Send packet (with ACK handling by packet layer). Header is 6 bytes:
     * cmd + flags + chunk_num(2) + chunk_size(2), then data_len data bytes. */
    result = ftx_send_packet(state, state->pkt_buf, 6 + data_len);
    if (result == FTX_OK) {
        /* Count original (uncompressed) bytes sent, so stats reflect progress
         * on the send side too (the receive side tracks this via ftx_update_stats). */
        state->stats.bytes_transferred += (uint32_t)bytes_read;
    }
    return result;
}

/**
 * Send END packet.
 */
static int send_end_packet(ftx_state_t *state)
{
    ftx_end_t end;

    end.cmd = FTX_CMD_END;
    end.total_chunks = state->total_chunks;
    end.bytes_sent = state->stats.bytes_transferred;
    end.file_crc = state->file_crc;
    end.status = FTX_STATUS_OK;

    return ftx_send_packet(state, (uint8_t *)&end, sizeof(end));
}

/*===========================================================================
 * Public Functions
 *===========================================================================*/

int ftx_send_file(ftx_state_t *state, const char *src_path,
                  uint8_t compression, ftx_progress_fn progress)
{
    int result;
    int16_t attr;
    uint16_t chunk;
    int retry_count;

    /* Reset state */
    state->compression = compression;
    state->direction = FTX_DIR_VICTOR_TO_PC;
    state->current_chunk = 0;

    /* Check if file exists */
    if (!ftx_file_exists(src_path)) {
        state->last_error = FTX_ERR_NOT_FOUND;
        return FTX_ERR_NOT_FOUND;
    }

    /* Get file attributes */
    attr = ftx_file_get_attr(src_path);
    if (attr < 0) {
        state->last_error = FTX_ERR_FILE;
        return FTX_ERR_FILE;
    }
    state->file_attr = (uint8_t)attr;

    /* Open file */
    state->file_handle = ftx_file_open(src_path, FTX_OPEN_READ);
    if (state->file_handle == FTX_FILE_INVALID) {
        state->last_error = FTX_ERR_FILE;
        return FTX_ERR_FILE;
    }

    /* Get file size */
    state->file_size = ftx_file_size(state->file_handle);
    if (state->file_size == 0) {
        ftx_file_close(state->file_handle);
        state->file_handle = FTX_FILE_INVALID;
        state->last_error = FTX_ERR_FILE;
        return FTX_ERR_FILE;
    }

    /* Get file date/time */
    ftx_file_get_datetime(state->file_handle, &state->file_date, &state->file_time);

    /* Calculate CRC-32 */
    state->file_crc = calc_file_crc(state);

    /* Seek back to beginning */
    ftx_file_seek(state->file_handle, 0, FTX_SEEK_SET);

    /* Calculate total chunks */
    state->total_chunks = FTX_CHUNKS_NEEDED(state->file_size);

    /* Initialize statistics */
    state->stats.total_bytes = state->file_size;
    state->stats.total_chunks = state->total_chunks;
    state->stats.bytes_transferred = 0;
    state->stats.chunks_done = 0;
    state->stats.errors = 0;
    state->stats.retries = 0;
    state->stats.start_ticks = ftx_get_ticks();

    /* Send START packet */
    result = send_start_packet(state, src_path);
    if (result != FTX_OK) {
        ftx_file_close(state->file_handle);
        state->file_handle = FTX_FILE_INVALID;
        return result;
    }

    /* Wait for READY response */
    result = wait_ready_response(state);
    if (result != FTX_OK) {
        ftx_file_close(state->file_handle);
        state->file_handle = FTX_FILE_INVALID;
        return result;
    }

    /* Send data chunks */
    for (chunk = 0; chunk < state->total_chunks; chunk++) {
        retry_count = 0;

        while (retry_count < FTX_MAX_RETRIES) {
            result = send_data_chunk(state, chunk);

            if (result == FTX_OK) {
                /* Update stats */
                state->current_chunk++;
                state->stats.chunks_done++;
                break;
            } else if (result == FTX_ERR_ABORT) {
                goto abort_transfer;
            } else {
                /* Error - retry */
                retry_count++;
                state->stats.retries++;

                /* Seek back to retry the chunk */
                ftx_file_seek(state->file_handle,
                              (int32_t)chunk * FTX_CHUNK_SIZE,
                              FTX_SEEK_SET);
            }
        }

        if (retry_count >= FTX_MAX_RETRIES) {
            state->stats.errors++;
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

    /* Send END packet */
    result = send_end_packet(state);

    /* Close file */
    ftx_file_close(state->file_handle);
    state->file_handle = FTX_FILE_INVALID;

    if (result != FTX_OK) {
        return result;
    }

    /* Done. The END packet was already acknowledged by the packet layer
     * (send_end_packet -> pkt_send waits for the ACK), so there is no further
     * application-level reply to wait for. A blind trailing receive here would
     * swallow the NEXT request from a back-to-back client, so don't do it. */
    return FTX_OK;

abort_transfer:
    /* Send abort packet */
    ftx_abort(state);

    /* Close file */
    if (state->file_handle != FTX_FILE_INVALID) {
        ftx_file_close(state->file_handle);
        state->file_handle = FTX_FILE_INVALID;
    }

    state->last_error = FTX_ERR_ABORT;
    return FTX_ERR_ABORT;
}
