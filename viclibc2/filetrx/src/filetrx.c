/**
 * @file filetrx.c
 * @brief File Transfer Library - Main Implementation
 *
 * Common functions, initialization, and directory listing support.
 */

#include <dos.h>
#include <string.h>
#include "filetrx.h"

/*===========================================================================
 * Error Messages
 *===========================================================================*/

static const char *error_messages[] = {
    "Success",                      /* FTX_OK = 0 */
    "Timeout",                      /* FTX_ERR_TIMEOUT = -1 */
    "CRC mismatch",                 /* FTX_ERR_CRC = -2 */
    "Protocol error",               /* FTX_ERR_PROTOCOL = -3 */
    "File I/O error",               /* FTX_ERR_FILE = -4 */
    "Disk full",                    /* FTX_ERR_DISK_FULL = -5 */
    "Transfer aborted",             /* FTX_ERR_ABORT = -6 */
    "Compression error",            /* FTX_ERR_COMPRESS = -7 */
    "Out of memory",                /* FTX_ERR_MEMORY = -8 */
    "File not found",               /* FTX_ERR_NOT_FOUND = -9 */
    "File already exists",          /* FTX_ERR_EXISTS = -10 */
    "Packet layer error"            /* FTX_ERR_PACKET = -11 */
};

#define NUM_ERROR_MESSAGES (sizeof(error_messages) / sizeof(error_messages[0]))

/*===========================================================================
 * Initialization
 *===========================================================================*/

int ftx_init(ftx_state_t *state, pkt_state_t *pkt)
{
    if (state == (ftx_state_t *)0 || pkt == (pkt_state_t *)0) {
        return FTX_ERR_PROTOCOL;
    }

    /* Clear state */
    memset(state, 0, sizeof(ftx_state_t));

    /* Store packet protocol reference */
    state->pkt = pkt;
    state->port = pkt->port;

    /* Initialize file handle as invalid */
    state->file_handle = FTX_FILE_INVALID;

    return FTX_OK;
}

int ftx_set_io_buffer(ftx_state_t *state, uint8_t *buf, uint16_t size)
{
    if (state == (ftx_state_t *)0 || buf == (uint8_t *)0 ||
        size < FTX_CHUNK_SIZE) {
        return FTX_ERR_PROTOCOL;
    }

    state->io_buf     = buf;
    state->io_bufsize = size;
    /* Start empty: nothing staged for write, no block buffered for read. */
    state->io_fill = 0;
    state->io_base = 0xFFFFFFFFUL;
    state->io_len  = 0;

    return FTX_OK;
}

void ftx_shutdown(ftx_state_t *state)
{
    if (state == (ftx_state_t *)0) {
        return;
    }

    /* Close any open file */
    if (state->file_handle != FTX_FILE_INVALID) {
        ftx_file_close(state->file_handle);
        state->file_handle = FTX_FILE_INVALID;
    }
}

/*===========================================================================
 * Packet I/O (wrappers around packet layer)
 *===========================================================================*/

int ftx_send_packet(ftx_state_t *state, const uint8_t *data, uint16_t len)
{
    int16_t result;

    result = pkt_send(state->pkt, data, len);
    if (result < 0) {
        state->last_error = FTX_ERR_PACKET;
        return FTX_ERR_PACKET;
    }

    return FTX_OK;
}

int ftx_recv_packet(ftx_state_t *state, uint8_t *data, uint16_t maxlen)
{
    int16_t result;

    result = pkt_receive(state->pkt, data, maxlen);
    if (result < 0) {
        if (result == PKT_ERR_TIMEOUT) {
            state->last_error = FTX_ERR_TIMEOUT;
            return FTX_ERR_TIMEOUT;
        }
        state->last_error = FTX_ERR_PACKET;
        return FTX_ERR_PACKET;
    }

    return result;
}

/*===========================================================================
 * Control Functions
 *===========================================================================*/

void ftx_abort(ftx_state_t *state)
{
    ftx_abort_t abort;

    if (state == (ftx_state_t *)0) {
        return;
    }

    /* Send abort packet */
    abort.cmd = FTX_CMD_ABORT;
    abort.reason = (uint8_t)(-state->last_error);
    abort.message[0] = '\0';

    pkt_send_raw(state->pkt, PKT_TYPE_DATA, (uint8_t *)&abort, sizeof(abort));

    /* Close any open file */
    if (state->file_handle != FTX_FILE_INVALID) {
        ftx_file_close(state->file_handle);
        state->file_handle = FTX_FILE_INVALID;
    }
}

int ftx_get_error(ftx_state_t *state)
{
    if (state == (ftx_state_t *)0) {
        return FTX_ERR_PROTOCOL;
    }
    return state->last_error;
}

const char *ftx_error_msg(int error)
{
    int idx;

    if (error >= 0) {
        return error_messages[0];  /* Success */
    }

    idx = -error;
    if (idx >= (int)NUM_ERROR_MESSAGES) {
        return "Unknown error";
    }

    return error_messages[idx];
}

/*===========================================================================
 * Statistics Functions
 *===========================================================================*/

const ftx_stats_t *ftx_get_stats(ftx_state_t *state)
{
    if (state == (ftx_state_t *)0) {
        return (ftx_stats_t *)0;
    }
    return &state->stats;
}

void ftx_update_stats(ftx_state_t *state, uint16_t bytes)
{
    state->stats.bytes_transferred += bytes;
    state->stats.chunks_done++;
    state->stats.elapsed_ticks = ftx_get_ticks() - state->stats.start_ticks;
}

uint32_t ftx_get_bytes_transferred(ftx_state_t *state)
{
    if (state == (ftx_state_t *)0) {
        return 0;
    }
    return state->stats.bytes_transferred;
}

uint16_t ftx_get_bytes_per_second(ftx_state_t *state)
{
    uint32_t cs;

    if (state == (ftx_state_t *)0) {
        return 0;
    }

    /* elapsed_ticks is centiseconds (1/100 s) - see ftx_get_ticks(). Compute the
     * rate as bytes*100/centiseconds so sub-second transfers still report a
     * non-zero rate. */
    cs = state->stats.elapsed_ticks;
    if (cs == 0) {
        return 0;
    }

    return (uint16_t)((state->stats.bytes_transferred * 100UL) / cs);
}

uint8_t ftx_get_percent_complete(ftx_state_t *state)
{
    if (state == (ftx_state_t *)0 || state->stats.total_bytes == 0) {
        return 0;
    }

    return (uint8_t)((state->stats.bytes_transferred * 100UL) /
                     state->stats.total_bytes);
}

/*===========================================================================
 * DOS Timer
 *===========================================================================*/

uint32_t ftx_get_ticks(void)
{
    /*
     * Centiseconds (1/100 s) since midnight, via DOS get-time (INT 21h AH=2Ch).
     *
     * The obvious source - the IBM-PC BIOS tick counter at 0040:006C - is DEAD on
     * the Victor 9000: it is not a PC clone, its ROM BIOS does not maintain that
     * data area, so the old read returned a constant and every elapsed/rate stat
     * came out 0. DOS itself does keep the clock (the timer ISR updates it), so
     * AH=2Ch advances. Effective resolution is the DOS timer tick (coarser than
     * 1 cs), but the value is in 1/100 s. Wraps at midnight - fine for the short
     * intervals a transfer spans. All callers treat the result as centiseconds.
     */
    union REGS r;
    r.h.ah = 0x2C;
    int86(0x21, &r, &r);
    /* CH=hour CL=min DH=sec DL=hundredths */
    return (((uint32_t)r.h.ch * 3600UL + (uint32_t)r.h.cl * 60UL
             + (uint32_t)r.h.dh) * 100UL) + (uint32_t)r.h.dl;
}

/*===========================================================================
 * Directory Listing
 *===========================================================================*/

int ftx_handle_list_request(ftx_state_t *state)
{
    int result;

    /* Receive the LIST request, then respond to it. */
    result = ftx_recv_packet(state, state->pkt_buf, FTX_MAX_PAYLOAD);
    if (result < 0) {
        return result;
    }

    return ftx_list_respond(state);
}

int ftx_list_respond(ftx_state_t *state)
{
    ftx_list_t *list_req;
    ftx_list_resp_t *list_resp;
    ftx_dta_t dta;
    char pattern[FTX_MAX_FILENAME + 4];
    int result;
    uint8_t entry_count;
    uint8_t max_entries;
    ftx_dir_entry_t *entry;

    if (state->pkt_buf[0] != FTX_CMD_LIST) {
        state->last_error = FTX_ERR_PROTOCOL;
        return FTX_ERR_PROTOCOL;
    }

    entry_count = 0;    /* guard the empty-directory check below */

    list_req = (ftx_list_t *)state->pkt_buf;

    /* Build search pattern */
    if (list_req->path_len > 0) {
        memcpy(pattern, list_req->path, list_req->path_len);
        pattern[list_req->path_len] = '\0';

        /* Append \*.* if path doesn't already have a wildcard */
        if (pattern[list_req->path_len - 1] != '*') {
            /* Check if path ends with backslash */
            if (pattern[list_req->path_len - 1] != '\\') {
                pattern[list_req->path_len] = '\\';
                list_req->path_len++;
            }
            pattern[list_req->path_len] = '*';
            pattern[list_req->path_len + 1] = '.';
            pattern[list_req->path_len + 2] = '*';
            pattern[list_req->path_len + 3] = '\0';
        }
    } else {
        /* Current directory */
        pattern[0] = '*';
        pattern[1] = '.';
        pattern[2] = '*';
        pattern[3] = '\0';
    }

    /* Calculate max entries per packet */
    max_entries = (uint8_t)FTX_MAX_DIR_ENTRIES;

    /* Find first file */
    result = ftx_dir_find_first(pattern, 0x37, &dta);  /* 0x37 = all attrs */

    while (result == 0) {
        /* Build response packet */
        list_resp = (ftx_list_resp_t *)state->pkt_buf;
        list_resp->cmd = FTX_CMD_LIST_RESP;
        entry_count = 0;
        entry = list_resp->entries;

        /* Fill entries */
        while (result == 0 && entry_count < max_entries) {
            /* Copy entry data */
            memcpy(entry->name, dta.name, 13);
            entry->attr = dta.attr;
            entry->date = dta.date;
            entry->time = dta.time;
            entry->size = dta.size;

            entry_count++;
            entry++;

            /* Find next */
            result = ftx_dir_find_next(&dta);
        }

        list_resp->entry_count = entry_count;
        list_resp->more_entries = (result == 0) ? 1 : 0;

        /* Send response. Use a separate status so `result` keeps holding the
         * find_next outcome (0 = more files) that governs the outer loop. */
        if (ftx_send_packet(state, state->pkt_buf,
                            3 + entry_count * sizeof(ftx_dir_entry_t)) != FTX_OK) {
            return FTX_ERR_PACKET;
        }

        /* Stop once the packet we just sent was the last one; otherwise
         * `result` is already 0 (more files) and the loop continues. Without
         * this, the final send (success, result=0) would re-enter the loop and
         * emit a spurious extra packet to a peer that has already stopped
         * listening, stalling the next request on retries. */
        if (!list_resp->more_entries) {
            break;
        }
    }

    /* If no entries found, send empty response */
    if (entry_count == 0) {
        list_resp = (ftx_list_resp_t *)state->pkt_buf;
        list_resp->cmd = FTX_CMD_LIST_RESP;
        list_resp->entry_count = 0;
        list_resp->more_entries = 0;

        return ftx_send_packet(state, state->pkt_buf, 3);
    }

    return FTX_OK;
}

int ftx_list_dir(ftx_state_t *state, const char *path,
                 ftx_dir_callback callback, void *user_data)
{
    ftx_list_t list_req;
    ftx_list_resp_t *list_resp;
    int result;
    uint8_t i;
    uint8_t path_len;

    if (callback == (ftx_dir_callback)0) {
        return FTX_ERR_PROTOCOL;
    }

    /* Build LIST request */
    list_req.cmd = FTX_CMD_LIST;
    path_len = 0;
    if (path != (const char *)0) {
        while (path[path_len] && path_len < FTX_MAX_FILENAME - 1) {
            list_req.path[path_len] = path[path_len];
            path_len++;
        }
    }
    list_req.path[path_len] = '\0';
    list_req.path_len = path_len;

    /* Send request */
    result = ftx_send_packet(state, (uint8_t *)&list_req,
                             2 + path_len + 1);
    if (result != FTX_OK) {
        return result;
    }

    /* Receive responses */
    do {
        result = ftx_recv_packet(state, state->pkt_buf, FTX_MAX_PAYLOAD);
        if (result < 0) {
            return result;
        }

        if (state->pkt_buf[0] == FTX_CMD_ERROR) {
            ftx_error_t *err = (ftx_error_t *)state->pkt_buf;
            state->last_error = err->error_code;
            return err->error_code;
        }

        if (state->pkt_buf[0] != FTX_CMD_LIST_RESP) {
            state->last_error = FTX_ERR_PROTOCOL;
            return FTX_ERR_PROTOCOL;
        }

        list_resp = (ftx_list_resp_t *)state->pkt_buf;

        /* Process entries */
        for (i = 0; i < list_resp->entry_count; i++) {
            if (callback(&list_resp->entries[i], user_data) != 0) {
                /* Callback requested stop */
                return FTX_OK;
            }
        }

    } while (list_resp->more_entries);

    return FTX_OK;
}
