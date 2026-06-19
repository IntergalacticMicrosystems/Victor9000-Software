/**
 * @file rxdiag.c
 * @brief Raw serial RX integrity probe for the Victor 9000.
 *
 * Isolates the receive path from the packet/file-transfer layers so we can
 * see WHY reception fails above 9600. The host sends a known incrementing
 * byte pattern (0,1,2,...,255,0,...) of BURST_N bytes at DIAG_BAUD; this
 * program receives it via the interrupt-driven serial library (exactly as
 * filetrx does, kick-poll fallback included) and reports back over TX:
 *
 *   recv      - bytes actually received
 *   gaps      - pattern discontinuities (a lost or corrupted byte)
 *   first_bad - 1-based index of the first discontinuity
 *   err       - ser_get_error() flags (SER_ERR_OVERRUN distinguishes a
 *               FIFO overrun from other corruption)
 *   kicks     - ser_int_kick_count() (high => running on the polling
 *               fallback because hardware interrupts are not firing)
 *
 * Build per baud with -DDIAG_BAUD=SER_BAUD_xxxx.
 */

#include <stdio.h>
#include "serial.h"

#define DIAG_PORT  SER_PORT_A
#ifndef DIAG_BAUD
#define DIAG_BAUD  SER_BAUD_19200
#endif

#define BURST_N    4096U
#ifdef RX_FLOW
/* With RX flow control the host stops and starts; at the ring-empty edge of
 * a resume the FTDI latency timer (~16 ms) can leave us briefly starved, so
 * give a much longer inter-byte budget before declaring the host done. */
#define BYTE_SPIN  400000UL
#else
#define BYTE_SPIN  40000UL   /* inter-byte spin budget; timeout => host done */
#endif

/* Read one byte with a bounded spin, self-healing lost edges like
 * pkt_read_timeout() in packet.c. Returns -1 on inter-byte timeout. */
static int16_t read_kick(void)
{
    uint32_t spin = BYTE_SPIN;
    int16_t d;

    while (spin > 0) {
        d = ser_int_read(DIAG_PORT);
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

int main(void)
{
    ser_config_t cfg;
    uint16_t recv = 0;
    uint16_t gaps = 0;
    uint16_t first_bad = 0xFFFF;
    uint16_t kicks;
    uint8_t errf;
    uint8_t exp = 0;
    uint8_t b;
    int16_t d;
    uint8_t rpt[11];
    int i;

    printf("rxdiag: port A, baud idx %u, expecting %u bytes\n",
           (unsigned)DIAG_BAUD, BURST_N);

    ser_init();
    cfg.port = DIAG_PORT;
    cfg.baud = DIAG_BAUD;
    cfg.data_bits = SER_DATA_8;
    cfg.stop_bits = SER_STOP_1;
    cfg.parity = SER_PARITY_NONE;
    cfg.flow_ctrl = SER_FLOW_NONE;
    if (!ser_init_port(&cfg)) {
        printf("ser_init_port failed\n");
        return 1;
    }
    ser_int_enable();

#ifdef RX_FLOW
    /* Throttle the remote via RTS when our ring fills: the decisive test of
     * whether the 8088 can sustain 19200 RX in flow-controlled bursts. */
    ser_set_rx_flow(DIAG_PORT, SER_TRUE);
    printf("RX flow control (RTS throttle) ENABLED\n");
#endif

    /* Signal the host we are ready to receive the burst. */
    ser_int_write(DIAG_PORT, 'R');
    ser_int_drain_tx(DIAG_PORT);

    /* Receive until the host stops (inter-byte timeout). */
    for (;;) {
        d = read_kick();
        if (d < 0) {
            break;
        }
        b = (uint8_t)d;
        if (recv < 0xFFFF) {
            recv++;
        }
        if (b != exp) {
            if (gaps < 0xFFFF) {
                gaps++;
            }
            if (first_bad == 0xFFFF) {
                first_bad = recv;   /* 1-based position of first bad byte */
            }
            exp = b;                /* resync so we count discrete events */
        }
        exp++;
    }

#ifdef RX_FLOW
    /* Done receiving: reassert RTS so any bytes the host paused mid-stream
     * can drain (else the host's flush() blocks on CTS forever) and so our
     * report TX is not itself throttled. */
    ser_set_rx_flow(DIAG_PORT, SER_FALSE);
#endif

    errf = ser_get_error(DIAG_PORT);
    kicks = ser_int_kick_count();

    /* Report back over TX (raw, framed by 'D'...'E'). */
    rpt[0]  = 'D';
    rpt[1]  = (uint8_t)(recv & 0xFF);
    rpt[2]  = (uint8_t)(recv >> 8);
    rpt[3]  = (uint8_t)(gaps & 0xFF);
    rpt[4]  = (uint8_t)(gaps >> 8);
    rpt[5]  = (uint8_t)(first_bad & 0xFF);
    rpt[6]  = (uint8_t)(first_bad >> 8);
    rpt[7]  = errf;
    rpt[8]  = (uint8_t)(kicks & 0xFF);
    rpt[9]  = (uint8_t)(kicks >> 8);
    rpt[10] = 'E';
    for (i = 0; i < 11; i++) {
        ser_int_write(DIAG_PORT, rpt[i]);
    }
    ser_int_drain_tx(DIAG_PORT);

    printf("recv=%u gaps=%u first=%u err=%02X kicks=%u\n",
           recv, gaps, first_bad, errf, kicks);

    ser_int_disable();
    ser_shutdown();
    return 0;
}
