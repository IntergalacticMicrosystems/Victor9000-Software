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

/* Read batching: getfile reads the file in big blocks (into the app-owned I/O
 * buffer, ftx_set_io_buffer -> state->io_buf) and serves 1 KB chunks out of the
 * block, so each INT 21h read (AH=3F) covers many sectors instead of one disk
 * access per ~1 KB chunk - a big win on slow media (floppy), where every read
 * costs a seek + rotational latency. Mirror of the receive-side write batching.
 * Keyed by absolute file offset so the retry path (which re-requests the same
 * chunk) is served from the block with no re-read. The app sizes the buffer; a
 * whole multiple of the 1024-byte chunk keeps a chunk request from straddling
 * two refills. */

/**
 * Point *out at `count` bytes at absolute file offset `off`, refilling the read
 * block (one seek + one big read) only when the requested range isn't already
 * buffered. Returns FTX_OK or FTX_ERR_FILE.
 */
static int read_at(ftx_state_t *state, uint32_t off, uint16_t count, uint8_t **out)
{
    if (state->io_base == 0xFFFFFFFFUL ||
        off < state->io_base ||
        (uint32_t)(off + count) > (uint32_t)(state->io_base + state->io_len)) {
        int16_t n;
        if (ftx_file_seek(state->file_handle, (int32_t)off, FTX_SEEK_SET) < 0) {
            return FTX_ERR_FILE;
        }
        n = ftx_file_read(state->file_handle, state->io_buf, state->io_bufsize);
        if (n <= 0) {
            return FTX_ERR_FILE;
        }
        state->io_base = off;
        state->io_len  = (uint16_t)n;
        if ((uint32_t)count > (uint32_t)state->io_len) {
            return FTX_ERR_FILE;   /* asked for more than the source holds here */
        }
    }
    *out = state->io_buf + (uint16_t)(off - state->io_base);
    return FTX_OK;
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
    start.flags = (state->file_offset != 0) ? FTX_FLAG_PARTIAL : 0;
    start.file_size = state->file_size;     /* bytes in this (possibly partial) send */
    start.offset = state->file_offset;      /* where this segment starts in the file */
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
static int send_data_chunk(ftx_state_t *state, uint16_t chunk_num,
                           uint32_t abs_off, uint16_t read_len)
{
    ftx_data_t *data;
    int16_t bytes_read;
    uint16_t data_len;
    uint32_t comp_len;
    int result;
    uint8_t *src;

    data = (ftx_data_t *)state->pkt_buf;
    data->cmd = FTX_CMD_DATA;
    data->flags = 0;
    data->chunk_num = chunk_num;

    /* Fetch the chunk from the read-ahead block (refills via one big disk read
     * when needed). read_len bounds it to the segment (the last chunk of a
     * partial send is short and must not spill into later bytes). Copy into
     * chunk_buf so compression and the running CRC see it exactly as before. */
    result = read_at(state, abs_off, read_len, &src);
    if (result != FTX_OK) {
        state->last_error = FTX_ERR_FILE;
        return FTX_ERR_FILE;
    }
    memcpy(state->chunk_buf, src, read_len);
    bytes_read = (int16_t)read_len;

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

/**
 * Drive a send once the START packet has gone out: wait for READY, stream the
 * segment as DATA chunks (read-batched via read_at), accumulate the file CRC,
 * and send END.
 *
 * Reads the segment geometry from state: file_offset is the absolute base byte
 * offset within the file, file_size is the segment length.
 *
 * Does NOT close the backing store; the caller does (the abort path here calls
 * ftx_abort, which closes any open file handle). Returns FTX_OK, a packet
 * error, or FTX_ERR_ABORT.
 */
static int send_stream(ftx_state_t *state, ftx_progress_fn progress)
{
    int result;
    uint16_t chunk;
    int retry_count;
    uint32_t base    = state->file_offset;
    uint32_t seg_len = state->file_size;
    uint32_t run_crc = FTX_CRC32_INIT;
    uint32_t done;              /* bytes already streamed from the segment */

    /* Wait for READY response */
    result = wait_ready_response(state);
    if (result != FTX_OK) {
        return result;
    }

    /* Send data chunks. A running offset (not chunk*CHUNK_SIZE) tracks how much
     * of the segment has been streamed. */
    chunk = 0;
    done  = 0;
    while (done < seg_len) {
        uint16_t this_len;
        uint32_t left = seg_len - done;

        this_len = (left < FTX_CHUNK_SIZE) ? (uint16_t)left : FTX_CHUNK_SIZE;

        retry_count = 0;
        while (retry_count < FTX_MAX_RETRIES) {
            result = send_data_chunk(state, chunk, base + done, this_len);

            if (result == FTX_OK) {
                /* Fold this chunk's original bytes into the running CRC.
                 * chunk_buf still holds them (compression wrote elsewhere).
                 * Done once, only on success, so a retried chunk is not
                 * double-counted. this_len is exactly this chunk's byte count. */
                run_crc = ftx_crc32_update(run_crc, state->chunk_buf, this_len);
                state->current_chunk++;
                state->stats.chunks_done++;
                break;
            } else if (result == FTX_ERR_ABORT) {
                goto abort_transfer;
            } else {
                /* Error - retry. read_at re-serves this same offset from the
                 * read block (or refills), so no explicit seek-back is needed. */
                retry_count++;
                state->stats.retries++;
            }
        }

        if (retry_count >= FTX_MAX_RETRIES) {
            state->stats.errors++;
            goto abort_transfer;
        }

        done += this_len;
        chunk++;

        /* Update progress */
        state->stats.elapsed_ticks = ftx_get_ticks() - state->stats.start_ticks;

        if (progress != (ftx_progress_fn)0) {
            if (progress(state, &state->stats) != 0) {
                ftx_abort(state);
                goto abort_transfer;
            }
        }
    }

    /* The actual chunk count; END carries it. */
    state->total_chunks = chunk;

    /* Finalize the incremental CRC; send_end_packet carries it in END. */
    state->file_crc = run_crc ^ FTX_CRC32_INIT;

    return send_end_packet(state);

abort_transfer:
    ftx_abort(state);
    state->last_error = FTX_ERR_ABORT;
    return FTX_ERR_ABORT;
}

/*===========================================================================
 * Public Functions
 *===========================================================================*/

int ftx_send_file(ftx_state_t *state, const char *src_path,
                  uint8_t compression, ftx_progress_fn progress)
{
    int result;
    int16_t attr;
    uint32_t req_offset;        /* read start requested by the PC (0 = whole file) */
    uint32_t req_len;           /* bytes requested (0 = to EOF) */
    uint32_t full_size;         /* actual size of the file on disk */
    uint32_t seg_len;           /* bytes this send will actually transfer */

    /* The request START was parsed into state by ftx_parse_start: file_offset is
     * the read start, file_size the requested length. Capture both before the
     * file-open logic below overwrites file_size with the real on-disk size. */
    req_offset = state->file_offset;
    req_len    = state->file_size;

    /* A registered staging buffer is required for the read batching below. */
    if (state->io_buf == (uint8_t *)0) {
        state->last_error = FTX_ERR_MEMORY;
        return FTX_ERR_MEMORY;
    }

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

    /* Get the real on-disk size, then derive the segment to send. */
    full_size = ftx_file_size(state->file_handle);
    if (full_size == 0) {
        ftx_file_close(state->file_handle);
        state->file_handle = FTX_FILE_INVALID;
        state->last_error = FTX_ERR_FILE;
        return FTX_ERR_FILE;
    }
    if (req_offset > full_size) req_offset = full_size;  /* clamp: empty segment */
    seg_len = full_size - req_offset;                    /* available from offset */
    if (req_len != 0 && req_len < seg_len) seg_len = req_len;  /* honor requested len */

    state->file_offset = req_offset;
    state->file_size   = seg_len;       /* this transfer's byte count (segment) */

    /* Get file date/time */
    ftx_file_get_datetime(state->file_handle, &state->file_date, &state->file_time);

    /* The file CRC is computed incrementally as the chunks are sent and carried
     * in the END packet - NOT pre-computed here. Pre-reading the whole file just
     * to CRC it (then reading it a second time to send it) doubled the disk I/O
     * and stalled the START by seconds on the 8088. START goes out with
     * file_crc = 0; the receiver verifies against END's file_crc. */
    state->file_crc = 0;

    /* Seek to the segment start. (read_at also seeks as it refills, keyed by
     * absolute offset, so this is just a tidy starting position.) Start with an
     * empty read block - a prior transfer may have left one buffered. */
    ftx_file_seek(state->file_handle, (int32_t)req_offset, FTX_SEEK_SET);
    state->io_base = 0xFFFFFFFFUL;
    state->io_len  = 0;

    /* Calculate total chunks for the segment */
    state->total_chunks = FTX_CHUNKS_NEEDED(seg_len);

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
    if (result == FTX_OK) {
        /* Wait for READY, stream the segment, send END. */
        result = send_stream(state, progress);
    }

    /* Close the file (send_stream's abort path may have already closed it via
     * ftx_abort). On success the END was already ACKed by the packet layer, so
     * there is no further application reply to wait for. */
    if (state->file_handle != FTX_FILE_INVALID) {
        ftx_file_close(state->file_handle);
        state->file_handle = FTX_FILE_INVALID;
    }

    return result;
}
