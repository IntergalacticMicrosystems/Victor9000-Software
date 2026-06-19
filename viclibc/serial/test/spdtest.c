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

#define SPD_PORT   SER_PORT_B

/* Direct SIO memory-mapped registers (Port B). The library's accessors wrap
 * every read in a CLI/STI pair and a function call - far too slow per byte at
 * 19200/38400. In pure-polled mode no ISR can move the chip's register
 * pointer, so once we select RR0 we can read it with a bare load. */
#define SIO_SEG    0xE004
#define SIO_DATA_B 1            /* Port B data register   */
#define SIO_CTRL_B 3            /* Port B control register */

#define RR0_RX_AVAIL 0x01
#define RR0_TX_EMPTY 0x04
#define RR1_OVERRUN  0x20       /* RR1 bit5: receiver overrun */

#define CMD_ERR_RST  0x30       /* CR0 command: error reset */

/* Inter-byte timeout for the RX loop, in poll iterations. A single MMIO poll
 * is a few microseconds, so this is ~hundreds of ms of silence => "host done".
 * Must exceed the PL2303's worst-case mid-stream USB chunk gap (~16 ms). */
#define RX_SPIN    400000UL

/* Stall duration for the RTS auto-flow probe (mode 'X'): long enough (~a few
 * seconds) for the PC's whole burst to arrive while we are NOT reading, so the
 * 3-byte RX FIFO overflows. RTS is left asserted; software never touches it. */
#define STALL_SPIN 1500000UL

static volatile unsigned char __far *sio;

/* Select RR0 so subsequent bare reads of the control port return status.
 * Reading RR0 leaves the 7201 pointer at 0, so this holds for the whole loop
 * as long as nothing else writes a register-select. */
static void sel_rr0(void)
{
    sio[SIO_CTRL_B] = 0;
}

/* Polled single-byte send: wait for the Tx buffer to be empty, then write. */
static void put_poll(unsigned char b)
{
    sel_rr0();
    while (!(sio[SIO_CTRL_B] & RR0_TX_EMPTY)) {
        /* spin */
    }
    sio[SIO_DATA_B] = b;
}

/* Read RR1 (error status), then restore the pointer to RR0. */
static unsigned char read_rr1(void)
{
    unsigned char v;
    sio[SIO_CTRL_B] = 1;        /* select RR1 */
    v = sio[SIO_CTRL_B];
    sio[SIO_CTRL_B] = 0;        /* back to RR0 */
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

    unsigned long recv = 0, gaps = 0, first = 0xFFFFFFFFUL;
    unsigned char rr1 = 0;
    unsigned char exp = 0, b;
    unsigned long spin;
    unsigned long i;

    if (argc < 4) {
        printf("usage: SPDTEST R|T|X <baudidx> <count> [i]\n");
        printf("  R=receive from PC, T=transmit to PC, X=RTS auto-flow probe;\n");
        printf("  i=IRQs off during xfer\n");
        return 1;
    }
    dir = argv[1][0];
    baud = atoi(argv[2]);
    count = strtoul(argv[3], (char **)0, 10);
    if (argc > 4 && (argv[4][0] == 'i' || argv[4][0] == 'I')) {
        irq_off = 1;
    }

    sio = (volatile unsigned char __far *)MK_FP(SIO_SEG, 0);

    ser_init();
    cfg.port = SPD_PORT;
    cfg.baud = (uint8_t)baud;
    cfg.data_bits = SER_DATA_8;
    cfg.stop_bits = SER_STOP_1;
    cfg.parity = SER_PARITY_NONE;
    cfg.flow_ctrl = SER_FLOW_NONE;
    if (!ser_init_port(&cfg)) {
        printf("ser_init_port failed (baudidx %d)\n", baud);
        return 1;
    }

    printf("SPDTEST dir=%c baudidx=%d count=%lu irq=%s : polled port B (COM2)\n",
           dir, baud, count, irq_off ? "OFF" : "on");

    /* Clear any latched receiver error before the run. */
    sio[SIO_CTRL_B] = CMD_ERR_RST;
    sel_rr0();

    if (dir == 'T' || dir == 't') {
        /* Announce ready, wait for the host's 'G' go-byte (polled RX). */
        put_poll('R');
        for (;;) {
            sel_rr0();
            while (!(sio[SIO_CTRL_B] & RR0_RX_AVAIL)) {
                /* spin (unbounded - host always answers) */
            }
            b = sio[SIO_DATA_B];
            if (b == 'G') break;
        }

        if (irq_off) { _asm { cli } }
        sel_rr0();
        for (i = 0; i < count; i++) {
            while (!(sio[SIO_CTRL_B] & RR0_TX_EMPTY)) {
                /* spin */
            }
            sio[SIO_DATA_B] = (unsigned char)(i & 0xFF);
        }
        if (irq_off) { _asm { sti } }

        recv = count;           /* bytes we put on the wire */
        rr1 = read_rr1();
        send_report(recv, 0, 0xFFFFFFFFUL, rr1);
        printf("TX done: sent=%lu rr1=%02X\n", recv, rr1);
    } else if (dir == 'X' || dir == 'x') {
        /* RTS auto-flow probe: assert RTS, announce ready, then deliberately
         * stall (do NOT read) so the PC's burst overflows the 3-byte FIFO.
         * Software never touches RTS during the stall. If the 7201
         * auto-deasserted RTS on FIFO-full, the PC would see its CTS drop;
         * we expect it does NOT (RTS is a software-only output on this chip),
         * and that RR1 shows OVERRUN. */
        ser_set_rts(SPD_PORT, SER_TRUE);
        put_poll('R');

        /* Idle without reading the receiver. Poll RR0 (a volatile read, so it
         * is not optimized away) but never touch the DATA register, so the
         * 3-byte RX FIFO is left to overflow. */
        sel_rr0();
        for (spin = STALL_SPIN; spin > 0; spin--) {
            b = sio[SIO_CTRL_B];
        }

        /* Drain the FIFO (only a few bytes deep) and accumulate RR1. On the
         * 7201 the error flags in RR1 track the byte currently at the TOP of
         * the FIFO, so the overrun flag only surfaces once the overrun-tagged
         * byte reaches the top - read RR1 before popping each byte and OR the
         * results. Bound the loop: after an overrun the chip latches the
         * errored byte and RX_AVAIL will not clear by reads alone, so cap
         * iterations and then Error-Reset to release it. */
        sel_rr0();
        {
            int guard = 16;
            rr1 = 0;
            while ((sio[SIO_CTRL_B] & RR0_RX_AVAIL) && guard-- > 0) {
                rr1 |= read_rr1();      /* flags for the byte on top */
                b = sio[SIO_DATA_B];    /* pop it */
                recv++;
            }
        }
        sio[SIO_CTRL_B] = CMD_ERR_RST;   /* clear latched overrun/error */
        sel_rr0();
        send_report(recv, (unsigned long)b, 0xFFFFFFFFUL, rr1);
        printf("RTS-probe done: drained=%lu rr1=%02X%s\n", recv, rr1,
               (rr1 & RR1_OVERRUN) ? " OVERRUN" : "");
    } else {
        /* RX: announce ready, then poll-drain until an inter-byte timeout. */
        put_poll('R');

        if (irq_off) { _asm { cli } }
        sel_rr0();
        for (;;) {
            spin = RX_SPIN;
            while (!(sio[SIO_CTRL_B] & RR0_RX_AVAIL)) {
                if (--spin == 0) goto rx_done;
            }
            b = sio[SIO_DATA_B];
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
    }

    ser_shutdown();
    return 0;
}
