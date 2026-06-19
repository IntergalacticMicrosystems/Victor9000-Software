/**
 * @file ftxtest.c
 * @brief Victor 9000 File Transfer Test Server
 *
 * Automated test server that waits for commands from the PC test suite.
 * No user interaction required beyond starting the program.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>
#include "serial.h"
#include "packet.h"
#include "filetrx.h"
#include "ftx_protocol.h"
#include "ftx_compress.h"
#include "ftx_crc32.h"

/*===========================================================================
 * Configuration
 *===========================================================================*/

#define DEFAULT_PORT    SER_PORT_A
#ifndef DEFAULT_BAUD
#define DEFAULT_BAUD    SER_BAUD_9600
#endif

/*===========================================================================
 * Global State
 *===========================================================================*/

static ser_config_t g_config;
static pkt_state_t g_pkt;
static ftx_state_t g_ftx;
static uint8_t g_compression = FTX_COMP_NONE;
static int g_running = 1;

/* Receive buffer for packets */
static uint8_t g_recv_buf[FTX_MAX_PAYLOAD];

/* Last transfer result (for GET_RESULT command) */
static ftx_test_result_t g_last_result;
static int g_has_result = 0;

/* Baud rate names */
static const char *baud_names[] = {
    "110", "300", "600", "1200", "2400", "4800", "9600", "19200", "38400"
};

/*===========================================================================
 * Progress Callback (Silent - for automated testing)
 *===========================================================================*/

static uint8_t g_last_pct = 255;

static int test_progress_callback(ftx_state_t *state, const ftx_stats_t *stats)
{
    uint8_t pct;
    (void)stats;  /* Unused parameter */

    /* Check for ESC key to abort (emergency exit) */
    if (kbhit()) {
        if (getch() == 27) {  /* ESC */
            return 1;  /* Abort */
        }
    }

    /* Show periodic progress (every 10%) */
    pct = ftx_get_percent_complete(state);
    if (pct / 10 != g_last_pct / 10) {
        g_last_pct = pct;
        printf("  %u%%", pct);
    }

    return 0;  /* Continue */
}

/*===========================================================================
 * Test Protocol Helpers
 *===========================================================================*/

/**
 * Build a test result packet.
 */
static void build_test_result(ftx_test_result_t *result, uint8_t result_code,
                              uint8_t error_code, const ftx_stats_t *stats,
                              const char *message)
{
    memset(result, 0, sizeof(*result));
    result->cmd = FTX_CMD_TEST_RESULT;
    result->result_code = result_code;
    result->error_code = error_code;

    if (stats != NULL) {
        result->bytes_transferred = stats->bytes_transferred;
        result->elapsed_ticks = stats->elapsed_ticks;
        result->retries = stats->retries;
        result->errors = stats->errors;
    }

    if (message != NULL) {
        strncpy(result->message, message, sizeof(result->message) - 1);
    }
}

/**
 * Send command response (uses ACK protocol for reliable delivery).
 * Used for responses to commands like SET_COMPRESS, SET_BAUD, RECV_FILE ready.
 */
static void send_cmd_response(uint8_t result_code, const char *message)
{
    ftx_test_result_t result;
    build_test_result(&result, result_code, 0, NULL, message);
    pkt_send(&g_pkt, (uint8_t *)&result, sizeof(result));
}

/**
 * Store transfer result for later retrieval via GET_RESULT.
 * Does not send - PC must request result explicitly for reliable delivery.
 */
static void store_transfer_result(uint8_t result_code, uint8_t error_code,
                                  const ftx_stats_t *stats, const char *message)
{
    build_test_result(&g_last_result, result_code, error_code, stats, message);
    g_has_result = 1;
}

/**
 * Handle GET_RESULT command - send stored transfer result.
 */
static void handle_test_get_result(void)
{
    if (g_has_result) {
        pkt_send(&g_pkt, (uint8_t *)&g_last_result, sizeof(g_last_result));
    } else {
        send_cmd_response(FTX_TEST_FAIL, "No result available");
    }
}

/**
 * Send PONG response to PING.
 */
static void send_pong(void)
{
    uint8_t pong = FTX_CMD_TEST_PONG;
    pkt_send(&g_pkt, &pong, 1);
}

/*===========================================================================
 * Test Command Handlers
 *===========================================================================*/

/**
 * Handle SEND_FILE command - Victor sends file to PC.
 */
static void handle_test_send(const ftx_test_cmd_t *cmd)
{
    int result;
    const ftx_stats_t *stats;

    printf("CMD: Send file '%s'\n", cmd->filename);

    result = ftx_send_file(&g_ftx, cmd->filename, g_compression,
                          test_progress_callback);
    printf("\n");

    stats = ftx_get_stats(&g_ftx);

    if (result == FTX_OK) {
        printf("  OK: %lu bytes\n", stats->bytes_transferred);
        store_transfer_result(FTX_TEST_OK, 0, stats, "Send complete");
    } else {
        printf("  FAIL: %s\n", ftx_error_msg(result));
        store_transfer_result(FTX_TEST_FAIL, (uint8_t)(-result), stats,
                             ftx_error_msg(result));
    }
}

/**
 * Handle RECV_FILE command - Victor receives file from PC.
 */
static void handle_test_recv(const ftx_test_cmd_t *cmd)
{
    int result;
    const ftx_stats_t *stats;
    const char *filename;

    /* Use filename from command if provided, else use remote name */
    filename = (cmd->filename[0] != '\0') ? cmd->filename : NULL;

    printf("CMD: Receive file");
    if (filename) {
        printf(" as '%s'", filename);
    }
    printf("\n");

    /* First, acknowledge we're ready to receive */
    send_cmd_response(FTX_TEST_OK, "Ready");

    /* Now receive the file */
    result = ftx_receive_file(&g_ftx, filename, 1, test_progress_callback);
    printf("\n");

    stats = ftx_get_stats(&g_ftx);

    if (result == FTX_OK) {
        printf("  OK: %lu bytes as '%s'\n",
               stats->bytes_transferred, g_ftx.filename);
        store_transfer_result(FTX_TEST_OK, 0, stats, "Receive complete");
    } else {
        printf("  FAIL: %s\n", ftx_error_msg(result));
        store_transfer_result(FTX_TEST_FAIL, (uint8_t)(-result), stats,
                             ftx_error_msg(result));
    }
}

/**
 * Handle SET_COMPRESS command - set compression mode.
 */
static void handle_test_set_compress(const ftx_test_cmd_t *cmd)
{
    g_compression = cmd->param1;
    printf("CMD: Set compression = %s\n",
           g_compression == FTX_COMP_RLE ? "RLE" : "None");
    send_cmd_response(FTX_TEST_OK,
                      g_compression == FTX_COMP_RLE ? "RLE" : "None");
}

/**
 * Handle SET_BAUD command - set baud rate.
 */
static void handle_test_set_baud(const ftx_test_cmd_t *cmd)
{
    uint8_t baud_idx = cmd->param1;

    if (baud_idx >= SER_BAUD_COUNT) {
        printf("CMD: Invalid baud index %u\n", baud_idx);
        send_cmd_response(FTX_TEST_FAIL, "Invalid baud");
        return;
    }

    /* Acknowledge at the CURRENT baud first: send_cmd_response uses the
     * reliable packet layer and blocks until the PC ACKs, so the whole
     * request/response/ACK exchange completes at the old rate. Only then do
     * we switch the hardware - and the PC switches its port at the same point
     * after it has drained that ACK. Switching before responding would force
     * the response (and its ACK) to cross at the new rate, which is racy. */
    g_config.baud = baud_idx;
    printf("CMD: Set baud = %s\n", baud_names[baud_idx]);
    send_cmd_response(FTX_TEST_OK, baud_names[baud_idx]);
    ser_set_baud(g_config.port, baud_idx);
}

/**
 * Handle DELETE_FILE command - delete a file.
 */
static void handle_test_delete(const ftx_test_cmd_t *cmd)
{
    printf("CMD: Delete file '%s'\n", cmd->filename);

    if (remove(cmd->filename) == 0) {
        printf("  OK\n");
        send_cmd_response(FTX_TEST_OK, "Deleted");
    } else {
        printf("  FAIL\n");
        send_cmd_response(FTX_TEST_FILE_ERROR, "Delete failed");
    }
}

/**
 * Handle QUIT command - exit test server.
 */
static void handle_test_quit(void)
{
    printf("CMD: Quit\n");
    send_cmd_response(FTX_TEST_OK, "Goodbye");
    g_running = 0;
}

/*===========================================================================
 * Test Server Main Loop
 *===========================================================================*/

/**
 * Process a single test command.
 * @return 0 to continue, non-zero to exit
 */
static int process_test_command(void)
{
    int16_t len;
    ftx_test_cmd_t *cmd;

    /* Wait for packet with short timeout (allows ESC check) */
    pkt_set_timeout(&g_pkt, PKT_TIMEOUT_SHORT);
    len = pkt_receive(&g_pkt, g_recv_buf, sizeof(g_recv_buf));
    pkt_set_timeout(&g_pkt, PKT_TIMEOUT_LONG);

    if (len < 0) {
        /* Timeout or error - just continue loop */
        return 0;
    }

    if (len == 0) {
        return 0;
    }

    /* Check packet type */
    switch (g_recv_buf[0]) {
    case FTX_CMD_TEST_PING:
        send_pong();
        return 0;

    case FTX_CMD_TEST_CMD:
        /* Process test command */
        break;

    default:
        /* Unknown packet - ignore silently (may be stray from previous transfer) */
        return 0;
    }

    /* Process test command */
    cmd = (ftx_test_cmd_t *)g_recv_buf;

    switch (cmd->test_cmd) {
    case FTX_TEST_SEND_FILE:
        handle_test_send(cmd);
        break;

    case FTX_TEST_RECV_FILE:
        handle_test_recv(cmd);
        break;

    case FTX_TEST_SET_COMPRESS:
        handle_test_set_compress(cmd);
        break;

    case FTX_TEST_SET_BAUD:
        handle_test_set_baud(cmd);
        break;

    case FTX_TEST_DELETE_FILE:
        handle_test_delete(cmd);
        break;

    case FTX_TEST_GET_RESULT:
        handle_test_get_result();
        break;

    case FTX_TEST_QUIT:
        handle_test_quit();
        return 1;  /* Exit loop */

    default:
        printf("Unknown test command: 0x%02X\n", cmd->test_cmd);
        send_cmd_response(FTX_TEST_FAIL, "Unknown command");
        break;
    }

    return 0;
}

/**
 * Test server main loop.
 */
static void test_server_loop(void)
{
    printf("\n");
    printf("File Transfer Test Server\n");
    printf("=========================\n");
    printf("Port: %c  Baud: %s\n",
           g_config.port == SER_PORT_A ? 'A' : 'B',
           baud_names[g_config.baud]);
    printf("\n");
    printf("Waiting for commands from PC...\n");
    printf("(Press ESC to exit manually)\n");
    printf("\n");

    while (g_running) {
        /* Check for ESC key to manually exit */
        if (kbhit()) {
            if (getch() == 27) {
                printf("\nManual exit requested.\n");
                break;
            }
        }

        /* Process next command */
        if (process_test_command() != 0) {
            break;
        }
    }

    printf("Test server stopped.\n");
}

/*===========================================================================
 * Main
 *===========================================================================*/

int main(void)
{
    printf("Initializing...\n");

    /* Initialize serial library */
    printf("[1] ser_init...\n");
    ser_init();
    printf("[1] ser_init OK\n");

    /* Configure port */
    printf("[2] Configuring port A at 9600...\n");
    g_config.port = DEFAULT_PORT;
    g_config.baud = DEFAULT_BAUD;
    g_config.data_bits = SER_DATA_8;
    g_config.stop_bits = SER_STOP_1;
    g_config.parity = SER_PARITY_NONE;
    g_config.flow_ctrl = SER_FLOW_NONE;

    printf("[3] ser_init_port...\n");
    if (!ser_init_port(&g_config)) {
        printf("Failed to initialize serial port!\n");
        return 1;
    }
    printf("[3] ser_init_port OK\n");

    /* Initialize packet protocol */
    printf("[4] pkt_init...\n");
    pkt_init(&g_pkt, g_config.port);
    printf("[4] pkt_init OK\n");

    printf("[5] pkt_set_timeout...\n");
    pkt_set_timeout(&g_pkt, PKT_TIMEOUT_LONG);
    printf("[5] pkt_set_timeout OK\n");

    printf("[6] pkt_set_retries...\n");
    pkt_set_retries(&g_pkt, 5);
    printf("[6] pkt_set_retries OK\n");

    /* Initialize file transfer */
    printf("[7] ftx_init...\n");
    if (ftx_init(&g_ftx, &g_pkt) != FTX_OK) {
        printf("Failed to initialize file transfer!\n");
        ser_shutdown();
        return 1;
    }
    printf("[7] ftx_init OK\n");

    printf("[8] Starting test server loop...\n");
    /* Run test server loop */
    test_server_loop();

    /* Cleanup */
    printf("Shutting down...\n");
    ftx_shutdown(&g_ftx);
    ser_int_disable();
    ser_shutdown();

    return 0;
}
