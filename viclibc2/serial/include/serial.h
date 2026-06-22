/**
 * @file serial.h
 * @brief Victor 9000 Serial Communications Library
 *
 * Low-level serial I/O for Victor 9000 using the SIO 7201 dual-channel
 * serial controller. Polled I/O only (no interrupts, no ring buffers): a
 * tight poll loop on the 8088 sustains clean RX to 38400, the async max.
 *
 * Hardware:
 *   - SIO 7201 (segment E004h) - Dual-channel serial controller
 *   - PIT 8253 (segment E002h) - Baud rate generator
 *   - VIA 6522 (segment E800h) - Clock selection
 *
 * Usage:
 *   ser_init();
 *   ser_set_baud(SER_PORT_A, SER_BAUD_9600);
 *   ser_poll_write(SER_PORT_A, 'H');
 *   int16_t c = ser_poll_read(SER_PORT_A);
 */

#ifndef SERIAL_H
#define SERIAL_H

/*===========================================================================
 * Standard Types (no libc)
 *===========================================================================*/

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned long  uint32_t;
typedef signed short   int16_t;

/*===========================================================================
 * Hardware Segments (Memory-Mapped I/O)
 *===========================================================================*/

#define SER_SIO_SEG     0xE004      /* SIO 7201 segment */
#define SER_PIT_SEG     0xE002      /* PIT 8253 segment */
#define SER_VIA_SEG     0xE800      /* VIA 6522 segment */

/*===========================================================================
 * Port Identifiers
 *===========================================================================*/

#define SER_PORT_A      0           /* Port A */
#define SER_PORT_B      1           /* Port B */

/*===========================================================================
 * Baud Rate Indices
 *===========================================================================*/

#define SER_BAUD_110    0
#define SER_BAUD_300    1
#define SER_BAUD_600    2
#define SER_BAUD_1200   3           /* Default */
#define SER_BAUD_2400   4
#define SER_BAUD_4800   5
#define SER_BAUD_9600   6
#define SER_BAUD_19200  7
#define SER_BAUD_38400  8
#define SER_BAUD_76800  9           /* PIT divisor 1 - hardware ceiling */
#define SER_BAUD_COUNT  10

/*===========================================================================
 * Data Format Constants
 *===========================================================================*/

/* Data bits */
#define SER_DATA_5      5
#define SER_DATA_6      6
#define SER_DATA_7      7
#define SER_DATA_8      8           /* Default */

/* Stop bits */
#define SER_STOP_1      1           /* Default */
#define SER_STOP_1_5    15          /* 1.5 stop bits (represented as 15) */
#define SER_STOP_2      2

/* Parity */
#define SER_PARITY_NONE 0           /* Default */
#define SER_PARITY_ODD  1
#define SER_PARITY_EVEN 2

/*===========================================================================
 * Flow Control Constants
 *===========================================================================*/

#define SER_FLOW_NONE   0           /* No flow control (default) */
#define SER_FLOW_XONOFF 2           /* Software XON/XOFF */

#define SER_XON         0x11        /* DC1 (Ctrl-Q) */
#define SER_XOFF        0x13        /* DC3 (Ctrl-S) */

/*===========================================================================
 * Error Flags (returned by ser_get_error)
 *===========================================================================*/

#define SER_ERR_NONE    0x00        /* No error */
#define SER_ERR_OVERRUN 0x01        /* Receiver overrun */
#define SER_ERR_PARITY  0x02        /* Parity error */
#define SER_ERR_FRAMING 0x04        /* Framing error */
#define SER_ERR_BREAK   0x08        /* Break detected */
#define SER_ERR_BUFOVFL 0x10        /* Software buffer overflow */

/*===========================================================================
 * Types
 *===========================================================================*/

/* Boolean return type: 0 = false/failure, non-zero = true/success */
typedef int ser_bool_t;

#define SER_FALSE   0
#define SER_TRUE    1

/* Port configuration structure */
typedef struct {
    uint8_t port;           /* SER_PORT_A or SER_PORT_B */
    uint8_t baud;           /* Baud rate index (SER_BAUD_*) */
    uint8_t data_bits;      /* 5-8 (default 8) */
    uint8_t stop_bits;      /* 1, 15=1.5, 2 (default 1) */
    uint8_t parity;         /* SER_PARITY_* (default NONE) */
    uint8_t flow_ctrl;      /* SER_FLOW_* (default NONE) */
} ser_config_t;

/* Port state structure (internal) */
typedef struct {
    uint8_t port;           /* Port identifier (0=A, 1=B) */
    uint8_t baud_idx;       /* Baud rate index */
    uint8_t data_bits;      /* Data bits (5-8) */
    uint8_t stop_bits;      /* Stop bits (1, 15=1.5, 2) */
    uint8_t parity;         /* Parity (0=none, 1=odd, 2=even) */
    uint8_t flow_ctrl;      /* Flow control mode */
    uint8_t error_flags;    /* Accumulated error flags */
    uint8_t cr5_shadow;     /* Shadow of CR5 (write-only register) */
} ser_state_t;

/*===========================================================================
 * Initialization Functions
 *===========================================================================*/

/**
 * Initialize serial subsystem (both ports to 8N1, 1200 baud).
 * Must be called before any other serial functions.
 */
void ser_init(void);

/**
 * Initialize a specific port with configuration.
 * @param config Port configuration
 * @return SER_TRUE on success, SER_FALSE on invalid parameters
 */
ser_bool_t ser_init_port(const ser_config_t *config);

/**
 * Shutdown serial subsystem (disables TX/RX on both ports).
 */
void ser_shutdown(void);

/*===========================================================================
 * Configuration Functions
 *===========================================================================*/

/**
 * Set baud rate for a port.
 * @param port SER_PORT_A or SER_PORT_B
 * @param baud_idx Baud rate index (SER_BAUD_*)
 * @return SER_TRUE on success, SER_FALSE on invalid index
 */
ser_bool_t ser_set_baud(uint8_t port, uint8_t baud_idx);

/**
 * Set data format (data bits, stop bits, parity).
 * @param port SER_PORT_A or SER_PORT_B
 * @param data_bits 5, 6, 7, or 8
 * @param stop_bits 1, 15 (1.5), or 2
 * @param parity SER_PARITY_NONE, SER_PARITY_ODD, or SER_PARITY_EVEN
 * @return SER_TRUE on success
 */
ser_bool_t ser_set_format(uint8_t port, uint8_t data_bits,
                          uint8_t stop_bits, uint8_t parity);

/**
 * Set flow control mode.
 * @param port SER_PORT_A or SER_PORT_B
 * @param flow_mode SER_FLOW_NONE or SER_FLOW_XONOFF
 * @return SER_TRUE on success
 */
ser_bool_t ser_set_flow(uint8_t port, uint8_t flow_mode);

/*===========================================================================
 * Hardware Status Functions
 *===========================================================================*/

/**
 * Check if receive data is available (non-blocking).
 * @param port SER_PORT_A or SER_PORT_B
 * @return SER_TRUE if data available, SER_FALSE if not
 */
ser_bool_t ser_rx_ready(uint8_t port);

/**
 * Check if transmitter is ready (non-blocking).
 * @param port SER_PORT_A or SER_PORT_B
 * @return SER_TRUE if ready to send, SER_FALSE if busy
 */
ser_bool_t ser_tx_ready(uint8_t port);

/*===========================================================================
 * Polled I/O Functions (no interrupts)
 *===========================================================================*/

/**
 * Polled, non-blocking read of one byte straight from the SIO receiver.
 * Does NOT use the ISR or RX ring buffer - reads the hardware directly.
 * Safe only when interrupt mode is NOT enabled on this port (the packet
 * layer's polled mode uses this). A tight poll loop on the 8088 keeps up
 * with the 3-byte FIFO well past 9600 - clean to 38400 (the async max).
 * @param port SER_PORT_A or SER_PORT_B
 * @return Received byte (0..255), or -1 if no byte is available
 */
int16_t ser_poll_read(uint8_t port);

/**
 * Polled, blocking send of one byte: spin until the transmit buffer is
 * empty, then write directly to the SIO. No ISR, no TX ring buffer.
 * @param port SER_PORT_A or SER_PORT_B
 * @param data Byte to send
 */
void ser_poll_write(uint8_t port, uint8_t data);

/**
 * Wait (polled) until the transmitter has fully drained - the last byte has
 * left the shift register (RR1 "All Sent"). Use before turning the line
 * around (e.g. after sending an ACK, before receiving) in polled mode.
 * @param port SER_PORT_A or SER_PORT_B
 */
void ser_poll_drain(uint8_t port);

/*===========================================================================
 * Status and Control Functions
 *===========================================================================*/

/**
 * Get and clear error flags.
 * @param port SER_PORT_A or SER_PORT_B
 * @return Error flags (SER_ERR_* ORed together)
 */
uint8_t ser_get_error(uint8_t port);

/**
 * Check Data Carrier Detect status.
 * @param port SER_PORT_A or SER_PORT_B
 * @return SER_TRUE if DCD active, SER_FALSE if not
 */
ser_bool_t ser_check_dcd(uint8_t port);

/**
 * Set Data Terminal Ready line state.
 * @param port SER_PORT_A or SER_PORT_B
 * @param active SER_TRUE to assert DTR, SER_FALSE to deassert
 */
void ser_set_dtr(uint8_t port, ser_bool_t active);

/**
 * Get current baud rate index.
 * @param port SER_PORT_A or SER_PORT_B
 * @return Baud rate index (SER_BAUD_*)
 */
uint8_t ser_get_baud(uint8_t port);

#endif /* SERIAL_H */
