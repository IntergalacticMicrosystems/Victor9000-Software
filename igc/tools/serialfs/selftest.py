#!/usr/bin/env python3
"""
End-to-end self-test for igcfs (no Victor hardware required).

Bridges two in-process Connection endpoints (a fake serial cable), runs the real
IgcFileServer on one end, and drives it from the other with a client that
emulates igc's *stock* Victor-side FTX byte sequences (ftx_send_file /
ftx_receive_file / ftx_list_dir + the delete/mkdir/rename extension). Exercises
the full packet + FTX + dispatch path: RESET, list (incl. 8.3 mangling), upload,
download, delete, mkdir, rename, and the disk-image job (IMG_OPEN..IMG_CLOSE).
"""

import math
import os
import random
import struct
import tempfile
import threading
from pathlib import Path

from v9kserial.connection import Connection
from v9kserial import PacketProtocol
from v9kfiletrx import (
    Command, Direction, Compression, Status, StartPacket, DataPacket, EndPacket,
    ReadyPacket, ListPacket, ListRespPacket, FileTransfer, crc32,
)
from v9kfiletrx.protocol import CHUNK_SIZE, DATA_FLAG_COMPRESSED, parse_packet
from v9kfiletrx.compress import compress_if_beneficial, rle_decompress
import igcfs
from igcfs import IgcFileServer, CMD_DELETE, CMD_MKDIR, CMD_RENAME, CMD_OK, CMD_QUIT
from igcfs import (CMD_IMG_OPEN, CMD_IMG_INFO, CMD_IMG_GET, CMD_IMG_TRACK,
                   CMD_IMG_CLOSE, IMG_FRAG_SIZE, IMG_FLAG_RLE)
import v9kimage


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


def igc_upload(pkt, data, remote_basename, rle=True):
    """Emulate igc's serialfs_put_named: START(VICTOR_TO_PC, RLE) -> READY ->
    DATA... -> END -> wait for the server's CMD_OK/ERROR store status. Like
    the C sender, each chunk is compressed and flagged only when that saves
    bytes; rle=False emulates an older igc that never compresses."""
    file_crc = crc32(data)
    total = max(1, (len(data) + CHUNK_SIZE - 1) // CHUNK_SIZE)
    date, tm = FileTransfer._unix_to_dos_datetime(0)
    start = StartPacket(direction=Direction.VICTOR_TO_PC,
                        compression=Compression.RLE if rle else Compression.NONE,
                        file_size=len(data), file_crc=file_crc, file_date=date,
                        file_time=tm, file_attr=0x20, filename=remote_basename)
    pkt.send_packet(start.pack())
    ready = parse_packet(pkt.receive_packet(timeout=5))
    assert isinstance(ready, ReadyPacket) and ready.status == 0, "no READY"
    for i in range(total):
        chunk = data[i * CHUNK_SIZE:(i + 1) * CHUNK_SIZE]
        flags = 0
        if rle:
            chunk, packed = compress_if_beneficial(chunk)
            flags = DATA_FLAG_COMPRESSED if packed else 0
        pkt.send_packet(DataPacket(chunk_num=i, data=chunk, flags=flags).pack(), timeout=5)
    pkt.send_packet(EndPacket(total_chunks=total, bytes_sent=len(data),
                              file_crc=file_crc, status=Status.OK).pack())
    reply = pkt.receive_packet(timeout=5)
    assert reply[0] == CMD_OK and reply[1] == 0, "upload not acknowledged"


def igc_download(pkt, relpath, rle=True, stats=None):
    """Emulate request + ftx_receive_file: request START(PC_TO_VICTOR, RLE) ->
    recv START -> READY -> recv DATA... -> END. Chunks are expanded on the
    per-chunk flag, as ftx_recv.c does. rle=False emulates an older igc that
    requests no compression. stats, if given, collects
    {'compressed': n, 'raw': n, 'wire': bytes}."""
    req = StartPacket(direction=Direction.PC_TO_VICTOR, filename=relpath,
                      compression=Compression.RLE if rle else Compression.NONE)
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
        packed = bool(p.flags & DATA_FLAG_COMPRESSED)
        assert rle or not packed, "server compressed although not asked to"
        if stats is not None:
            stats['compressed' if packed else 'raw'] = \
                stats.get('compressed' if packed else 'raw', 0) + 1
            stats['wire'] = stats.get('wire', 0) + len(p.data)
        out.extend(rle_decompress(p.data) if packed else p.data)
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


# --- disk images (Victor-side byte sequences of the IMG_* mini-protocol) -----

def igc_img_open(pkt, mode, name, total_sectors=0):
    """IMG_OPEN -> IMG_INFO; returns (status, total_sectors)."""
    n = name.encode('ascii')
    pkt.send_packet(struct.pack('<BBHB', CMD_IMG_OPEN, mode, total_sectors, len(n)) + n)
    reply = pkt.receive_packet(timeout=5)
    assert reply[0] == CMD_IMG_INFO, f"expected IMG_INFO, got 0x{reply[0]:02X}"
    return reply[1], struct.unpack_from('<H', reply, 2)[0]


def igc_img_close(pkt, status, crc):
    """IMG_CLOSE -> True on CMD_OK, False on ERROR."""
    pkt.send_packet(struct.pack('<BBI', CMD_IMG_CLOSE, status, crc))
    reply = pkt.receive_packet(timeout=10)
    return reply[0] == CMD_OK


def igc_img_send_track(pkt, img, t, stats=None):
    """Push one track as fragments, like serialfs_img_put_track: each 1 KB slice
    compressed and flagged only when that saves bytes. Returns the reply byte."""
    start = v9kimage.track_first_sector(t) * v9kimage.SECTOR_SIZE
    data = img[start:start + v9kimage.track_bytes(t)]
    nfrag = math.ceil(len(data) / IMG_FRAG_SIZE)
    for i in range(nfrag):
        frag, packed = compress_if_beneficial(data[i * IMG_FRAG_SIZE:(i + 1) * IMG_FRAG_SIZE])
        if stats is not None:
            stats['rle' if packed else 'raw'] = stats.get('rle' if packed else 'raw', 0) + 1
        pkt.send_packet(struct.pack('<BBBBBH', CMD_IMG_TRACK, t, i, nfrag,
                                    IMG_FLAG_RLE if packed else 0, len(frag)) + frag, timeout=5)
    return pkt.receive_packet(timeout=5)[0]


def igc_img_push(pkt, name, img, stats=None):
    """Emulate 'Read disk to image file' to the server: OPEN mode 0, every
    track with a per-track CMD_OK checkpoint, CLOSE with the image CRC."""
    sectors = len(img) // v9kimage.SECTOR_SIZE
    status, echo = igc_img_open(pkt, 0, name, sectors)
    assert status == 0 and echo == sectors, f"IMG_OPEN refused: {status}"
    for t in range(v9kimage.track_count(sectors)):
        assert igc_img_send_track(pkt, img, t, stats) == CMD_OK, f"track {t} not acknowledged"
    assert igc_img_close(pkt, 0, crc32(img)), "IMG_CLOSE not acknowledged"


def igc_img_pull(pkt, name, stats=None):
    """Emulate 'Write image file to disk' from the server: OPEN mode 1, IMG_GET
    and reassemble each track, CLOSE with the CRC; returns the image bytes."""
    status, sectors = igc_img_open(pkt, 1, name)
    assert status == 0, f"IMG_OPEN refused: {status}"
    out = bytearray()
    for t in range(v9kimage.track_count(sectors)):
        tbytes = v9kimage.track_bytes(t)
        nfrag = math.ceil(tbytes / IMG_FRAG_SIZE)
        pkt.send_packet(bytes([CMD_IMG_GET, t]))
        for i in range(nfrag):
            p = pkt.receive_packet(timeout=5)
            cmd, trk, frag, nf, flags, dlen = struct.unpack_from('<BBBBBH', p)
            assert (cmd, trk, frag, nf) == (CMD_IMG_TRACK, t, i, nfrag), "bad fragment header"
            data = p[7:7 + dlen]
            assert len(data) == dlen
            packed = bool(flags & IMG_FLAG_RLE)
            if stats is not None:
                stats['rle' if packed else 'raw'] = stats.get('rle' if packed else 'raw', 0) + 1
            data = rle_decompress(data) if packed else data
            assert len(data) == min(IMG_FRAG_SIZE, tbytes - i * IMG_FRAG_SIZE), "bad fragment length"
            out.extend(data)
    assert len(out) == sectors * v9kimage.SECTOR_SIZE
    assert igc_img_close(pkt, 0, crc32(bytes(out))), "IMG_CLOSE CRC not confirmed"
    return bytes(out)


def make_test_image(sectors, double_sided):
    """Synthetic disk: a label whose sidedness bit matches, then a mix of long
    runs (compressible fragments) and random bytes (raw fragments)."""
    rnd = random.Random(sectors)
    img = bytearray()
    while len(img) < sectors * v9kimage.SECTOR_SIZE:
        if rnd.random() < 0.5:
            img += bytes([rnd.randrange(256)]) * rnd.randrange(100, 5000)
        else:
            img += rnd.randbytes(rnd.randrange(100, 5000))
    img = img[:sectors * v9kimage.SECTOR_SIZE]
    img[0:32] = b'\xff' + bytes(31)
    img[32:34] = struct.pack('<H', 1 if double_sided else 0)
    return bytes(img)


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
        (rootp / 'Long Dir Name').mkdir()  # mangled dir name (8.3 won't fit)
        (rootp / 'Long Dir Name' / 'INSIDE.TXT').write_bytes(b'inside long dir')

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

        # --- long-named (mangled) directory: every path component is reverse-
        #     mapped, so we can browse into and transfer through it ---
        longdir = next(e['name'] for e in entries
                       if e['is_dir'] and '~' in e['name'])
        ldnames = {e['name'] for e in igc_list(pkt, longdir)}
        check("list into mangled dir", 'INSIDE.TXT' in ldnames)
        check("download from mangled dir",
              igc_download(pkt, longdir + '\\INSIDE.TXT') == b'inside long dir')
        igc_list(pkt, '')  # reset cwd to root
        igc_upload(pkt, b'Z' * 100, longdir + '\\PUT.BIN')
        check("upload into mangled dir",
              (rootp / 'Long Dir Name' / 'PUT.BIN').read_bytes() == b'Z' * 100)

        # --- download (Victor pulls) ---
        got = igc_download(pkt, 'HELLO.TXT')
        check("download HELLO.TXT bytes match", got == b'hello victor\r\n')
        got2 = igc_download(pkt, 'SUB\\INNER.TXT')
        check("download SUB\\INNER.TXT", got2 == b'inner file')
        big = igc_download(pkt, mangled[0])
        check("download multi-chunk mangled file", big == (bytes(range(256)) * 20))

        # --- upload over an existing long-named file: the final component is
        #     reverse-mapped, so the write updates the original rather than
        #     forking a literal MYDOCU~1-style file next to it ---
        igc_list(pkt, '')
        updated = b'updated content!' * 8
        igc_upload(pkt, updated, mangled[0])
        check("upload over mangled name updates real file",
              (rootp / 'a-long-name.dat').read_bytes() == updated)
        check("upload over mangled name does not fork a new file",
              not (rootp / mangled[0]).exists())

        # --- upload (Victor pushes) into root (cwd from last list of root) ---
        igc_list(pkt, '')  # set server cwd back to root
        payload = b'X' * 3000  # spans 3 chunks
        igc_upload(pkt, payload, 'NEWFILE.BIN')
        check("upload created file", (rootp / 'NEWFILE.BIN').read_bytes() == payload)

        # --- path-qualified upload (directory tree): name carries a separator,
        #     so the server stores it under root and auto-creates the parents,
        #     regardless of cwd. This is how igc copies whole directories. ---
        # --- RLE: a run-heavy file shrinks on the wire, a 0x90-heavy one (the
        #     escape byte, which would expand) stays raw, and a mix of both
        #     exercises the per-chunk flag within one transfer. Older igcs that
        #     request/announce no compression must still get raw bytes. ---
        runs = b'\0' * 5000
        mixed = (b'\x90' * 1024) + (b'A' * 1024) + bytes(range(256)) * 4 + b'\x90' * 7
        (rootp / 'RUNS.BIN').write_bytes(runs)
        (rootp / 'MIXED.BIN').write_bytes(mixed)
        st = {}
        check("rle download: run-heavy bytes match", igc_download(pkt, 'RUNS.BIN', stats=st) == runs)
        check("rle download: run-heavy shrank on the wire",
              st.get('wire', 0) < len(runs) // 10 and st.get('raw', 0) == 0)
        st = {}
        check("rle download: mixed bytes match", igc_download(pkt, 'MIXED.BIN', stats=st) == mixed)
        check("rle download: mixed has both raw and compressed chunks",
              st.get('raw', 0) >= 1 and st.get('compressed', 0) >= 1)
        st = {}
        check("no-rle download: old client gets raw chunks",
              igc_download(pkt, 'RUNS.BIN', rle=False, stats=st) == runs
              and st.get('compressed', 0) == 0)
        igc_list(pkt, '')
        igc_upload(pkt, mixed, 'MIXUP.BIN')
        check("rle upload: mixed round-trips", (rootp / 'MIXUP.BIN').read_bytes() == mixed)
        igc_upload(pkt, runs, 'RUNSUP.BIN', rle=False)
        check("no-rle upload: old client round-trips", (rootp / 'RUNSUP.BIN').read_bytes() == runs)

        igc_list(pkt, 'SUB')  # point cwd elsewhere to prove it is ignored
        tree_payload = b'Y' * 2500
        igc_upload(pkt, tree_payload, 'TREE\\DEEP\\LEAF.BIN')
        check("path-qualified upload created nested file",
              (rootp / 'TREE' / 'DEEP' / 'LEAF.BIN').read_bytes() == tree_payload)

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

        # --- a failed upload (bad CRC) must report ERROR and must not
        #     destroy the existing destination (temp-file + atomic rename) ---
        igc_list(pkt, '')
        bad = StartPacket(direction=Direction.VICTOR_TO_PC, file_size=5,
                          file_crc=0xDEADBEEF, file_date=0, file_time=0,
                          file_attr=0x20, filename='HELLO.TXT')
        pkt.send_packet(bad.pack())
        ready = parse_packet(pkt.receive_packet(timeout=5))
        assert isinstance(ready, ReadyPacket) and ready.status == 0, "no READY"
        pkt.send_packet(DataPacket(chunk_num=0, data=b'wrong').pack(), timeout=5)
        pkt.send_packet(EndPacket(total_chunks=1, bytes_sent=5,
                                  file_crc=0xDEADBEEF, status=Status.OK).pack())
        reply = pkt.receive_packet(timeout=5)
        check("failed upload -> ERROR", reply[0] == Command.ERROR)
        check("failed upload leaves original intact",
              (rootp / 'HELLO.TXT').read_bytes() == b'hello victor\r\n')
        check("failed upload leaves no temp file",
              not list(rootp.glob('*.igctmp')))

        # --- sandbox escape rejected ---
        ok, _ = igc_simple(pkt, bytes([CMD_DELETE]) + _p('..\\..\\etc\\passwd'))
        check("sandbox escape rejected", not ok)

        # --- disk images: push (Victor reads its floppy into a file here) ---
        ss = make_test_image(v9kimage.SECTORS_SS, False)
        ds = make_test_image(v9kimage.SECTORS_DS, True)
        igc_list(pkt, '')
        st = {}
        igc_img_push(pkt, 'PUSHED.IMG', ss, stats=st)
        check("img push SS .IMG stored", (rootp / 'PUSHED.IMG').read_bytes() == ss)
        check("img push used raw and RLE fragments",
              st.get('raw', 0) >= 1 and st.get('rle', 0) >= 1)
        igc_img_push(pkt, 'PUSHED.RLE', ss)
        rle = (rootp / 'PUSHED.RLE').read_bytes()
        check("img push SS .RLE decodes", v9kimage.rle_to_img(rle) == ss)
        check("img push .RLE header size/CRC",
              v9kimage.unpack_rle_header(rle) == (len(ss), crc32(ss)))
        igc_img_push(pkt, 'PUSHDS.IMG', ds)
        got = (rootp / 'PUSHDS.IMG').read_bytes()
        check("img push DS .IMG stored", len(got) == 1224192 and got == ds)

        # --- disk images: pull (Victor writes a floppy from a file here) ---
        st = {}
        check("img pull .RLE matches", igc_img_pull(pkt, 'PUSHED.RLE', stats=st) == ss)
        check("img pull used raw and RLE fragments",
              st.get('raw', 0) >= 1 and st.get('rle', 0) >= 1)
        check("img pull .IMG matches", igc_img_pull(pkt, 'PUSHED.IMG') == ss)
        check("img pull DS .IMG matches", igc_img_pull(pkt, 'PUSHDS.IMG') == ds)
        # The Victor writes .rle files as independently compressed 1 KB slices;
        # that concatenation must load like a one-shot stream.
        sliced = v9kimage.pack_rle_header(len(ss), crc32(ss)) + b''.join(
            v9kimage.rle_compress(ss[i:i + 1024]) for i in range(0, len(ss), 1024))
        (rootp / 'SLICED.RLE').write_bytes(sliced)
        check("img pull Victor-style sliced .RLE", igc_img_pull(pkt, 'SLICED.RLE') == ss)

        # --- disk images: error paths ---
        check("img open bad extension refused", igc_img_open(pkt, 0, 'DISK.TXT', 1224)[0] != 0)
        check("img open bad sector count refused", igc_img_open(pkt, 0, 'DISK.IMG', 1000)[0] == 2)
        check("img open missing file -> 1", igc_img_open(pkt, 1, 'NOPE.IMG') == (1, 0))
        (rootp / 'FOO.IMG').write_bytes(b'\0' * 1000)
        check("img open wrong size -> 2", igc_img_open(pkt, 1, 'FOO.IMG') == (2, 0))
        bad = bytearray(rle)
        bad[-1] ^= 0xFF
        (rootp / 'BADCRC.RLE').write_bytes(bytes(bad))
        check("img open corrupt .RLE -> 2", igc_img_open(pkt, 1, 'BADCRC.RLE')[0] == 2)
        pkt.send_packet(bytes([CMD_IMG_GET, 0]))
        check("img get without open job -> ERROR",
              pkt.receive_packet(timeout=5)[0] == Command.ERROR)

        assert igc_img_open(pkt, 0, 'WRONGCRC.IMG', v9kimage.SECTORS_SS)[0] == 0
        for trk in range(80):
            igc_img_send_track(pkt, ss, trk)
        check("img close wrong CRC -> ERROR", not igc_img_close(pkt, 0, crc32(ss) ^ 1))
        check("img close wrong CRC writes nothing",
              not (rootp / 'WRONGCRC.IMG').exists() and not list(rootp.glob('*.igctmp')))

        assert igc_img_open(pkt, 0, 'SHORT.IMG', v9kimage.SECTORS_SS)[0] == 0
        for trk in range(79):
            igc_img_send_track(pkt, ss, trk)
        check("img close with tracks missing -> ERROR", not igc_img_close(pkt, 0, crc32(ss)))
        check("img close with tracks missing writes nothing", not (rootp / 'SHORT.IMG').exists())

        assert igc_img_open(pkt, 0, 'ABORTED.IMG', v9kimage.SECTORS_SS)[0] == 0
        igc_img_send_track(pkt, ss, 0)
        # A fragment out of order (frag 1 before frag 0) poisons its track; the
        # error is reported once, after the track's last fragment.
        tb = v9kimage.track_bytes(1)
        nf = math.ceil(tb / 1024)
        for i in [1, 0] + list(range(2, nf)):
            seg = ss[tb + i * 1024:tb + min((i + 1) * 1024, tb)]
            pkt.send_packet(struct.pack('<BBBBBH', CMD_IMG_TRACK, 1, i,
                                        nf, 0, len(seg)) + seg, timeout=5)
        check("img out-of-order fragment -> ERROR at track end",
              pkt.receive_packet(timeout=5)[0] == Command.ERROR)
        check("img track resent after error -> OK", igc_img_send_track(pkt, ss, 1) == CMD_OK)
        check("img abort -> OK", igc_img_close(pkt, 1, 0))
        check("img abort writes nothing", not (rootp / 'ABORTED.IMG').exists())
        check("img close with no job -> ERROR", not igc_img_close(pkt, 0, 0))

        igc_list(pkt, 'TREE')  # cwd elsewhere: a path-qualified name ignores it
        igc_img_push(pkt, 'SUB\\DEEP.IMG', ss)
        check("img path-qualified name lands under root/SUB",
              (rootp / 'SUB' / 'DEEP.IMG').read_bytes() == ss)
        # Mode 1 names are root-relative, like downloads, whatever was listed last.
        check("img pull root file while cwd elsewhere", igc_img_pull(pkt, 'PUSHED.RLE') == ss)
        check("img pull path-qualified name", igc_img_pull(pkt, 'SUB\\DEEP.IMG') == ss)

        # --- the image job state must not disturb ordinary requests ---
        check("list after image tests", 'PUSHED.IMG' in {e['name'] for e in igc_list(pkt, '')})
        check("download after image tests", igc_download(pkt, 'HELLO.TXT') == b'hello victor\r\n')

        # --- malformed image packets get a reply and do not wedge the job ---
        pkt.send_packet(bytes([CMD_IMG_OPEN, 0, 0, 0]))
        r = pkt.receive_packet(timeout=5)
        check("short IMG_OPEN -> IMG_INFO refused", r[0] == CMD_IMG_INFO and r[1] != 0)
        igc_list(pkt, '')
        n = b'SHORTCL.IMG'
        pkt.send_packet(struct.pack('<BBHB', CMD_IMG_OPEN, 0, 1224, len(n)) + n)
        r = pkt.receive_packet(timeout=5)
        check("open for the short-CLOSE test", r[0] == CMD_IMG_INFO and r[1] == 0)
        pkt.send_packet(bytes([CMD_IMG_TRACK, 0, 0]))
        r = pkt.receive_packet(timeout=5)
        check("short IMG_TRACK -> ERROR", r[0] == Command.ERROR)
        pkt.send_packet(bytes([CMD_IMG_CLOSE, 0]))
        r = pkt.receive_packet(timeout=5)
        check("short IMG_CLOSE -> ERROR", r[0] == Command.ERROR)
        pkt.send_packet(struct.pack('<BBI', CMD_IMG_CLOSE, 1, 0))
        r = pkt.receive_packet(timeout=5)
        check("job survives a short CLOSE: abort -> OK", r[0] == CMD_OK)
        check("short-CLOSE test wrote nothing", not (rootp / 'SHORTCL.IMG').exists())

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
