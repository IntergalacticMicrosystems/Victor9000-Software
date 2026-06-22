/**
 * @file ftxtest.c
 * @brief Victor 9000 File Transfer Receive Test (COM1 / 38400)
 *
 * A focused, hardware-in-the-loop test of the putfile receive path: it listens
 * on COM1 (SER_PORT_A) at 38400 baud, polled, and serves file-transfer requests
 * with ftx_serve_one(). A "putfile" from the PC (the v9kfiletrx FileTransfer
 * client) is received straight to the current drive - run it from C:\ to drop
 * incoming files on the C: drive.
 *
 * This exercises the per-chunk credit flow control and the 4 KB write batching
 * in ftx_recv.c: the PC client requests FTX_FLAG_FLOWCTRL, this side credits
 * after each committed chunk, and chunks are flushed to disk in 4 KB blocks.
 *
 * Unlike the older test-command server, there is no FTX_CMD_TEST_* indirection
 * and no baud switching - it sits at 38400 and just serves. getfile/listdir/
 * query/quit all still work (ftx_serve_one handles them), so the same console
 * can pull a file back to verify a round trip.
 *
 *   Usage:  FTXTEST [/allowwrite]      (COM1, 38400, polled)
 *           /allowwrite permits destructive raw-sector writes (disk-image
 *           restore via the SECTOR protocol); off by default.
 *
 * Delivery: push FTXTEST.EXE to the Victor with the Pico remote control over
 * COM2/FTXSERV, boot/cd to C:, run FTXTEST, then drive a putfile from the PC
 * over ttyUSB0 (COM1). Press ESC on the Victor (or send QUIT) to stop.
 */

#include <stdio.h>
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
    int r;

    if (argc > 1) {
        printf("Usage: FTXTEST\n");
        return 1;
    }
    (void)argv;

    ser_init();

    g_config.port      = SER_PORT_A;        /* COM1 - ttyUSB0 on the PC side */
    g_config.baud      = SER_BAUD_38400;
    g_config.data_bits = SER_DATA_8;
    g_config.stop_bits = SER_STOP_1;
    g_config.parity    = SER_PARITY_NONE;
    g_config.flow_ctrl = SER_FLOW_NONE;     /* 3-wire link, no RTS/CTS */
    if (!ser_init_port(&g_config)) {
        printf("Failed to initialize COM1!\n");
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

    printf("\nVictor 9000 File Transfer Test (FTXTEST)\n");
    printf("Port: COM1 (A)   Baud: 38400   Mode: polled (3-wire)\n");
    printf("Receiving putfile to the current drive. getfile/listdir served.\n");
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
        /* ftx_serve_one prints each request received and its result. */
    }

    ftx_shutdown(&g_ftx);
    ser_shutdown();
    return 0;
}
