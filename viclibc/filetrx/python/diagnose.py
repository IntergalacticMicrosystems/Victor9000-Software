#!/usr/bin/env python3
"""
Serial Communication Diagnostic Tool.

Tests low-level serial and packet communication with Victor 9000.
"""

import sys
import time
from pathlib import Path

# Add paths for imports
sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent.parent.parent / 'serial' / 'python'))

from v9kserial import MamePtyConnection, PacketProtocol
from v9kserial.constants import PKT_SYNC, PKT_TYPE_DATA, PKT_TYPE_ACK
from v9kfiletrx.protocol import (
    Command, StartPacket, ReadyPacket, DataPacket, EndPacket,
    Direction, Compression, parse_packet
)


def hexdump(data: bytes, prefix: str = "") -> None:
    """Print hex dump of data."""
    if not data:
        print(f"{prefix}(empty)")
        return
    hex_str = ' '.join(f'{b:02X}' for b in data)
    ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data)
    print(f"{prefix}{hex_str}  |{ascii_str}|")


def test_connection():
    """Test basic connection to MAME PTY."""
    print("=" * 60)
    print("Step 1: Testing MAME PTY Connection")
    print("=" * 60)

    try:
        conn = MamePtyConnection(baudrate=9600)
        pty_path = conn._find_mame_pty()
        print(f"Found MAME PTY: {pty_path}")
    except Exception as e:
        print(f"ERROR: Could not find MAME PTY: {e}")
        print("\nMake sure MAME is running with: ./mame victor9k -rs232a pty")
        return None

    try:
        conn.connect()
        print(f"Connected successfully")
        return conn
    except Exception as e:
        print(f"ERROR: Could not connect: {e}")
        return None


def test_file_transfer_protocol(conn):
    """Test the complete file transfer protocol."""
    print("\n" + "=" * 60)
    print("Step 2: Testing File Transfer Protocol")
    print("=" * 60)

    pkt = PacketProtocol(conn, timeout=10.0, retries=3)

    # Clear any pending data
    pkt.flush()
    time.sleep(0.5)

    # Build a proper START packet
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

    start_data = start.pack()
    print(f"\nSending START packet ({len(start_data)} bytes):")
    print(f"  Filename: {start.filename}")
    print(f"  Size: {start.file_size}")
    hexdump(start_data, "  Data: ")

    # Send START and wait for ACK
    print("\n--- Sending START packet via packet protocol ---")
    try:
        pkt.send_packet(start_data)
        print("START packet sent and ACKed!")
    except Exception as e:
        print(f"ERROR sending START: {e}")
        print(f"Last error: {pkt.last_error}")
        return False

    # Wait for READY response
    print("\n--- Waiting for READY response ---")
    try:
        resp_data = pkt.receive_packet(timeout=10.0)
        print(f"Received response ({len(resp_data)} bytes):")
        hexdump(resp_data, "  ")

        if resp_data[0] == Command.READY:
            ready = ReadyPacket.unpack(resp_data)
            print(f"  READY packet received!")
            print(f"  Status: {ready.status}")
            if ready.status == 0:
                print("  Victor is READY to receive file!")
                return True
            else:
                print(f"  ERROR: Victor not ready, status={ready.status}")
                print(f"  (Status 252/-4 = FTX_ERR_FILE = file creation failed)")
                return False
        else:
            print(f"  Unexpected command: 0x{resp_data[0]:02X}")
            return False

    except Exception as e:
        print(f"ERROR receiving READY: {e}")
        print(f"Last error: {pkt.last_error}")
        return False


def test_simple_packet_exchange(conn):
    """Test basic packet send/receive with a minimal packet."""
    print("\n" + "=" * 60)
    print("Step 3: Testing Simple Packet Exchange")
    print("=" * 60)
    print("(This tests if Victor responds to any packet)")

    pkt = PacketProtocol(conn, timeout=5.0, retries=2)
    pkt.flush()
    time.sleep(0.3)

    # Send a minimal packet
    test_payload = b'\xFF'  # Invalid command, Victor should still ACK at packet layer
    print(f"\nSending minimal test packet (1 byte payload: 0xFF)")

    try:
        pkt.send_packet(test_payload)
        print("Packet sent and ACKed!")
        print("Victor's packet layer is working correctly.")
        return True
    except Exception as e:
        print(f"ERROR: {e}")
        print(f"Last error: {pkt.last_error}")

        # Check what's in the buffer
        print("\nChecking receive buffer...")
        time.sleep(0.5)
        raw = conn.receive_available(timeout=0.5)
        if raw:
            print(f"Found {len(raw)} bytes in buffer:")
            hexdump(raw, "  ")
        else:
            print("Buffer is empty - Victor may not be responding at all")
        return False


def main():
    print("Victor 9000 Serial Communication Diagnostics")
    print("=" * 60)
    print()
    print("IMPORTANT: Before running this test:")
    print()
    print("  1. Start MAME: ./mame victor9k -rs232a pty")
    print("  2. Boot Victor to DOS")
    print("  3. Run FTXTEST.EXE on Victor")
    print("  4. Select option 1 'Receive file from PC'")
    print("  5. Victor should show 'Waiting for file from PC...'")
    print()
    print("If Victor is stuck from a previous failed transfer,")
    print("press ESC on Victor to abort and restart from step 3.")
    print()
    input("Press Enter when Victor is ready and waiting...")
    print()

    # Step 1: Connect
    conn = test_connection()
    if not conn:
        return 1

    try:
        # Step 2: Test file transfer protocol
        if test_file_transfer_protocol(conn):
            print("\n" + "=" * 60)
            print("SUCCESS! File transfer protocol is working.")
            print("Victor accepted the START packet and is ready.")
            print()
            print("NOTE: Victor is now waiting for data chunks.")
            print("You should restart Victor before running send_file.py")
            print("=" * 60)
        else:
            print("\n" + "=" * 60)
            print("File transfer protocol test failed.")
            print()
            print("Trying basic packet test...")
            print("=" * 60)

            # Wait for Victor to timeout and return to menu
            print("\nWaiting 5 seconds for Victor to reset...")
            time.sleep(5)

            # Step 3: Test basic packet
            test_simple_packet_exchange(conn)

    finally:
        print("\n" + "=" * 60)
        print("Disconnecting...")
        conn.disconnect()
        print("Done.")

    return 0


if __name__ == '__main__':
    sys.exit(main())
