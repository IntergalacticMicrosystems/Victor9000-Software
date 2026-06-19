/**
 * @file packet.h
 * @brief Victor 9000 Serial Packet Protocol
 *
 * Reliable packet-based communication protocol with framing,
 * CRC-16 integrity checking, and acknowledgment/retry mechanism.
 *
 * Packet Format:
 * +------+--------+------+---------+------+
 * | SYNC | LENGTH | TYPE | PAYLOAD | CRC  |
 * +------+--------+------+---------+------+
 *    1       2       1     0-1024    2
 *
 * SYNC:   0x7E (framing byte; 0x7E/0x7D/XON/XOFF are escaped everywhere
 *         after the SYNC as 0x7D followed by the byte XOR 0x20)
 * LENGTH: 16-bit payload length (little-endian, max 1024)
 * TYPE:   Packet type (DATA, ACK, NAK) in bits 0-6; bit 7 carries a
 *         1-bit alternating sequence number on DATA and ACK packets so
 *         retransmitted data (lost ACK) is not delivered twice
 * CRC:    CRC-16-CCITT of TYPE+PAYLOAD (TYPE including sequence bit)
 */

#ifndef PACKET_H
#define PACKET_H

#include "serial.h"

/*===========================================================================
 * Constants
 *===========================================================================*/

/* Framing characters */
#define PKT_SYNC        0x7E    /* Start of packet */
#define PKT_ESC         0x7D    /* Escape character */
#define PKT_ESC_XOR     0x20    /* XOR to escape/unescape */

/* Packet types (bits 0-6 of the TYPE byte) */
#define PKT_TYPE_DATA   0x01    /* Data packet */
#define PKT_TYPE_ACK    0x02    /* Acknowledgment */
#define PKT_TYPE_NAK    0x03    /* Negative acknowledgment */
#define PKT_TYPE_RESET  0x04    /* Sequence-bit resync (connection hello) */

/* Bit 7 of the TYPE byte: alternating sequence bit (DATA and ACK) */
#define PKT_SEQ_BIT     0x80
#define PKT_TYPE_MASK   0x7F

/* Maximum payload size */
#define PKT_MAX_PAYLOAD 1024

/* Timeout values (in loop iterations, adjust for CPU speed) */
#define PKT_TIMEOUT_SHORT   5000    /* ~100ms at 5MHz */
#define PKT_TIMEOUT_LONG    50000   /* ~1s at 5MHz */

/* Retry counts */
#define PKT_MAX_RETRIES     3

/* Error codes (negative return values) */
#define PKT_ERR_TIMEOUT     -1      /* Receive timeout */
#define PKT_ERR_CRC         -2      /* CRC mismatch */
#define PKT_ERR_FRAME       -3      /* Framing error */
#define PKT_ERR_OVERFLOW    -4      /* Packet too large */
#define PKT_ERR_NAK         -5      /* NAK received */
#define PKT_ERR_RETRIES     -6      /* Max retries exceeded */

/*===========================================================================
 * Types
 *===========================================================================*/

/* Boolean type */
typedef int pkt_bool_t;
#define PKT_FALSE   0
#define PKT_TRUE    1

/* Packet state */
typedef struct {
    uint8_t port;               /* Serial port (SER_PORT_A or SER_PORT_B) */
    uint8_t seq_tx;             /* TX sequence number */
    uint8_t seq_rx;             /* Expected RX sequence number */
    uint8_t last_error;         /* Last error code (positive) */
    uint16_t timeout;           /* Timeout setting (loop iterations) */
    uint8_t retries;            /* Retry count */
    uint8_t polled;             /* 1 = polled byte I/O (no ISR); 0 = interrupt */
} pkt_state_t;

/*===========================================================================
 * CRC Functions
 *===========================================================================*/

/**
 * Calculate CRC-16-CCITT.
 * @param data Pointer to data
 * @param len Length of data
 * @return 16-bit CRC value
 *
 * Polynomial: 0x1021
 * Initial value: 0xFFFF
 */
uint16_t pkt_crc16(const uint8_t *data, uint16_t len);

/**
 * Update CRC with one byte.
 * @param crc Current CRC value
 * @param data Byte to add
 * @return Updated CRC value
 */
uint16_t pkt_crc16_byte(uint16_t crc, uint8_t data);

/*===========================================================================
 * Initialization
 *===========================================================================*/

/**
 * Initialize packet protocol layer.
 * Automatically enables interrupt mode.
 * @param state Pointer to packet state structure
 * @param port Serial port (SER_PORT_A or SER_PORT_B)
 */
void pkt_init(pkt_state_t *state, uint8_t port);

/**
 * Initialize the packet layer in POLLED mode (no interrupts).
 *
 * Byte I/O goes straight to the SIO via ser_poll_*; the SIO interrupt is
 * never installed/unmasked (so do NOT call ser_int_enable on this port).
 * A tight poll loop on the 8088 sustains clean RX far past the interrupt
 * path's 9600 ceiling - to 38400 (the async max). For the receiving side
 * of a bulk transfer (putfile), polled mode also gates the peer with RTS
 * around the caller's between-packet work (the disk write) - it deasserts
 * RTS immediately after ACKing fresh data and reasserts on the next
 * receive - so the peer (honoring CTS) cannot overflow the 3-byte FIFO
 * while we are not polling. The peer must have CTS hardware flow control
 * enabled (the Pico's UART1 does).
 *
 * @param state Pointer to packet state structure
 * @param port Serial port (SER_PORT_A or SER_PORT_B)
 */
void pkt_init_polled(pkt_state_t *state, uint8_t port);

/**
 * Set timeout for packet operations.
 * Governs sync acquisition and response waits. The inter-byte gap
 * within a frame is fixed at PKT_TIMEOUT_SHORT so a lost byte draws a
 * NAK while the peer is still listening for it.
 * @param state Packet state
 * @param timeout Timeout in loop iterations
 */
void pkt_set_timeout(pkt_state_t *state, uint16_t timeout);

/**
 * Set maximum retry count.
 * @param state Packet state
 * @param retries Maximum retries
 */
void pkt_set_retries(pkt_state_t *state, uint8_t retries);

/*===========================================================================
 * Low-Level Packet I/O
 *===========================================================================*/

/**
 * Send a raw packet (no ACK waiting).
 * @param state Packet state
 * @param type Packet type (PKT_TYPE_*)
 * @param data Payload data (may be NULL if len=0)
 * @param len Payload length
 * @return PKT_TRUE on success, PKT_FALSE on error
 */
pkt_bool_t pkt_send_raw(pkt_state_t *state, uint8_t type,
                        const uint8_t *data, uint16_t len);

/**
 * Receive a raw packet (no ACK sending).
 * @param state Packet state
 * @param type Pointer to receive packet type
 * @param buf Buffer for payload
 * @param maxlen Maximum payload length
 * @return Payload length on success, negative error code on failure
 */
int16_t pkt_recv_raw(pkt_state_t *state, uint8_t *type,
                     uint8_t *buf, uint16_t maxlen);

/*===========================================================================
 * High-Level Packet I/O (with ACK/NAK)
 *===========================================================================*/

/**
 * Send data packet with acknowledgment.
 * Waits for ACK/NAK and retries on NAK or timeout.
 * @param state Packet state
 * @param data Payload data
 * @param len Payload length
 * @return PKT_TRUE on success (ACK received), negative error code on failure
 */
int16_t pkt_send(pkt_state_t *state, const uint8_t *data, uint16_t len);

/**
 * Receive data packet with acknowledgment.
 * Sends ACK on successful receive, NAK on CRC error.
 * @param state Packet state
 * @param buf Buffer for payload
 * @param maxlen Maximum payload length
 * @return Payload length on success, negative error code on failure
 */
int16_t pkt_receive(pkt_state_t *state, uint8_t *buf, uint16_t maxlen);

/**
 * Send ACK packet.
 * @param state Packet state
 * @param seq Sequence bit of the packet being acknowledged (0 or 1)
 * @return PKT_TRUE on success
 */
pkt_bool_t pkt_send_ack(pkt_state_t *state, uint8_t seq);

/**
 * Send NAK packet.
 * @param state Packet state
 * @return PKT_TRUE on success
 */
pkt_bool_t pkt_send_nak(pkt_state_t *state);

/**
 * Resynchronize sequence bits with the peer (connection hello).
 *
 * Sequence bits persist for the life of a pkt_state_t, so a peer that
 * reconnects with fresh state (seq 0) while this side's seq_rx is odd
 * would have its first DATA packet mistaken for a duplicate. The
 * initiator calls pkt_send_reset() once after (re)connecting: it zeroes
 * the local sequence bits, sends a RESET packet, and waits for the ACK.
 * The responder side handles an incoming RESET transparently inside
 * pkt_receive() (zeroes its own sequence bits and ACKs).
 *
 * @param state Packet state
 * @return PKT_TRUE if the RESET was acknowledged
 */
pkt_bool_t pkt_send_reset(pkt_state_t *state);

/*===========================================================================
 * Utility Functions
 *===========================================================================*/

/**
 * Get last error code.
 * @param state Packet state
 * @return Last error code (positive value)
 */
uint8_t pkt_get_error(pkt_state_t *state);

/**
 * Clear error state.
 * @param state Packet state
 */
void pkt_clear_error(pkt_state_t *state);

/**
 * Flush pending data from serial port.
 * @param state Packet state
 */
void pkt_flush(pkt_state_t *state);

/**
 * Wait for sync byte (with timeout).
 * @param state Packet state
 * @return PKT_TRUE if sync found, PKT_FALSE on timeout
 */
pkt_bool_t pkt_wait_sync(pkt_state_t *state);

#endif /* PACKET_H */
