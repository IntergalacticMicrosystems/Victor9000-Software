#!/usr/bin/env python3
"""
Host driver for FTXTEST on the Victor (COM1/SerialA @ 38400).

Sends a local file to the Victor's current drive (C:) via the new v9kfiletrx
FileTransfer.send_file path - exercising per-chunk credit flow control, 1024-byte
chunks, and the receiver's 4 KB write batching - over a real serial link.

    ftxtest_host.py put  <local> <REMOTE.8x3> [port] [baud]
    ftxtest_host.py get  <REMOTE.8x3> <local>  [port] [baud]   (FTXTEST serves getfile too)

Defaults: port=/dev/ttyUSB0  baud=38400. Start FTXTEST on the Victor first.
"""
import sys, time

HERE = __file__.rsplit('/', 1)[0]
sys.path.insert(0, HERE + '/../serial/python')
sys.path.insert(0, HERE + '/python')

from v9kserial import SerialConnection, PacketProtocol            # noqa: E402
from v9kfiletrx.filetrx import FileTransfer, TransferError        # noqa: E402
from v9kfiletrx.protocol import (StartPacket, Direction, parse_packet,  # noqa: E402
                                 ReadyPacket, Caps, Flags)


def open_link(port, baud):
    conn = SerialConnection(port, baudrate=baud)
    conn.connect()
    # 5 retries at the packet layer; 2 s ACK wait is plenty at 38400.
    pkt = PacketProtocol(conn, timeout=2.0, retries=5)
    # Resync sequence bits with the RESET-aware Victor before the first request.
    if not pkt.reset():
        print("warning: RESET not acknowledged (continuing anyway)")
    return conn, pkt


def cmd_put(local, remote, port, baud):
    conn, pkt = open_link(port, baud)
    ftx = FileTransfer(pkt, timeout=5.0)
    t0 = time.time()
    try:
        stats = ftx.send_file(local, remote, overwrite=True)
    except TransferError as e:
        print(f"FAIL: {e} (code {e.error_code})")
        conn.disconnect()
        sys.exit(1)
    dt = time.time() - t0
    rate = stats.bytes_transferred / dt if dt else 0
    print(f"OK put {local} -> {remote}: {stats.bytes_transferred} bytes in "
          f"{dt:.1f}s ({rate/1024:.1f} KB/s)  chunks={stats.chunks_sent} "
          f"retries={stats.retries} errors={stats.errors}  "
          f"flow_control={'ENGAGED' if getattr(ftx, 'flow_control', False) else 'fallback'}")
    conn.disconnect()


def cmd_get(remote, local, port, baud):
    """Request a file from FTXTEST (START dir=VICTOR_TO_PC), then receive it."""
    conn, pkt = open_link(port, baud)
    # With the CRC now computed during the send (carried in END), the Victor sends
    # START immediately - no whole-file pre-read. 10 s per packet is ample at 38400
    # (a chunk is ~0.27 s) while tolerating an occasional slow disk read mid-send.
    ftx = FileTransfer(pkt, timeout=10.0)
    # Send the getfile request: a START describing the file we want pulled.
    req = StartPacket(direction=Direction.VICTOR_TO_PC, filename=remote.upper())
    pkt.send_packet(req.pack())
    t0 = time.time()
    try:
        stats = ftx.receive_file(remote.upper(), local)
    except TransferError as e:
        print(f"FAIL: {e} (code {e.error_code})")
        conn.disconnect()
        sys.exit(1)
    dt = time.time() - t0
    rate = stats.bytes_transferred / dt if dt else 0
    print(f"OK get {remote} -> {local}: {stats.bytes_transferred} bytes in "
          f"{dt:.1f}s ({rate/1024:.2f} KB/s)")
    conn.disconnect()


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    op = sys.argv[1]
    a, b = sys.argv[2], sys.argv[3]
    port = sys.argv[4] if len(sys.argv) > 4 else '/dev/ttyUSB0'
    baud = int(sys.argv[5]) if len(sys.argv) > 5 else 38400
    if op == 'put':
        cmd_put(a, b, port, baud)
    elif op == 'get':
        cmd_get(a, b, port, baud)
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main()
