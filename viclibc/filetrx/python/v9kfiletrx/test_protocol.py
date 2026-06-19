"""
Test Protocol for Automated Testing.

Provides packet types and helpers for controlling the Victor test server.
These match the definitions in ftx_protocol.h.
"""

import struct
from dataclasses import dataclass
from enum import IntEnum
from typing import Optional


# Test protocol command values (matches ftx_protocol.h)
CMD_TEST_CMD = 0x20     # Test command from PC
CMD_TEST_RESULT = 0x21  # Test result from Victor
CMD_TEST_PING = 0x22    # Ping/heartbeat
CMD_TEST_PONG = 0x23    # Ping response


class TestCommand(IntEnum):
    """Test command sub-types (param in CMD_TEST_CMD)."""
    SEND_FILE = 0x01      # Request Victor to send a file
    RECV_FILE = 0x02      # Request Victor to prepare for receive
    SET_COMPRESS = 0x03   # Set compression mode
    SET_BAUD = 0x04       # Set baud rate
    DELETE_FILE = 0x05    # Delete a file
    GET_RESULT = 0x06     # Get last transfer result
    QUIT = 0xFF           # Exit test server mode


class TestResult(IntEnum):
    """Test result codes."""
    OK = 0x00
    FAIL = 0x01
    FILE_ERROR = 0x02
    TIMEOUT = 0x03


# Compression modes
COMPRESS_NONE = 0
COMPRESS_RLE = 1

# Baud rate indices (matches Victor serial library)
BAUD_110 = 0
BAUD_300 = 1
BAUD_600 = 2
BAUD_1200 = 3
BAUD_2400 = 4
BAUD_4800 = 5
BAUD_9600 = 6
BAUD_19200 = 7

BAUD_NAMES = ["110", "300", "600", "1200", "2400", "4800", "9600", "19200"]


def baud_to_index(baud: int) -> int:
    """Convert baud rate to index."""
    baud_map = {110: 0, 300: 1, 600: 2, 1200: 3, 2400: 4, 4800: 5, 9600: 6,
                19200: 7, 38400: 8}
    return baud_map.get(baud, BAUD_9600)


@dataclass
class TestCmdPacket:
    """
    Test command packet sent from PC to Victor.

    Format (68 bytes):
        cmd (1)        - FTX_CMD_TEST_CMD (0x20)
        test_cmd (1)   - Test sub-command (FTX_TEST_*)
        param1 (1)     - First parameter (compression mode, baud index, etc)
        param2 (1)     - Second parameter (reserved)
        filename (64)  - Filename if applicable (null-terminated)
    """
    test_cmd: int
    param1: int = 0
    param2: int = 0
    filename: str = ""

    def pack(self) -> bytes:
        """Pack command into bytes for transmission."""
        # Encode filename, truncate to 63 chars + null
        filename_bytes = self.filename.encode('ascii', errors='replace')[:63]
        # Pad to 64 bytes with nulls
        filename_padded = filename_bytes + b'\x00' * (64 - len(filename_bytes))

        return struct.pack(
            '<BBBB',
            CMD_TEST_CMD,
            self.test_cmd,
            self.param1,
            self.param2
        ) + filename_padded

    @classmethod
    def send_file(cls, filename: str) -> 'TestCmdPacket':
        """Create command to tell Victor to send a file."""
        return cls(test_cmd=TestCommand.SEND_FILE, filename=filename)

    @classmethod
    def recv_file(cls, filename: str = "") -> 'TestCmdPacket':
        """Create command to tell Victor to receive a file."""
        return cls(test_cmd=TestCommand.RECV_FILE, filename=filename)

    @classmethod
    def set_compress(cls, mode: int) -> 'TestCmdPacket':
        """Create command to set compression mode."""
        return cls(test_cmd=TestCommand.SET_COMPRESS, param1=mode)

    @classmethod
    def set_baud(cls, baud_index: int) -> 'TestCmdPacket':
        """Create command to set baud rate."""
        return cls(test_cmd=TestCommand.SET_BAUD, param1=baud_index)

    @classmethod
    def delete_file(cls, filename: str) -> 'TestCmdPacket':
        """Create command to delete a file."""
        return cls(test_cmd=TestCommand.DELETE_FILE, filename=filename)

    @classmethod
    def quit(cls) -> 'TestCmdPacket':
        """Create command to quit test server."""
        return cls(test_cmd=TestCommand.QUIT)

    @classmethod
    def get_result(cls) -> 'TestCmdPacket':
        """Create command to get last transfer result."""
        return cls(test_cmd=TestCommand.GET_RESULT)


@dataclass
class TestResultPacket:
    """
    Test result packet received from Victor.

    Format (52 bytes):
        cmd (1)               - FTX_CMD_TEST_RESULT (0x21)
        result_code (1)       - FTX_TEST_OK, FTX_TEST_FAIL, etc.
        error_code (1)        - FTX_ERR_* if failure
        reserved (1)
        bytes_transferred (4) - Bytes transferred (for file ops)
        elapsed_ticks (4)     - Time taken in DOS ticks (18.2/sec)
        retries (2)           - Retry count
        errors (2)            - Error count
        message (32)          - Status message
    """
    result_code: int = TestResult.OK
    error_code: int = 0
    bytes_transferred: int = 0
    elapsed_ticks: int = 0
    retries: int = 0
    errors: int = 0
    message: str = ""

    @property
    def success(self) -> bool:
        """Check if result indicates success."""
        return self.result_code == TestResult.OK

    @property
    def elapsed_seconds(self) -> float:
        """Convert elapsed ticks to seconds (18.2 ticks/sec)."""
        return self.elapsed_ticks / 18.2

    @property
    def bytes_per_second(self) -> float:
        """Calculate transfer speed."""
        if self.elapsed_seconds > 0:
            return self.bytes_transferred / self.elapsed_seconds
        return 0.0

    @classmethod
    def unpack(cls, data: bytes) -> 'TestResultPacket':
        """Unpack result from received bytes."""
        if len(data) < 16:
            raise ValueError(f"Result packet too short: {len(data)} bytes")

        if data[0] != CMD_TEST_RESULT:
            raise ValueError(f"Not a TEST_RESULT packet: 0x{data[0]:02X}")

        # Unpack fixed fields
        result_code = data[1]
        error_code = data[2]
        # data[3] is reserved

        bytes_xfer, elapsed, retries, errors = struct.unpack('<IIHH', data[4:16])

        # Extract message if present
        message = ""
        if len(data) > 16:
            msg_bytes = data[16:48]
            message = msg_bytes.rstrip(b'\x00').decode('ascii', errors='replace')

        return cls(
            result_code=result_code,
            error_code=error_code,
            bytes_transferred=bytes_xfer,
            elapsed_ticks=elapsed,
            retries=retries,
            errors=errors,
            message=message
        )


def create_ping() -> bytes:
    """Create a PING packet."""
    return bytes([CMD_TEST_PING])


def is_pong(data: bytes) -> bool:
    """Check if data is a PONG response."""
    return len(data) >= 1 and data[0] == CMD_TEST_PONG
