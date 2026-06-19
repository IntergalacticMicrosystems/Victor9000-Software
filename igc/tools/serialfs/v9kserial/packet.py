"""
Packet Protocol Implementation.

Provides reliable packet-based communication with CRC-16 integrity
checking and ACK/NAK acknowledgment protocol.

Packet Format:
    +------+--------+------+---------+------+
    | SYNC | LENGTH | TYPE | PAYLOAD | CRC  |
    +------+--------+------+---------+------+
       1       2       1     0-1024    2

TYPE carries the packet type in bits 0-6 and a 1-bit alternating
sequence number in bit 7 (DATA and ACK packets) so retransmissions
caused by a lost ACK are not delivered twice. 0x7E/0x7D/XON/XOFF are
escaped after the SYNC byte as 0x7D followed by the byte XOR 0x20.

Compatible with the Victor 9000 C library packet protocol.
"""

import struct
import time
from typing import Optional, Tuple

from .connection import Connection
from .constants import (
    PKT_SYNC, PKT_ESC, PKT_ESC_XOR,
    PKT_TYPE_DATA, PKT_TYPE_ACK, PKT_TYPE_NAK, PKT_TYPE_RESET,
    PKT_SEQ_BIT, PKT_TYPE_MASK,
    PKT_MAX_PAYLOAD, DEFAULT_TIMEOUT, DEFAULT_RETRIES,
    XON, XOFF
)


class PacketError(Exception):
    """Packet protocol error."""
    pass


class PacketProtocol:
    """
    Reliable packet protocol layer.

    Wraps a Connection object to provide packet-based communication
    with framing, CRC-16 integrity checking, and ACK/NAK handshaking.

    Example:
        conn = MamePtyConnection(baudrate=9600)
        conn.connect()

        pkt = PacketProtocol(conn)
        pkt.send_packet(b"Hello Victor!")

        response = pkt.receive_packet(timeout=5.0)
        print(f"Received: {response}")

        conn.disconnect()
    """

    # CRC-16-CCITT lookup table
    _CRC_TABLE = [
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
    ]

    def __init__(self, connection: Connection,
                 timeout: float = DEFAULT_TIMEOUT,
                 retries: int = DEFAULT_RETRIES):
        """
        Initialize packet protocol.

        Args:
            connection: Underlying connection object
            timeout: Timeout for operations in seconds
            retries: Number of retries on failure
        """
        self.conn = connection
        self.timeout = timeout
        self.retries = retries
        self._last_error: Optional[str] = None
        self._seq_tx = 0  # Sequence bit for next DATA packet we send
        self._seq_rx = 0  # Sequence bit we expect on the next fresh DATA packet

    @classmethod
    def crc16(cls, data: bytes, initial: int = 0xFFFF) -> int:
        """
        Calculate CRC-16-CCITT.

        Args:
            data: Data to checksum
            initial: Initial CRC value

        Returns:
            16-bit CRC value
        """
        crc = initial
        for byte in data:
            crc = ((crc << 8) ^ cls._CRC_TABLE[(crc >> 8) ^ byte]) & 0xFFFF
        return crc

    def _escape_byte(self, byte: int) -> bytes:
        """
        Escape a byte if needed.

        XON/XOFF are escaped too: the Victor-side receive ISR consumes
        them as flow control characters, so they must never appear raw
        inside a packet.
        """
        if byte in (PKT_SYNC, PKT_ESC, XON, XOFF):
            return bytes([PKT_ESC, byte ^ PKT_ESC_XOR])
        return bytes([byte])

    def _escape_data(self, data: bytes) -> bytes:
        """Escape special bytes in data."""
        result = bytearray()
        for byte in data:
            result.extend(self._escape_byte(byte))
        return bytes(result)

    def _read_byte_timeout(self, timeout: float) -> Optional[int]:
        """Read a single byte with timeout."""
        data = self.conn.receive(1, timeout=timeout)
        if len(data) == 1:
            return data[0]
        return None

    def _read_unescaped(self, timeout: float) -> Optional[int]:
        """Read a byte with unescaping."""
        byte = self._read_byte_timeout(timeout)
        if byte is None:
            return None

        if byte == PKT_SYNC:
            # Unexpected sync - new packet starting
            return -1

        if byte == PKT_ESC:
            byte = self._read_byte_timeout(timeout)
            if byte is None:
                return None
            byte ^= PKT_ESC_XOR

        return byte

    def send_raw(self, pkt_type: int, payload: bytes = b'') -> bool:
        """
        Send a raw packet (no ACK waiting).

        Args:
            pkt_type: Packet type (PKT_TYPE_*)
            payload: Payload data

        Returns:
            True on success
        """
        if len(payload) > PKT_MAX_PAYLOAD:
            self._last_error = "Payload too large"
            return False

        # Calculate CRC over type + payload
        crc = self.crc16(bytes([pkt_type]) + payload)

        # Build packet
        packet = bytearray()
        packet.append(PKT_SYNC)

        # Length (little-endian, escaped)
        length = len(payload)
        packet.extend(self._escape_byte(length & 0xFF))
        packet.extend(self._escape_byte((length >> 8) & 0xFF))

        # Type (escaped)
        packet.extend(self._escape_byte(pkt_type))

        # Payload (escaped)
        packet.extend(self._escape_data(payload))

        # CRC (little-endian, escaped)
        packet.extend(self._escape_byte(crc & 0xFF))
        packet.extend(self._escape_byte((crc >> 8) & 0xFF))

        self.conn.send(bytes(packet))
        return True

    def recv_raw(self, timeout: Optional[float] = None) -> Tuple[Optional[int], bytes]:
        """
        Receive a raw packet (no ACK sending).

        Args:
            timeout: Timeout in seconds

        Returns:
            Tuple of (packet_type, payload) or (None, b'') on error
        """
        if timeout is None:
            timeout = self.timeout

        deadline = time.time() + timeout

        # Wait for sync
        while time.time() < deadline:
            byte = self._read_byte_timeout(deadline - time.time())
            if byte == PKT_SYNC:
                break
            if byte is None:
                self._last_error = "Timeout waiting for sync"
                return (None, b'')
        else:
            self._last_error = "Timeout waiting for sync"
            return (None, b'')

        def read_unescaped() -> Optional[int]:
            # Recompute the per-read timeout from the overall deadline so a
            # stalled sender cannot stretch reception beyond the deadline.
            return self._read_unescaped(max(0.0, deadline - time.time()))

        # Read length (little-endian)
        len_lo = read_unescaped()
        len_hi = read_unescaped()
        if len_lo is None or len_hi is None or len_lo < 0 or len_hi < 0:
            self._last_error = "Timeout or frame error reading length"
            return (None, b'')

        length = len_lo | (len_hi << 8)
        if length > PKT_MAX_PAYLOAD:
            self._last_error = "Payload too large"
            return (None, b'')

        # Read type
        pkt_type = read_unescaped()
        if pkt_type is None or pkt_type < 0:
            self._last_error = "Timeout or frame error reading type"
            return (None, b'')

        # Start CRC calculation
        crc_calc = self.crc16(bytes([pkt_type]))

        # Read payload
        payload = bytearray()
        for _ in range(length):
            byte = read_unescaped()
            if byte is None or byte < 0:
                self._last_error = "Timeout or frame error reading payload"
                return (None, b'')
            payload.append(byte)
            crc_calc = self.crc16(bytes([byte]), crc_calc)

        # Read CRC (little-endian)
        crc_lo = read_unescaped()
        crc_hi = read_unescaped()
        if crc_lo is None or crc_hi is None or crc_lo < 0 or crc_hi < 0:
            self._last_error = "Timeout or frame error reading CRC"
            return (None, b'')

        crc_recv = crc_lo | (crc_hi << 8)

        # Verify CRC
        if crc_recv != crc_calc:
            self._last_error = f"CRC mismatch: expected {crc_calc:04X}, got {crc_recv:04X}"
            return (None, b'')

        return (pkt_type, bytes(payload))

    def send_ack(self, seq: int = 0) -> bool:
        """Send ACK packet carrying the acknowledged sequence bit."""
        return self.send_raw(PKT_TYPE_ACK | (PKT_SEQ_BIT if seq else 0))

    def send_nak(self) -> bool:
        """Send NAK packet."""
        return self.send_raw(PKT_TYPE_NAK)

    def reset(self, timeout: Optional[float] = None) -> bool:
        """
        Resynchronize sequence bits with the peer (connection hello).

        Sequence bits persist for the life of a peer's packet state, so a
        freshly (re)connected host (seq 0) talking to a long-running server
        whose seq_rx is odd would have its first DATA packet mistaken for a
        duplicate (ACKed but not delivered). Call this once right after
        connecting: it zeroes the local sequence bits, sends a RESET packet,
        and waits for the ACK. The server resyncs its own sequence bits when
        it sees the RESET inside its receive loop.

        Returns:
            True if the RESET was acknowledged. False is best-effort: an
            older peer that predates RESET will NAK/ignore it, in which case
            the caller may proceed (and rely on a fresh server start).
        """
        if timeout is None:
            timeout = self.timeout

        self._seq_tx = 0
        self._seq_rx = 0

        for _attempt in range(self.retries):
            if not self.send_raw(PKT_TYPE_RESET):
                continue
            pkt_type, _payload = self.recv_raw(timeout)
            if pkt_type is None:
                continue
            if (pkt_type & PKT_TYPE_MASK) == PKT_TYPE_ACK:
                return True
        return False

    def send_packet(self, data: bytes, timeout: Optional[float] = None) -> bool:
        """
        Send data packet with acknowledgment.

        Waits for ACK/NAK and retries on failure. The DATA packet carries
        an alternating sequence bit; only an ACK with the matching bit
        completes the send, so the receiver can detect retransmissions.

        Args:
            data: Payload data
            timeout: Timeout for ACK

        Returns:
            True on success (ACK received)

        Raises:
            PacketError: On failure after all retries
        """
        if timeout is None:
            timeout = self.timeout

        tx_type = PKT_TYPE_DATA | (PKT_SEQ_BIT if self._seq_tx else 0)

        last_result = None
        for attempt in range(self.retries):
            if not self.send_raw(tx_type, data):
                last_result = "send_raw failed"
                continue

            # Wait for ACK, handling any unexpected DATA packets from peer
            deadline = time.time() + timeout
            while time.time() < deadline:
                remaining = deadline - time.time()
                pkt_type, payload = self.recv_raw(remaining)

                if pkt_type is None:
                    last_result = f"timeout/error: {self._last_error}"
                    break  # Retry sending

                base_type = pkt_type & PKT_TYPE_MASK
                seq = 1 if (pkt_type & PKT_SEQ_BIT) else 0

                if base_type == PKT_TYPE_ACK:
                    if seq == self._seq_tx:
                        self._seq_tx ^= 1
                        return True
                    # Stale ACK for a previous packet
                    last_result = "stale ACK (sequence mismatch)"
                    self._last_error = last_result
                    break  # Retry sending

                if base_type == PKT_TYPE_NAK:
                    last_result = "NAK received"
                    self._last_error = last_result
                    break  # Retry sending

                if base_type == PKT_TYPE_DATA:
                    # Received a DATA packet from peer (e.g., peer's retry)
                    # ACK it to stop their retries, then keep waiting for our ACK
                    self.send_ack(seq)
                    last_result = "received unexpected DATA packet, ACKed it"
                    continue  # Keep waiting for our ACK

                last_result = f"unexpected response: type=0x{pkt_type:02X}"
                self._last_error = last_result
                break  # Retry sending

        raise PacketError(f"Send failed after {self.retries} attempts: {last_result}")

    def receive_packet(self, timeout: Optional[float] = None) -> bytes:
        """
        Receive data packet with acknowledgment.

        Sends ACK on success, NAK on CRC error. Duplicates (the peer's
        retransmission after a lost ACK) are re-ACKed but not delivered;
        reception continues until fresh data or timeout.

        Args:
            timeout: Timeout in seconds

        Returns:
            Received payload data

        Raises:
            PacketError: On timeout or protocol error
        """
        if timeout is None:
            timeout = self.timeout

        deadline = time.time() + timeout

        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise PacketError("Receive failed: timeout")

            pkt_type, payload = self.recv_raw(remaining)

            if pkt_type is None:
                # NAK genuine mid-frame corruption (CRC/framing error) so the
                # sender retransmits - but stay silent on a pure idle timeout
                # where no SYNC ever arrived. A server polling for the next
                # request would otherwise spew NAKs between requests.
                if self._last_error and "sync" not in self._last_error:
                    self.send_nak()
                raise PacketError(f"Receive failed: {self._last_error}")

            base_type = pkt_type & PKT_TYPE_MASK
            seq = 1 if (pkt_type & PKT_SEQ_BIT) else 0

            if base_type == PKT_TYPE_RESET:
                # Connection hello from a freshly (re)connected peer. Resync
                # BOTH sequence bits and ACK it, then keep waiting for the real
                # request. Mirrors the C pkt_receive RESET handling (packet.c:
                # 682-683), which zeroes seq_rx AND seq_tx.
                #
                # Resetting _seq_tx is essential for reconnect to a long-running
                # server: the client's RESET zeroes its _seq_rx, so our next
                # DATA reply (e.g. a LIST_RESP) must also start at seq 0 or the
                # client treats it as a duplicate, drops it, and times out. A
                # fresh server has _seq_tx == 0 already, which is why only
                # reconnects-after-activity exposed this.
                self._seq_rx = 0
                self._seq_tx = 0
                self.send_ack(0)
                continue

            if base_type != PKT_TYPE_DATA:
                raise PacketError(f"Unexpected packet type: {base_type}")

            # ACK with the sender's sequence bit (also re-ACKs duplicates,
            # which stops the sender's retransmissions)
            self.send_ack(seq)

            if seq == self._seq_rx:
                # Fresh data
                self._seq_rx ^= 1
                return payload

            # Duplicate of already-delivered data - keep waiting

    def flush(self) -> None:
        """Flush pending data from connection."""
        self.conn.flush_rx()

    @property
    def last_error(self) -> Optional[str]:
        """Get last error message."""
        return self._last_error
