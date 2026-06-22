/**
 * @file serial.c
 * @brief Victor 9000 Serial Communications Library - Core Implementation
 *
 * Main API implementation for serial communications on the Victor 9000.
 * Polled I/O only (no interrupts, no ring buffers).
 */

#include "serial.h"
#include <i86.h>

/*===========================================================================
 * External Declarations (from serial_hw.c)
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
extern uint8_t ser_hw_read_status(uint8_t port);
extern uint8_t ser_hw_read_error_status(uint8_t port);
extern void ser_hw_reset_errors(uint8_t port);
extern void ser_hw_set_baud_divisor(uint8_t port, uint16_t divisor);
extern uint16_t ser_hw_get_baud_divisor(uint8_t baud_idx);
extern void ser_hw_config_bus(void);
extern void ser_hw_config_receiver(uint8_t port, uint8_t data_bits);
extern void ser_hw_config_mode(uint8_t port, uint8_t stop_bits, uint8_t parity);
extern void ser_hw_config_transmitter(uint8_t port, uint8_t data_bits,
                                       ser_bool_t dtr, ser_bool_t rts);
extern void ser_hw_update_cr5(uint8_t port);
extern uint16_t ser_hw_disable_int(void);
extern void ser_hw_restore_int(uint16_t flags);

/*===========================================================================
 * Status Register Bit Definitions
 *===========================================================================*/

#define RR0_RX_AVAIL    0x01
#define RR0_TX_EMPTY    0x04
#define RR0_DCD         0x08
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

    /* Configure Port B: 8N1, DTR/RTS active */
    ser_hw_config_mode(SER_PORT_B, SER_STOP_1, SER_PARITY_NONE);
    ser_hw_config_receiver(SER_PORT_B, SER_DATA_8);
    ser_hw_config_transmitter(SER_PORT_B, SER_DATA_8, SER_TRUE, SER_TRUE);

    /* Initialize state structures */
    ser_state_a.baud_idx = SER_BAUD_1200;
    ser_state_a.data_bits = SER_DATA_8;
    ser_state_a.stop_bits = SER_STOP_1;
    ser_state_a.parity = SER_PARITY_NONE;
    ser_state_a.flow_ctrl = SER_FLOW_NONE;
    ser_state_a.error_flags = 0;

    ser_state_b.baud_idx = SER_BAUD_1200;
    ser_state_b.data_bits = SER_DATA_8;
    ser_state_b.stop_bits = SER_STOP_1;
    ser_state_b.parity = SER_PARITY_NONE;
    ser_state_b.flow_ctrl = SER_FLOW_NONE;
    ser_state_b.error_flags = 0;

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

    /* Update state */
    state->baud_idx = config->baud;
    state->data_bits = config->data_bits;
    state->stop_bits = config->stop_bits;
    state->parity = config->parity;
    state->flow_ctrl = config->flow_ctrl;
    state->error_flags = 0;

    _enable();

    return SER_TRUE;
}

/**
 * Shutdown serial subsystem.
 */
void ser_shutdown(void) {
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
 * Check DCD status.
 */
ser_bool_t ser_check_dcd(uint8_t port) {
    return (ser_read_status_atomic(port) & RR0_DCD) ? SER_TRUE : SER_FALSE;
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
