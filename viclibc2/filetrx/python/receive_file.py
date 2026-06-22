#!/usr/bin/env python3
"""
Receive a file from Victor 9000.

Usage:
    python receive_file.py LOCAL_FILE [OPTIONS]

Examples:
    python receive_file.py received.txt
    python receive_file.py received.txt --baud 19200
"""

import argparse
import sys
from pathlib import Path

# Add parent directories to path for imports
sys.path.insert(0, str(Path(__file__).parent))  # v9kfiletrx package
sys.path.insert(0, str(Path(__file__).parent.parent.parent / 'serial' / 'python'))

from v9kserial import MamePtyConnection, SerialConnection, PacketProtocol
from v9kfiletrx import FileTransfer, TransferStats


def progress_bar(stats: TransferStats) -> bool:
    """Display progress bar and return True to continue."""
    width = 40
    filled = int(width * stats.percent_complete / 100)
    bar = '=' * filled + '-' * (width - filled)

    print(f'\rProgress: [{bar}] {stats.percent_complete:5.1f}%  '
          f'{stats.bytes_transferred:,}/{stats.total_bytes:,} bytes  '
          f'{stats.bytes_per_second:,.0f} B/s  ', end='', flush=True)

    return True  # Continue transfer


def main():
    parser = argparse.ArgumentParser(
        description='Receive a file from Victor 9000'
    )
    parser.add_argument('local_file', help='Local file to save as')
    parser.add_argument('--baud', '-b', type=int, default=9600,
                        help='Baud rate (default: 9600)')
    parser.add_argument('--port', '-p', default=None,
                        help='Serial port (default: auto-detect MAME PTY)')
    parser.add_argument('--mame', '-m', action='store_true',
                        help='Use MAME PTY connection')
    parser.add_argument('--timeout', '-t', type=float, default=30.0,
                        help='Timeout in seconds (default: 30.0)')
    parser.add_argument('--quiet', '-q', action='store_true',
                        help='Suppress progress output')

    args = parser.parse_args()

    local_path = Path(args.local_file)

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
        print(f"Error connecting: {e}", file=sys.stderr)
        return 1

    try:
        # Create packet protocol and file transfer
        pkt = PacketProtocol(conn)
        ftx = FileTransfer(pkt, timeout=args.timeout)

        print(f"\nWaiting for file from Victor 9000...")
        print(f"Will save to: {local_path}")
        print("(Press Ctrl+C to cancel)")
        print()

        # Perform transfer
        progress = None if args.quiet else progress_bar
        stats = ftx.receive_file(
            "",  # Accept any filename from Victor
            local_path,
            progress=progress
        )

        print()  # Newline after progress bar
        print()
        print("Transfer complete!")
        print(f"       File: {local_path}")
        print(f"       Size: {stats.bytes_transferred:,} bytes")
        print(f"       Time: {stats.elapsed_seconds:.1f} seconds")
        print(f"      Speed: {stats.bytes_per_second:,.0f} bytes/sec")
        print(f"    Retries: {stats.retries}")

        return 0

    except KeyboardInterrupt:
        print("\nTransfer cancelled.")
        return 1
    except Exception as e:
        print(f"\nError: {e}", file=sys.stderr)
        return 1
    finally:
        conn.disconnect()


if __name__ == '__main__':
    sys.exit(main())
