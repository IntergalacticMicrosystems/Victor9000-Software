"""
Victor 9000 File Transfer Library.

Provides bidirectional file transfer between PC and Victor 9000
over serial connection.
"""

from .protocol import (
    Command, Compression, Direction, Error, Status,
    StartPacket, DataPacket, EndPacket, ReadyPacket,
    ListPacket, ListRespPacket, DirEntry,
    CHUNK_SIZE, MAX_PAYLOAD
)
from .crc32 import crc32, crc32_file, crc32_update
from .compress import rle_compress, rle_decompress, compress_if_beneficial
from .filetrx import FileTransfer, TransferStats, TransferError, ProgressCallback
from .test_protocol import (
    TestCommand, TestResult, TestCmdPacket, TestResultPacket,
    CMD_TEST_CMD, CMD_TEST_RESULT, CMD_TEST_PING, CMD_TEST_PONG,
    COMPRESS_NONE, COMPRESS_RLE,
    BAUD_9600, BAUD_19200, baud_to_index,
    create_ping, is_pong
)
from .error_injector import ErrorInjector, ErrorConfig, ErrorStats, ErrorPresets

__version__ = "1.0.0"

__all__ = [
    # Main classes
    'FileTransfer',
    'TransferStats',
    'TransferError',
    'ProgressCallback',

    # Protocol
    'Command',
    'Compression',
    'Direction',
    'Error',
    'Status',
    'StartPacket',
    'DataPacket',
    'EndPacket',
    'ReadyPacket',
    'ListPacket',
    'ListRespPacket',
    'DirEntry',
    'CHUNK_SIZE',
    'MAX_PAYLOAD',

    # Utilities
    'crc32',
    'crc32_file',
    'crc32_update',
    'rle_compress',
    'rle_decompress',
    'compress_if_beneficial',

    # Test Protocol
    'TestCommand',
    'TestResult',
    'TestCmdPacket',
    'TestResultPacket',
    'CMD_TEST_CMD',
    'CMD_TEST_RESULT',
    'CMD_TEST_PING',
    'CMD_TEST_PONG',
    'COMPRESS_NONE',
    'COMPRESS_RLE',
    'BAUD_9600',
    'BAUD_19200',
    'baud_to_index',
    'create_ping',
    'is_pong',

    # Error Injection
    'ErrorInjector',
    'ErrorConfig',
    'ErrorStats',
    'ErrorPresets',
]
