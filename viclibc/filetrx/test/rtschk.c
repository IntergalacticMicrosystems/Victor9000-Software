/**
 * @file rtschk.c
 * @brief RTS/CTS wiring probe for the Victor 9000 <-> FTDI cable.
 *
 * Before investing in RX-side RTS flow control we must know whether the
 * modem-control handshake lines are physically wired. This program lets the
 * host characterise BOTH directions:
 *
 *   Direction A (Victor RTS -> host CTS): the wire we NEED to throttle the
 *     PC sender so the Victor stops overrunning above 9600. Host commands an
 *     RTS state, the Victor sets it and acks, host then samples its own CTS.
 *
 *   Direction B (host RTS -> Victor CTS): the wire the existing TX-side
 *     SER_FLOW_RTSCTS code already assumes. Host sets its RTS, queries, and
 *     the Victor reports ser_check_cts().
 *
 * Wire protocol (Victor is the responder), single bytes:
 *   'R'  (Victor->host)  ready marker, sent once at startup
 *   '1'  (host->Victor)  assert RTS,    Victor replies 'A'
 *   '0'  (host->Victor)  deassert RTS,  Victor replies 'A'
 *   'q'  (host->Victor)  query CTS,     Victor replies 'Y' (active) or 'N'
 *   'Q'  (host->Victor)  quit
 *
 * Build:
 *   wcc -ms -0 -zq -s -ox -w4 -zp1 -i=include -i=../serial/include \
 *       -fo=obj/rtschk.obj test/rtschk.c
 *   wlink system dos name rtschk.exe file obj/rtschk.obj \
 *       library ../serial/serial.lib
 */

#include <stdio.h>
#include "serial.h"

#define CHK_PORT  SER_PORT_A
#define CHK_BAUD  SER_BAUD_9600

/* Bounded blocking read with kick-poll fallback, like the other probes.
 * Returns -1 only if no byte arrives within a generous spin budget. */
static int16_t read_block(void)
{
    uint32_t spin = 8000000UL;
    int16_t d;

    while (spin > 0) {
        d = ser_int_read(CHK_PORT);
        if (d >= 0) {
            return d;
        }
        if ((spin & 0x07) == 0) {
            ser_int_kick();
        }
        spin--;
    }
    return -1;
}

static void reply(uint8_t b)
{
    ser_int_write(CHK_PORT, b);
    ser_int_drain_tx(CHK_PORT);
}

int main(void)
{
    ser_config_t cfg;
    int16_t d;

    printf("rtschk: RTS/CTS wiring probe on port A @ 9600\n");

    ser_init();
    cfg.port = CHK_PORT;
    cfg.baud = CHK_BAUD;
    cfg.data_bits = SER_DATA_8;
    cfg.stop_bits = SER_STOP_1;
    cfg.parity = SER_PARITY_NONE;
    cfg.flow_ctrl = SER_FLOW_NONE;   /* manual RTS control for the probe */
    if (!ser_init_port(&cfg)) {
        printf("ser_init_port failed\n");
        return 1;
    }
    ser_int_enable();

    /* Start with RTS asserted (the default), then announce readiness. */
    ser_set_rts(CHK_PORT, SER_TRUE);
    reply('R');

    for (;;) {
        d = read_block();
        if (d < 0) {
            printf("timeout waiting for command\n");
            break;
        }
        switch ((uint8_t)d) {
            case '1':
                ser_set_rts(CHK_PORT, SER_TRUE);
                reply('A');
                break;
            case '0':
                ser_set_rts(CHK_PORT, SER_FALSE);
                reply('A');
                break;
            case 'q':
                reply(ser_check_cts(CHK_PORT) ? 'Y' : 'N');
                break;
            case 'Q':
                reply('A');
                goto done;
            default:
                /* ignore noise */
                break;
        }
    }

done:
    ser_int_disable();
    ser_shutdown();
    printf("rtschk: done\n");
    return 0;
}
