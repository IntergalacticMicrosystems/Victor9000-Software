#!/usr/bin/env python3
"""
End-to-end self-test for igcfs (no Victor hardware required).

Bridges two in-process Connection endpoints (a fake serial cable), runs the real
IgcFileServer on one end, and drives it from the other with a client that
emulates igc's *stock* Victor-side FTX byte sequences (ftx_send_file /
ftx_receive_file / ftx_list_dir + the delete/mkdir/rename extension). Exercises
the full packet + FTX + dispatch path: RESET, list (incl. 8.3 mangling), upload,
download, delete, mkdir, rename.
"""

import os
import struct
import tempfile
import threading
from pathlib import Path

from v9kserial.connection import Connection
from v9kserial import PacketProtocol
from v9kfiletrx import (
    Command, Direction, Status, StartPacket, DataPacket, EndPacket,
    ReadyPacket, ListPacket, ListRespPacket, FileTransfer, crc32,
)
from v9kfiletrx.protocol import CHUNK_SIZE, parse_packet
import igcfs
from igcfs import IgcFileServer, CMD_DELETE, CMD_MKDIR, CMD_RENAME, CMD_OK, CMD_QUIT


class BridgedConnection(Connection):
    """One end of an in-process bidirectional byte pipe."""

    def __init__(self):
        super().__init__(baudrate=38400)
        self._inbox = bytearray()
        self._ilock = threading.Lock()
        self.peer = None

    @staticmethod
    def pair():
        a, b = BridgedConnection(), BridgedConnection()
        a.peer, b.peer = b, a
        return a, b

    def connect(self):
        self._connected = True
        self.start_receiver()
        return True

    def disconnect(self):
        self.stop_receiver()
        self._connected = False

    def _raw_send(self, data):
        with self.peer._ilock:
            self.peer._inbox.extend(data)
        return len(data)

    def _raw_receive(self, count):
        with self._ilock:
            n = min(count, len(self._inbox))
            out = bytes(self._inbox[:n])
            del self._inbox[:n]
        return out

    @property
    def bytes_available(self):
        with self._ilock:
            return len(self._inbox)


# --- igc-emulating client (Victor-side byte sequences) ----------------------

def igc_list(pkt, path=''):
    return FileTransfer(pkt).list_directory(path)


def igc_upload(pkt, data, remote_basename):
    """Emulate ftx_send_file: START(VICTOR_TO_PC) -> READY -> DATA... -> END."""
    file_crc = crc32(data)
    total = max(1, (len(data) + CHUNK_SIZE - 1) // CHUNK_SIZE)
    date, tm = FileTransfer._unix_to_dos_datetime(0)
    start = StartPacket(direction=Direction.VICTOR_TO_PC, file_size=len(data),
                        file_crc=file_crc, file_date=date, file_time=tm,
                        file_attr=0x20, filename=remote_basename)
    pkt.send_packet(start.pack())
    ready = parse_packet(pkt.receive_packet(timeout=5))
    assert isinstance(ready, ReadyPacket) and ready.status == 0, "no READY"
    for i in range(total):
        chunk = data[i * CHUNK_SIZE:(i + 1) * CHUNK_SIZE]
        pkt.send_packet(DataPacket(chunk_num=i, data=chunk).pack(), timeout=5)
    pkt.send_packet(EndPacket(total_chunks=total, bytes_sent=len(data),
                              file_crc=file_crc, status=Status.OK).pack())


def igc_download(pkt, relpath):
    """Emulate request + ftx_receive_file: request START(PC_TO_VICTOR) ->
    recv START -> READY -> recv DATA... -> END."""
    req = StartPacket(direction=Direction.PC_TO_VICTOR, filename=relpath)
    pkt.send_packet(req.pack())
    start = parse_packet(pkt.receive_packet(timeout=5))
    assert isinstance(start, StartPacket), f"expected START, got {start}"
    pkt.send_packet(ReadyPacket(status=0).pack())
    out = bytearray()
    expected = 0
    while True:
        p = parse_packet(pkt.receive_packet(timeout=5))
        if isinstance(p, EndPacket):
            break
        assert isinstance(p, DataPacket) and p.chunk_num == expected
        out.extend(p.data)
        expected += 1
    assert crc32(bytes(out)) == start.file_crc, "download CRC mismatch"
    return bytes(out)


def igc_simple(pkt, payload):
    """Send a delete/mkdir/rename request, return (ok, reply_cmd)."""
    pkt.send_packet(payload)
    reply = pkt.receive_packet(timeout=5)
    return reply[0] == CMD_OK, reply[0]


def _p(path):
    b = path.encode('ascii')
    return bytes([len(b)]) + b


def main():
    failures = []

    def check(name, cond):
        print(("  ok   " if cond else "  FAIL ") + name)
        if not cond:
            failures.append(name)

    with tempfile.TemporaryDirectory() as root:
        rootp = Path(root)
        (rootp / 'HELLO.TXT').write_bytes(b'hello victor\r\n')
        (rootp / 'a-long-name.dat').write_bytes(bytes(range(256)) * 20)  # 8.3 mangle
        (rootp / 'SUB').mkdir()
        (rootp / 'SUB' / 'INNER.TXT').write_bytes(b'inner file')

        srv_conn, cli_conn = BridgedConnection.pair()
        srv_conn.connect()
        cli_conn.connect()

        server = IgcFileServer(rootp, srv_conn, verbose=True)
        t = threading.Thread(target=server.serve_forever, daemon=True)
        t.start()

        pkt = PacketProtocol(cli_conn, timeout=2.0, retries=5)
        check("RESET acknowledged", pkt.reset())

        # --- LIST root ---
        entries = igc_list(pkt, '')
        names = {e['name'] for e in entries}
        check("list: HELLO.TXT present", 'HELLO.TXT' in names)
        check("list: SUB dir present", any(e['name'] == 'SUB' and e['is_dir'] for e in entries))
        mangled = [e['name'] for e in entries if e['name'].startswith('A') and '~' in e['name']]
        check("list: long name mangled to 8.3", len(mangled) == 1)

        # --- LIST subdir ---
        sub = igc_list(pkt, 'SUB')
        subnames = {e['name'] for e in sub}
        check("list SUB: '..' offered", '..' in subnames)
        check("list SUB: INNER.TXT present", 'INNER.TXT' in subnames)

        # --- download (Victor pulls) ---
        got = igc_download(pkt, 'HELLO.TXT')
        check("download HELLO.TXT bytes match", got == b'hello victor\r\n')
        got2 = igc_download(pkt, 'SUB\\INNER.TXT')
        check("download SUB\\INNER.TXT", got2 == b'inner file')
        big = igc_download(pkt, mangled[0])
        check("download multi-chunk mangled file", big == (bytes(range(256)) * 20))

        # --- upload (Victor pushes) into root (cwd from last list of root) ---
        igc_list(pkt, '')  # set server cwd back to root
        payload = b'X' * 3000  # spans 3 chunks
        igc_upload(pkt, payload, 'NEWFILE.BIN')
        check("upload created file", (rootp / 'NEWFILE.BIN').read_bytes() == payload)

        # --- mkdir / rename / delete (explicit relative paths) ---
        ok, _ = igc_simple(pkt, bytes([CMD_MKDIR]) + _p('MADE'))
        check("mkdir MADE", ok and (rootp / 'MADE').is_dir())
        ok, _ = igc_simple(pkt, bytes([CMD_RENAME]) + _p('NEWFILE.BIN') + _p('RENAMED.BIN'))
        check("rename NEWFILE.BIN -> RENAMED.BIN",
              ok and (rootp / 'RENAMED.BIN').exists() and not (rootp / 'NEWFILE.BIN').exists())
        ok, _ = igc_simple(pkt, bytes([CMD_DELETE]) + _p('RENAMED.BIN'))
        check("delete RENAMED.BIN", ok and not (rootp / 'RENAMED.BIN').exists())

        # --- error paths ---
        req = StartPacket(direction=Direction.PC_TO_VICTOR, filename='NOPE.TXT')
        pkt.send_packet(req.pack())
        reply = pkt.receive_packet(timeout=5)
        check("download missing file -> ERROR", reply[0] == Command.ERROR)

        # --- sandbox escape rejected ---
        ok, _ = igc_simple(pkt, bytes([CMD_DELETE]) + _p('..\\..\\etc\\passwd'))
        check("sandbox escape rejected", not ok)

        # --- QUIT ---
        pkt.send_packet(bytes([CMD_QUIT]))
        t.join(timeout=3)
        check("server stopped on QUIT", not t.is_alive())

        srv_conn.disconnect()
        cli_conn.disconnect()

    print()
    if failures:
        print(f"FAILED ({len(failures)}): {failures}")
        return 1
    print("ALL PASSED")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
