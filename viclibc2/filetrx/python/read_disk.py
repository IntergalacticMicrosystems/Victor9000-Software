#!/usr/bin/env python3
"""
Image logical sectors from a Victor 9000 drive into a local file.

The Victor side must be running FTXSERV/FTXTEST (which serve SECTOR/DISKINFO
via ftx_serve_one). Reads are always allowed; no special server flag is needed.

Usage:
    python read_disk.py DRIVE OUTPUT.IMG [OPTIONS]

Examples:
    python read_disk.py 0 driveA.img --port /dev/ttyUSB0 --baud 38400
    python read_disk.py 0 driveA.img --compress          # RLE blank sectors
    python read_disk.py 0 head.img --count 64            # first 64 sectors
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))  # v9kfiletrx package
sys.path.insert(0, str(Path(__file__).parent.parent.parent / 'serial' / 'python'))

from v9kserial import MamePtyConnection, SerialConnection, PacketProtocol
from v9kfiletrx import FileTransfer, Compression, TransferStats


def progress_bar(stats: TransferStats) -> bool:
    width = 40
    filled = int(width * stats.percent_complete / 100)
    bar = '=' * filled + '-' * (width - filled)
    print(f'\rProgress: [{bar}] {stats.percent_complete:5.1f}%  '
          f'{stats.bytes_transferred:,}/{stats.total_bytes:,} bytes  '
          f'{stats.bytes_per_second:,.0f} B/s  ', end='', flush=True)
    return True


def main():
    parser = argparse.ArgumentParser(description='Image a Victor 9000 disk to a file')
    parser.add_argument('drive', type=int, help='Drive number (0=A, 1=B, ...)')
    parser.add_argument('output', help='Local image file to create')
    parser.add_argument('--start', type=int, default=0,
                        help='First sector to read (default: 0)')
    parser.add_argument('--count', type=int, default=None,
                        help='Sectors to read (default: to end of device)')
    parser.add_argument('--segment', type=int, default=0,
                        help='Split into segments of this many sectors (0 = one)')
    parser.add_argument('--compress', '-c', action='store_true',
                        help='Enable per-chunk RLE compression')
    parser.add_argument('--baud', '-b', type=int, default=38400,
                        help='Baud rate (default: 38400)')
    parser.add_argument('--port', '-p', default=None,
                        help='Serial port (default: auto-detect MAME PTY)')
    parser.add_argument('--timeout', '-t', type=float, default=5.0,
                        help='Timeout in seconds (default: 5.0)')
    parser.add_argument('--quiet', '-q', action='store_true',
                        help='Suppress progress output')

    args = parser.parse_args()

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
        pkt = PacketProtocol(conn)
        ftx = FileTransfer(pkt, timeout=args.timeout)
        compression = Compression.RLE if args.compress else Compression.NONE

        bps, total = ftx.disk_info(args.drive)
        print(f"\nDrive {args.drive}: {bps} bytes/sector, {total} sectors "
              f"({total * bps:,} bytes)")
        print(f"Reading to: {args.output}")
        print()

        progress = None if args.quiet else progress_bar
        stats = ftx.read_disk(
            args.drive, args.output,
            start_sector=args.start, total_sectors=args.count,
            segment_sectors=args.segment, compression=compression,
            progress=progress)

        print("\n\nImage complete!")
        print(f"  Bytes read: {stats.bytes_transferred:,}")
        print(f"        Time: {stats.elapsed_seconds:.1f} seconds")
        print(f"       Speed: {stats.bytes_per_second:,.0f} bytes/sec")
        print(f"     Retries: {stats.retries}")
        return 0

    except KeyboardInterrupt:
        print("\nCancelled.")
        return 1
    except Exception as e:
        print(f"\nError: {e}", file=sys.stderr)
        return 1
    finally:
        conn.disconnect()


if __name__ == '__main__':
    sys.exit(main())
