/**
 * @file serial_isr.c
 * @brief Victor 9000 Serial Interrupt Service Routine
 *
 * Handles interrupts from the SIO 7201 dual-channel serial controller.
 * Dispatches to appropriate handlers for RX, TX, and error conditions.
 *
 * Interrupt sources (from RR2B bits 0-2):
 *   0: TX interrupt Port B
 *   1: External/status Port B
 *   2: RX interrupt Port B
 *   3: RX error Port B
 *   4: TX interrupt Port A
 *   5: External/status Port A
 *   6: RX interrupt Port A
 *   7: RX error Port A
 */

#include "serial.h"
#include <i86.h>

/*===========================================================================
 * External Declarations
 *===========================================================================*/

/* State access */
extern ser_state_t ser_state_a;
extern ser_state_t ser_state_b;

/* Buffer access */
extern ser_buffer_t ser_rx_buf_a;
extern ser_buffer_t ser_tx_buf_a;
extern ser_buffer_t ser_rx_buf_b;
extern ser_buffer_t ser_tx_buf_b;
extern ser_bool_t ser_buf_put(ser_buffer_t *buf, uint8_t data);
extern int16_t ser_buf_get(ser_buffer_t *buf);

/* Hardware access */
extern void ser_hw_write_ctrl(uint8_t port, uint8_t reg, uint8_t value);
extern uint8_t ser_hw_read_ctrl(uint8_t port, uint8_t reg);
extern uint8_t ser_hw_read_data(uint8_t port);
extern void ser_hw_write_data(uint8_t port, uint8_t data);
extern uint8_t ser_hw_read_status(uint8_t port);
extern uint8_t ser_hw_read_error_status(uint8_t port);
extern uint8_t ser_hw_read_int_vector(void);
extern void ser_hw_reset_errors(uint8_t port);
extern void ser_hw_reset_ext_int(uint8_t port);
extern void ser_hw_reset_tx_int(uint8_t port);
extern void ser_hw_sio_eoi(void);
extern void ser_hw_pic_eoi(void);

/*===========================================================================
 * Hardware Constants
 *===========================================================================*/

#define CR_SEL_1        0x01
#define CR_SEL_2        0x02

/* RR0 bits */
#define RR0_RX_AVAIL    0x01
#define RR0_INT_PENDING 0x02    /* Valid in RR0 of channel A only */
#define RR0_TX_EMPTY    0x04
#define RR0_CTS         0x20

/* CR5 (transmitter control) - for RX-side RTS flow control */
#define CR_SEL_5        0x05
#define CR5_RTS         0x02

/* RR1 bits */
#define RR1_PARITY_ERR  0x10
#define RR1_OVERRUN_ERR 0x20
#define RR1_FRAME_ERR   0x40

/* Interrupt sources from RR2B */
#define INT_TX_B        0
#define INT_EXT_B       1
#define INT_RX_B        2
#define INT_ERR_B       3
#define INT_TX_A        4
#define INT_EXT_A       5
#define INT_RX_A        6
#define INT_ERR_A       7

/* Flow control characters */
#define XON             0x11
#define XOFF            0x13

/* WR0 command: error reset (clears latched overrun/parity/framing) */
#define CMD_ERR_RST     0x30

/*===========================================================================
 * Fast Receive Path (Port A)
 *===========================================================================*/

/**
 * Tight Port A receive drain - the throughput-critical path.
 *
 * The generic isr_service_port() path costs ~10 SIO register accesses and
 * a handful of function calls PER BYTE (it re-reads RR0, reads RR1 error
 * status, and services Port B on every single character). At 9600 the
 * SIO's 3-byte RX FIFO gives ~3 ms of slack, so that overhead is hidden;
 * at 19200 the per-byte budget halves to ~1.5 ms and the FIFO overruns,
 * corrupting packets (CRC fails -> NAK). See [[real-hw-test-result]].
 *
 * This routine empties the FIFO with direct, inlined register access: no
 * function calls, no Port-A/B branch, no per-byte RR1 read, and an inlined
 * ring-buffer put. It drains every byte currently in the FIFO in one pass
 * so the fixed interrupt entry/exit cost is amortised across the burst.
 *
 * Error handling is deliberately NOT done per byte here: a parity/framing
 * error just stores a byte the packet CRC will reject anyway, and a latched
 * overrun is cleared once, after the drain, by the RR1 check below (and
 * again by the generic loop). Overrun does not block further reception on
 * the 7201, so draining stays correct.
 *
 * XON/XOFF flow control needs per-byte inspection, so when it is active we
 * bail and let the generic path handle the port.
 */
/* Hard cap on bytes drained per call: the FIFO is 3 deep, so this is only
 * a safety net. It guarantees the ISR can never spin forever on a stuck
 * RX_AVAIL (e.g. a special-receive-condition that locks the FIFO until
 * Error Reset) - the next interrupt/kick picks up where we left off. */
#define FAST_RX_MAX   32

/**
 * RX-side RTS flow control (Port A): deassert RTS once the receive ring has
 * filled past the high-water mark, telling the remote to pause. The read
 * path (ser_int_read*) reasserts RTS once the ring drains below the
 * low-water mark. Kept independent of SER_FLOW_RTSCTS (TX-side CTS gating)
 * so a stale/latched CTS bit can never gate our own transmitter.
 *
 * Cheap: one ring-count test per ISR pass (early-out when disabled or
 * already throttled), and an actual CR5 write only on the rare high-water
 * transition.
 */
static void isr_rx_throttle_a(void) {
    if (!ser_state_a.rx_flow || ser_state_a.rx_throttled) {
        return;
    }
    if (ser_rx_buf_a.count >= SER_RX_FLOW_HIGH) {
        ser_state_a.cr5_shadow &= ~CR5_RTS;
        ser_hw_write_ctrl(SER_PORT_A, CR_SEL_5, ser_state_a.cr5_shadow);
        ser_state_a.rx_throttled = 1;
    }
}

static void isr_fast_rx_a(void) {
    volatile uint8_t __far *ctrl;
    volatile uint8_t __far *dport;
    ser_buffer_t *buf;
    uint8_t rr0, rr1, data;
    uint8_t n;

    if (ser_state_a.flow_ctrl == SER_FLOW_XONOFF) {
        return;
    }

    ctrl  = (volatile uint8_t __far *)MK_FP(SER_SIO_SEG, SER_SIO_CTRL_A);
    dport = (volatile uint8_t __far *)MK_FP(SER_SIO_SEG, SER_SIO_DATA_A);
    buf   = &ser_rx_buf_a;

    for (n = 0; n < FAST_RX_MAX; n++) {
        *ctrl = 0;                      /* select RR0 */
        rr0 = *ctrl;
        if (!(rr0 & RR0_RX_AVAIL)) {
            break;
        }

        /* Check the error status of the byte now at the FIFO top BEFORE
         * unloading it. A special receive condition (overrun/framing/
         * parity) locks the FIFO: RX_AVAIL stays asserted and reading the
         * data register does NOT advance until an Error Reset is issued.
         * Reading RR1 here lets us detect that and reset, so the drain
         * always makes progress and can never wedge. One extra register
         * read per byte is still far cheaper than the generic path's
         * function calls + Port B servicing + repeated status reads. */
        *ctrl = CR_SEL_1;               /* select RR1 */
        rr1 = *ctrl;

        data = *dport;                  /* unload the byte */

        if (buf->count < SER_BUF_SIZE) {
            buf->data[buf->head] = data;
            buf->head = (buf->head + 1) & SER_BUF_MASK;
            buf->count++;
        } else {
            ser_state_a.error_flags |= SER_ERR_BUFOVFL;
        }

        if (rr1 & (RR1_OVERRUN_ERR | RR1_PARITY_ERR | RR1_FRAME_ERR)) {
            if (rr1 & RR1_OVERRUN_ERR) ser_state_a.error_flags |= SER_ERR_OVERRUN;
            if (rr1 & RR1_PARITY_ERR)  ser_state_a.error_flags |= SER_ERR_PARITY;
            if (rr1 & RR1_FRAME_ERR)   ser_state_a.error_flags |= SER_ERR_FRAMING;
            *ctrl = CMD_ERR_RST;        /* WR0 error reset - unlock the FIFO */
        }
    }

    isr_rx_throttle_a();    /* throttle the remote if the ring is filling */
}

/*===========================================================================
 * ISR Helper Functions
 *===========================================================================*/

/**
 * Restart a suspended/idle transmitter if data is pending.
 *
 * Once the TX buffer runs dry (or TX is suspended by flow control) the
 * TX interrupt chain is dead: the last TX interrupt was reset and no new
 * one will fire. After the stall condition clears, the transmitter must
 * be re-primed with a byte to get the chain going again.
 */
static void isr_tx_resume(uint8_t port, ser_buffer_t *tx_buf) {
    int16_t data;

    if (tx_buf->count == 0) {
        return;
    }

    /* Only write if the TX register is empty, or we'd clobber a byte
     * that is still being shifted out */
    if (!(ser_hw_read_status(port) & RR0_TX_EMPTY)) {
        return;
    }

    data = ser_buf_get(tx_buf);
    if (data >= 0) {
        ser_hw_write_data(port, (uint8_t)data);
    }
}

/**
 * Handle receive interrupt for Port A.
 */
static void isr_rx_a(void) {
    uint8_t data;

    /* Read the received byte */
    data = ser_hw_read_data(SER_PORT_A);

    /* Handle XON/XOFF flow control */
    if (ser_state_a.flow_ctrl == SER_FLOW_XONOFF) {
        if (data == XOFF) {
            ser_state_a.tx_suspended = 1;
            return;
        } else if (data == XON) {
            ser_state_a.tx_suspended = 0;
            /* TX interrupt chain died while suspended - restart it */
            isr_tx_resume(SER_PORT_A, &ser_tx_buf_a);
            return;
        }
    }

    /* Store in buffer */
    if (!ser_buf_put(&ser_rx_buf_a, data)) {
        /* Buffer overflow */
        ser_state_a.error_flags |= SER_ERR_BUFOVFL;
    }

    isr_rx_throttle_a();    /* throttle the remote if the ring is filling */
}

/**
 * Handle receive interrupt for Port B.
 */
static void isr_rx_b(void) {
    uint8_t data;

    data = ser_hw_read_data(SER_PORT_B);

    if (ser_state_b.flow_ctrl == SER_FLOW_XONOFF) {
        if (data == XOFF) {
            ser_state_b.tx_suspended = 1;
            return;
        } else if (data == XON) {
            ser_state_b.tx_suspended = 0;
            isr_tx_resume(SER_PORT_B, &ser_tx_buf_b);
            return;
        }
    }

    if (!ser_buf_put(&ser_rx_buf_b, data)) {
        ser_state_b.error_flags |= SER_ERR_BUFOVFL;
    }
}

/**
 * Handle transmit interrupt for Port A.
 */
static void isr_tx_a(void) {
    int16_t data;

    /* Check if TX is suspended by flow control */
    if (ser_state_a.tx_suspended) {
        ser_hw_reset_tx_int(SER_PORT_A);
        return;
    }

    /* Check CTS for hardware flow control */
    if (ser_state_a.flow_ctrl == SER_FLOW_RTSCTS) {
        if (!(ser_hw_read_status(SER_PORT_A) & RR0_CTS)) {
            ser_hw_reset_tx_int(SER_PORT_A);
            return;
        }
    }

    /* Get next byte from buffer */
    data = ser_buf_get(&ser_tx_buf_a);

    if (data >= 0) {
        ser_hw_write_data(SER_PORT_A, (uint8_t)data);
    } else {
        /* Buffer empty, reset TX interrupt */
        ser_hw_reset_tx_int(SER_PORT_A);
    }
}

/**
 * Handle transmit interrupt for Port B.
 */
static void isr_tx_b(void) {
    int16_t data;

    if (ser_state_b.tx_suspended) {
        ser_hw_reset_tx_int(SER_PORT_B);
        return;
    }

    if (ser_state_b.flow_ctrl == SER_FLOW_RTSCTS) {
        if (!(ser_hw_read_status(SER_PORT_B) & RR0_CTS)) {
            ser_hw_reset_tx_int(SER_PORT_B);
            return;
        }
    }

    data = ser_buf_get(&ser_tx_buf_b);

    if (data >= 0) {
        ser_hw_write_data(SER_PORT_B, (uint8_t)data);
    } else {
        ser_hw_reset_tx_int(SER_PORT_B);
    }
}

/**
 * Handle receive error for Port A.
 */
static void isr_err_a(void) {
    uint8_t rr1;

    /* Read error status */
    rr1 = ser_hw_read_error_status(SER_PORT_A);

    /* Accumulate error flags */
    if (rr1 & RR1_OVERRUN_ERR) ser_state_a.error_flags |= SER_ERR_OVERRUN;
    if (rr1 & RR1_PARITY_ERR) ser_state_a.error_flags |= SER_ERR_PARITY;
    if (rr1 & RR1_FRAME_ERR) ser_state_a.error_flags |= SER_ERR_FRAMING;

    /* Discard the errored byte at the FIFO head - but only for
     * parity/framing, where the head byte itself is the bad one. On a
     * pure overrun the lost byte is already gone and the head byte is
     * good data. */
    if ((rr1 & (RR1_PARITY_ERR | RR1_FRAME_ERR))
            && (ser_hw_read_status(SER_PORT_A) & RR0_RX_AVAIL)) {
        (void)ser_hw_read_data(SER_PORT_A);
    }

    /* Reset errors */
    ser_hw_reset_errors(SER_PORT_A);
}

/**
 * Handle receive error for Port B.
 */
static void isr_err_b(void) {
    uint8_t rr1;

    rr1 = ser_hw_read_error_status(SER_PORT_B);

    if (rr1 & RR1_OVERRUN_ERR) ser_state_b.error_flags |= SER_ERR_OVERRUN;
    if (rr1 & RR1_PARITY_ERR) ser_state_b.error_flags |= SER_ERR_PARITY;
    if (rr1 & RR1_FRAME_ERR) ser_state_b.error_flags |= SER_ERR_FRAMING;

    if ((rr1 & (RR1_PARITY_ERR | RR1_FRAME_ERR))
            && (ser_hw_read_status(SER_PORT_B) & RR0_RX_AVAIL)) {
        (void)ser_hw_read_data(SER_PORT_B);
    }

    ser_hw_reset_errors(SER_PORT_B);
}

/**
 * Handle external/status interrupt for Port A.
 */
static void isr_ext_a(void) {
    uint8_t rr0;

    rr0 = ser_hw_read_status(SER_PORT_A);

    /* Check for break */
    if (rr0 & 0x80) {   /* RR0_BREAK */
        ser_state_a.error_flags |= SER_ERR_BREAK;
    }

    /* Check CTS change for hardware flow control */
    if (ser_state_a.flow_ctrl == SER_FLOW_RTSCTS) {
        if ((rr0 & RR0_CTS) && !ser_state_a.tx_suspended) {
            /* CTS active, resume TX if data pending */
            isr_tx_resume(SER_PORT_A, &ser_tx_buf_a);
        }
    }

    ser_hw_reset_ext_int(SER_PORT_A);
}

/**
 * Handle external/status interrupt for Port B.
 */
static void isr_ext_b(void) {
    uint8_t rr0;

    rr0 = ser_hw_read_status(SER_PORT_B);

    if (rr0 & 0x80) {
        ser_state_b.error_flags |= SER_ERR_BREAK;
    }

    if (ser_state_b.flow_ctrl == SER_FLOW_RTSCTS) {
        if ((rr0 & RR0_CTS) && !ser_state_b.tx_suspended) {
            isr_tx_resume(SER_PORT_B, &ser_tx_buf_b);
        }
    }

    ser_hw_reset_ext_int(SER_PORT_B);
}

/*===========================================================================
 * Main Interrupt Service Routine
 *===========================================================================*/

/* Bound on interrupt sources serviced per invocation. Generous
 * (4 sources x 2 ports plus slack); prevents a wedged status bit from
 * trapping us in the ISR forever. */
#define ISR_MAX_LOOPS   16

/**
 * Service one port by status: RX first (overrun risk), then receive
 * errors, then TX. Returns nonzero if anything was serviced.
 */
static uint8_t isr_service_port(uint8_t port,
                                ser_buffer_t *tx_buf,
                                void (*rx_fn)(void),
                                void (*tx_fn)(void),
                                void (*err_fn)(void)) {
    uint8_t rr0;
    uint8_t progress = 0;

    rr0 = ser_hw_read_status(port);

    /* Errors first: the errored byte is at the FIFO head, and the
     * error handler must discard it before the RX path stores it */
    if (ser_hw_read_error_status(port)
            & (RR1_PARITY_ERR | RR1_OVERRUN_ERR | RR1_FRAME_ERR)) {
        err_fn();
        progress = 1;
        rr0 = ser_hw_read_status(port);
    }

    if (rr0 & RR0_RX_AVAIL) {
        rx_fn();
        progress = 1;
    }

    /* TX: only a write (or the reset inside the tx handler when
     * suspended/CTS-blocked) clears the TX interrupt-pending latch */
    if ((rr0 & RR0_TX_EMPTY) && tx_buf->count > 0) {
        tx_fn();
        progress = 1;
    }

    return progress;
}

/**
 * Anything for the service loop to do? True when the SIO reports an
 * interrupt pending (RR0A bit 1) OR a received character is sitting in
 * either channel's FIFO. The RX check matters for the polling
 * fallback: if RX interrupts fail to latch on real silicon, the
 * pending bit alone would never expose waiting data.
 */
uint8_t ser_isr_work_pending(void) {
    if (ser_hw_read_status(SER_PORT_A)
            & (RR0_INT_PENDING | RR0_RX_AVAIL)) {
        return 1;
    }
    return (ser_hw_read_status(SER_PORT_B) & RR0_RX_AVAIL) ? 1 : 0;
}

/**
 * Service all pending SIO interrupt sources, by STATUS, until the SIO
 * reports nothing pending (RR0A bit 1, valid on channel A only).
 *
 * Dispatch deliberately does NOT use the RR2B status-affects-vector
 * field: the position of the source bits within RR2B depends on the
 * CR2A bus/vector mode and on silicon revision, and the vector base in
 * WR2B adds bits of its own. MAME's 7201 model is lenient about this;
 * a real chip that decodes differently leaves the missed source's
 * interrupt-pending latch set, the INT line never falls, the
 * edge-triggered 8259A never sees another edge, and interrupts stop
 * dead. Status bits in RR0/RR1 are unambiguous on every variant.
 *
 * Called from the ISR (interrupts already off) and from ser_int_kick()
 * (inside a CLI window).
 *
 * @return Number of service passes that did work
 */
uint8_t ser_isr_service(void) {
    uint8_t loops;
    uint8_t worked = 0;
    uint8_t progress;

    /* Throughput-critical path first: empty the Port A RX FIFO as fast as
     * possible. The generic loop below then mops up errors, TX, Port B and
     * clears any stuck interrupt latches - but no longer pays the heavy
     * per-byte cost for the common case of inbound Port A data. */
    isr_fast_rx_a();

    for (loops = 0; loops < ISR_MAX_LOOPS; loops++) {
        /* Anything (still) pending? */
        if (!ser_isr_work_pending()) {
            break;
        }

        progress = isr_service_port(SER_PORT_A, &ser_tx_buf_a,
                                    isr_rx_a, isr_tx_a, isr_err_a);
        progress |= isr_service_port(SER_PORT_B, &ser_tx_buf_b,
                                     isr_rx_b, isr_tx_b, isr_err_b);

        if (!progress) {
            /* Pending but no RX/error/TX-with-data work: an external/
             * status interrupt, or a TX-empty latch with nothing to
             * send. Clear all of them or the INT line stays high. */
            isr_ext_a();
            isr_ext_b();
            ser_hw_reset_tx_int(SER_PORT_A);
            ser_hw_reset_tx_int(SER_PORT_B);
        }

        /* Reset SIO interrupt-under-service (no-op when the chip never
         * saw an INTA cycle, as on the Victor's 8259A wiring) */
        ser_hw_sio_eoi();

        worked++;
    }

    return worked;
}

/**
 * SIO 7201 Interrupt Service Routine.
 */
void __interrupt __far ser_isr(void) {
    ser_isr_service();

    /* Send End of Interrupt to PIC */
    ser_hw_pic_eoi();
}
