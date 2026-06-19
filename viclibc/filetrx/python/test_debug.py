#!/usr/bin/env python3
"""Debug script for file transfer testing."""

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent.parent.parent / 'serial' / 'python'))

from v9kserial import MamePtyConnection, PacketProtocol
from v9kfiletrx import FileTransfer, Compression
from v9kfiletrx.protocol import (
    StartPacket, DataPacket, EndPacket, ReadyPacket,
    Command, Direction, Status, parse_packet, CHUNK_SIZE
)
from v9kfiletrx.crc32 import crc32_file


def hex_dump(data: bytes, prefix: str = "") -> str:
    """Format bytes as hex dump."""
    if len(data) <= 32:
        return prefix + " ".join(f"{b:02X}" for b in data)
    return prefix + " ".join(f"{b:02X}" for b in data[:32]) + f"... ({len(data)} bytes)"


def main():
    print("=== File Transfer Debug Test ===\n")

    # Create test file
    test_file = Path("/tmp/debug_test.txt")
    test_data = b"Test data for debugging\n"
    test_file.write_bytes(test_data)
    print(f"Test file: {test_file} ({len(test_data)} bytes)")

    # Connect to MAME
    print("\nConnecting to MAME PTY...")
    conn = MamePtyConnection(baudrate=9600)
    conn.connect()
    print(f"Connected: {conn._port}")

    try:
        pkt = PacketProtocol(conn, timeout=10.0, retries=3)

        # Prepare START packet
        file_crc = crc32_file(str(test_file))
        start = StartPacket(
            direction=Direction.PC_TO_VICTOR,
            compression=Compression.NONE,
            flags=0x01,
            file_size=len(test_data),
            compressed_size=0,
            file_date=0,
            file_time=0,
            file_attr=0x20,
            filename="TEST.TXT",
            file_crc=file_crc
        )

        print(f"\n[1] Sending START packet ({len(start.pack())} bytes)...")
        print(hex_dump(start.pack(), "    "))
        pkt.send_packet(start.pack())
        print("    START sent and ACKed")

        print("\n[2] Waiting for READY response...")
        resp_data = pkt.receive_packet(timeout=10.0)
        print(hex_dump(resp_data, "    Received: "))
        resp = parse_packet(resp_data)
        if isinstance(resp, ReadyPacket):
            print(f"    READY received, status={resp.status}")
        else:
            print(f"    Unexpected response: {type(resp)}")
            return 1

        # Send DATA chunk
        data_pkt = DataPacket(chunk_num=0, data=test_data)
        print(f"\n[3] Sending DATA chunk 0 ({len(data_pkt.pack())} bytes)...")
        print(hex_dump(data_pkt.pack(), "    "))
        pkt.send_packet(data_pkt.pack())
        print("    DATA sent and ACKed")

        # Small delay to let Victor process
        print("\n[4] Waiting 500ms for Victor to process...")
        time.sleep(0.5)

        # Send END packet
        end = EndPacket(
            total_chunks=1,
            bytes_sent=len(test_data),
            file_crc=file_crc,
            status=Status.OK
        )
        print(f"\n[5] Sending END packet ({len(end.pack())} bytes)...")
        print(hex_dump(end.pack(), "    "))

        try:
            pkt.send_packet(end.pack())
            print("    END sent and ACKed - SUCCESS!")
        except Exception as e:
            print(f"    END failed: {e}")
            print(f"    Last error: {pkt.last_error}")
            return 1

        print("\n=== Transfer Complete ===")
        return 0

    except Exception as e:
        print(f"\nError: {e}")
        import traceback
        traceback.print_exc()
        return 1
    finally:
        conn.disconnect()


if __name__ == '__main__':
    sys.exit(main())
