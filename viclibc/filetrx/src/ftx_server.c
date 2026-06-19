/**
 * @file ftx_server.c
 * @brief File Transfer Server Dispatch
 *
 * Request-driven server for the file-transfer library. Reads one request
 * packet at a time and acts on it:
 *   - START(direction PC_TO_VICTOR): receive a file from the PC
 *   - START(direction VICTOR_TO_PC): a request - send the named file to the PC
 *   - LIST: respond with a directory listing
 *   - QUIT: stop the server
 *
 * Built on the RESET-aware packet layer, so a freshly (re)connected PC client
 * resyncs sequence bits transparently before each request.
 */

#include <stdio.h>
#include "filetrx.h"

int ftx_serve_one(ftx_state_t *state, ftx_progress_fn progress)
{
    int16_t len;
    uint8_t cmd;
    int rc;

    /* Wait for one request. A timeout just means the link is idle. */
    len = ftx_recv_packet(state, state->pkt_buf, FTX_MAX_PAYLOAD);
    if (len < 0) {
        return FTX_SERVE_IDLE;
    }

    cmd = state->pkt_buf[0];

    if (cmd == FTX_CMD_QUIT) {
        printf("  <- QUIT\n");
        return FTX_SERVE_QUIT;
    }

    if (cmd == FTX_CMD_START) {
        rc = ftx_parse_start(state);
        if (rc != FTX_OK) {
            printf("  <- START (rejected: %s)\n", ftx_error_msg(rc));
            state->last_error = rc;
            return FTX_SERVE_DONE;
        }
        if (state->direction == FTX_DIR_VICTOR_TO_PC) {
            /* PC requested a file: send it (filename came in the START). */
            printf("  <- getfile %s\n", state->filename);
            rc = ftx_send_file(state, state->filename,
                               state->compression, progress);
            if (rc == FTX_OK) {
                printf("  -> sent %lu bytes\n",
                       (unsigned long)state->stats.bytes_transferred);
            } else {
                printf("  -> getfile %s failed: %s\n",
                       state->filename, ftx_error_msg(rc));
            }
            if (rc == FTX_ERR_NOT_FOUND || rc == FTX_ERR_FILE) {
                /* Failed before any START went out: tell the PC so it doesn't
                 * sit through the metadata timeout. */
                state->pkt_buf[0] = FTX_CMD_ERROR;
                state->pkt_buf[1] = (uint8_t)rc;
                state->pkt_buf[2] = 0;
                ftx_send_packet(state, state->pkt_buf, 3);
            }
        } else {
            /* PC is sending us a file: receive it, overwriting. */
            printf("  <- putfile %s (%lu bytes)\n",
                   state->filename, (unsigned long)state->file_size);
            rc = ftx_recv_after_start(state, (const char *)0, 1, progress);
            if (rc == FTX_OK) {
                printf("  -> received %lu bytes\n",
                       (unsigned long)state->stats.bytes_transferred);
            } else {
                printf("  -> putfile %s failed: %s\n",
                       state->filename, ftx_error_msg(rc));
            }
        }
        /* Report the operation's own result, not a trailing idle-recv timeout
         * (e.g. ftx_send_file's ignored "final ACK" read). */
        state->last_error = rc;
        return FTX_SERVE_DONE;
    }

    if (cmd == FTX_CMD_LIST) {
        ftx_list_t *lreq = (ftx_list_t *)state->pkt_buf;
        if (lreq->path_len > 0) {
            printf("  <- listdir %.*s\n", (int)lreq->path_len, lreq->path);
        } else {
            printf("  <- listdir (current dir)\n");
        }
        state->stats.bytes_transferred = 0;   /* a listing moves no file bytes */
        state->last_error = ftx_list_respond(state);
        if (state->last_error == FTX_OK) {
            printf("  -> listing sent\n");
        } else {
            printf("  -> listdir failed: %s\n", ftx_error_msg(state->last_error));
        }
        return FTX_SERVE_DONE;
    }

    /* Unknown/blank request: ignore and keep serving. */
    printf("  <- (unknown request 0x%02X)\n", cmd);
    return FTX_SERVE_DONE;
}

int ftx_serve(ftx_state_t *state, ftx_progress_fn progress)
{
    int r;

    do {
        r = ftx_serve_one(state, progress);
    } while (r != FTX_SERVE_QUIT);

    return FTX_OK;
}
