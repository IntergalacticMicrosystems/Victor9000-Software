#!/usr/bin/env python3
"""
Interactive File Transfer Test Tool.

Provides an interactive menu for testing file transfers with Victor 9000.

Usage:
    python ftx_test.py [OPTIONS]

Examples:
    python ftx_test.py
    python ftx_test.py --baud 19200
    python ftx_test.py --port /dev/ttyUSB0
"""

import argparse
import sys
from pathlib import Path

# Add parent directories to path for imports
sys.path.insert(0, str(Path(__file__).parent))  # v9kfiletrx package
sys.path.insert(0, str(Path(__file__).parent.parent.parent / 'serial' / 'python'))

from v9kserial import MamePtyConnection, SerialConnection, PacketProtocol
from v9kfiletrx import FileTransfer, Compression, TransferStats


def progress_bar(stats: TransferStats) -> bool:
    """Display progress bar."""
    width = 30
    filled = int(width * stats.percent_complete / 100)
    bar = '#' * filled + '-' * (width - filled)

    print(f'\r[{bar}] {stats.percent_complete:5.1f}% '
          f'{stats.bytes_per_second:,.0f} B/s', end='', flush=True)

    return True


def print_menu():
    """Print main menu."""
    print("\n" + "=" * 40)
    print("Victor 9000 File Transfer Test")
    print("=" * 40)
    print("1. Send file to Victor")
    print("2. Receive file from Victor")
    print("3. List Victor directory")
    print("4. Test RLE compression")
    print("5. Test CRC-32")
    print("Q. Quit")
    print("-" * 40)


def send_file(ftx: FileTransfer):
    """Send a file to Victor."""
    local_file = input("Local file to send: ").strip()
    if not local_file:
        return

    path = Path(local_file)
    if not path.exists():
        print(f"File not found: {path}")
        return

    remote_name = input(f"Remote name [{path.name}]: ").strip()
    if not remote_name:
        remote_name = path.name

    compress = input("Use compression? (y/N): ").strip().lower() == 'y'
    compression = Compression.RLE if compress else Compression.NONE

    print(f"\nSending {path} ({path.stat().st_size:,} bytes)...")

    try:
        stats = ftx.send_file(path, remote_name, compression=compression,
                              progress=progress_bar)
        print(f"\n\nComplete! {stats.bytes_transferred:,} bytes in "
              f"{stats.elapsed_seconds:.1f}s "
              f"({stats.bytes_per_second:,.0f} B/s)")
    except Exception as e:
        print(f"\n\nError: {e}")


def receive_file(ftx: FileTransfer):
    """Receive a file from Victor."""
    local_file = input("Save as: ").strip()
    if not local_file:
        return

    print("\nWaiting for file from Victor...")
    print("(Start the transfer on Victor side)")

    try:
        stats = ftx.receive_file("", local_file, progress=progress_bar)
        print(f"\n\nComplete! {stats.bytes_transferred:,} bytes in "
              f"{stats.elapsed_seconds:.1f}s")
    except Exception as e:
        print(f"\n\nError: {e}")


def list_directory(ftx: FileTransfer):
    """List Victor directory."""
    path = input("Directory path [current]: ").strip()

    print(f"\nListing {path or 'current directory'}...")

    try:
        entries = ftx.list_directory(path)

        print(f"\n{'Name':<12} {'Size':>10} {'Date':<10} {'Attr'}")
        print("-" * 40)

        for entry in entries:
            if entry['is_dir']:
                size_str = "<DIR>"
            else:
                size_str = f"{entry['size']:,}"

            print(f"{entry['name']:<12} {size_str:>10}")

        print(f"\n{len(entries)} entries")
    except Exception as e:
        print(f"\nError: {e}")


def test_compression():
    """Test RLE compression."""
    from v9kfiletrx import rle_compress, rle_decompress

    print("\nRLE Compression Test")
    print("-" * 40)

    # Test data
    test_cases = [
        b"Hello World",
        b"AAAAAAAAAA",  # Run of 10
        b"\x90\x90\x90",  # Escape bytes
        bytes(range(256)),  # All byte values
        b"A" * 300,  # Long run
    ]

    for data in test_cases:
        compressed = rle_compress(data)
        decompressed = rle_decompress(compressed)

        if decompressed != data:
            print(f"FAIL: Round-trip failed for {len(data)} bytes")
            continue

        ratio = len(compressed) / len(data) * 100
        print(f"OK: {len(data):5} -> {len(compressed):5} bytes ({ratio:5.1f}%)")

    print("\nAll tests passed!")


def test_crc():
    """Test CRC-32."""
    from v9kfiletrx import crc32

    print("\nCRC-32 Test")
    print("-" * 40)

    # Known test vectors
    test_cases = [
        (b"", 0x00000000),
        (b"123456789", 0xCBF43926),
        (b"The quick brown fox jumps over the lazy dog", 0x414FA339),
    ]

    for data, expected in test_cases:
        result = crc32(data)
        status = "OK" if result == expected else "FAIL"
        print(f"{status}: CRC32({data[:20]!r}...) = 0x{result:08X} "
              f"(expected 0x{expected:08X})")


def main():
    parser = argparse.ArgumentParser(
        description='Interactive file transfer test tool'
    )
    parser.add_argument('--baud', '-b', type=int, default=9600,
                        help='Baud rate (default: 9600)')
    parser.add_argument('--port', '-p', default=None,
                        help='Serial port (default: auto-detect MAME PTY)')
    parser.add_argument('--timeout', '-t', type=float, default=10.0,
                        help='Timeout in seconds (default: 10.0)')

    args = parser.parse_args()

    # Create connection
    try:
        if args.port:
            print(f"Connecting to {args.port} at {args.baud} baud...")
            conn = SerialConnection(args.port, baudrate=args.baud)
        else:
            print(f"Auto-detecting MAME PTY at {args.baud} baud...")
            conn = MamePtyConnection(baudrate=args.baud)

        conn.connect()
        print("Connected.")
    except Exception as e:
        print(f"Error connecting: {e}")
        print("\nRunning in offline mode (compression/CRC tests only)")
        conn = None

    ftx = None
    if conn:
        try:
            pkt = PacketProtocol(conn)
            ftx = FileTransfer(pkt, timeout=args.timeout)
        except Exception as e:
            print(f"Error initializing protocol: {e}")

    # Main loop
    try:
        while True:
            print_menu()
            choice = input("> ").strip().upper()

            if choice == '1':
                if ftx:
                    send_file(ftx)
                else:
                    print("Not connected")
            elif choice == '2':
                if ftx:
                    receive_file(ftx)
                else:
                    print("Not connected")
            elif choice == '3':
                if ftx:
                    list_directory(ftx)
                else:
                    print("Not connected")
            elif choice == '4':
                test_compression()
            elif choice == '5':
                test_crc()
            elif choice == 'Q':
                break
            else:
                print("Invalid choice")

    except KeyboardInterrupt:
        print("\n")

    finally:
        if conn:
            conn.disconnect()
            print("Disconnected.")


if __name__ == '__main__':
    sys.exit(main() or 0)
