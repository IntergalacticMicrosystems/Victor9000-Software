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

/* Write batching: chunks are staged in the application-owned I/O buffer
 * (ftx_set_io_buffer -> state->io_buf) and flushed to disk in whole blocks so
 * each INT 21h write (AH=40) covers many full 512-byte sectors instead of
 * forcing a partial-sector read-modify-write per ~1KB chunk. A bigger buffer
 * means fewer, far larger disk writes - a big win on slow media (floppy), where
 * each write costs a seek + rotational latency. The app sizes the buffer; a
 * multiple of both the 1024-byte chunk and the sector size is ideal. */

/**
 * Flush staged bytes to disk (one INT 21h write). No-op when empty. Returns
 * FTX_OK or FTX_ERR_FILE; leaves the buffer empty on success.
 */
static int flush_write_buf(ftx_state_t *state)
{
    int16_t written;

    if (state->io_fill == 0) {
        return FTX_OK;
    }

    written = ftx_file_write(state->file_handle, state->io_buf, state->io_fill);
    if (written != (int16_t)state->io_fill) {
        state->last_error = FTX_ERR_FILE;
        return FTX_ERR_FILE;
    }
    state->io_fill = 0;
    return FTX_OK;
}

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
    /* Honor per-chunk credit flow control only if the sender asked for it. We
     * always support it, so the sender's flag alone decides; we confirm back in
     * the START READY's caps byte (see send_ready). */
    state->flow_ctrl = (start->flags & FTX_FLAG_FLOWCTRL) ? 1 : 0;
    state->file_size = start->file_size;
    state->file_offset = start->offset;     /* 0 = whole file; >0 = segment */
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
    /* Advertise the features we actually turned on for this transfer, so the
     * sender knows whether to wait for per-chunk credits. */
    ready.caps = state->flow_ctrl ? FTX_CAP_FLOWCTRL : 0;

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
    int frc;
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

    /* Stage into the app-owned write buffer; flush in whole io_bufsize blocks so
     * the actual INT 21h write is sector-aligned and amortized over several
     * chunks. write_len <= FTX_CHUNK_SIZE (1024) <= io_bufsize, so after a flush
     * it always fits. The flush is the slow op; the credit below still gates the
     * sender across it. */
    if ((uint16_t)(state->io_fill + write_len) > state->io_bufsize) {
        frc = flush_write_buf(state);
        if (frc != FTX_OK) {
            state->stats.errors++;
            return frc;
        }
    }
    memcpy(state->io_buf + state->io_fill, write_buf, write_len);
    state->io_fill += write_len;
    if (state->io_fill >= state->io_bufsize) {
        frc = flush_write_buf(state);
        if (frc != FTX_OK) {
            state->stats.errors++;
            return frc;
        }
    }

    /* Update statistics */
    ftx_update_stats(state, write_len);
    state->current_chunk++;

    /* Per-chunk credit: the chunk is now committed to disk and we are about to
     * loop back for the next one, so tell the sender it may proceed. Sent only
     * in flow-control mode; the credit is a normal reliable packet (the packet
     * layer ACKs/retransmits it), so the sender's matching wait can't miss it
     * short of a dead link. This is what keeps the sender off the wire during
     * the (possibly multi-second) ftx_file_write above, so its next chunk never
     * streams into the 3-byte RX FIFO while we're busy. */
    if (state->flow_ctrl) {
        send_ready(state, 0);
    }

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

/**
 * Receive the DATA stream once the file is open and READY has been sent: loop
 * receiving chunks (each staged + flushed via flush_write_buf) until END
 * arrives. On return FTX_OK the END packet sits in state->pkt_buf for the
 * caller to finalize; the staged tail is NOT flushed here (the caller flushes
 * before close/verify). Returns FTX_ERR_ABORT on abort/error.
 *
 * The per-chunk turnaround below is timing-critical - do not add DOS calls to
 * the hot path.
 */
static int recv_data_loop(ftx_state_t *state, ftx_progress_fn progress)
{
    int result;
    uint16_t chunk;
    int retry_count;

    /* Start with an empty write buffer (a prior aborted transfer may have left
     * staged bytes; they belong to something we're no longer writing). */
    state->io_fill = 0;

    /* Receive data chunks */
    for (chunk = 0; chunk < state->total_chunks; chunk++) {
        retry_count = 0;

        while (retry_count < FTX_MAX_RETRIES) {
            result = recv_data_packet(state, chunk);

            if (result == FTX_OK) {
                /* Success - ACK is handled by packet layer */
                break;
            } else if (result == 1) {
                /* Received END packet early - pkt_buf holds the END. */
                return FTX_OK;
            } else if (result == FTX_ERR_ABORT) {
                /* Transfer aborted by PC */
                return FTX_ERR_ABORT;
            } else {
                /* Error - retry */
                retry_count++;
                state->stats.retries++;
            }
        }

        if (retry_count >= FTX_MAX_RETRIES) {
            /* Max retries exceeded */
            return FTX_ERR_ABORT;
        }

        /* Progress/stats. ftx_get_ticks() is an INT 21h (AH=2Ch) DOS call, and
         * this point sits in the critical line turnaround: the sender has just
         * been released by our per-chunk credit and is already streaming the
         * next chunk into the 3-byte RX FIFO. Spending DOS-call time here makes
         * us miss the frame start (dropped byte / FIFO overrun -> NAK -> a wasted
         * full-chunk retransmit, ~2x slower putfile). So only pay it when a
         * progress callback actually consumes the elapsed time; otherwise defer
         * it to the end. Keeps us re-armed for the next chunk in microseconds. */
        if (progress != (ftx_progress_fn)0) {
            state->stats.elapsed_ticks = ftx_get_ticks() - state->stats.start_ticks;
            if (progress(state, &state->stats) != 0) {
                /* User requested abort */
                ftx_abort(state);
                return FTX_ERR_ABORT;
            }
        }
    }

    /* Wait for END. All the file data is already in; the END is the last packet
     * and may simply be delayed - a slow/laggy USB-serial adapter (e.g. PL2303)
     * can stretch the post-last-chunk turnaround past one packet-wait window. So
     * be PATIENT on a timeout (keep waiting) rather than aborting a transfer
     * whose payload all arrived: aborting here would delete a fully-received file
     * over a missed trailing byte. Mirrors the per-chunk tolerance above. Only a
     * genuinely unexpected packet type is fatal. */
    retry_count = 0;
    while (retry_count < FTX_RECV_ERR_RETRIES) {
        result = ftx_recv_packet(state, state->pkt_buf, FTX_MAX_PAYLOAD);
        if (result < 0) {
            /* Timeout waiting for END - keep waiting a while longer. */
            retry_count++;
            continue;
        }

        if (state->pkt_buf[0] == FTX_CMD_END) {
            break;
        }

        if (state->pkt_buf[0] == FTX_CMD_DATA) {
            /* Retransmitted last chunk (our ACK was slow). Ignore, keep waiting. */
            retry_count++;
            continue;
        }

        /* Unexpected packet type */
        return FTX_ERR_ABORT;
    }

    if (retry_count >= FTX_RECV_ERR_RETRIES) {
        return FTX_ERR_ABORT;
    }

    return FTX_OK;
}

/*===========================================================================
 * Public Functions
 *===========================================================================*/

int ftx_wait_request(ftx_state_t *state, uint16_t timeout)
{
    uint32_t start_ticks;
    uint32_t timeout_ticks;
    int result;

    /* ftx_get_ticks() is centiseconds; convert the seconds timeout to match. */
    timeout_ticks = (uint32_t)timeout * 100UL;
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

    /* A registered staging buffer is required for the write batching below. */
    if (state->io_buf == (uint8_t *)0) {
        state->last_error = FTX_ERR_MEMORY;
        return FTX_ERR_MEMORY;
    }

    /* Determine destination filename */
    filename = (dst_path != (const char *)0) ? dst_path : state->filename;

    /* Check if file exists (only for a fresh whole-file/first-segment write;
     * a resumed segment at offset>0 is *expected* to already exist). */
    if (state->file_offset == 0 && !overwrite && ftx_file_exists(filename)) {
        /* Send error response */
        send_ready(state, FTX_ERR_EXISTS);
        state->last_error = FTX_ERR_EXISTS;
        return FTX_ERR_EXISTS;
    }

    /* Check disk space */
    /* Note: Could check ftx_disk_free() here */

    if (state->file_offset == 0) {
        /* Fresh transfer (or first segment): create/truncate. */
        state->file_handle = ftx_file_create(filename, FTX_ATTR_NORMAL);
    } else {
        /* Continuation segment: open the existing partial and seek to where
         * this segment's bytes belong. Earlier segments must be preserved. */
        state->file_handle = ftx_file_open(filename, FTX_OPEN_READWRITE);
        if (state->file_handle != FTX_FILE_INVALID) {
            if (ftx_file_seek(state->file_handle, (int32_t)state->file_offset,
                              FTX_SEEK_SET) < 0) {
                ftx_file_close(state->file_handle);
                state->file_handle = FTX_FILE_INVALID;
            }
        }
    }
    if (state->file_handle == FTX_FILE_INVALID) {
        send_ready(state, FTX_ERR_FILE);
        state->last_error = FTX_ERR_FILE;
        return FTX_ERR_FILE;
    }

    /* Send READY response */
    result = send_ready(state, 0);
    if (result != FTX_OK) {
        ftx_file_close(state->file_handle);
        if (state->file_offset == 0) ftx_file_delete(filename);
        return result;
    }

    /* Receive the data stream. */
    result = recv_data_loop(state, progress);

    /* Final elapsed time for stats (the per-chunk update was skipped to keep the
     * hot turnaround free of DOS calls - see recv_data_loop). */
    state->stats.elapsed_ticks = ftx_get_ticks() - state->stats.start_ticks;

    if (result != FTX_OK) {
        /* Abort/error. Clean up: only delete a file we created from scratch; a
         * segment at offset>0 leaves the already-written prefix intact for a
         * later resume. (recv_data_loop's user-abort path already sent ABORT.) */
        if (state->file_handle != FTX_FILE_INVALID) {
            ftx_file_close(state->file_handle);
            state->file_handle = FTX_FILE_INVALID;
        }
        if (state->file_offset == 0) ftx_file_delete(filename);
        state->last_error = FTX_ERR_ABORT;
        return FTX_ERR_ABORT;
    }

    /* END received. Commit any bytes still staged in the write buffer before we
     * close/verify - the final chunk(s) usually don't land on a block boundary. */
    if (flush_write_buf(state) != FTX_OK) {
        ftx_file_close(state->file_handle);
        state->file_handle = FTX_FILE_INVALID;
        if (state->file_offset == 0) ftx_file_delete(filename);
        state->last_error = FTX_ERR_FILE;
        return FTX_ERR_FILE;
    }

    /* Process END packet */
    result = process_end_packet(state);

    /* Close file and set date/time */
    ftx_file_set_datetime(state->file_handle, state->file_date, state->file_time);
    ftx_file_close(state->file_handle);
    state->file_handle = FTX_FILE_INVALID;

    if (result != FTX_OK) {
        /* Verification failed - delete a fresh file, but keep a partial
         * (its earlier segments are valid; the host can re-drive this one). */
        if (state->file_offset == 0) ftx_file_delete(filename);
        return result;
    }

    /* TODO: Verify file CRC by re-reading file */

    return FTX_OK;
}
