/**
 * @file pkttest.c
 * @brief Packet-layer receive test for the Victor 9000 - exercises the
 *        pkt_recv_raw() hot loop (the inline-asm capture loop in packet.c) and
 *        reports the per-byte guard waits the same way spdtest.c does.
 *
 * spdtest.c polls the SIO directly and never touches the packet layer; this
 * test instead drives the real thing: pkt_wait_sync() + pkt_recv_raw() per
 * frame, so the asm hot loop is what receives every type/payload/CRC byte.
 * Built with -DPKT_PROFILE_GUARD so pkt_recv_raw fills pkt_guard_log[]; after
 * each packet we fold those into a running min/max across the whole run and
 * print them at the end.
 *
 * The PC host (pkttest_host.py) frames a known byte pattern into packets and
 * streams them with a short inter-frame gap: within a frame the bytes arrive
 * back-to-back at line rate (which stresses the hot loop), while the gap gives
 * the between-frame CRC/copy work room so it can't overrun the 3-byte FIFO.
 *
 * Usage (driven from DOS via the remote-control 'type' command):
 *   PKTTEST <baudidx> <npackets> [a|b]
 * where <baudidx> is a SER_BAUD_* index (6=9600, 7=19200, 8=38400) and the
 * port defaults to A (COM1).
 *
 * Wire: Victor sends 'R' when ready, then receives <npackets> frames. Guard
 * semantics match spdtest - a stored guard near PKT_TIMEOUT_SHORT means the
 * byte was already in the FIFO (no wait); a smaller value means the loop spun
 * (PKT_TIMEOUT_SHORT - guard) times for it, so min over all bytes is the
 * worst-case (longest) inter-byte wait. CRC failures (e.g. a FIFO overrun)
 * are counted as errors rather than folded into the guard stats.
 *
 * Build: Makefile target pkttest.exe (links serial.lib objects, the packet
 * object compiled with -DPKT_PROFILE_GUARD).
 */
#include <stdio.h>
#include <stdlib.h>
#include "serial.h"
#include "packet.h"

int main(int argc, char **argv)
{
    ser_config_t cfg;
    pkt_state_t  pkt;
    static uint8_t buf[PKT_MAX_PAYLOAD];
    int      baud;
    unsigned long npackets, p;
    uint8_t  port = SER_PORT_A;
    uint8_t  type;
    int16_t  r;
    uint16_t i, g;
    int      ai;

    unsigned long pkts = 0, bytes = 0, errors = 0;
    uint16_t ming = PKT_TIMEOUT_SHORT;   /* smallest guard => longest wait  */
    uint16_t maxg = 0;                   /* largest guard => shortest wait  */

    if (argc < 3) {
        printf("usage: PKTTEST <baudidx> <npackets> [a|b]\n");
        printf("  receive <npackets> packets via the packet layer (asm hot loop)\n");
        return 1;
    }
    baud = atoi(argv[1]);
    npackets = strtoul(argv[2], (char **)0, 10);
    for (ai = 3; ai < argc; ai++) {
        char f = argv[ai][0];
        if (f == 'b' || f == 'B') port = SER_PORT_B;
        else if (f == 'a' || f == 'A') port = SER_PORT_A;
    }

    ser_init();
    cfg.port = port;
    cfg.baud = (uint8_t)baud;
    cfg.data_bits = SER_DATA_8;
    cfg.stop_bits = SER_STOP_1;
    cfg.parity = SER_PARITY_NONE;
    cfg.flow_ctrl = SER_FLOW_NONE;
    if (!ser_init_port(&cfg)) {
        printf("ser_init_port failed (baudidx %d)\n", baud);
        return 1;
    }

    pkt_init_polled(&pkt, port);
    pkt_set_timeout(&pkt, PKT_TIMEOUT_LONG);

    printf("PKTTEST baudidx=%d npackets=%lu : packet RX on port %c (COM%d)\n",
           baud, npackets, (port == SER_PORT_B) ? 'B' : 'A',
           (port == SER_PORT_B) ? 2 : 1);

    /* Ready marker (raw byte, like spdtest) so the host starts streaming. */
    ser_poll_write(port, 'R');

    for (p = 0; p < npackets; p++) {
        if (!pkt_wait_sync(&pkt)) {
            break;              /* no SYNC within the window - host is done */
        }
        r = pkt_recv_raw(&pkt, &type, buf, sizeof(buf));
        if (r < 0) {
            errors++;                   /* CRC / framing / inter-byte timeout */
            ser_poll_write(port, 'N');  /* NAK - host advances to the next   */
            continue;
        }
        pkts++;
        bytes += (unsigned long)r;
        /* Fold this packet's per-byte guard waits into the running min/max. */
        for (i = 0; i < pkt_guard_n; i++) {
            g = pkt_guard_log[i];
            if (g < ming) ming = g;
            if (g > maxg) maxg = g;
        }
        /* One-byte ACK: a deterministic between-frame gate so the host doesn't
         * stream the next frame into the FIFO while we're still doing the CRC
         * and copy above. Within a frame the bytes arrive at line rate, which
         * is what actually exercises the hot loop. */
        ser_poll_write(port, 'A');
    }

    printf("PKT done: pkts=%lu bytes=%lu errors=%lu\n", pkts, bytes, errors);
    if (pkts > 0) {
        printf("  guard/byte: min=%u max=%u  slack(5000-guard): best=%u tight=%u\n",
               ming, maxg,
               (unsigned)(PKT_TIMEOUT_SHORT - ming),
               (unsigned)(PKT_TIMEOUT_SHORT - maxg));

        /* Dump the last packet's full per-byte guard array to the host - it has
         * pkt_guard_n entries (len + 3), too many for the Victor console. Wire
         * format: 'G', count (2 bytes LE), then count guard values (2 bytes LE
         * each). The host prints each as slack = PKT_TIMEOUT_SHORT - guard. */
        ser_poll_write(port, 'G');
        ser_poll_write(port, (uint8_t)(pkt_guard_n & 0xFF));
        ser_poll_write(port, (uint8_t)(pkt_guard_n >> 8));
        for (i = 0; i < pkt_guard_n; i++) {
            ser_poll_write(port, (uint8_t)(pkt_guard_log[i] & 0xFF));
            ser_poll_write(port, (uint8_t)(pkt_guard_log[i] >> 8));
        }
    }

    ser_shutdown();
    return 0;
}
