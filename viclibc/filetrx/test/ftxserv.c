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
 * ceiling. Inbound flow control for putfile is handled inside the packet
 * layer (it deasserts RTS around each between-packet disk write so the Pico,
 * honoring CTS, pauses); no ISR ring buffer or watermark throttle is used.
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

int main(int argc, char **argv)
{
    uint8_t     baud = SER_BAUD_38400;
    const char *baud_str = "38400";
    int         r;

    if (argc > 1) {
        if (strcmp(argv[1], "38400") == 0) {
            baud = SER_BAUD_38400; baud_str = "38400";
        } else if (strcmp(argv[1], "19200") == 0) {
            baud = SER_BAUD_19200; baud_str = "19200";
        } else if (strcmp(argv[1], "9600") == 0) {
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
    /* Leave TX-side CTS gating OFF (SER_FLOW_NONE): our replies must never be
     * blocked waiting on a CTS we don't strictly need. Inbound flow control
     * for putfile is the RX-side RTS throttle enabled below. */
    g_config.flow_ctrl = SER_FLOW_NONE;
    if (!ser_init_port(&g_config)) {
        printf("Failed to initialize COM2!\n");
        return 1;
    }

    pkt_init_polled(&g_pkt, g_config.port);  /* polled byte I/O, no ISR */
    pkt_set_timeout(&g_pkt, PKT_TIMEOUT_LONG);
    pkt_set_retries(&g_pkt, 5);

    /* No ISR ring buffer / watermark throttle in polled mode: the packet
     * layer gates the sender with RTS around each between-packet disk write
     * (see pkt_init_polled), which is what keeps the 8088 from overrunning the
     * 3-byte FIFO during putfile at high baud. */

    if (ftx_init(&g_ftx, &g_pkt) != FTX_OK) {
        printf("Failed to initialize file transfer!\n");
        ser_shutdown();
        return 1;
    }

    printf("\nVictor 9000 File Server (FTXSERV)\n");
    printf("Port: COM2 (B)   Baud: %s   Mode: polled (RTS-gated)\n", baud_str);
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
