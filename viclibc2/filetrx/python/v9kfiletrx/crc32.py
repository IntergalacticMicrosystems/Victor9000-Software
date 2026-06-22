"""
CRC-32 Implementation.

Standard CRC-32 (IEEE 802.3) for file integrity verification.
Compatible with the C implementation in ftx_crc32.c.

Polynomial: 0xEDB88320 (reflected)
Initial value: 0xFFFFFFFF
XOR output: 0xFFFFFFFF
"""

import zlib


def crc32(data: bytes) -> int:
    """
    Calculate CRC-32 of data.

    Args:
        data: Bytes to calculate CRC of

    Returns:
        32-bit CRC value (unsigned)
    """
    # Python's zlib.crc32 uses the same algorithm
    return zlib.crc32(data) & 0xFFFFFFFF


def crc32_update(crc: int, data: bytes) -> int:
    """
    Update running CRC-32 with additional data.

    Use this for computing CRC incrementally (chunk by chunk).

    Example:
        crc = CRC32_INIT
        crc = crc32_update(crc, chunk1)
        crc = crc32_update(crc, chunk2)
        final_crc = crc ^ CRC32_INIT

    Args:
        crc: Current CRC value
        data: Additional data to include

    Returns:
        Updated CRC value
    """
    # zlib.crc32 can continue from a previous value
    return zlib.crc32(data, crc) & 0xFFFFFFFF


# Initial/final XOR value
CRC32_INIT = 0xFFFFFFFF


def crc32_file(filepath: str, chunk_size: int = 65536) -> int:
    """
    Calculate CRC-32 of a file.

    Args:
        filepath: Path to file
        chunk_size: Read buffer size (default 64KB)

    Returns:
        32-bit CRC value
    """
    crc = 0
    with open(filepath, 'rb') as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF
