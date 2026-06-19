/**
 * @file serial.c
 * @brief Victor 9000 Serial Communications Library - Core Implementation
 *
 * Main API implementation for serial communications on the Victor 9000.
 * Uses interrupt-driven I/O for both serial ports.
 */

#include "serial.h"
#include <i86.h>

/*===========================================================================
 * External Declarations (from serial_hw.c and serial_buffer.c)
 *===========================================================================*/

/* State access */
extern ser_state_t ser_state_a;
extern ser_state_t ser_state_b;
extern ser_state_t *ser_hw_get_state(uint8_t port);

/* Hardware functions */
extern void ser_hw_init_via(void);
extern void ser_hw_reset_channel(uint8_t port);
extern void ser_hw_write_ctrl(uint8_t port, uint8_t reg, uint8_t value);
extern uint8_t ser_hw_read_ctrl(uint8_t port, uint8_t reg);
extern uint8_t ser_hw_read_data(uint8_t port);
extern void ser_hw_write_data(uint8_t port, uint8_t data);
extern uint8_t ser_hw_read_status(uint8_t port);
extern uint8_t ser_hw_read_error_status(uint8_t port);
extern void ser_hw_reset_errors(uint8_t port);
extern void ser_hw_reset_tx_int(uint8_t port);
extern void ser_hw_set_baud_divisor(uint8_t port, uint16_t divisor);
extern uint16_t ser_hw_get_baud_divisor(uint8_t baud_idx);
extern void ser_hw_config_bus(void);
extern void ser_hw_config_receiver(uint8_t port, uint8_t data_bits);
extern void ser_hw_config_mode(uint8_t port, uint8_t stop_bits, uint8_t parity);
extern void ser_hw_config_transmitter(uint8_t port, uint8_t data_bits,
                                       ser_bool_t dtr, ser_bool_t rts);
extern void ser_hw_update_cr5(uint8_t port);
extern void ser_hw_config_interrupts(uint8_t port, ser_bool_t enable);
extern void ser_hw_enable_pic(void);
extern void ser_hw_disable_pic(void);
extern ser_bool_t ser_hw_isr_installed(void);
extern void ser_hw_install_isr(uint8_t vector, void (__interrupt __far *handler)(void));
extern void ser_hw_restore_isr(uint8_t vector);
extern uint16_t ser_hw_disable_int(void);
extern void ser_hw_restore_int(uint16_t flags);

/* Buffer functions */
extern ser_buffer_t *ser_buf_get_rx(uint8_t port);
extern ser_buffer_t *ser_buf_get_tx(uint8_t port);
extern void ser_buf_clear(ser_buffer_t *buf);
extern ser_bool_t ser_buf_put(ser_buffer_t *buf, uint8_t data);
extern int16_t ser_buf_get(ser_buffer_t *buf);
extern ser_bool_t ser_buf_put_safe(ser_buffer_t *buf, uint8_t data);
extern int16_t ser_buf_get_safe(ser_buffer_t *buf);
extern uint16_t ser_buf_count(ser_buffer_t *buf);
extern uint16_t ser_buf_free(ser_buffer_t *buf);
extern uint16_t ser_buf_put_block(ser_buffer_t *buf, const uint8_t *data, uint16_t len);
extern uint16_t ser_buf_get_block(ser_buffer_t *buf, uint8_t *data, uint16_t len);
extern void ser_buf_init_all(void);

/* ISR (from serial_isr.c) */
extern void __interrupt __far ser_isr(void);
extern uint8_t ser_isr_service(void);
extern uint8_t ser_isr_work_pending(void);

/* Lost-edge recoveries performed by ser_int_kick() */
static uint16_t kick_count;

/*===========================================================================
 * Status Register Bit Definitions
 *===========================================================================*/

#define RR0_RX_AVAIL    0x01
#define RR0_TX_EMPTY    0x04
#define RR0_DCD         0x08
#define RR0_CTS         0x20
#define RR0_BREAK       0x80

#define RR1_PARITY_ERR  0x10
#define RR1_OVERRUN_ERR 0x20
#define RR1_FRAME_ERR   0x40

/* CR5 bits for RTS/DTR */
#define CR5_RTS         0x02
#define CR5_DTR         0x80

/*===========================================================================
 * Initialization Functions
 *===========================================================================*/

/**
 * Initialize serial subsystem (both ports to 8N1, 1200 baud).
 */
void ser_init(void) {
    _disable();

    /* Initialize VIA for internal clocks */
    ser_hw_init_via();

    /* Dummy status reads first, like the BIOS does: a previous program
     * interrupted mid register-select leaves the SIO's internal
     * pointer non-zero, and the next control write would land in the
     * wrong register. A read resets the pointer. */
    (void)ser_hw_read_status(SER_PORT_A);
    (void)ser_hw_read_status(SER_PORT_B);

    /* Reset both channels */
    ser_hw_reset_channel(SER_PORT_A);
    ser_hw_reset_channel(SER_PORT_B);

    /* Set default baud rates (1200) */
    ser_hw_set_baud_divisor(SER_PORT_A, ser_hw_get_baud_divisor(SER_BAUD_1200));
    ser_hw_set_baud_divisor(SER_PORT_B, ser_hw_get_baud_divisor(SER_BAUD_1200));

    /* Configure bus interface (Port A, affects both) */
    ser_hw_config_bus();

    /* Configure Port A: 8N1, DTR/RTS active */
    ser_hw_config_mode(SER_PORT_A, SER_STOP_1, SER_PARITY_NONE);
    ser_hw_config_receiver(SER_PORT_A, SER_DATA_8);
    ser_hw_config_transmitter(SER_PORT_A, SER_DATA_8, SER_TRUE, SER_TRUE);
    ser_hw_config_interrupts(SER_PORT_A, SER_FALSE);

    /* Configure Port B: 8N1, DTR/RTS active */
    ser_hw_config_mode(SER_PORT_B, SER_STOP_1, SER_PARITY_NONE);
    ser_hw_config_receiver(SER_PORT_B, SER_DATA_8);
    ser_hw_config_transmitter(SER_PORT_B, SER_DATA_8, SER_TRUE, SER_TRUE);
    ser_hw_config_interrupts(SER_PORT_B, SER_FALSE);

    /* Initialize state structures */
    ser_state_a.baud_idx = SER_BAUD_1200;
    ser_state_a.data_bits = SER_DATA_8;
    ser_state_a.stop_bits = SER_STOP_1;
    ser_state_a.parity = SER_PARITY_NONE;
    ser_state_a.flow_ctrl = SER_FLOW_NONE;
    ser_state_a.error_flags = 0;
    ser_state_a.tx_suspended = 0;

    ser_state_b.baud_idx = SER_BAUD_1200;
    ser_state_b.data_bits = SER_DATA_8;
    ser_state_b.stop_bits = SER_STOP_1;
    ser_state_b.parity = SER_PARITY_NONE;
    ser_state_b.flow_ctrl = SER_FLOW_NONE;
    ser_state_b.error_flags = 0;
    ser_state_b.tx_suspended = 0;

    /* Clear buffers */
    ser_buf_init_all();

    _enable();
}

/**
 * Initialize a specific port with configuration.
 */
ser_bool_t ser_init_port(const ser_config_t *config) {
    ser_state_t *state;

    if (config == (void *)0) return SER_FALSE;
    if (config->port > SER_PORT_B) return SER_FALSE;
    if (config->baud >= SER_BAUD_COUNT) return SER_FALSE;

    state = ser_hw_get_state(config->port);

    _disable();

    /* Reset channel */
    ser_hw_reset_channel(config->port);

    /* Set baud rate */
    ser_hw_set_baud_divisor(config->port,
                            ser_hw_get_baud_divisor(config->baud));

    /* Configure format */
    ser_hw_config_mode(config->port, config->stop_bits, config->parity);
    ser_hw_config_receiver(config->port, config->data_bits);
    ser_hw_config_transmitter(config->port, config->data_bits,
                               SER_TRUE, SER_TRUE);

    /* No interrupts initially */
    ser_hw_config_interrupts(config->port, SER_FALSE);

    /* Update state */
    state->baud_idx = config->baud;
    state->data_bits = config->data_bits;
    state->stop_bits = config->stop_bits;
    state->parity = config->parity;
    state->flow_ctrl = config->flow_ctrl;
    state->error_flags = 0;
    state->tx_suspended = 0;

    _enable();

    return SER_TRUE;
}

/**
 * Shutdown serial subsystem.
 */
void ser_shutdown(void) {
    /* Disable interrupt mode if active */
    if (ser_hw_isr_installed()) {
        ser_int_disable();
    }

    _disable();

    /* Disable TX/RX on both ports */
    ser_hw_config_transmitter(SER_PORT_A, SER_DATA_8, SER_FALSE, SER_FALSE);
    ser_hw_config_transmitter(SER_PORT_B, SER_DATA_8, SER_FALSE, SER_FALSE);

    _enable();
}

/*===========================================================================
 * Configuration Functions
 *===========================================================================*/

/**
 * Set baud rate for a port.
 */
ser_bool_t ser_set_baud(uint8_t port, uint8_t baud_idx) {
    ser_state_t *state;

    if (port > SER_PORT_B) return SER_FALSE;
    if (baud_idx >= SER_BAUD_COUNT) return SER_FALSE;

    state = ser_hw_get_state(port);

    _disable();
    ser_hw_set_baud_divisor(port, ser_hw_get_baud_divisor(baud_idx));
    state->baud_idx = baud_idx;
    _enable();

    return SER_TRUE;
}

/**
 * Set data format.
 */
ser_bool_t ser_set_format(uint8_t port, uint8_t data_bits,
                          uint8_t stop_bits, uint8_t parity) {
    ser_state_t *state;

    if (port > SER_PORT_B) return SER_FALSE;
    if (data_bits < 5 || data_bits > 8) return SER_FALSE;

    state = ser_hw_get_state(port);

    _disable();

    ser_hw_config_mode(port, stop_bits, parity);
    ser_hw_config_receiver(port, data_bits);
    ser_hw_config_transmitter(port, data_bits,
                               (state->cr5_shadow & CR5_DTR) ? SER_TRUE : SER_FALSE,
                               (state->cr5_shadow & CR5_RTS) ? SER_TRUE : SER_FALSE);

    state->data_bits = data_bits;
    state->stop_bits = stop_bits;
    state->parity = parity;

    _enable();

    return SER_TRUE;
}

/**
 * Set flow control mode.
 */
ser_bool_t ser_set_flow(uint8_t port, uint8_t flow_mode) {
    ser_state_t *state;

    if (port > SER_PORT_B) return SER_FALSE;

    state = ser_hw_get_state(port);
    state->flow_ctrl = flow_mode;

    return SER_TRUE;
}

/**
 * Enable/disable RX-side RTS flow control. See serial.h.
 */
void ser_set_rx_flow(uint8_t port, ser_bool_t enable) {
    ser_state_t *state = ser_hw_get_state(port);
    uint16_t flags;

    if (port > SER_PORT_B) return;

    flags = ser_hw_disable_int();

    state->rx_flow = enable ? 1 : 0;

    /* Disabling while throttled: restore RTS so the remote isn't left paused. */
    if (!enable && state->rx_throttled) {
        state->cr5_shadow |= CR5_RTS;
        ser_hw_update_cr5(port);
        state->rx_throttled = 0;
    }

    ser_hw_restore_int(flags);
}

/**
 * Reassert RTS once the receive ring has drained below the low-water mark.
 * Called from the read path. Cheap no-op unless currently throttled.
 */
static void ser_rx_flow_release(uint8_t port) {
    ser_state_t *state = ser_hw_get_state(port);
    uint16_t flags;

    if (!state->rx_throttled) {
        return;
    }

    flags = ser_hw_disable_int();
    if (state->rx_throttled
            && ser_buf_count(ser_buf_get_rx(port)) <= SER_RX_FLOW_LOW) {
        state->cr5_shadow |= CR5_RTS;
        ser_hw_update_cr5(port);
        state->rx_throttled = 0;
    }
    ser_hw_restore_int(flags);
}

/*===========================================================================
 * Hardware Status Functions
 *===========================================================================*/

/**
 * Read RR0 atomically. The SIO register read is a two-step sequence
 * (write register select, then read); if the ISR runs in between and
 * touches the control registers, the register pointer is clobbered.
 */
static uint8_t ser_read_status_atomic(uint8_t port) {
    uint8_t status;
    uint16_t flags = ser_hw_disable_int();

    status = ser_hw_read_status(port);

    ser_hw_restore_int(flags);

    return status;
}

/**
 * Check if receive data is available.
 */
ser_bool_t ser_rx_ready(uint8_t port) {
    return (ser_read_status_atomic(port) & RR0_RX_AVAIL) ? SER_TRUE : SER_FALSE;
}

/**
 * Check if transmitter is ready.
 */
ser_bool_t ser_tx_ready(uint8_t port) {
    return (ser_read_status_atomic(port) & RR0_TX_EMPTY) ? SER_TRUE : SER_FALSE;
}

/*===========================================================================
 * Polled I/O Functions (no interrupts)
 *
 * These read/write the SIO directly and never touch the ISR or ring
 * buffers. They are for the packet layer's polled mode (filetrx/FTXSERV),
 * which the hardware proved can sustain clean RX to 38400 - far past the
 * interrupt path's 9600 ceiling, because there is no per-byte ISR cost.
 * Only use these when interrupt mode is NOT enabled on the port.
 *===========================================================================*/

#define RR1_ALL_SENT    0x01    /* RR1 bit0: transmitter fully drained */

/* Direct memory-mapped SIO access for the polled hot path. The library's
 * accessors reselect register 0 on every status read (an extra MMIO write),
 * and each access to the E004 I/O region carries wait states - so going
 * through them costs well over one byte time (~260us) at 38400 and a long
 * packet slowly overruns the 3-byte FIFO. Reading RR0 with a single bare load
 * (as the hardware-proven spdtest loop does) halves the per-byte MMIO and
 * keeps the receiver clean to 38400.
 *
 * Invariant: the 7201 register pointer is 0 here. It is 0 after init/config
 * (a control WRITE auto-resets the pointer) and after every polled op below
 * (a status/error READ also auto-resets it), and nothing else touches the SIO
 * control port during a polled transfer. So a bare read of the control port
 * always returns RR0. ser_poll_drain reads RR1 (pointer->1) but the read
 * auto-resets it back to 0 before returning. */
#define SIO_OFF_DATA(port)  ((port) == SER_PORT_A ? 0 : 1)
#define SIO_OFF_CTRL(port)  ((port) == SER_PORT_A ? 2 : 3)

static volatile uint8_t __far *ser_sio_base(void) {
    return (volatile uint8_t __far *)MK_FP(SER_SIO_SEG, 0);
}

/**
 * Polled non-blocking read: one byte from the receiver if available.
 */
int16_t ser_poll_read(uint8_t port) {
    volatile uint8_t __far *sio = ser_sio_base();
    if (sio[SIO_OFF_CTRL(port)] & RR0_RX_AVAIL) {      /* bare RR0 read */
        return (int16_t)sio[SIO_OFF_DATA(port)];
    }
    return -1;
}

/**
 * Polled blocking write: spin until the TX buffer is empty, then write.
 */
void ser_poll_write(uint8_t port, uint8_t data) {
    volatile uint8_t __far *sio = ser_sio_base();
    while (!(sio[SIO_OFF_CTRL(port)] & RR0_TX_EMPTY)) {
        /* spin */
    }
    sio[SIO_OFF_DATA(port)] = data;
}

/**
 * Wait until the transmitter is fully drained (last byte off the wire).
 */
void ser_poll_drain(uint8_t port) {
    /* First the buffer must be free, then the shift register fully empties.
     * Use the library accessor for RR1 (it selects reg 1, reads, and the read
     * leaves the pointer back at 0 for the next ser_poll_read). */
    volatile uint8_t __far *sio = ser_sio_base();
    while (!(sio[SIO_OFF_CTRL(port)] & RR0_TX_EMPTY)) {
        /* spin */
    }
    while (!(ser_hw_read_error_status(port) & RR1_ALL_SENT)) {
        /* spin */
    }
}

/*===========================================================================
 * Interrupt-Driven I/O Functions
 *===========================================================================*/

/**
 * Enable interrupt mode.
 * NOTE: No printf allowed in critical section - DOS re-enables interrupts
 * internally, which would cause ISR to fire before initialization completes.
 */
ser_bool_t ser_int_enable(void) {
    if (ser_hw_isr_installed()) {
        return SER_TRUE;    /* Already enabled */
    }

    /* Clear buffers */
    ser_buf_init_all();

    /* Install ISR */
    ser_hw_install_isr(SER_INT_VECTOR, ser_isr);

    _disable();

    /* Clear any pending errors and interrupts before enabling */
    ser_hw_reset_errors(SER_PORT_A);
    ser_hw_reset_errors(SER_PORT_B);
    ser_hw_reset_tx_int(SER_PORT_A);
    ser_hw_reset_tx_int(SER_PORT_B);

    /* Enable SIO in PIC first (before SIO generates interrupts) */
    ser_hw_enable_pic();

    /* Now enable interrupts on both ports */
    ser_hw_config_interrupts(SER_PORT_A, SER_TRUE);
    ser_hw_config_interrupts(SER_PORT_B, SER_TRUE);

    /* Clear error flags */
    ser_state_a.error_flags = 0;
    ser_state_b.error_flags = 0;

    _enable();

    return SER_TRUE;
}

/**
 * Disable interrupt mode.
 */
void ser_int_disable(void) {
    if (!ser_hw_isr_installed()) {
        return;
    }

    _disable();

    /* Disable interrupts on both ports */
    ser_hw_config_interrupts(SER_PORT_A, SER_FALSE);
    ser_hw_config_interrupts(SER_PORT_B, SER_FALSE);

    /* Disable SIO in PIC */
    ser_hw_disable_pic();

    /* Restore original ISR */
    ser_hw_restore_isr(SER_INT_VECTOR);

    _enable();
}

/**
 * Service pending SIO interrupt sources by polling.
 *
 * Recovery for a lost interrupt: the Victor's 8259A is edge-triggered,
 * so if the SIO INT line is high while no edge is latched (a source
 * became pending at the wrong moment, or dispatch missed one), no
 * interrupt ever fires again. Calling this from wait loops services
 * the stuck source inline, which drops the INT line and lets the
 * normal interrupt chain restart on the next event.
 *
 * @return SER_TRUE if a recovery service was performed
 */
ser_bool_t ser_int_kick(void) {
    uint16_t flags;
    ser_bool_t worked = SER_FALSE;

    if (!ser_hw_isr_installed()) {
        return SER_FALSE;
    }

    flags = ser_hw_disable_int();

    if (ser_isr_work_pending()) {
        /* Could be the microsecond window between the SIO latching the
         * source and the CPU taking the interrupt. Give the ISR a
         * chance to claim it (no-op if the caller had interrupts off);
         * only work still pending after that means a lost interrupt. */
        ser_hw_restore_int(flags);
        flags = ser_hw_disable_int();

        if (ser_isr_work_pending()) {
            if (ser_isr_service()) {
                kick_count++;
                worked = SER_TRUE;
            }
        }
    }

    ser_hw_restore_int(flags);

    return worked;
}

/**
 * Get the number of lost-edge recoveries since startup.
 * A steadily climbing count means the interrupt chain is unhealthy
 * and I/O is effectively running on the polling fallback.
 */
uint16_t ser_int_kick_count(void) {
    return kick_count;
}

/**
 * Polling pump for TX wait loops: first try a lost-edge recovery; if
 * nothing was pending, force-feed the transmitter directly (the case
 * where the TX interrupt was never even latched). Honors flow control
 * exactly like the prime path in ser_int_write().
 */
static ser_bool_t pump_tx_stalled(uint8_t port, ser_buffer_t *tx_buf,
                                  ser_state_t *state) {
    if (tx_buf->count == 0 || state->tx_suspended) {
        return SER_FALSE;
    }
    if (state->flow_ctrl == SER_FLOW_RTSCTS
            && !(ser_hw_read_status(port) & RR0_CTS)) {
        return SER_FALSE;
    }
    return (ser_hw_read_status(port) & RR0_TX_EMPTY) ? SER_TRUE
                                                     : SER_FALSE;
}

static void ser_int_pump(uint8_t port) {
    ser_buffer_t *tx_buf;
    ser_state_t *state;
    uint16_t flags;
    int16_t data;

    if (ser_int_kick()) {
        return;
    }

    tx_buf = ser_buf_get_tx(port);
    state = ser_hw_get_state(port);

    flags = ser_hw_disable_int();

    if (pump_tx_stalled(port, tx_buf, state)) {
        /* Give the ISR a chance to claim the work (same double-check
         * as ser_int_kick); feed the transmitter only if it is still
         * starved afterwards. */
        ser_hw_restore_int(flags);
        flags = ser_hw_disable_int();

        if (pump_tx_stalled(port, tx_buf, state)) {
            /* Not counted as a kick: with TX interrupts off (the
             * silicon-proven CR1 value), pump feeding is normal
             * operation, not a failure recovery */
            data = ser_buf_get(tx_buf);
            if (data >= 0) {
                ser_hw_write_data(port, (uint8_t)data);
            }
        }
    }

    ser_hw_restore_int(flags);
}

/**
 * Read byte from receive buffer.
 */
int16_t ser_int_read(uint8_t port) {
    int16_t c = ser_buf_get_safe(ser_buf_get_rx(port));
    ser_rx_flow_release(port);
    return c;
}

/**
 * Write byte to transmit buffer.
 */
ser_bool_t ser_int_write(uint8_t port, uint8_t data) {
    ser_buffer_t *tx_buf = ser_buf_get_tx(port);
    ser_state_t *state = ser_hw_get_state(port);
    ser_bool_t was_empty;
    ser_bool_t can_prime;
    uint16_t flags;

    flags = ser_hw_disable_int();

    was_empty = (tx_buf->count == 0);

    if (!ser_buf_put(tx_buf, data)) {
        ser_hw_restore_int(flags);
        return SER_FALSE;   /* Buffer full */
    }

    /* Priming must honor flow control, like the ISR TX path does:
     * when TX is suspended (XOFF received) or CTS is dropped, the byte
     * stays queued and the ISR re-primes once flow resumes. */
    can_prime = !state->tx_suspended;
    if (can_prime && state->flow_ctrl == SER_FLOW_RTSCTS) {
        if (!(ser_hw_read_status(port) & RR0_CTS)) {
            can_prime = SER_FALSE;
        }
    }

    /* If TX was idle, prime the transmitter with first byte */
    if (was_empty && can_prime && (ser_hw_read_status(port) & RR0_TX_EMPTY)) {
        int16_t first = ser_buf_get(tx_buf);
        if (first >= 0) {
            ser_hw_write_data(port, (uint8_t)first);
        }
    }

    ser_hw_restore_int(flags);

    return SER_TRUE;
}

/**
 * Get bytes available in receive buffer.
 */
uint16_t ser_int_rx_available(uint8_t port) {
    return ser_buf_count(ser_buf_get_rx(port));
}

/**
 * Get space available in transmit buffer.
 */
uint16_t ser_int_tx_free(uint8_t port) {
    return ser_buf_free(ser_buf_get_tx(port));
}

/**
 * Flush receive buffer.
 */
void ser_int_flush_rx(uint8_t port) {
    ser_buf_clear(ser_buf_get_rx(port));
}

/**
 * Flush transmit buffer.
 */
void ser_int_flush_tx(uint8_t port) {
    ser_buf_clear(ser_buf_get_tx(port));
}

/**
 * Wait for transmit buffer to empty.
 */
void ser_int_drain_tx(uint8_t port) {
    ser_buffer_t *tx_buf = ser_buf_get_tx(port);

    while (tx_buf->count > 0) {
        /* Wait for buffer to empty via ISR; self-heal a lost edge or
         * a TX interrupt that never latched */
        ser_int_pump(port);
    }

    /* Also wait for last byte to actually transmit */
    while (!(ser_read_status_atomic(port) & RR0_TX_EMPTY)) {
        /* Spin */
    }
}

/**
 * Read from interrupt buffer into a block.
 */
uint16_t ser_int_read_block(uint8_t port, uint8_t *buf, uint16_t len) {
    uint16_t n = ser_buf_get_block(ser_buf_get_rx(port), buf, len);
    ser_rx_flow_release(port);
    return n;
}

/**
 * Write to interrupt buffer from a block.
 */
uint16_t ser_int_write_block(uint8_t port, const uint8_t *buf, uint16_t len) {
    uint16_t written = 0;

    while (len > 0) {
        if (ser_int_write(port, *buf++)) {
            written++;
            len--;
        } else {
            break;  /* Buffer full */
        }
    }

    return written;
}

/**
 * Write null-terminated string using interrupt mode.
 */
uint16_t ser_write_str(uint8_t port, const char *str) {
    uint16_t count = 0;

    while (*str) {
        while (!ser_int_write(port, *str)) {
            /* Wait for TX buffer space; self-heal a stalled TX path */
            ser_int_pump(port);
        }
        str++;
        count++;
    }

    return count;
}

/*===========================================================================
 * Status and Control Functions
 *===========================================================================*/

/**
 * Get and clear error flags.
 */
uint8_t ser_get_error(uint8_t port) {
    ser_state_t *state = ser_hw_get_state(port);
    uint8_t errors;
    uint16_t flags;

    flags = ser_hw_disable_int();

    /* Read hardware errors */
    errors = ser_hw_read_error_status(port);

    /* Merge into the flags already accumulated by the ISR (which may
     * hold SER_ERR_BUFOVFL and errors whose hardware bits were already
     * cleared by the error ISR) */
    if (errors & RR1_OVERRUN_ERR) state->error_flags |= SER_ERR_OVERRUN;
    if (errors & RR1_PARITY_ERR) state->error_flags |= SER_ERR_PARITY;
    if (errors & RR1_FRAME_ERR) state->error_flags |= SER_ERR_FRAMING;
    if (ser_hw_read_status(port) & RR0_BREAK) state->error_flags |= SER_ERR_BREAK;

    /* Reset hardware errors */
    ser_hw_reset_errors(port);

    errors = state->error_flags;
    state->error_flags = 0;

    ser_hw_restore_int(flags);

    return errors;
}

/**
 * Check CTS status.
 */
ser_bool_t ser_check_cts(uint8_t port) {
    return (ser_read_status_atomic(port) & RR0_CTS) ? SER_TRUE : SER_FALSE;
}

/**
 * Check DCD status.
 */
ser_bool_t ser_check_dcd(uint8_t port) {
    return (ser_read_status_atomic(port) & RR0_DCD) ? SER_TRUE : SER_FALSE;
}

/**
 * Set RTS line state.
 */
void ser_set_rts(uint8_t port, ser_bool_t active) {
    ser_state_t *state = ser_hw_get_state(port);
    uint16_t flags = ser_hw_disable_int();

    if (active) {
        state->cr5_shadow |= CR5_RTS;
    } else {
        state->cr5_shadow &= ~CR5_RTS;
    }

    ser_hw_update_cr5(port);

    ser_hw_restore_int(flags);
}

/**
 * Set DTR line state.
 */
void ser_set_dtr(uint8_t port, ser_bool_t active) {
    ser_state_t *state = ser_hw_get_state(port);
    uint16_t flags = ser_hw_disable_int();

    if (active) {
        state->cr5_shadow |= CR5_DTR;
    } else {
        state->cr5_shadow &= ~CR5_DTR;
    }

    ser_hw_update_cr5(port);

    ser_hw_restore_int(flags);
}

/**
 * Get current baud rate index.
 */
uint8_t ser_get_baud(uint8_t port) {
    return ser_hw_get_state(port)->baud_idx;
}

/**
 * Check whether TX is suspended by XON/XOFF flow control.
 */
ser_bool_t ser_tx_suspended(uint8_t port) {
    return ser_hw_get_state(port)->tx_suspended ? SER_TRUE : SER_FALSE;
}
