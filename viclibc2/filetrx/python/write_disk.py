#!/usr/bin/env python3
"""
Write a local image's logical sectors onto a Victor 9000 drive. DESTRUCTIVE.

This overwrites raw disk with NO file-system safety net. The Victor server must
have sector writes enabled (run FTXSERV /allowwrite); a default server rejects
the write. You are prompted to confirm unless --yes is given. NEVER target a
disk whose contents matter, and never write over media holding a running EXE.

Usage:
    python write_disk.py DRIVE IMAGE.IMG [OPTIONS]

Examples:
    python write_disk.py 0 driveA.img --port /dev/ttyUSB0 --baud 38400
    python write_disk.py 0 driveA.img --yes --compress
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
    parser = argparse.ArgumentParser(
        description='Write a local image onto a Victor 9000 disk (DESTRUCTIVE)')
    parser.add_argument('drive', type=int, help='Drive number (0=A, 1=B, ...)')
    parser.add_argument('image', help='Local image file to write')
    parser.add_argument('--start', type=int, default=0,
                        help='First sector to write (default: 0)')
    parser.add_argument('--count', type=int, default=None,
                        help='Sectors to write (default: whole image)')
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
    parser.add_argument('--yes', '-y', action='store_true',
                        help='Skip the destructive-write confirmation prompt')
    parser.add_argument('--quiet', '-q', action='store_true',
                        help='Suppress progress output')

    args = parser.parse_args()

    image_path = Path(args.image)
    if not image_path.exists():
        print(f"Error: image not found: {image_path}", file=sys.stderr)
        return 1

    if not args.yes:
        print(f"About to OVERWRITE raw sectors on Victor drive {args.drive} "
              f"from {image_path} ({image_path.stat().st_size:,} bytes).")
        print("This is destructive and cannot be undone.")
        if input("Type 'yes' to proceed: ").strip().lower() != 'yes':
            print("Aborted.")
            return 1

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
        print(f"\nDrive {args.drive}: {bps} bytes/sector, {total} sectors")
        print(f"Writing from: {image_path}")
        print()

        progress = None if args.quiet else progress_bar
        stats = ftx.write_disk(
            args.drive, image_path,
            start_sector=args.start, total_sectors=args.count,
            segment_sectors=args.segment, compression=compression,
            progress=progress)

        print("\n\nWrite complete!")
        print(f"  Bytes written: {stats.bytes_transferred:,}")
        print(f"           Time: {stats.elapsed_seconds:.1f} seconds")
        print(f"          Speed: {stats.bytes_per_second:,.0f} bytes/sec")
        print(f"        Retries: {stats.retries}")
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
