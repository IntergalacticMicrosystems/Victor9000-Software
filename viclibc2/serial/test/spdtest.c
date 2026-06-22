/**
 * @file spdtest.c
 * @brief Pure-polled raw serial speed/integrity test for the Victor 9000.
 *
 * Finds the practical ceiling of POLLED (no interrupts) serial transfer on
 * the uPD7201, on Port B = COM2 = SER_PORT_B (the null-modem link to the
 * PC's /dev/ttyUSB0). No ISR is installed and the PIC SIO line stays masked,
 * so this never uses the library's interrupt path - the tight loops read the
 * SIO status register (RR0) directly via memory-mapped I/O and move bytes one
 * at a time. The host side (spdtest_host.py) sources/sinks a known
 * incrementing byte pattern and times throughput.
 *
 * The interesting direction is RX (PC -> Victor): the 7201 has only a 3-byte
 * RX FIFO, so the question is whether the 8088 can poll-drain it fast enough
 * to avoid overruns above 9600. TX (Victor -> PC) is never FIFO-limited on
 * our side; it just probes clean byte timing at high baud.
 *
 * Usage (driven from DOS via the remote-control 'type' command):
 *   SPDTEST R <baudidx> <count> [i]   receive <count> bytes from the PC
 *   SPDTEST T <baudidx> <count> [i]   transmit <count> bytes to the PC
 * where <baudidx> is a SER_BAUD_* index (6=9600, 7=19200, 8=38400) and the
 * optional 'i' disables CPU interrupts (CLI) around the transfer loop so the
 * 18.2 Hz timer tick can't steal cycles from the poll loop.
 *
 * Wire protocol on Port B (Victor is the agent):
 *   'R'                       Victor -> PC : ready marker (once, at start)
 *   RX run: PC streams the pattern; Victor receives until an inter-byte
 *           timeout, then sends a 15-byte report frame:
 *             'D' recv[4] gaps[4] first_bad[4] rr1[1] 'E'   (all LE u32)
 *   TX run: PC replies 'G' (go); Victor blasts <count> pattern bytes, then
 *           sends the same 15-byte report frame (recv = bytes sent).
 *
 * Build: see Makefile target spdtest.exe (links serial.lib only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <i86.h>
#include "serial.h"

/* Direct SIO memory-mapped registers. The library's accessors wrap every read
 * in a CLI/STI pair and a function call - far too slow per byte at 19200/38400.
 * In pure-polled mode no ISR can move the chip's register pointer, so once we
 * select RR0 we can read it with a bare load. The data/ctrl register offsets
 * differ per channel; the active pair is chosen at runtime (see g_data/g_ctrl)
 * from the port argument so this can test either COM1/A or COM2/B. */
#define SIO_SEG    0xE004
#define SIO_DATA_A 0            /* Port A (COM1) data register    */
#define SIO_CTRL_A 2            /* Port A (COM1) control register  */
#define SIO_DATA_B 1            /* Port B (COM2) data register    */
#define SIO_CTRL_B 3            /* Port B (COM2) control register  */

#define RR0_RX_AVAIL 0x01
#define RR0_TX_EMPTY 0x04
#define RR1_OVERRUN  0x20       /* RR1 bit5: receiver overrun */

#define CMD_ERR_RST  0x30       /* CR0 command: error reset */

/* Inter-byte timeout for the RX loop, in poll iterations. A single MMIO poll
 * is a few microseconds, so this is ~hundreds of ms of silence => "host done".
 * Must exceed the PL2303's worst-case mid-stream USB chunk gap (~16 ms). */
#define RX_SPIN    400000UL

static volatile unsigned char __far *sio;

/* Active channel register offsets, chosen from the port argument in main(). */
static unsigned g_data = SIO_DATA_A;
static unsigned g_ctrl = SIO_CTRL_A;

/* Select RR0 so subsequent bare reads of the control port return status.
 * Reading RR0 leaves the 7201 pointer at 0, so this holds for the whole loop
 * as long as nothing else writes a register-select. */
static void sel_rr0(void)
{
    sio[g_ctrl] = 0;
}

/* Polled single-byte send: wait for the Tx buffer to be empty, then write. */
static void put_poll(unsigned char b)
{
    sel_rr0();
    while (!(sio[g_ctrl] & RR0_TX_EMPTY)) {
        /* spin */
    }
    sio[g_data] = b;
}

/* Read RR1 (error status), then restore the pointer to RR0. */
static unsigned char read_rr1(void)
{
    unsigned char v;
    sio[g_ctrl] = 1;        /* select RR1 */
    v = sio[g_ctrl];
    sio[g_ctrl] = 0;        /* back to RR0 */
    return v;
}

static void send_report(unsigned long recv, unsigned long gaps,
                        unsigned long first, unsigned char rr1)
{
    put_poll('D');
    put_poll((unsigned char)(recv));
    put_poll((unsigned char)(recv >> 8));
    put_poll((unsigned char)(recv >> 16));
    put_poll((unsigned char)(recv >> 24));
    put_poll((unsigned char)(gaps));
    put_poll((unsigned char)(gaps >> 8));
    put_poll((unsigned char)(gaps >> 16));
    put_poll((unsigned char)(gaps >> 24));
    put_poll((unsigned char)(first));
    put_poll((unsigned char)(first >> 8));
    put_poll((unsigned char)(first >> 16));
    put_poll((unsigned char)(first >> 24));
    put_poll(rr1);
    put_poll('E');
}

int main(int argc, char **argv)
{
    ser_config_t cfg;
    char dir;
    int baud;
    unsigned long count;
    int irq_off = 0;
    uint8_t spd_port = SER_PORT_A;   /* default COM1/SerialA (ttyUSB0 wiring) */
    int ai;

    unsigned long recv = 0, gaps = 0, first = 0xFFFFFFFFUL;
    unsigned char rr1 = 0;
    unsigned char exp = 0, b;
    unsigned long spin;
    unsigned long i;
    /* Per-byte wait profile for the RX loop. spin counts down from RX_SPIN once
     * per failed poll, so a byte read with spin left near RX_SPIN waited barely
     * at all (it was already in the FIFO) while a small spin means the loop
     * spun a long time for it. min_spin is therefore the worst-case (longest)
     * inter-byte wait. The first byte is tracked separately because its wait
     * includes the host's post-'R' startup latency, not steady-state timing. */
    unsigned long min_spin = RX_SPIN;
    unsigned long max_spin = 0;
    unsigned long first_spin = 0;

    if (argc < 4) {
        printf("usage: SPDTEST R|T <baudidx> <count> [i]\n");
        printf("  R=receive from PC, T=transmit to PC; i=IRQs off during xfer\n");
        return 1;
    }
    dir = argv[1][0];
    baud = atoi(argv[2]);
    count = strtoul(argv[3], (char **)0, 10);
    /* Optional trailing flags, order-independent: 'i' = IRQs off during the
     * loop; 'a'/'b' = which SIO channel (COM1/A or COM2/B) to test. */
    for (ai = 4; ai < argc; ai++) {
        char f = argv[ai][0];
        if (f == 'i' || f == 'I') irq_off = 1;
        else if (f == 'a' || f == 'A') spd_port = SER_PORT_A;
        else if (f == 'b' || f == 'B') spd_port = SER_PORT_B;
    }
    if (spd_port == SER_PORT_B) {
        g_data = SIO_DATA_B; g_ctrl = SIO_CTRL_B;
    } else {
        g_data = SIO_DATA_A; g_ctrl = SIO_CTRL_A;
    }

    sio = (volatile unsigned char __far *)MK_FP(SIO_SEG, 0);

    ser_init();
    cfg.port = spd_port;
    cfg.baud = (uint8_t)baud;
    cfg.data_bits = SER_DATA_8;
    cfg.stop_bits = SER_STOP_1;
    cfg.parity = SER_PARITY_NONE;
    cfg.flow_ctrl = SER_FLOW_NONE;
    if (!ser_init_port(&cfg)) {
        printf("ser_init_port failed (baudidx %d)\n", baud);
        return 1;
    }

    printf("SPDTEST dir=%c baudidx=%d count=%lu irq=%s : polled port %c (COM%d)\n",
           dir, baud, count, irq_off ? "OFF" : "on",
           (spd_port == SER_PORT_B) ? 'B' : 'A',
           (spd_port == SER_PORT_B) ? 2 : 1);

    /* Clear any latched receiver error before the run. */
    sio[g_ctrl] = CMD_ERR_RST;
    sel_rr0();

    if (dir == 'T' || dir == 't') {
        /* Announce ready, wait for the host's 'G' go-byte (polled RX). */
        put_poll('R');
        for (;;) {
            sel_rr0();
            while (!(sio[g_ctrl] & RR0_RX_AVAIL)) {
                /* spin (unbounded - host always answers) */
            }
            b = sio[g_data];
            if (b == 'G') break;
        }

        if (irq_off) { _asm { cli } }
        sel_rr0();
        for (i = 0; i < count; i++) {
            while (!(sio[g_ctrl] & RR0_TX_EMPTY)) {
                /* spin */
            }
            sio[g_data] = (unsigned char)(i & 0xFF);
        }
        if (irq_off) { _asm { sti } }

        recv = count;           /* bytes we put on the wire */
        rr1 = read_rr1();
        send_report(recv, 0, 0xFFFFFFFFUL, rr1);
        printf("TX done: sent=%lu rr1=%02X\n", recv, rr1);
    } else {
        /* RX: announce ready, then poll-drain until an inter-byte timeout. */
        put_poll('R');

        if (irq_off) { _asm { cli } }
        sel_rr0();
        for (;;) {
            spin = RX_SPIN;
            while (!(sio[g_ctrl] & RR0_RX_AVAIL)) {
                if (--spin == 0) goto rx_done;
            }
            b = sio[g_data];
            /* Capture the wait once the byte is in hand. Skip the first byte
             * (host startup latency); track worst/best case for the rest. */
            if (recv == 0) {
                first_spin = spin;
            } else {
                if (spin < min_spin) min_spin = spin;
                if (spin > max_spin) max_spin = spin;
            }
            recv++;
            if (b != exp) {
                gaps++;
                if (first == 0xFFFFFFFFUL) first = recv;
                exp = b;        /* resync so we count discrete events */
            }
            exp++;
        }
rx_done:
        if (irq_off) { _asm { sti } }

        rr1 = read_rr1();
        send_report(recv, gaps, first, rr1);
        printf("RX done: recv=%lu gaps=%lu first=%lu rr1=%02X%s\n",
               recv, gaps, (first == 0xFFFFFFFFUL) ? 0UL : first, rr1,
               (rr1 & RR1_OVERRUN) ? " OVERRUN" : "");
        if (recv > 1) {
            printf("  wait spin/byte (of %lu): min=%lu max=%lu first=%lu"
                   "  -> worst-case wait=%lu polls\n",
                   RX_SPIN, min_spin, max_spin, first_spin,
                   RX_SPIN - min_spin);
        }
    }

    ser_shutdown();
    return 0;
}
