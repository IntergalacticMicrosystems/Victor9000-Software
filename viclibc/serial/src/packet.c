/**
 * @file packet.c
 * @brief Victor 9000 Serial Packet Protocol Implementation
 *
 * Implements reliable packet-based communication with CRC-16
 * integrity checking and ACK/NAK acknowledgment protocol.
 */

#include "packet.h"
#include "serial.h"

/*===========================================================================
 * Internal Helper Functions
 *===========================================================================*/

/**
 * Read a byte from serial port (polled or interrupt-buffered).
 */
static int16_t pkt_read_byte(pkt_state_t *state) {
    if (state->polled) {
        return ser_poll_read(state->port);
    }
    return ser_int_read(state->port);
}

/**
 * Write a byte to serial port (polled blocking, or interrupt-buffered).
 */
static pkt_bool_t pkt_write_byte(pkt_state_t *state, uint8_t data) {
    if (state->polled) {
        ser_poll_write(state->port, data);
        return PKT_TRUE;
    }
    return ser_int_write(state->port, data);
}

/**
 * Wait for the transmitter to fully drain (so the line can be turned around).
 */
static void pkt_drain_tx(pkt_state_t *state) {
    if (state->polled) {
        ser_poll_drain(state->port);
    } else {
        ser_int_drain_tx(state->port);
    }
}

/**
 * Lost-edge kick, only meaningful in interrupt mode (no-op when polled).
 */
static void pkt_kick(pkt_state_t *state) {
    if (!state->polled) {
        ser_int_kick();
    }
}

/**
 * Read a byte with a bounded inter-byte timeout.
 *
 * Deliberately uses PKT_TIMEOUT_SHORT, not state->timeout: within a
 * frame the sender streams bytes back-to-back at line rate, so a long
 * stall means a byte was lost. Giving up quickly gets the NAK out
 * while the peer is still listening for it; the configurable
 * state->timeout governs sync acquisition and response waits.
 *
 * ser_int_kick() runs every 8th iteration: often enough to service the
 * SIO FIFO well within one character time even with a dead interrupt
 * chain, rare enough that it does not distort the loop calibration.
 */
static int16_t pkt_read_timeout(pkt_state_t *state) {
    uint32_t timeout = PKT_TIMEOUT_SHORT;
    int16_t data;

    (void)state;

    while (timeout > 0) {
        data = pkt_read_byte(state);
        if (data >= 0) {
            return data;
        }
        if ((timeout & 0x07) == 0) {
            pkt_kick(state);    /* self-heal a lost interrupt edge (no-op when polled) */
        }
        timeout--;
    }

    return -1;  /* Timeout */
}

/**
 * Send byte with escaping if needed.
 *
 * XON/XOFF are escaped as well: the receive ISR consumes them as flow
 * control characters (when SER_FLOW_XONOFF is active), so they must
 * never appear raw inside a packet.
 */
static void pkt_send_escaped(pkt_state_t *state, uint8_t data) {
    if (data == PKT_SYNC || data == PKT_ESC ||
        data == SER_XON || data == SER_XOFF) {
        pkt_write_byte(state, PKT_ESC);
        pkt_write_byte(state, data ^ PKT_ESC_XOR);
    } else {
        pkt_write_byte(state, data);
    }
}

/**
 * Map a negative pkt_recv_unescaped() result to a PKT_ERR_* code,
 * recording it in last_error. -1 is a timeout; -2 is a frame error
 * (unexpected SYNC mid-packet).
 */
static int16_t pkt_map_recv_err(pkt_state_t *state, int16_t err) {
    int16_t code = (err == -2) ? PKT_ERR_FRAME : PKT_ERR_TIMEOUT;
    state->last_error = (uint8_t)(-code);
    return code;
}

/**
 * Receive byte with unescaping.
 * @return Byte value, or negative on error/timeout
 */
static int16_t pkt_recv_unescaped(pkt_state_t *state) {
    int16_t data;

    data = pkt_read_timeout(state);
    if (data < 0) {
        return data;
    }

    if (data == PKT_SYNC) {
        /* Unexpected sync - new packet starting */
        return -2;  /* Frame error */
    }

    if (data == PKT_ESC) {
        data = pkt_read_timeout(state);
        if (data < 0) {
            return data;
        }
        data ^= PKT_ESC_XOR;
    }

    return data;
}

/*===========================================================================
 * CRC Functions
 *===========================================================================*/

/**
 * CRC-16-CCITT lookup table (polynomial 0x1021).
 * Pre-computed for efficiency on 8086.
 */
static const uint16_t crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0
};

/**
 * Calculate CRC-16-CCITT over a buffer.
 */
uint16_t pkt_crc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;

    while (len--) {
        crc = (crc << 8) ^ crc16_table[(crc >> 8) ^ *data++];
    }

    return crc;
}

/**
 * Update CRC with one byte.
 */
uint16_t pkt_crc16_byte(uint16_t crc, uint8_t data) {
    return (crc << 8) ^ crc16_table[(crc >> 8) ^ data];
}

/*===========================================================================
 * Initialization
 *===========================================================================*/

/**
 * Initialize packet protocol layer.
 * Automatically enables interrupt mode.
 */
void pkt_init(pkt_state_t *state, uint8_t port) {
    state->port = port;
    state->seq_tx = 0;
    state->seq_rx = 0;
    state->last_error = 0;
    state->timeout = PKT_TIMEOUT_LONG;
    state->retries = PKT_MAX_RETRIES;
    state->polled = 0;

    /* Always enable interrupt mode */
    ser_int_enable();
}

/**
 * Initialize the packet layer in polled mode (no interrupts). See packet.h.
 */
void pkt_init_polled(pkt_state_t *state, uint8_t port) {
    state->port = port;
    state->seq_tx = 0;
    state->seq_rx = 0;
    state->last_error = 0;
    state->timeout = PKT_TIMEOUT_LONG;
    state->retries = PKT_MAX_RETRIES;
    state->polled = 1;

    /* No ISR: byte I/O is polled straight from the SIO. Make sure RTS is
     * asserted so the peer may send to us. */
    ser_set_rts(port, SER_TRUE);
}

/**
 * Set timeout.
 */
void pkt_set_timeout(pkt_state_t *state, uint16_t timeout) {
    state->timeout = timeout;
}

/**
 * Set retries.
 */
void pkt_set_retries(pkt_state_t *state, uint8_t retries) {
    state->retries = retries;
}

/*===========================================================================
 * Low-Level Packet I/O
 *===========================================================================*/

/**
 * Send a raw packet.
 */
pkt_bool_t pkt_send_raw(pkt_state_t *state, uint8_t type,
                        const uint8_t *data, uint16_t len) {
    uint16_t crc;
    uint16_t i;

    if (len > PKT_MAX_PAYLOAD) {
        state->last_error = (uint8_t)(-PKT_ERR_OVERFLOW);
        return PKT_FALSE;
    }

    /* Calculate CRC over type + payload */
    crc = pkt_crc16_byte(0xFFFF, type);
    if (data != (void *)0 && len > 0) {
        for (i = 0; i < len; i++) {
            crc = pkt_crc16_byte(crc, data[i]);
        }
    }

    /* Send sync */
    pkt_write_byte(state, PKT_SYNC);

    /* Send length (little-endian, escaped) */
    pkt_send_escaped(state, len & 0xFF);
    pkt_send_escaped(state, (len >> 8) & 0xFF);

    /* Send type (escaped) */
    pkt_send_escaped(state, type);

    /* Send payload (escaped) */
    if (data != (void *)0) {
        for (i = 0; i < len; i++) {
            pkt_send_escaped(state, data[i]);
        }
    }

    /* Send CRC (little-endian, escaped) */
    pkt_send_escaped(state, crc & 0xFF);
    pkt_send_escaped(state, (crc >> 8) & 0xFF);

    /* Wait for TX to complete */
    pkt_drain_tx(state);

    return PKT_TRUE;
}

/**
 * Polled shallow unescaped read: one logical byte straight from the SIO.
 *
 * Used by the polled fast path in pkt_recv_raw. Unlike pkt_recv_unescaped(),
 * it calls ser_poll_read() directly (no pkt_read_byte/pkt_read_timeout/
 * pkt_kick layers) so the 38400 capture loop stays ahead of the 3-byte FIFO.
 *
 * @return byte value, -1 on inter-byte timeout, -2 on unexpected SYNC.
 */
static int16_t pkt_poll_unescaped(uint8_t port) {
    uint16_t guard = PKT_TIMEOUT_SHORT;
    int16_t b;

    while ((b = ser_poll_read(port)) < 0) {
        if (--guard == 0) {
            return -1;          /* dropped byte -> bail so the NAK goes out */
        }
    }
    if (b == PKT_SYNC) {
        return -2;              /* frame restart */
    }
    if (b == PKT_ESC) {
        guard = PKT_TIMEOUT_SHORT;
        while ((b = ser_poll_read(port)) < 0) {
            if (--guard == 0) {
                return -1;
            }
        }
        b ^= PKT_ESC_XOR;
    }
    return b;
}

/**
 * Receive a raw packet.
 */
int16_t pkt_recv_raw(pkt_state_t *state, uint8_t *type,
                     uint8_t *buf, uint16_t maxlen) {
    int16_t data;
    uint16_t len;
    uint16_t crc_recv, crc_calc;
    uint16_t i;

    if (state->polled) {
        /*
         * Polled fast path. At 38400 the sender streams the whole frame
         * back-to-back and the 7201 has only a 3-byte RX FIFO. The generic
         * path below reads each byte through four nested calls and folds it
         * into the CRC before fetching the next - more than one byte time
         * (~260us) on the 8088 - so the FIFO overruns mid-packet whenever the
         * sender leaves no inter-byte gap (e.g. a USB-serial adapter).
         *
         * Instead: read the length, then capture type+payload+CRC into a
         * buffer in one uniform loop that does nothing but poll, unescape and
         * store. CRC and field-splitting happen afterward, off the wire.
         */
        static uint8_t frame[PKT_MAX_PAYLOAD + 3];  /* type + payload + crc */
        uint8_t  port = state->port;
        uint16_t total, guard;

        /* 1. Length first (little-endian), before the capture loop. */
        data = pkt_poll_unescaped(port);
        if (data < 0) {
            return pkt_map_recv_err(state, data);
        }
        len = (uint16_t)data;
        data = pkt_poll_unescaped(port);
        if (data < 0) {
            return pkt_map_recv_err(state, data);
        }
        len |= ((uint16_t)data << 8);
        if (len > maxlen || len > PKT_MAX_PAYLOAD) {
            state->last_error = (uint8_t)(-PKT_ERR_OVERFLOW);
            return PKT_ERR_OVERFLOW;
        }

        /* 2. Capture type + payload + CRC (len + 3 bytes) - the hot loop.
         *    Poll the SIO and unescape inline: at 38400 even one wrapper call
         *    per byte is too slow for the 3-byte FIFO. (The length read above
         *    can afford the helper - only 2 bytes, absorbed by the FIFO.) */
        total = (uint16_t)(len + 3);
        for (i = 0; i < total; i++) {
            guard = PKT_TIMEOUT_SHORT;
            while ((data = ser_poll_read(port)) < 0) {
                if (--guard == 0) {
                    return pkt_map_recv_err(state, -1);
                }
            }
            if (data == PKT_ESC) {
                guard = PKT_TIMEOUT_SHORT;
                while ((data = ser_poll_read(port)) < 0) {
                    if (--guard == 0) {
                        return pkt_map_recv_err(state, -1);
                    }
                }
                data ^= PKT_ESC_XOR;
            } else if (data == PKT_SYNC) {
                return pkt_map_recv_err(state, -2);
            }
            frame[i] = (uint8_t)data;
        }

        /* 3. Split out type and CRC, copy payload, verify - off the wire. */
        *type = frame[0];
        crc_calc = pkt_crc16_byte(0xFFFF, frame[0]);
        for (i = 0; i < len; i++) {
            buf[i] = frame[i + 1];
            crc_calc = pkt_crc16_byte(crc_calc, buf[i]);
        }
        crc_recv = (uint16_t)frame[len + 1] | ((uint16_t)frame[len + 2] << 8);
        if (crc_recv != crc_calc) {
            state->last_error = (uint8_t)(-PKT_ERR_CRC);
            return PKT_ERR_CRC;
        }
        return (int16_t)len;
    }

    /* Read length (little-endian) */
    data = pkt_recv_unescaped(state);
    if (data < 0) {
        return pkt_map_recv_err(state, data);
    }
    len = data;

    data = pkt_recv_unescaped(state);
    if (data < 0) {
        return pkt_map_recv_err(state, data);
    }
    len |= ((uint16_t)data << 8);

    if (len > maxlen || len > PKT_MAX_PAYLOAD) {
        state->last_error = (uint8_t)(-PKT_ERR_OVERFLOW);
        return PKT_ERR_OVERFLOW;
    }

    /* Read type */
    data = pkt_recv_unescaped(state);
    if (data < 0) {
        return pkt_map_recv_err(state, data);
    }
    *type = (uint8_t)data;

    /* Start CRC calculation */
    crc_calc = pkt_crc16_byte(0xFFFF, *type);

    /* Read payload */
    for (i = 0; i < len; i++) {
        data = pkt_recv_unescaped(state);
        if (data < 0) {
            return pkt_map_recv_err(state, data);
        }
        buf[i] = (uint8_t)data;
        crc_calc = pkt_crc16_byte(crc_calc, buf[i]);
    }

    /* Read CRC (little-endian) */
    data = pkt_recv_unescaped(state);
    if (data < 0) {
        return pkt_map_recv_err(state, data);
    }
    crc_recv = data;

    data = pkt_recv_unescaped(state);
    if (data < 0) {
        return pkt_map_recv_err(state, data);
    }
    crc_recv |= ((uint16_t)data << 8);

    /* Verify CRC */
    if (crc_recv != crc_calc) {
        state->last_error = (uint8_t)(-PKT_ERR_CRC);
        return PKT_ERR_CRC;
    }

    return (int16_t)len;
}

/*===========================================================================
 * High-Level Packet I/O
 *===========================================================================*/

/**
 * Wait for sync byte.
 *
 * The timeout counts loop iterations regardless of whether a byte was
 * read, so a peer streaming non-sync garbage cannot stall us forever.
 */
pkt_bool_t pkt_wait_sync(pkt_state_t *state) {
    uint32_t timeout = state->timeout;
    int16_t data;

    while (timeout > 0) {
        data = pkt_read_byte(state);
        if (data == PKT_SYNC) {
            return PKT_TRUE;
        }
        if ((timeout & 0x07) == 0) {
            pkt_kick(state);    /* self-heal a lost interrupt edge (no-op when polled) */
        }
        timeout--;
    }

    return PKT_FALSE;
}

/**
 * Send ACK packet carrying the acknowledged sequence bit.
 */
pkt_bool_t pkt_send_ack(pkt_state_t *state, uint8_t seq) {
    uint8_t type = PKT_TYPE_ACK | (seq ? PKT_SEQ_BIT : 0);
    return pkt_send_raw(state, type, (void *)0, 0);
}

/**
 * Send NAK packet.
 */
pkt_bool_t pkt_send_nak(pkt_state_t *state) {
    return pkt_send_raw(state, PKT_TYPE_NAK, (void *)0, 0);
}

/**
 * Resynchronize sequence bits with the peer (connection hello).
 *
 * Zeroes the local sequence bits, then sends a RESET packet and waits
 * for the ACK. The responder zeroes its own sequence bits when it sees
 * the RESET inside pkt_receive(). Retransmits like pkt_send().
 */
pkt_bool_t pkt_send_reset(pkt_state_t *state) {
    uint8_t retries = state->retries;
    uint8_t type;
    uint8_t ack_buf[4];
    int16_t result;

    state->seq_tx = 0;
    state->seq_rx = 0;

    while (retries > 0) {
        if (!pkt_send_raw(state, PKT_TYPE_RESET, (void *)0, 0)) {
            return PKT_FALSE;
        }

        if (!pkt_wait_sync(state)) {
            retries--;
            continue;
        }

        result = pkt_recv_raw(state, &type, ack_buf, sizeof(ack_buf));
        if (result < 0) {
            retries--;
            continue;
        }

        if ((type & PKT_TYPE_MASK) == PKT_TYPE_ACK) {
            return PKT_TRUE;
        }

        retries--;
    }

    state->last_error = (uint8_t)(-PKT_ERR_RETRIES);
    return PKT_FALSE;
}

/**
 * Send data packet with acknowledgment.
 *
 * The DATA packet carries the current TX sequence bit; only an ACK
 * carrying the same bit completes the send. This lets the receiver
 * detect (and not re-deliver) retransmissions caused by a lost ACK.
 */
int16_t pkt_send(pkt_state_t *state, const uint8_t *data, uint16_t len) {
    uint8_t retries = state->retries;
    uint8_t type;
    uint8_t tx_type;
    uint8_t ack_buf[4];
    int16_t result;

    tx_type = PKT_TYPE_DATA | (state->seq_tx ? PKT_SEQ_BIT : 0);

    /* Polled mode: assert RTS so the peer's ACK (it honors our CTS) can reach
     * us. A prior pkt_receive may have left RTS deasserted to pause the peer
     * during the caller's between-packet work. */
    if (state->polled) {
        ser_set_rts(state->port, SER_TRUE);
    }

    while (retries > 0) {
        /* Send the data packet */
        if (!pkt_send_raw(state, tx_type, data, len)) {
            return -(int16_t)state->last_error;
        }

        /* Wait for response */
        if (!pkt_wait_sync(state)) {
            retries--;
            continue;
        }

        /* Receive response packet */
        result = pkt_recv_raw(state, &type, ack_buf, sizeof(ack_buf));

        if (result < 0) {
            retries--;
            continue;
        }

        if ((type & PKT_TYPE_MASK) == PKT_TYPE_ACK) {
            if (((type & PKT_SEQ_BIT) ? 1 : 0) == state->seq_tx) {
                state->seq_tx ^= 1;
                return PKT_TRUE;    /* Success */
            }
            /* Stale ACK for a previous packet - resend */
            retries--;
            continue;
        }

        if ((type & PKT_TYPE_MASK) == PKT_TYPE_NAK) {
            retries--;
            continue;
        }

        /* Unexpected packet type */
        retries--;
    }

    state->last_error = (uint8_t)(-PKT_ERR_RETRIES);
    return PKT_ERR_RETRIES;
}

/**
 * Receive data packet with acknowledgment.
 *
 * Duplicates (retransmissions after a lost ACK) are re-ACKed but not
 * delivered; reception continues until fresh data or timeout.
 */
int16_t pkt_receive(pkt_state_t *state, uint8_t *buf, uint16_t maxlen) {
    uint8_t type;
    uint8_t seq;
    int16_t result;

    /* Polled mode: assert RTS while we are actively receiving (the peer,
     * honoring our CTS, may send). We deassert it again the instant we have
     * ACKed fresh data and are about to hand control back to the caller, so
     * the caller's between-packet work (e.g. a disk write in putfile) happens
     * with the peer paused - the 3-byte FIFO cannot overflow while we are not
     * polling. */
    if (state->polled) {
        ser_set_rts(state->port, SER_TRUE);
    }

    for (;;) {
        /* Wait for sync */
        if (!pkt_wait_sync(state)) {
            state->last_error = (uint8_t)(-PKT_ERR_TIMEOUT);
            return PKT_ERR_TIMEOUT;
        }

        /* Receive packet */
        result = pkt_recv_raw(state, &type, buf, maxlen);

        if (result < 0) {
            /* Error - send NAK */
            pkt_send_nak(state);
            return result;
        }

        if ((type & PKT_TYPE_MASK) == PKT_TYPE_RESET) {
            /* Connection hello: peer (re)connected with fresh state.
             * Resync both sequence bits and ACK; keep waiting for data. */
            state->seq_rx = 0;
            state->seq_tx = 0;
            pkt_send_ack(state, 0);
            continue;
        }

        if ((type & PKT_TYPE_MASK) != PKT_TYPE_DATA) {
            /* Not a data packet */
            state->last_error = (uint8_t)(-PKT_ERR_FRAME);
            return PKT_ERR_FRAME;
        }

        seq = (type & PKT_SEQ_BIT) ? 1 : 0;

        /* ACK with the sender's sequence bit (also re-ACKs duplicates,
         * which stops the sender's retransmissions) */
        pkt_send_ack(state, seq);

        if (seq == state->seq_rx) {
            /* Fresh data - advance expected sequence and deliver. In polled
             * mode pause the peer (deassert RTS) now that the ACK is fully on
             * the wire, so it cannot send more while the caller processes this
             * packet (disk write) and we are not polling. The next pkt_send/
             * pkt_receive reasserts RTS. */
            state->seq_rx ^= 1;
            if (state->polled) {
                ser_set_rts(state->port, SER_FALSE);
            }
            return result;
        }

        /* Duplicate of already-delivered data - keep waiting */
    }
}

/*===========================================================================
 * Utility Functions
 *===========================================================================*/

/**
 * Get last error code.
 */
uint8_t pkt_get_error(pkt_state_t *state) {
    return state->last_error;
}

/**
 * Clear error state.
 */
void pkt_clear_error(pkt_state_t *state) {
    state->last_error = 0;
}

/**
 * Flush pending data.
 */
void pkt_flush(pkt_state_t *state) {
    int16_t data;

    /* Drain any pending data */
    do {
        data = pkt_read_byte(state);
    } while (data >= 0);

    /* Clear the ISR RX ring (none exists in polled mode). */
    if (!state->polled) {
        ser_int_flush_rx(state->port);
    }
}
