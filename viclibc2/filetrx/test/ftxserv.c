/**
 * @file ftxserv.c
 * @brief Victor 9000 File Transfer Server (FTXSERV.EXE)
 *
 * Dedicated file server intended for the Pico remote-control bridge (and usable
 * by any filetrx PC client). Listens on COM2 (SER_PORT_B) and serves
 * putfile / getfile / listdir requests until the PC sends a QUIT command or the
 * operator presses ESC.
 *
 * Usage:  FTXSERV [baud]      baud = 9600, 19200, or 38400 (default)
 *
 * Runs the packet/file-transfer layer in POLLED mode (no serial interrupts):
 * a tight 8088 poll loop drains the uPD7201's 3-byte RX FIFO fast enough to
 * stay clean all the way to 38400 - 4x the interrupt path's reliable 9600
 * ceiling. The link is 3-wire (no RTS/CTS flow control), so the sender must
 * be paced so its bursts don't outrun the FIFO across a between-packet disk
 * write; a lost byte just draws a NAK/retry from the packet layer.
 *
 * The Pico (firmware/ftx_xfer.c) is the "PC side"; this is the Victor side.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include "serial.h"
#include "packet.h"
#include "filetrx.h"

static ser_config_t g_config;
static pkt_state_t  g_pkt;
static ftx_state_t  g_ftx;

/* App-owned file staging buffer (read-ahead on getfile, write-batching on
 * putfile). Bigger = fewer, larger disk ops on slow media; a multiple of the
 * 1024-byte chunk keeps writes block-aligned. Registered via
 * ftx_set_io_buffer. */
static uint8_t g_io_buf[16384];

int main(int argc, char **argv)
{
    uint8_t     baud = SER_BAUD_38400;
    const char *baud_str = "38400";
    int         i;
    int         r;

    /* Args (any order): a baud rate (9600|19200|38400). */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "38400") == 0) {
            baud = SER_BAUD_38400; baud_str = "38400";
        } else if (strcmp(argv[i], "19200") == 0) {
            baud = SER_BAUD_19200; baud_str = "19200";
        } else if (strcmp(argv[i], "9600") == 0) {
            baud = SER_BAUD_9600;  baud_str = "9600";
        } else {
            printf("Usage: FTXSERV [9600|19200|38400]\n");
            return 1;
        }
    }

    ser_init();

    g_config.port      = SER_PORT_B;        /* COM2 */
    g_config.baud      = baud;
    g_config.data_bits = SER_DATA_8;
    g_config.stop_bits = SER_STOP_1;
    g_config.parity    = SER_PARITY_NONE;
    /* 3-wire link: no hardware flow control. */
    g_config.flow_ctrl = SER_FLOW_NONE;
    if (!ser_init_port(&g_config)) {
        printf("Failed to initialize COM2!\n");
        return 1;
    }

    pkt_init_polled(&g_pkt, g_config.port);  /* polled byte I/O, no ISR */
    pkt_set_timeout(&g_pkt, PKT_TIMEOUT_LONG);
    pkt_set_retries(&g_pkt, 5);

    if (ftx_init(&g_ftx, &g_pkt) != FTX_OK) {
        printf("Failed to initialize file transfer!\n");
        ser_shutdown();
        return 1;
    }
    ftx_set_io_buffer(&g_ftx, g_io_buf, sizeof(g_io_buf));

    printf("\nVictor 9000 File Server (FTXSERV)\n");
    printf("Port: COM2 (B)   Baud: %s   Mode: polled (3-wire)\n", baud_str);
    printf("Serving putfile / getfile / listdir.\n");
    printf("Press ESC to exit (or the PC may send QUIT).\n\n");

    for (;;) {
        if (kbhit() && getch() == 27) {
            printf("ESC - exiting.\n");
            break;
        }

        r = ftx_serve_one(&g_ftx, (ftx_progress_fn)0);
        if (r == FTX_SERVE_QUIT) {
            printf("QUIT received - exiting.\n");
            break;
        }
        /* ftx_serve_one prints each command received and its result. */
    }

    ftx_shutdown(&g_ftx);
    ser_shutdown();     /* polled mode installed no ISR to disable */
    return 0;
}
