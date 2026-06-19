#!/usr/bin/env python3
"""
Send a file to Victor 9000.

Usage:
    python send_file.py LOCAL_FILE [REMOTE_NAME] [OPTIONS]

Examples:
    python send_file.py myfile.txt
    python send_file.py myfile.txt MYFILE.TXT --compress
    python send_file.py myfile.txt --baud 19200 --port /dev/ttyUSB0
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
        description='Send a file to Victor 9000'
    )
    parser.add_argument('local_file', help='Local file to send')
    parser.add_argument('remote_name', nargs='?', default=None,
                        help='Filename on Victor (default: same as local)')
    parser.add_argument('--compress', '-c', action='store_true',
                        help='Enable RLE compression')
    parser.add_argument('--baud', '-b', type=int, default=9600,
                        help='Baud rate (default: 9600)')
    parser.add_argument('--port', '-p', default=None,
                        help='Serial port (default: auto-detect MAME PTY)')
    parser.add_argument('--mame', '-m', action='store_true',
                        help='Use MAME PTY connection')
    parser.add_argument('--timeout', '-t', type=float, default=5.0,
                        help='Timeout in seconds (default: 5.0)')
    parser.add_argument('--quiet', '-q', action='store_true',
                        help='Suppress progress output')

    args = parser.parse_args()

    # Check file exists
    local_path = Path(args.local_file)
    if not local_path.exists():
        print(f"Error: File not found: {local_path}", file=sys.stderr)
        return 1

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

        # Determine compression
        compression = Compression.RLE if args.compress else Compression.NONE

        # Set remote name
        remote_name = args.remote_name or local_path.name

        # Print transfer info
        print(f"\nSending: {local_path}")
        print(f"     To: {remote_name}")
        print(f"   Size: {local_path.stat().st_size:,} bytes")
        print(f"Compress: {'RLE' if args.compress else 'None'}")
        print()

        # Perform transfer
        progress = None if args.quiet else progress_bar
        stats = ftx.send_file(
            local_path,
            remote_name,
            compression=compression,
            progress=progress
        )

        print()  # Newline after progress bar
        print()
        print("Transfer complete!")
        print(f"  Bytes sent: {stats.bytes_transferred:,}")
        print(f"        Time: {stats.elapsed_seconds:.1f} seconds")
        print(f"       Speed: {stats.bytes_per_second:,.0f} bytes/sec")
        if args.compress and stats.compressed_bytes > 0:
            ratio = (1 - stats.compression_ratio) * 100
            print(f" Compression: {ratio:.1f}% saved")
        print(f"     Retries: {stats.retries}")

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
