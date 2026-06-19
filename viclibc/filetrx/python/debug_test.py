#!/usr/bin/env python3
"""
Simple debug test for file transfer protocol.

Tests basic packet communication before attempting file transfers.
"""

import sys
import time
from pathlib import Path

# Add paths for imports
sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent.parent.parent / 'serial' / 'python'))

from v9kserial import MamePtyConnection, PacketProtocol
from v9kfiletrx.protocol import (
    Command, Compression, Direction,
    StartPacket, ReadyPacket, parse_packet
)


def test_packet_echo():
    """Test basic packet send/receive."""
    print("Debug Test - File Transfer Protocol")
    print("=" * 40)

    # Connect to MAME
    print("\nConnecting to MAME PTY at 9600 baud...")
    try:
        conn = MamePtyConnection(baudrate=9600)
        conn.connect()
        print(f"Connected to: {conn._pty_path}")
    except Exception as e:
        print(f"Failed to connect: {e}")
        return 1

    try:
        pkt = PacketProtocol(conn, timeout=10.0, retries=3)

        print("\n--- Test 1: Send START packet ---")
        print("Make sure Victor is waiting to receive a file...")
        input("Press Enter when ready...")

        # Build a START packet
        start = StartPacket(
            direction=Direction.PC_TO_VICTOR,
            compression=Compression.NONE,
            flags=0x01,  # overwrite
            file_size=100,
            compressed_size=0,
            file_date=0,
            file_time=0,
            file_attr=0x20,
            filename="TEST.TXT",
            file_crc=0x12345678
        )

        print(f"Sending START packet: {len(start.pack())} bytes")
        print(f"  Filename: {start.filename}")
        print(f"  Size: {start.file_size}")

        try:
            # Send the START packet
            pkt.send_packet(start.pack())
            print("START packet sent, waiting for response...")

            # Wait for READY response
            resp_data = pkt.receive_packet(timeout=10.0)
            print(f"Received response: {len(resp_data)} bytes")
            print(f"  Raw: {resp_data.hex()}")

            resp = parse_packet(resp_data)
            print(f"  Parsed: {type(resp).__name__}")

            if isinstance(resp, ReadyPacket):
                print(f"  Status: {resp.status}")
                if resp.status == 0:
                    print("SUCCESS! Victor is ready to receive.")
                else:
                    print(f"Victor not ready, status={resp.status}")
            else:
                print(f"Unexpected response type: {type(resp)}")

        except Exception as e:
            print(f"Error during transfer: {e}")
            import traceback
            traceback.print_exc()

        print("\n--- Test Complete ---")

    finally:
        conn.disconnect()
        print("Disconnected.")

    return 0


def test_raw_packet():
    """Test raw packet send without high-level protocol."""
    print("Debug Test - Raw Packet Layer")
    print("=" * 40)

    print("\nConnecting to MAME PTY at 9600 baud...")
    try:
        conn = MamePtyConnection(baudrate=9600)
        conn.connect()
        print(f"Connected to: {conn._pty_path}")
    except Exception as e:
        print(f"Failed to connect: {e}")
        return 1

    try:
        pkt = PacketProtocol(conn, timeout=10.0)

        print("\nSending test DATA packet with 'HELLO' payload...")
        print("Victor should receive this if packet layer works.")
        input("Press Enter when Victor is ready...")

        # Send a simple test packet
        test_data = b"HELLO"
        pkt.send_raw(0x01, test_data)  # 0x01 = PKT_TYPE_DATA
        print(f"Sent raw packet: {test_data}")

        print("\nWaiting for response (ACK/NAK)...")
        pkt_type, payload = pkt.recv_raw(timeout=10.0)

        if pkt_type is not None:
            print(f"Received packet type: 0x{pkt_type:02X}")
            if pkt_type == 0x02:
                print("Got ACK!")
            elif pkt_type == 0x03:
                print("Got NAK!")
            else:
                print(f"Unexpected type, payload: {payload.hex()}")
        else:
            print(f"No response received. Last error: {pkt.last_error}")

    finally:
        conn.disconnect()
        print("Disconnected.")

    return 0


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--raw':
        sys.exit(test_raw_packet())
    else:
        sys.exit(test_packet_echo())
