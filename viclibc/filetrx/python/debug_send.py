#!/usr/bin/env python3
"""Debug script to analyze file transfer packets."""

import sys
import struct
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent.parent.parent / 'serial' / 'python'))

from v9kfiletrx.protocol import DataPacket, CHUNK_SIZE

def analyze_data_packet(chunk_num: int, data: bytes):
    """Analyze a DATA packet."""
    pkt = DataPacket(chunk_num=chunk_num, data=data)
    packed = pkt.pack()

    print(f"\nChunk {chunk_num}:")
    print(f"  Data length: {len(data)} bytes")
    print(f"  Packed header (first 8 bytes): {packed[:8].hex()}")
    print(f"  Header breakdown:")
    print(f"    Byte 0 (cmd):        0x{packed[0]:02X}")
    print(f"    Bytes 1-2 (chunk_num): 0x{packed[1]:02X} 0x{packed[2]:02X} = {struct.unpack('<H', packed[1:3])[0]}")
    print(f"    Bytes 3-4 (chunk_size): 0x{packed[3]:02X} 0x{packed[4]:02X} = {struct.unpack('<H', packed[3:5])[0]}")
    print(f"  Total packet size: {len(packed)} bytes")

    # Verify struct.pack alignment
    expected = struct.pack('<BHH', 0x11, chunk_num, len(data))
    print(f"  Expected header: {expected.hex()}")
    print(f"  Match: {packed[:5] == expected}")

def main():
    # Create test file content
    file_size = 2027  # About 2 chunks
    test_data = bytes(range(256)) * (file_size // 256 + 1)
    test_data = test_data[:file_size]

    print(f"Test file size: {file_size} bytes")
    print(f"CHUNK_SIZE: {CHUNK_SIZE}")
    print(f"Number of chunks: {(file_size + CHUNK_SIZE - 1) // CHUNK_SIZE}")

    # Simulate chunking
    offset = 0
    chunk_num = 0
    while offset < file_size:
        chunk_data = test_data[offset:offset + CHUNK_SIZE]
        analyze_data_packet(chunk_num, chunk_data)
        offset += CHUNK_SIZE
        chunk_num += 1

    print("\n" + "="*60)
    print("C struct ftx_data_t layout (with -zp1):")
    print("  offset 0: cmd (uint8_t, 1 byte)")
    print("  offset 1: chunk_num (uint16_t, 2 bytes)")
    print("  offset 3: chunk_size (uint16_t, 2 bytes)")
    print("  offset 5: data (uint8_t[], variable)")
    print("="*60)

if __name__ == '__main__':
    main()
