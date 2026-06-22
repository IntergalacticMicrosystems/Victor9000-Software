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
 * Read a byte from the serial port (polled, non-blocking).
 */
static int16_t pkt_read_byte(pkt_state_t *state) {
    return ser_poll_read(state->port);
}

/**
 * Write a byte to the serial port (polled, blocking).
 */
static pkt_bool_t pkt_write_byte(pkt_state_t *state, uint8_t data) {
    ser_poll_write(state->port, data);
    return PKT_TRUE;
}

/**
 * Wait for the transmitter to fully drain (so the line can be turned around).
 */
static void pkt_drain_tx(pkt_state_t *state) {
    ser_poll_drain(state->port);
}

/**
 * Send byte with escaping if needed.
 *
 * XON/XOFF are escaped as well so they can never appear raw inside a
 * packet (a peer running software flow control would otherwise consume
 * them).
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
 * Map a negative pkt_poll_unescaped() result to a PKT_ERR_* code,
 * recording it in last_error. -1 is a timeout; -2 is a frame error
 * (unexpected SYNC mid-packet).
 */
static int16_t pkt_map_recv_err(pkt_state_t *state, int16_t err) {
    int16_t code = (err == -2) ? PKT_ERR_FRAME : PKT_ERR_TIMEOUT;
    state->last_error = (uint8_t)(-code);
    return code;
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
 * Initialize the packet protocol layer (polled byte I/O). See packet.h.
 */
void pkt_init(pkt_state_t *state, uint8_t port) {
    state->port = port;
    state->seq_tx = 0;
    state->seq_rx = 0;
    state->last_error = 0;
    state->timeout = PKT_TIMEOUT_LONG;
    state->retries = PKT_MAX_RETRIES;
}

/**
 * Alias for pkt_init(), retained for source compatibility.
 */
void pkt_init_polled(pkt_state_t *state, uint8_t port) {
    pkt_init(state, port);
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
 * Used to read the length field at the head of pkt_recv_raw. Calls
 * ser_poll_read() directly so the 38400 capture loop stays ahead of the
 * 3-byte FIFO.
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

#ifdef PKT_PROFILE_GUARD
/*
 * Per-byte wait profile for the pkt_recv_raw hot loop. pkt_guard_log[i] is the
 * guard value left when frame byte i arrived: PKT_TIMEOUT_SHORT means the byte
 * was already sitting in the FIFO (no wait); a smaller value means the loop
 * spun (PKT_TIMEOUT_SHORT - guard) times polling for it. pkt_guard_n is how
 * many entries the last pkt_recv_raw call filled. Both are globals so a test
 * harness can dump them after a receive. Enabled only when PKT_PROFILE_GUARD
 * is defined, because the extra per-byte store perturbs the very timing it
 * measures and would slow the production hot loop.
 */
uint16_t pkt_guard_log[PKT_MAX_PAYLOAD + 3];
uint16_t pkt_guard_n = 0;
#endif

/**
 * Receive a raw packet.
 */
int16_t pkt_recv_raw(pkt_state_t *state, uint8_t *type,
                     uint8_t *buf, uint16_t maxlen) {
    int16_t data;
    uint16_t len;
    uint16_t crc_recv, crc_calc;
    static uint8_t frame[PKT_MAX_PAYLOAD + 3];  /* type + payload + crc */
    uint8_t  port = state->port;
    uint16_t total;

    /*
     * At 38400 the sender streams the whole frame back-to-back and the
     * 7201 has only a 3-byte RX FIFO. Reading each byte through nested
     * helper calls and folding it into the CRC before fetching the next
     * takes more than one byte time (~260us) on the 8088, so the FIFO
     * overruns mid-packet whenever the sender leaves no inter-byte gap
     * (e.g. a USB-serial adapter).
     *
     * Instead: read the length, then capture type+payload+CRC into a
     * buffer in one uniform loop that does nothing but poll, unescape and
     * store. CRC and field-splitting happen afterward, off the wire.
     */

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

#if 0   /* ---- Original C capture loop --------------------------------------
         * Superseded by the inline-asm version below, which polls and reads the
         * SIO directly instead of calling ser_poll_read() once per byte. Kept
         * verbatim for reference / A-B testing: flip this to #if 1 and delete
         * the _asm block to restore it. ------------------------------------- */
    {
        uint16_t guard;
        uint16_t i;
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
#ifdef PKT_PROFILE_GUARD
            /* Record after the byte is stored, so an escaped byte logs the
             * guard of the (second) poll that produced the stored value - the
             * true worst-case wait for that frame position. */
            pkt_guard_log[i] = guard;
#endif
        }
    }
#endif  /* ---- end original C capture loop ----------------------------------- */

    /* 2a. Inline-asm capture loop (direct uPD7201 MMIO). Equivalent to the C
     *     loop above, but reads the SIO straight from registers instead of a
     *     per-byte ser_poll_read() call - the per-byte budget is what the 3-byte
     *     FIFO cannot afford at 38400. Tuned for the 8088's 4-byte prefetch
     *     queue and 8-bit bus, where every taken jump flushes the queue:
     *       - common path falls THROUGH (esc/sync are out of line), and a ready
     *         byte falls through the poll (jz to spin only when NOT ready), so
     *         the hot path has a single taken jump (the loop-back);
     *       - DS points at the SIO segment, so the high-frequency poll is a bare
     *         [si] read with no segment-override prefix to fetch;
     *       - frame is walked with a pointer in BX (no index*disp address calc).
     *     Register use:
     *       DS = SIO segment (0xE004); poll = [si] (ctrl), data = [si-2]
     *       ES = DGROUP; frame store = es:[bx], guard log = es:pkt_guard_log[di]
     *       SI = control-reg offset (port + 2)
     *       BX = frame write pointer (&frame[0] .. &frame[total])
     *       DX = frame end pointer (&frame[total]) - loop bound
     *       CX = guard countdown (PKT_TIMEOUT_SHORT..0) - the kept guard count
     *       DI = byte offset into pkt_guard_log (2*i), profiling build only
     *     rc_kind carries the C-visible outcome: 0 ok, -1 inter-byte timeout,
     *     -2 unescaped SYNC (frame restart). The RR0 pointer is already 0 from
     *     the length read (ser_poll_read leaves it there), so no re-select.
     *     SS is untouched, so the C locals (port/total/rc_kind, BP-relative)
     *     are still reachable while DS is repointed at the SIO. */
    {
        int16_t rc_kind = 0;
        _asm {
            push ds
            push si
            push di
            push es
            mov  ax, ds
            mov  es, ax                 /* ES = DGROUP (frame + guard log) */
            lea  bx, frame              /* BX = &frame[0] */
            mov  dx, total
            add  dx, bx                 /* DX = &frame[total] (loop bound) */
            mov  al, port
            xor  ah, ah
            add  ax, 2
            mov  si, ax                 /* SI = ctrl offset; data = [si-2] */
            mov  ax, 0E004h             /* SER_SIO_SEG */
            mov  ds, ax                 /* DS = SIO (poll/read, no override) */
#ifdef PKT_PROFILE_GUARD
            xor  di, di                 /* DI = 2*i into pkt_guard_log */
#endif
        next_byte:
            mov  cx, 5000               /* PKT_TIMEOUT_SHORT */
        poll1:
            test byte ptr [si], 1       /* RR0 & RR0_RX_AVAIL (DS:ctrl) */
            jz   spin1                  /* not ready -> spin (rare when keeping up) */
            mov  al, byte ptr [si-2]    /* DS:data */
            cmp  al, 125                /* PKT_ESC  (0x7D) */
            je   do_esc
            cmp  al, 126                /* PKT_SYNC (0x7E) */
            je   do_sync
        store:
            mov  byte ptr es:[bx], al   /* frame[i] = al */
#ifdef PKT_PROFILE_GUARD
            mov  word ptr es:pkt_guard_log[di], cx
            add  di, 2
#endif
            inc  bx
            cmp  bx, dx
            jb   next_byte
            jmp  done
        spin1:
            dec  cx
            jnz  poll1
            mov  word ptr rc_kind, -1   /* inter-byte timeout */
            jmp  done
        do_esc:
            mov  cx, 5000
        poll2:
            test byte ptr [si], 1
            jz   spin2
            mov  al, byte ptr [si-2]
            xor  al, 32                 /* PKT_ESC_XOR (0x20) */
            jmp  store
        spin2:
            dec  cx
            jnz  poll2
            mov  word ptr rc_kind, -1
            jmp  done
        do_sync:
            mov  word ptr rc_kind, -2   /* unescaped SYNC -> frame restart */
        done:
            pop  es
            pop  di
            pop  si
            pop  ds
        }
        if (rc_kind != 0) {
            return pkt_map_recv_err(state, rc_kind);
        }
    }
#ifdef PKT_PROFILE_GUARD
    pkt_guard_n = total;        /* entries pkt_guard_log[0..total) just filled */
#endif

    /* 3. Split out type and CRC, copy payload, verify - off the wire.
     *
     *    This runs AFTER the (sacred) capture loop, but it still gates the ACK:
     *    pkt_receive sends the ACK only once the CRC checks out, so the sender
     *    sits and waits for it. The old per-byte C version here (a pkt_crc16_byte
     *    call plus a frame->buf copy for every byte) cost ~100 ms for a 1 KB
     *    chunk on the 8088's 8-bit bus - which became the dominant per-chunk cost
     *    on receive (measured: it is the bulk of the ~100 ms ACK latency). This
     *    inline asm does the identical work - copy each payload byte to buf and
     *    fold it into the CRC-16 - in one tight pass, ~6-8x faster. The capture
     *    loop above is untouched.
     *
     *    CRC-16/CCITT, MSB-first: crc = (crc<<8) ^ crc16_table[(crc>>8) ^ byte],
     *    init 0xFFFF, over type + payload (frame[0..len]). Registers: AX=crc,
     *    BX=table index, CX=byte count, DL=current byte, SI=frame ptr (read),
     *    DI=buf ptr (write). Everything is near (DGROUP=DS in small model), so
     *    [si]/[di]/crc16_table[bx] need no segment override. */
    *type = frame[0];
    {
        const uint8_t *fptr = frame;
        uint8_t       *bptr = buf;
        uint16_t       plen = len;
        uint16_t       vcrc = 0;    /* assigned by the asm below; init quiets W200 */
        _asm {
            push si
            push di
            mov  si, fptr           /* SI -> frame[0] (type byte)        */
            mov  di, bptr           /* DI -> buf[0]                      */
            mov  ax, 0FFFFh         /* crc = 0xFFFF                      */
            /* fold the type byte (frame[0]); it is not copied to buf */
            mov  bl, ah             /* crc >> 8                          */
            xor  bl, [si]           /* ^ type                            */
            inc  si
            xor  bh, bh
            shl  bx, 1              /* index*2 (2-byte table entries)    */
            mov  ah, al             /* crc <<= 8 ...                     */
            xor  al, al
            xor  ax, word ptr crc16_table[bx]
            /* payload: copy frame[1+i] -> buf[i] and fold into crc */
            mov  cx, plen
            jcxz vdone
        vloop:
            mov  bl, ah             /* crc >> 8                          */
            mov  dl, [si]           /* payload byte                      */
            xor  bl, dl             /* index = (crc>>8) ^ byte           */
            mov  byte ptr [di], dl  /* buf[i] = byte                     */
            inc  si
            inc  di
            xor  bh, bh
            shl  bx, 1
            mov  ah, al             /* crc <<= 8 ...                     */
            xor  al, al
            xor  ax, word ptr crc16_table[bx]
            loop vloop
        vdone:
            mov  vcrc, ax
            pop  di
            pop  si
        }
        crc_calc = vcrc;
    }
    crc_recv = (uint16_t)frame[len + 1] | ((uint16_t)frame[len + 2] << 8);
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

        if ((type & PKT_TYPE_MASK) == PKT_TYPE_RESET) {
            /* Peer reconnected mid-send. This happens when the peer already
             * received this packet, ACKed it, and moved on to its next request -
             * but that ACK was lost, so we are still retransmitting. Rather than
             * burn our remaining retries (which leaves us retransmitting into,
             * and ignoring, the peer's RESET handshake - desyncing the next
             * transfer), treat the RESET as authoritative: resync the sequence
             * bits and ACK it, exactly as pkt_receive() does, then report success
             * (the data WAS delivered; only its ACK was lost). The caller returns
             * to its receive loop in lockstep with the peer's new request. */
            state->seq_rx = 0;
            state->seq_tx = 0;
            pkt_send_ack(state, 0);
            return PKT_TRUE;
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
            /* Fresh data - advance expected sequence and deliver. */
            state->seq_rx ^= 1;
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

    /* Drain any pending data straight from the SIO receiver. */
    do {
        data = pkt_read_byte(state);
    } while (data >= 0);
}
