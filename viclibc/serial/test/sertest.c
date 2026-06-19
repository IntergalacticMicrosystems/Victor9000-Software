/**
 * @file sertest.c
 * @brief Victor 9000 Serial Library Automated Test Agent
 *
 * Command-driven test agent for the serial library. The host side
 * (victor9k_serial_test.py) drives this agent over the serial line and
 * asserts on the results; together they exercise every public function
 * in serial.h and packet.h.
 *
 * Protocol: each command is a packet-protocol DATA packet whose payload
 * is [opcode][args...]. The agent answers with one (or for two-phase
 * commands, two) DATA packets of the form [opcode][status][data...].
 * Status 0 = OK, 1 = operation incomplete (e.g. short read), 2 = bad args.
 *
 * The opcode list and all argument/response layouts MUST be kept in
 * sync with victor9k_serial_test.py.
 *
 * Usage on the Victor 9000:
 *   sertest [baud]      (default 9600; host must use the same rate)
 */

#include "serial.h"
#include "packet.h"
#include <dos.h>
#include <string.h>

/*===========================================================================
 * Test Configuration
 *===========================================================================*/

#define TEST_PORT       SER_PORT_A
#define DEFAULT_BAUD    SER_BAUD_9600

#define AGENT_VER_MAJOR 2
#define AGENT_VER_MINOR 5

/* Hardware access for the CMD_HWSTATE diagnostic (resolved from
 * serial.lib; intentionally not part of the public serial.h API) */
extern uint16_t ser_hw_disable_int(void);
extern void ser_hw_restore_int(uint16_t flags);
extern uint8_t ser_hw_get_pic_mask(void);
extern uint8_t ser_hw_read_pic_reg(uint8_t which);
extern uint8_t ser_hw_read_status(uint8_t port);

/* Runtime baud rate (set from command line or default) */
static uint8_t g_baud = DEFAULT_BAUD;

/*===========================================================================
 * Command Opcodes (keep in sync with victor9k_serial_test.py)
 *===========================================================================*/

#define CMD_PING         0x01   /* -> [ver_hi][ver_lo][cmds u16][pings u16] */
#define CMD_ECHO         0x02   /* blob -> blob */
#define CMD_CRC          0x03   /* blob -> [crc u16][crc_bytewise u16] */
#define CMD_STATUS       0x04   /* -> [baud][err][cts][dcd][txr][rxr][rxav u16][txfree u16] */
#define CMD_RAW_ECHO     0x05   /* [n u16]; host then sends n raw bytes, agent echoes each */
#define CMD_WRITE_STR    0x06   /* string; agent ser_write_str()s it raw -> [count u16] */
#define CMD_WRITE_BLOCK  0x07   /* [n u16][seed]; raw pattern out -> [written u16][free_during u16][free_after u16] */
#define CMD_READ_BLOCK   0x08   /* [n u16]; host sends n raw -> [total u16][data...] */
#define CMD_RX_FLUSH     0x09   /* [n u16][mode]; host sends n junk; mode 0=ser_int_flush_rx 1=pkt_flush -> [before u16][after u16] */
#define CMD_OVERFLOW     0x0A   /* host floods >SER_BUF_SIZE junk -> [max_avail u16][err][err_after_clear] */
#define CMD_FLUSH_TX     0x0B   /* [n u16][seed]; queue then flush_tx -> [queued u16] */
#define CMD_FLOW_XONOFF  0x0C   /* two-phase; see dispatch -> [queued u16][free_during u16][rxav u16] */
#define CMD_FLOW_SMOKE   0x0D   /* [mode] -> [ret][cts] */
#define CMD_BAUD_BAD     0x0E   /* [idx] -> [ret][baud] */
#define CMD_BAUD_CYCLE   0x0F   /* [idx]; two-phase -> [r1][during][r2][after][err] */
#define CMD_FORMAT_BAD   0x10   /* [data][stop][parity] -> [ret] */
#define CMD_FORMAT_CYCLE 0x11   /* [data][stop][parity]; two-phase -> [r1][r2][err] */
#define CMD_INIT_PORT    0x12   /* [port][baud][data][stop][parity][flow] -> [ret] */
#define CMD_LINES        0x13   /* -> [cts0][dcd0][cts1][dcd1][txr][rxr] */
#define CMD_RX_READY     0x14   /* host sends 1 raw byte during ISR-off window -> [observed][txr] */
#define CMD_PKT_PARAMS   0x15   /* [timeout u16][retries] -> [timeout u16][retries] */
#define CMD_STATS        0x16   /* -> [cmds u16][pings u16][last_err_rc i16][pkt_err]; clears err state */
#define CMD_HWSTATE      0x17   /* host sends 1 raw byte right after; agent waits
                                   WITHOUT polling, then snapshots interrupt
                                   hardware -> [if][imr][irr][isr][rr0a][rr0b]
                                   [rxavail u16][kicks u16] */
#define CMD_QUIT         0x7F   /* -> [ok]; then ser_shutdown() and exit */

/* Pause units: same order of cost per iteration as the packet layer's
 * polling loops, so PKT_TIMEOUT_LONG (50000 ~ 1s at 5MHz) calibrates these */
#define PAUSE_100MS     5000UL
#define PAUSE_1S        50000UL

/*===========================================================================
 * Globals
 *===========================================================================*/

static pkt_state_t g_pkt;
static uint8_t g_cmd[PKT_MAX_PAYLOAD];
static uint8_t g_rsp[PKT_MAX_PAYLOAD];
static uint8_t g_work[PKT_MAX_PAYLOAD];
static uint16_t g_rsp_len;

static struct {
    uint16_t cmd_count;     /* DATA command packets delivered */
    uint16_t ping_count;    /* CMD_PING dispatches */
    int16_t  last_err_rc;   /* last pkt_receive error other than idle timeout */
} g_stats;

/*===========================================================================
 * DOS Screen Output (status display only; serial carries the protocol)
 *===========================================================================*/

static void scr_putc(char ch) {
    union REGS regs;
    regs.h.ah = 0x02;   /* DOS function: write character */
    regs.h.dl = ch;
    int86(0x21, &regs, &regs);
}

static void scr_puts(const char *str) {
    while (*str) {
        scr_putc(*str++);
        /* Screen output via DOS is slow; with an unhealthy interrupt
         * chain, RX bytes arriving meanwhile would overrun the SIO's
         * 3-byte FIFO unless we service it by hand. */
        ser_int_kick();
    }
}

static void scr_print(const char *str) {
    scr_puts(str);
    scr_putc('\r');
    scr_putc('\n');
}

static void scr_hex_byte(uint8_t val) {
    static const char hex_chars[] = "0123456789ABCDEF";
    scr_putc(hex_chars[(val >> 4) & 0x0F]);
    scr_putc(hex_chars[val & 0x0F]);
}

/*===========================================================================
 * Helpers
 *===========================================================================*/

/**
 * Busy pause. Polls the RX count so each iteration costs about as much
 * as one packet-layer timeout iteration (PAUSE_1S ~ 1 second).
 */
static void agent_pause(uint32_t units) {
    while (units--) {
        (void)ser_int_rx_available(TEST_PORT);
        if ((units & 0x03) == 0) {
            ser_int_kick();
        }
    }
}

/**
 * Poll for one received byte, up to `loops` iterations.
 * @return Byte value, or -1 on timeout
 */
static int16_t read_byte_wait(uint32_t loops) {
    int16_t data;

    while (loops--) {
        data = ser_int_read(TEST_PORT);
        if (data >= 0) {
            return data;
        }
        ser_int_kick();
    }

    return -1;
}

/**
 * Wait until at least n bytes are pending in the RX buffer.
 * @return SER_TRUE if reached, SER_FALSE on timeout
 */
static ser_bool_t wait_rx_count(uint16_t n, uint32_t loops) {
    while (loops--) {
        if (ser_int_rx_available(TEST_PORT) >= n) {
            return SER_TRUE;
        }
        ser_int_kick();
    }

    return SER_FALSE;
}

/**
 * Deterministic test pattern shared with the host: printable ASCII
 * 0x20..0x7C, which avoids SYNC/ESC/XON/XOFF so raw pattern data can
 * never be mistaken for framing or flow control bytes.
 */
static uint8_t pat_byte(uint8_t seed, uint16_t i) {
    return (uint8_t)(0x20 + ((uint16_t)(seed + i) % 0x5D));
}

/* Response builder */
static void rsp_start(uint8_t op, uint8_t status) {
    g_rsp[0] = op;
    g_rsp[1] = status;
    g_rsp_len = 2;
}

static void rsp_add8(uint8_t v) {
    g_rsp[g_rsp_len++] = v;
}

static void rsp_add16(uint16_t v) {
    g_rsp[g_rsp_len++] = (uint8_t)(v & 0xFF);
    g_rsp[g_rsp_len++] = (uint8_t)(v >> 8);
}

static void rsp_add_block(const uint8_t *data, uint16_t len) {
    memcpy(&g_rsp[g_rsp_len], data, len);
    g_rsp_len += len;
}

/**
 * Send the built response. pkt_send retries on its own; if the host is
 * truly gone there is nothing useful to do, so the result is screen-only.
 */
static void rsp_send(void) {
    int16_t rc = pkt_send(&g_pkt, g_rsp, g_rsp_len);
    if (rc != PKT_TRUE) {
        scr_puts(" rsp fail rc=");
        scr_hex_byte((uint8_t)(-rc));
        scr_print("");
    }
}

static uint16_t arg_u16(const uint8_t *args) {
    return (uint16_t)args[0] | ((uint16_t)args[1] << 8);
}

/*===========================================================================
 * Command Handlers
 *===========================================================================*/

/**
 * PING - liveness, version, counters.
 */
static void cmd_ping(void) {
    g_stats.ping_count++;
    rsp_start(CMD_PING, 0);
    rsp_add8(AGENT_VER_MAJOR);
    rsp_add8(AGENT_VER_MINOR);
    rsp_add16(g_stats.cmd_count);
    rsp_add16(g_stats.ping_count);
    rsp_send();
}

/**
 * ECHO - send the payload straight back (packet round trip incl. escaping).
 */
static void cmd_echo(const uint8_t *args, uint16_t len) {
    if (len > PKT_MAX_PAYLOAD - 2) {    /* response adds 2 header bytes */
        rsp_start(CMD_ECHO, 2);
        rsp_send();
        return;
    }

    rsp_start(CMD_ECHO, 0);
    rsp_add_block(args, len);
    rsp_send();
}

/**
 * CRC - pkt_crc16() over the blob, plus the same CRC built byte-by-byte
 * with pkt_crc16_byte(). Host checks both against its own table.
 */
static void cmd_crc(const uint8_t *args, uint16_t len) {
    uint16_t crc_block;
    uint16_t crc_bytes;
    uint16_t i;

    crc_block = pkt_crc16(args, len);

    crc_bytes = 0xFFFF;
    for (i = 0; i < len; i++) {
        crc_bytes = pkt_crc16_byte(crc_bytes, args[i]);
    }

    rsp_start(CMD_CRC, 0);
    rsp_add16(crc_block);
    rsp_add16(crc_bytes);
    rsp_send();
}

/**
 * STATUS - snapshot of configuration and status functions.
 */
static void cmd_status(void) {
    rsp_start(CMD_STATUS, 0);
    rsp_add8(ser_get_baud(TEST_PORT));
    rsp_add8(ser_get_error(TEST_PORT));
    rsp_add8(ser_check_cts(TEST_PORT) ? 1 : 0);
    rsp_add8(ser_check_dcd(TEST_PORT) ? 1 : 0);
    rsp_add8(ser_tx_ready(TEST_PORT) ? 1 : 0);
    rsp_add8(ser_rx_ready(TEST_PORT) ? 1 : 0);
    rsp_add16(ser_int_rx_available(TEST_PORT));
    rsp_add16(ser_int_tx_free(TEST_PORT));
    rsp_send();
}

/**
 * RAW_ECHO - per-byte ser_int_read()/ser_int_write() echo of n raw bytes
 * that the host sends after the command.
 */
static void cmd_raw_echo(const uint8_t *args, uint16_t len) {
    uint16_t n;
    uint16_t count = 0;
    int16_t data;

    if (len < 2) {
        rsp_start(CMD_RAW_ECHO, 2);
        rsp_send();
        return;
    }

    n = arg_u16(args);
    if (n > PKT_MAX_PAYLOAD) {
        n = PKT_MAX_PAYLOAD;
    }

    while (count < n) {
        data = read_byte_wait(PAUSE_1S * 3);
        if (data < 0) {
            break;
        }
        while (!ser_int_write(TEST_PORT, (uint8_t)data)) { }
        count++;
    }

    ser_int_drain_tx(TEST_PORT);

    rsp_start(CMD_RAW_ECHO, (count == n) ? 0 : 1);
    rsp_add16(count);
    rsp_send();
}

/**
 * WRITE_STR - send the argument string raw via ser_write_str().
 */
static void cmd_write_str(const uint8_t *args, uint16_t len) {
    uint16_t count;

    if (len > 512) {
        rsp_start(CMD_WRITE_STR, 2);
        rsp_send();
        return;
    }

    memcpy(g_work, args, len);
    g_work[len] = 0;

    count = ser_write_str(TEST_PORT, (const char *)g_work);
    ser_int_drain_tx(TEST_PORT);

    rsp_start(CMD_WRITE_STR, 0);
    rsp_add16(count);
    rsp_send();
}

/**
 * WRITE_BLOCK - queue n pattern bytes with ser_int_write_block(), sample
 * ser_int_tx_free() while data is still draining and again after
 * ser_int_drain_tx().
 */
static void cmd_write_block(const uint8_t *args, uint16_t len) {
    uint16_t n, i, written, free_during, free_after;
    uint8_t seed;
    uint32_t guard;

    if (len < 3) {
        rsp_start(CMD_WRITE_BLOCK, 2);
        rsp_send();
        return;
    }

    n = arg_u16(args);
    seed = args[2];
    if (n > PKT_MAX_PAYLOAD) {
        n = PKT_MAX_PAYLOAD;
    }

    for (i = 0; i < n; i++) {
        g_work[i] = pat_byte(seed, i);
    }

    written = ser_int_write_block(TEST_PORT, g_work, n);
    free_during = ser_int_tx_free(TEST_PORT);

    guard = PAUSE_1S * 5;
    while (written < n && guard--) {
        written += ser_int_write_block(TEST_PORT, g_work + written,
                                       n - written);
    }

    ser_int_drain_tx(TEST_PORT);
    free_after = ser_int_tx_free(TEST_PORT);

    rsp_start(CMD_WRITE_BLOCK, (written == n) ? 0 : 1);
    rsp_add16(written);
    rsp_add16(free_during);
    rsp_add16(free_after);
    rsp_send();
}

/**
 * READ_BLOCK - collect n raw bytes via ser_int_read_block() and return
 * them in the response payload.
 */
static void cmd_read_block(const uint8_t *args, uint16_t len) {
    uint16_t n;
    uint16_t total = 0;
    uint32_t guard;

    if (len < 2) {
        rsp_start(CMD_READ_BLOCK, 2);
        rsp_send();
        return;
    }

    n = arg_u16(args);
    if (n > 512) {
        n = 512;
    }

    guard = PAUSE_1S * 5;
    while (total < n && guard--) {
        total += ser_int_read_block(TEST_PORT, g_work + total, n - total);
    }

    rsp_start(CMD_READ_BLOCK, (total == n) ? 0 : 1);
    rsp_add16(total);
    rsp_add_block(g_work, total);
    rsp_send();
}

/**
 * RX_FLUSH - host sends n junk bytes after the command; wait for them,
 * then flush via ser_int_flush_rx() (mode 0) or pkt_flush() (mode 1)
 * and report the count before/after.
 */
static void cmd_rx_flush(const uint8_t *args, uint16_t len) {
    uint16_t n, before, after;
    uint8_t mode;
    ser_bool_t reached;

    if (len < 3) {
        rsp_start(CMD_RX_FLUSH, 2);
        rsp_send();
        return;
    }

    n = arg_u16(args);
    mode = args[2];

    reached = wait_rx_count(n, PAUSE_1S * 8);
    before = ser_int_rx_available(TEST_PORT);

    if (mode == 1) {
        pkt_flush(&g_pkt);
    } else {
        ser_int_flush_rx(TEST_PORT);
    }

    after = ser_int_rx_available(TEST_PORT);

    rsp_start(CMD_RX_FLUSH, reached ? 0 : 1);
    rsp_add16(before);
    rsp_add16(after);
    rsp_send();
}

/**
 * OVERFLOW - host floods more than SER_BUF_SIZE junk bytes. Wait for the
 * RX buffer to fill, confirm SER_ERR_BUFOVFL via ser_get_error() (which
 * also clears it), flush, and confirm a second read reports no error.
 */
static void cmd_overflow(void) {
    uint16_t max_avail;
    uint8_t err, err_after;
    uint16_t waits;

    /* Wait for the buffer to fill (host floods at line rate) */
    waits = 200;    /* x 100ms = 20s cap */
    while (waits-- && ser_int_rx_available(TEST_PORT) < SER_BUF_SIZE) {
        agent_pause(PAUSE_100MS);
    }

    /* Let the overflow tail arrive and set the error flag */
    agent_pause(PAUSE_1S);

    max_avail = ser_int_rx_available(TEST_PORT);
    err = ser_get_error(TEST_PORT);

    ser_int_flush_rx(TEST_PORT);
    agent_pause(PAUSE_100MS * 3);   /* stragglers from the line */
    ser_int_flush_rx(TEST_PORT);

    err_after = ser_get_error(TEST_PORT);

    rsp_start(CMD_OVERFLOW, 0);
    rsp_add16(max_avail);
    rsp_add8(err);
    rsp_add8(err_after);
    rsp_send();
}

/**
 * FLUSH_TX - queue n pattern bytes, immediately flush the TX buffer.
 * Only the byte(s) already handed to the transmitter escape; the host
 * counts them.
 */
static void cmd_flush_tx(const uint8_t *args, uint16_t len) {
    uint16_t n, i, queued;
    uint8_t seed;

    if (len < 3) {
        rsp_start(CMD_FLUSH_TX, 2);
        rsp_send();
        return;
    }

    n = arg_u16(args);
    seed = args[2];
    if (n > PKT_MAX_PAYLOAD) {
        n = PKT_MAX_PAYLOAD;
    }

    for (i = 0; i < n; i++) {
        g_work[i] = pat_byte(seed, i);
    }

    queued = ser_int_write_block(TEST_PORT, g_work, n);
    ser_int_flush_tx(TEST_PORT);
    ser_int_drain_tx(TEST_PORT);

    rsp_start(CMD_FLUSH_TX, 0);
    rsp_add16(queued);
    rsp_send();
}

/**
 * FLOW_XONOFF - two-phase XON/XOFF suspension test, event-driven.
 *
 * Phase 1: enable SER_FLOW_XONOFF and answer "armed". The host then
 * sends a raw XOFF. Phase 2 (here): poll ser_tx_suspended() until the
 * ISR has consumed that XOFF (no wall-clock choreography - busy-wait
 * "seconds" stretch on a real 8088 and the host cannot know our
 * pace), then queue 256 pattern bytes (which must NOT go out), sample
 * tx_free and rx_available, and block in drain_tx until the host
 * sends XON.
 */
static void cmd_flow_xonoff(void) {
    uint16_t queued = 0;
    uint16_t free_during = 0;
    uint16_t rxav = 0;
    uint8_t observed = 0;
    uint32_t budget;
    uint16_t i;

    ser_set_flow(TEST_PORT, SER_FLOW_XONOFF);

    rsp_start(CMD_FLOW_XONOFF, 0);
    rsp_send();                     /* "armed" - host sends XOFF on receipt */

    /* Wait for the suspension event itself */
    budget = PAUSE_1S * 2;
    while (budget--) {
        ser_int_kick();
        if (ser_tx_suspended(TEST_PORT)) {
            observed = 1;
            break;
        }
    }

    if (observed) {
        for (i = 0; i < 256; i++) {
            g_work[i] = pat_byte(7, i);
        }
        queued = ser_int_write_block(TEST_PORT, g_work, 256);

        /* Still suspended: these samples show the data being held */
        free_during = ser_int_tx_free(TEST_PORT);
        rxav = ser_int_rx_available(TEST_PORT);

        ser_int_drain_tx(TEST_PORT);    /* blocks until host sends XON */
    }

    ser_set_flow(TEST_PORT, SER_FLOW_NONE);
    ser_int_flush_rx(TEST_PORT);
    (void)ser_get_error(TEST_PORT);

    rsp_start(CMD_FLOW_XONOFF, observed ? 0 : 1);
    rsp_add8(observed);
    rsp_add16(queued);
    rsp_add16(free_during);
    rsp_add16(rxav);
    rsp_send();
}

/**
 * FLOW_SMOKE - set a flow mode, sample CTS, restore SER_FLOW_NONE.
 * (RTS/CTS cannot be functionally tested over a MAME PTY.)
 */
static void cmd_flow_smoke(const uint8_t *args, uint16_t len) {
    uint8_t ret, cts;

    if (len < 1) {
        rsp_start(CMD_FLOW_SMOKE, 2);
        rsp_send();
        return;
    }

    ret = ser_set_flow(TEST_PORT, args[0]) ? 1 : 0;
    cts = ser_check_cts(TEST_PORT) ? 1 : 0;
    ser_set_flow(TEST_PORT, SER_FLOW_NONE);

    rsp_start(CMD_FLOW_SMOKE, 0);
    rsp_add8(ret);
    rsp_add8(cts);
    rsp_send();
}

/**
 * BAUD_BAD - ser_set_baud() with the given (invalid) index; report the
 * return value and that ser_get_baud() is unchanged.
 */
static void cmd_baud_bad(const uint8_t *args, uint16_t len) {
    uint8_t ret;

    if (len < 1) {
        rsp_start(CMD_BAUD_BAD, 2);
        rsp_send();
        return;
    }

    ret = ser_set_baud(TEST_PORT, args[0]) ? 1 : 0;

    rsp_start(CMD_BAUD_BAD, 0);
    rsp_add8(ret);
    rsp_add8(ser_get_baud(TEST_PORT));
    rsp_send();
}

/**
 * BAUD_CYCLE - two-phase: acknowledge at the current rate, switch to the
 * requested rate for a while (the line garbles against a fixed-rate
 * peer, like real hardware), switch back, flush the garbage, report.
 */
static void cmd_baud_cycle(const uint8_t *args, uint16_t len) {
    uint8_t old, r1, r2, during, err;

    if (len < 1 || args[0] >= SER_BAUD_COUNT) {
        rsp_start(CMD_BAUD_CYCLE, 2);
        rsp_send();
        return;
    }

    old = ser_get_baud(TEST_PORT);

    rsp_start(CMD_BAUD_CYCLE, 0);
    rsp_send();                     /* phase 1: about to switch */

    agent_pause(PAUSE_100MS * 3);

    r1 = ser_set_baud(TEST_PORT, args[0]) ? 1 : 0;
    during = ser_get_baud(TEST_PORT);

    agent_pause(PAUSE_1S * 2);

    r2 = ser_set_baud(TEST_PORT, old) ? 1 : 0;

    agent_pause(PAUSE_100MS * 3);
    err = ser_get_error(TEST_PORT);
    pkt_flush(&g_pkt);

    rsp_start(CMD_BAUD_CYCLE, 0);
    rsp_add8(r1);
    rsp_add8(during);
    rsp_add8(r2);
    rsp_add8(ser_get_baud(TEST_PORT));
    rsp_add8(err);
    rsp_send();
}

/**
 * FORMAT_BAD - ser_set_format() with invalid parameters; report result.
 */
static void cmd_format_bad(const uint8_t *args, uint16_t len) {
    uint8_t ret;

    if (len < 3) {
        rsp_start(CMD_FORMAT_BAD, 2);
        rsp_send();
        return;
    }

    ret = ser_set_format(TEST_PORT, args[0], args[1], args[2]) ? 1 : 0;

    rsp_start(CMD_FORMAT_BAD, 0);
    rsp_add8(ret);
    rsp_send();
}

/**
 * FORMAT_CYCLE - two-phase: acknowledge at 8N1, apply the requested
 * format for a while, restore 8N1, flush, report.
 */
static void cmd_format_cycle(const uint8_t *args, uint16_t len) {
    uint8_t r1, r2, err;

    if (len < 3) {
        rsp_start(CMD_FORMAT_CYCLE, 2);
        rsp_send();
        return;
    }

    rsp_start(CMD_FORMAT_CYCLE, 0);
    rsp_send();                     /* phase 1: about to switch */

    agent_pause(PAUSE_100MS * 3);

    r1 = ser_set_format(TEST_PORT, args[0], args[1], args[2]) ? 1 : 0;

    agent_pause(PAUSE_1S * 2);

    r2 = ser_set_format(TEST_PORT, SER_DATA_8, SER_STOP_1,
                        SER_PARITY_NONE) ? 1 : 0;

    agent_pause(PAUSE_100MS * 3);
    err = ser_get_error(TEST_PORT);
    pkt_flush(&g_pkt);

    rsp_start(CMD_FORMAT_CYCLE, 0);
    rsp_add8(r1);
    rsp_add8(r2);
    rsp_add8(err);
    rsp_send();
}

/**
 * INIT_PORT - ser_init_port() with the given config. A successful init
 * leaves the channel's SIO interrupts disabled, so cycle
 * ser_int_disable()/ser_int_enable() to bring interrupt mode back up
 * (which also exercises that pair).
 */
static void cmd_init_port(const uint8_t *args, uint16_t len) {
    ser_config_t cfg;
    uint8_t ret;

    if (len < 6) {
        rsp_start(CMD_INIT_PORT, 2);
        rsp_send();
        return;
    }

    cfg.port = args[0];
    cfg.baud = args[1];
    cfg.data_bits = args[2];
    cfg.stop_bits = args[3];
    cfg.parity = args[4];
    cfg.flow_ctrl = args[5];

    ret = ser_init_port(&cfg) ? 1 : 0;

    if (ret) {
        ser_int_disable();
        ser_int_enable();
    }

    rsp_start(CMD_INIT_PORT, 0);
    rsp_add8(ret);
    rsp_send();
}

/**
 * LINES - toggle RTS/DTR low then high, sampling CTS/DCD around the
 * toggle, plus ser_tx_ready()/ser_rx_ready() in the idle state.
 */
static void cmd_lines(void) {
    uint8_t cts0, dcd0, cts1, dcd1, txr, rxr;

    cts0 = ser_check_cts(TEST_PORT) ? 1 : 0;
    dcd0 = ser_check_dcd(TEST_PORT) ? 1 : 0;

    ser_set_rts(TEST_PORT, SER_FALSE);
    ser_set_dtr(TEST_PORT, SER_FALSE);
    agent_pause(PAUSE_100MS);

    cts1 = ser_check_cts(TEST_PORT) ? 1 : 0;
    dcd1 = ser_check_dcd(TEST_PORT) ? 1 : 0;

    ser_set_rts(TEST_PORT, SER_TRUE);
    ser_set_dtr(TEST_PORT, SER_TRUE);
    agent_pause(PAUSE_100MS);

    txr = ser_tx_ready(TEST_PORT) ? 1 : 0;
    rxr = ser_rx_ready(TEST_PORT) ? 1 : 0;

    rsp_start(CMD_LINES, 0);
    rsp_add8(cts0);
    rsp_add8(dcd0);
    rsp_add8(cts1);
    rsp_add8(dcd1);
    rsp_add8(txr);
    rsp_add8(rxr);
    rsp_send();
}

/**
 * RX_READY - observe ser_rx_ready() going true. With the ISR installed
 * the receive interrupt drains the SIO immediately, so the hardware bit
 * is only observable with interrupt mode off: disable it, let the host
 * send one raw byte, poll the bit, then bring interrupt mode back.
 */
static void cmd_rx_ready(void) {
    uint8_t observed = 0;
    uint8_t txr;
    uint32_t budget;

    ser_int_disable();

    budget = PAUSE_1S * 4;
    while (budget--) {
        if (ser_rx_ready(TEST_PORT)) {
            observed = 1;
            break;
        }
    }

    txr = ser_tx_ready(TEST_PORT) ? 1 : 0;

    ser_int_enable();
    agent_pause(PAUSE_100MS);
    ser_int_flush_rx(TEST_PORT);
    (void)ser_get_error(TEST_PORT);

    rsp_start(CMD_RX_READY, 0);
    rsp_add8(observed);
    rsp_add8(txr);
    rsp_send();
}

/**
 * PKT_PARAMS - set packet timeout/retries, echo the state back (the
 * response itself is sent with the new parameters), restore defaults.
 */
static void cmd_pkt_params(const uint8_t *args, uint16_t len) {
    if (len < 3) {
        rsp_start(CMD_PKT_PARAMS, 2);
        rsp_send();
        return;
    }

    pkt_set_timeout(&g_pkt, arg_u16(args));
    pkt_set_retries(&g_pkt, args[2]);

    rsp_start(CMD_PKT_PARAMS, 0);
    rsp_add16(g_pkt.timeout);
    rsp_add8(g_pkt.retries);
    rsp_send();

    pkt_set_timeout(&g_pkt, PKT_TIMEOUT_LONG);
    pkt_set_retries(&g_pkt, PKT_MAX_RETRIES);
}

/**
 * HWSTATE - interrupt-chain diagnostic.
 *
 * The host sends one raw byte right after this command. We wait
 * WITHOUT polling so that byte sits in the SIO FIFO with its receive
 * interrupt asserted (unless a working ISR consumes it), then we
 * snapshot the whole interrupt path in one go:
 *
 *   IF      CPU interrupt flag (0 = interrupts globally off!)
 *   IMR     8259A mask (bit 1 = SIO masked)
 *   IRR     8259A request register (bit 1 = SIO INT reaches the PIC)
 *   ISR     8259A in-service (bit 1 stuck = EOI never delivered)
 *   RR0A/B  SIO status (bit 0 = RX byte still pending, bit 1 = int
 *           pending on the chip)
 *   rxavail library RX buffer count (byte already consumed by a
 *           working ISR shows up here instead)
 *
 * The combination pinpoints where the interrupt chain is broken.
 */
static void cmd_hwstate(void) {
    volatile uint16_t spin;
    uint32_t n;
    uint16_t flags;
    uint8_t if_bit, imr, irr, isr, rr0a, rr0b;
    uint16_t rxavail;

    /* Plain delay, deliberately NO kicks: let the host's byte land in
     * the SIO and stay there unless real interrupts take it */
    spin = 0;
    for (n = 0; n < 400000UL; n++) {
        spin++;
    }

    flags = ser_hw_disable_int();

    if_bit = (uint8_t)((flags >> 9) & 1);
    imr = ser_hw_get_pic_mask();
    irr = ser_hw_read_pic_reg(0x0A);
    isr = ser_hw_read_pic_reg(0x0B);
    rr0a = ser_hw_read_status(SER_PORT_A);
    rr0b = ser_hw_read_status(SER_PORT_B);
    rxavail = ser_int_rx_available(TEST_PORT);

    ser_hw_restore_int(flags);

    /* Consume the probe byte and clear any flags it raised */
    ser_int_kick();
    ser_int_flush_rx(TEST_PORT);
    (void)ser_get_error(TEST_PORT);

    rsp_start(CMD_HWSTATE, 0);
    rsp_add8(if_bit);
    rsp_add8(imr);
    rsp_add8(irr);
    rsp_add8(isr);
    rsp_add8(rr0a);
    rsp_add8(rr0b);
    rsp_add16(rxavail);
    rsp_add16(ser_int_kick_count());
    rsp_send();
}

/**
 * STATS - report and clear the error bookkeeping (exercises
 * pkt_get_error()/pkt_clear_error()). Also reports the lost-edge
 * recovery count: nonzero means the interrupt chain needed the
 * polling fallback (interesting on real hardware).
 */
static void cmd_stats(void) {
    rsp_start(CMD_STATS, 0);
    rsp_add16(g_stats.cmd_count);
    rsp_add16(g_stats.ping_count);
    rsp_add16((uint16_t)g_stats.last_err_rc);
    rsp_add8(pkt_get_error(&g_pkt));
    rsp_add16(ser_int_kick_count());
    rsp_send();

    g_stats.last_err_rc = 0;
    pkt_clear_error(&g_pkt);
}

/*===========================================================================
 * Dispatch and Main Loop
 *===========================================================================*/

/**
 * Dispatch one delivered command.
 * @return 0 to keep running, 1 to quit
 */
static int dispatch(uint8_t op, const uint8_t *args, uint16_t len) {
    uint16_t kicks = ser_int_kick_count();

    scr_puts("CMD ");
    scr_hex_byte(op);
    if (kicks) {
        scr_puts(" kick=");
        scr_hex_byte((uint8_t)(kicks >> 8));
        scr_hex_byte((uint8_t)kicks);
    }
    scr_print("");

    switch (op) {
        case CMD_PING:          cmd_ping();                 break;
        case CMD_ECHO:          cmd_echo(args, len);        break;
        case CMD_CRC:           cmd_crc(args, len);         break;
        case CMD_STATUS:        cmd_status();               break;
        case CMD_RAW_ECHO:      cmd_raw_echo(args, len);    break;
        case CMD_WRITE_STR:     cmd_write_str(args, len);   break;
        case CMD_WRITE_BLOCK:   cmd_write_block(args, len); break;
        case CMD_READ_BLOCK:    cmd_read_block(args, len);  break;
        case CMD_RX_FLUSH:      cmd_rx_flush(args, len);    break;
        case CMD_OVERFLOW:      cmd_overflow();             break;
        case CMD_FLUSH_TX:      cmd_flush_tx(args, len);    break;
        case CMD_FLOW_XONOFF:   cmd_flow_xonoff();          break;
        case CMD_FLOW_SMOKE:    cmd_flow_smoke(args, len);  break;
        case CMD_BAUD_BAD:      cmd_baud_bad(args, len);    break;
        case CMD_BAUD_CYCLE:    cmd_baud_cycle(args, len);  break;
        case CMD_FORMAT_BAD:    cmd_format_bad(args, len);  break;
        case CMD_FORMAT_CYCLE:  cmd_format_cycle(args, len); break;
        case CMD_INIT_PORT:     cmd_init_port(args, len);   break;
        case CMD_LINES:         cmd_lines();                break;
        case CMD_RX_READY:      cmd_rx_ready();             break;
        case CMD_PKT_PARAMS:    cmd_pkt_params(args, len);  break;
        case CMD_STATS:         cmd_stats();                break;
        case CMD_HWSTATE:       cmd_hwstate();              break;

        case CMD_QUIT:
            rsp_start(CMD_QUIT, 0);
            rsp_send();
            agent_pause(PAUSE_100MS * 5);
            return 1;

        default:
            rsp_start(op, 2);   /* unknown opcode */
            rsp_send();
            break;
    }

    return 0;
}

/*===========================================================================
 * Command Line Parsing
 *===========================================================================*/

static const char *baud_to_str(uint8_t baud) {
    switch (baud) {
        case SER_BAUD_110:   return "110";
        case SER_BAUD_300:   return "300";
        case SER_BAUD_600:   return "600";
        case SER_BAUD_1200:  return "1200";
        case SER_BAUD_2400:  return "2400";
        case SER_BAUD_4800:  return "4800";
        case SER_BAUD_9600:  return "9600";
        case SER_BAUD_19200: return "19200";
        default:             return "?";
    }
}

/**
 * Parse a baud rate string.
 * @return Baud index, or 0xFF if invalid
 */
static uint8_t parse_baud(const char *str) {
    uint32_t val = 0;

    while (*str >= '0' && *str <= '9') {
        val = val * 10 + (*str - '0');
        str++;
    }

    if (*str != '\0') return 0xFF;

    switch (val) {
        case 110:   return SER_BAUD_110;
        case 300:   return SER_BAUD_300;
        case 600:   return SER_BAUD_600;
        case 1200:  return SER_BAUD_1200;
        case 2400:  return SER_BAUD_2400;
        case 4800:  return SER_BAUD_4800;
        case 9600:  return SER_BAUD_9600;
        case 19200: return SER_BAUD_19200;
        default:    return 0xFF;
    }
}

static void show_usage(void) {
    scr_print("Usage: sertest [baud]");
    scr_print("  baud: 110 300 600 1200 2400 4800 9600 19200");
    scr_print("  default: 9600");
}

/*===========================================================================
 * Main Program
 *===========================================================================*/

int main(int argc, char *argv[]) {
    int16_t len;
    int quit = 0;

    if (argc > 1) {
        uint8_t baud = parse_baud(argv[1]);
        if (baud == 0xFF) {
            scr_puts("Invalid baud rate: ");
            scr_puts(argv[1]);
            scr_print("");
            show_usage();
            return 1;
        }
        g_baud = baud;
    }

    scr_print("");
    scr_print("Victor 9000 Serial Test Agent v2");
    scr_print("=================================");

    ser_init();
    ser_set_baud(TEST_PORT, g_baud);
    pkt_init(&g_pkt, TEST_PORT);    /* enables interrupt mode */

    scr_puts("Port A at ");
    scr_puts(baud_to_str(g_baud));
    scr_print(" baud");
    scr_print("Waiting for host commands...");
    scr_print("(run victor9k_serial_test.py on the host)");

    /* One-time banner so a human (or --probe) watching the line sees
     * the Victor->host direction is alive. Contains no SYNC byte, so
     * the packet layer on the host skips it. */
    agent_pause(PAUSE_100MS * 2);
    ser_write_str(TEST_PORT, "\r\nSERTEST AGENT v2 READY\r\n");
    ser_int_drain_tx(TEST_PORT);

    while (!quit) {
        len = pkt_receive(&g_pkt, g_cmd, PKT_MAX_PAYLOAD);

        if (len < 0) {
            /* Idle timeouts are normal; anything else is bookkept for
             * the host's protocol-misbehavior tests */
            if (len != PKT_ERR_TIMEOUT) {
                uint16_t kicks = ser_int_kick_count();
                g_stats.last_err_rc = len;
                scr_puts("rx err rc=");
                scr_hex_byte((uint8_t)(-len));
                scr_puts(" hw=");
                scr_hex_byte(ser_get_error(TEST_PORT));
                scr_puts(" k=");
                scr_hex_byte((uint8_t)(kicks >> 8));
                scr_hex_byte((uint8_t)kicks);
                scr_print("");
            }
            continue;
        }

        if (len < 1) {
            continue;
        }

        g_stats.cmd_count++;
        quit = dispatch(g_cmd[0], g_cmd + 1, (uint16_t)(len - 1));
    }

    scr_print("Agent shutting down.");
    ser_shutdown();

    return 0;
}
