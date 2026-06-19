/**
 * @file serial_buffer.c
 * @brief Circular Buffer Implementation for Interrupt-Driven Serial I/O
 *
 * Provides SER_BUF_SIZE-byte circular buffers for interrupt-driven serial
 * I/O. Four buffers total: RX and TX for each of the two ports.
 *
 * The "safe" functions save and restore the caller's interrupt flag
 * (rather than unconditionally enabling interrupts) so they may be
 * called from inside a critical section without breaking it.
 */

#include "serial.h"
#include <i86.h>

/* Interrupt flag save/restore (from serial_hw.c) */
extern uint16_t ser_hw_disable_int(void);
extern void ser_hw_restore_int(uint16_t flags);

/*===========================================================================
 * Buffer Instances
 *===========================================================================*/

/* Port A buffers */
ser_buffer_t ser_rx_buf_a = { {0}, 0, 0, 0 };
ser_buffer_t ser_tx_buf_a = { {0}, 0, 0, 0 };

/* Port B buffers */
ser_buffer_t ser_rx_buf_b = { {0}, 0, 0, 0 };
ser_buffer_t ser_tx_buf_b = { {0}, 0, 0, 0 };

/*===========================================================================
 * Buffer Access Functions
 *===========================================================================*/

/**
 * Get RX buffer for a port.
 */
ser_buffer_t *ser_buf_get_rx(uint8_t port) {
    return (port == SER_PORT_A) ? &ser_rx_buf_a : &ser_rx_buf_b;
}

/**
 * Get TX buffer for a port.
 */
ser_buffer_t *ser_buf_get_tx(uint8_t port) {
    return (port == SER_PORT_A) ? &ser_tx_buf_a : &ser_tx_buf_b;
}

/**
 * Clear (flush) a buffer.
 */
void ser_buf_clear(ser_buffer_t *buf) {
    uint16_t flags = ser_hw_disable_int();
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
    ser_hw_restore_int(flags);
}

/**
 * Put a byte into a buffer.
 * @param buf Buffer to write to
 * @param data Byte to store
 * @return SER_TRUE if stored, SER_FALSE if buffer full
 *
 * Note: This function does NOT disable interrupts.
 * Caller must ensure atomicity if called from main code.
 */
ser_bool_t ser_buf_put(ser_buffer_t *buf, uint8_t data) {
    if (buf->count >= SER_BUF_SIZE) {
        return SER_FALSE;   /* Buffer full */
    }

    buf->data[buf->head] = data;
    buf->head = (buf->head + 1) & SER_BUF_MASK;
    buf->count++;

    return SER_TRUE;
}

/**
 * Get a byte from a buffer.
 * @param buf Buffer to read from
 * @return Byte read, or -1 if buffer empty
 *
 * Note: This function does NOT disable interrupts.
 * Caller must ensure atomicity if called from main code.
 */
int16_t ser_buf_get(ser_buffer_t *buf) {
    uint8_t data;

    if (buf->count == 0) {
        return -1;          /* Buffer empty */
    }

    data = buf->data[buf->tail];
    buf->tail = (buf->tail + 1) & SER_BUF_MASK;
    buf->count--;

    return data;
}

/**
 * Put a byte into a buffer (interrupt-safe version for main code).
 */
ser_bool_t ser_buf_put_safe(ser_buffer_t *buf, uint8_t data) {
    ser_bool_t result;
    uint16_t flags = ser_hw_disable_int();

    result = ser_buf_put(buf, data);

    ser_hw_restore_int(flags);

    return result;
}

/**
 * Get a byte from a buffer (interrupt-safe version for main code).
 */
int16_t ser_buf_get_safe(ser_buffer_t *buf) {
    int16_t result;
    uint16_t flags = ser_hw_disable_int();

    result = ser_buf_get(buf);

    ser_hw_restore_int(flags);

    return result;
}

/**
 * Get bytes available in buffer.
 */
uint16_t ser_buf_count(ser_buffer_t *buf) {
    uint16_t count;
    uint16_t flags = ser_hw_disable_int();

    count = buf->count;

    ser_hw_restore_int(flags);

    return count;
}

/**
 * Get space available in buffer.
 */
uint16_t ser_buf_free(ser_buffer_t *buf) {
    uint16_t free;
    uint16_t flags = ser_hw_disable_int();

    free = SER_BUF_SIZE - buf->count;

    ser_hw_restore_int(flags);

    return free;
}

/**
 * Check if buffer is empty.
 */
ser_bool_t ser_buf_is_empty(ser_buffer_t *buf) {
    return (buf->count == 0) ? SER_TRUE : SER_FALSE;
}

/**
 * Check if buffer is full.
 */
ser_bool_t ser_buf_is_full(ser_buffer_t *buf) {
    return (buf->count >= SER_BUF_SIZE) ? SER_TRUE : SER_FALSE;
}

/**
 * Peek at next byte without removing it.
 * @param buf Buffer to peek at
 * @return Byte at tail, or -1 if empty
 */
int16_t ser_buf_peek(ser_buffer_t *buf) {
    if (buf->count == 0) {
        return -1;
    }
    return buf->data[buf->tail];
}

/**
 * Put multiple bytes into buffer.
 * @param buf Buffer to write to
 * @param data Source data
 * @param len Number of bytes to write
 * @return Number of bytes actually written
 */
uint16_t ser_buf_put_block(ser_buffer_t *buf, const uint8_t *data, uint16_t len) {
    uint16_t written = 0;
    uint16_t flags = ser_hw_disable_int();

    while (len > 0 && buf->count < SER_BUF_SIZE) {
        buf->data[buf->head] = *data++;
        buf->head = (buf->head + 1) & SER_BUF_MASK;
        buf->count++;
        len--;
        written++;
    }

    ser_hw_restore_int(flags);

    return written;
}

/**
 * Get multiple bytes from buffer.
 * @param buf Buffer to read from
 * @param data Destination buffer
 * @param len Maximum bytes to read
 * @return Number of bytes actually read
 */
uint16_t ser_buf_get_block(ser_buffer_t *buf, uint8_t *data, uint16_t len) {
    uint16_t read = 0;
    uint16_t flags = ser_hw_disable_int();

    while (len > 0 && buf->count > 0) {
        *data++ = buf->data[buf->tail];
        buf->tail = (buf->tail + 1) & SER_BUF_MASK;
        buf->count--;
        len--;
        read++;
    }

    ser_hw_restore_int(flags);

    return read;
}

/**
 * Initialize all buffers (clear them).
 */
void ser_buf_init_all(void) {
    ser_buf_clear(&ser_rx_buf_a);
    ser_buf_clear(&ser_tx_buf_a);
    ser_buf_clear(&ser_rx_buf_b);
    ser_buf_clear(&ser_tx_buf_b);
}
