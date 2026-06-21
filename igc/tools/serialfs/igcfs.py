#!/usr/bin/env python3
"""
igcfs - igc serial file server for modern PCs.

Serves a directory on this machine to the igc file manager running on a
Victor 9000, over a direct serial cable (e.g. this host's /dev/ttyUSB0 wired to
the Victor's COM1 / Serial A). igc browses and edits the served files in a pane,
exactly as if it were a local drive.

This is the PC-side *server* counterpart of the vetted file-transfer protocol in
viclibc (serial/ + filetrx/). In the original test harness the PC *initiated*
and the Victor served (FTXSERV); here the roles invert: the Victor (igc) is the
client and this program serves. We reuse the vetted packet/FTX primitives
(vendored under v9kserial/ + v9kfiletrx/) and add the serve loop - the mirror of
the C ftx_serve_one() dispatch, read from the server's (PC) point of view.

Wire protocol (per request, all carried as reliable DATA packets):
  - LIST   (0x15): reply one or more LIST_RESP (0x16) pages of 8.3 dir entries.
  - START  (0x10):
        direction VICTOR_TO_PC -> Victor is pushing a file: we RECEIVE & store.
        direction PC_TO_VICTOR  -> Victor is pulling a file: we SEND it.
  - DELETE (0x1A) / MKDIR (0x1B) / RENAME (0x1C): filesystem op, reply OK/ERROR.
  - QUIT   (0x1F): stop serving.

Run:
  python3 igcfs.py --root /srv/victor --dev /dev/ttyUSB0 --baud 38400 -v
"""

import argparse
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from v9kserial import SerialConnection, PacketProtocol, PacketError
from v9kserial.constants import BAUD_RATES, BAUD_38400
from v9kfiletrx import (
    Command, Direction, Compression, Status, Error,
    StartPacket, DataPacket, EndPacket, ReadyPacket,
    ListPacket, ListRespPacket, DirEntry,
    FileTransfer, crc32_file, crc32,
)
from v9kfiletrx.compress import rle_decompress
from v9kfiletrx.protocol import CHUNK_SIZE, ResendPacket, ErrorPacket, parse_packet

# --- igc protocol extension (must match igc/src/serialfs_proto.h) -------------
# The vetted FTX command space uses 0x10-0x1F (0x1F = QUIT); 0x1A-0x1D are free.
CMD_DELETE = 0x1A   # payload: [cmd][path_len][path]
CMD_MKDIR  = 0x1B   # payload: [cmd][path_len][path]
CMD_RENAME = 0x1C   # payload: [cmd][old_len][old][new_len][new]
CMD_OK     = 0x1D   # reply:   [cmd][status]  (status 0 = ok)
CMD_QUIT   = 0x1F   # FTX_CMD_QUIT

# DOS attribute bits we surface in listings.
ATTR_READONLY = 0x01
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20

# Max 8.3 entries per LIST_RESP packet: 3-byte header + N*22 must stay <= 1024.
# (We leave margin below the theoretical 46.)
ENTRIES_PER_PACKET = 40

IDLE_TIMEOUT = 0.5      # serve-loop poll interval while no request is pending


class QuitServer(Exception):
    """Raised to unwind the serve loop on a QUIT command."""


def log(msg):
    print(msg, flush=True)


def _rate(nbytes, secs):
    """Format an elapsed-time + throughput suffix for transfer logs."""
    if secs <= 0:
        return ""
    return f"  ({secs*1000:.0f} ms, {nbytes/secs/1024:.2f} KB/s)"


# ---------------------------------------------------------------------------
# 8.3 name mapping
#
# Modern files may have long/mixed-case names; DOS/igc only understands 8.3
# uppercase. build_dos_map() assigns each real entry in a directory a unique
# 8.3 name deterministically (sorted, ~N on collision), so listing and any
# later resolve-by-8.3-name agree without server-side state.
# ---------------------------------------------------------------------------

_DOS_OK = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~")


def _clean(part, maxlen):
    out = []
    for ch in part.upper():
        if ch in _DOS_OK:
            out.append(ch)
        elif ch in (' ', '.'):
            continue  # drop spaces and stray dots
        else:
            out.append('_')
        if len(out) >= maxlen:
            break
    return ''.join(out)


def _dos_remote_name(name):
    """Uppercase a Victor file name and clamp only its 8.3 *filename* component
    to 12 chars (8 + '.' + 3), preserving any drive/directory prefix. A bare
    [:12] on the whole string corrupts qualified paths, e.g. 'B:\\COMMAND.COM'
    -> 'B:\\COMMAND.C' (then 'file not found'); the wire field holds 63 chars."""
    name = name.upper()
    idx = max(name.rfind('\\'), name.rfind('/'), name.rfind(':'))
    return name[:idx + 1] + name[idx + 1:][:12]


def _short_name(real_name, is_dir):
    """Best-effort 8.3 form of a single filename (no collision handling)."""
    if is_dir:
        base, ext = real_name, ''
    else:
        dot = real_name.rfind('.')
        if dot > 0:
            base, ext = real_name[:dot], real_name[dot + 1:]
        else:
            base, ext = real_name, ''
    base8 = _clean(base, 8)
    ext3 = _clean(ext, 3)
    if not base8:
        base8 = 'FILE'
    fits = (real_name.upper() == (base8 + ('.' + ext3 if ext3 else '')) and
            len(_clean(base, 9)) <= 8 and len(_clean(ext, 4)) <= 3)
    return base8, ext3, fits


def build_dos_map(dirpath):
    """Return {DOSNAME: real_name} for dirpath, deterministic & collision-free."""
    try:
        names = sorted(os.listdir(dirpath))
    except OSError:
        return {}
    mapping = {}
    used = set()
    for real in names:
        full = os.path.join(dirpath, real)
        is_dir = os.path.isdir(full)
        base8, ext3, fits = _short_name(real, is_dir)
        cand = base8 + ('.' + ext3 if ext3 else '')
        if not fits or cand in used:
            # Mangle with ~N until unique.
            n = 1
            while True:
                suffix = '~' + str(n)
                trunc = base8[:max(1, 8 - len(suffix))]
                cand = trunc + suffix + ('.' + ext3 if ext3 else '')
                if cand not in used:
                    break
                n += 1
        used.add(cand)
        mapping[cand] = real
    return mapping


# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------

class IgcFileServer:
    def __init__(self, root, conn, verbose=False, frame_gap=0.0):
        self.root = Path(root).resolve()
        self.conn = conn
        self.verbose = verbose
        # Optional fixed pause after each DATA frame on download (PC->Victor).
        # Gives the polled Victor receiver time to send its ACK and re-enter its
        # poll loop before the next chunk arrives - covering the post-ACK
        # turnaround that the 3-byte RX FIFO cannot absorb at high baud. Far
        # cheaper than per-byte pacing (one gap per ~1 KB chunk, not per byte).
        self.frame_gap = frame_gap
        # One packet layer shared by the serve loop and FileTransfer, so the
        # alternating sequence bits stay consistent across every exchange.
        self.pkt = PacketProtocol(conn, timeout=2.0, retries=5)
        self.ft = FileTransfer(self.pkt, timeout=5.0)
        # Current directory (relative to root) - tracked from the last LIST so
        # uploads, whose START carries only a basename, land in the dir igc is
        # viewing. Explicit-path ops (download/delete/mkdir/rename) ignore it.
        self.cwd = ''

    # --- path helpers ------------------------------------------------------

    def _resolve(self, rel):
        """Map a Victor relative path to a real path under root (sandboxed)."""
        rel = (rel or '').replace('\\', '/').strip('/')
        target = (self.root / rel).resolve() if rel else self.root
        # Reject escapes outside the sandbox root.
        if target != self.root and self.root not in target.parents:
            raise PermissionError(f"path escapes root: {rel!r}")
        return target

    def _resolve_dir_and_name(self, rel):
        """Split a relative path into (real_dir, real_basename) resolving the
        final 8.3 component against the directory's DOS map."""
        rel = (rel or '').replace('\\', '/').strip('/')
        if '/' in rel:
            parent_rel, dosname = rel.rsplit('/', 1)
        else:
            parent_rel, dosname = '', rel
        real_dir = self._resolve(parent_rel)
        mapping = build_dos_map(real_dir)
        real_name = mapping.get(dosname.upper(), dosname)
        return real_dir, real_name

    # --- dispatch ----------------------------------------------------------

    def serve_forever(self):
        log(f"igcfs serving {self.root}  (Ctrl-C to stop)")
        while True:
            if self.conn.rx_error:
                log(f"serial error: {self.conn.rx_error}")
                return
            try:
                payload = self.pkt.receive_packet(timeout=IDLE_TIMEOUT)
            except PacketError:
                continue  # idle (no request) or a transient frame error
            if not payload:
                continue
            try:
                self.dispatch(payload)
            except QuitServer:
                log("  <- QUIT")
                return
            except Exception as e:  # never let one bad request kill the server
                log(f"  ! request failed: {e}")

    def dispatch(self, payload):
        cmd = payload[0]
        if cmd == Command.LIST:
            self.handle_list(payload)
        elif cmd == Command.START:
            start = StartPacket.unpack(payload)
            if start.direction == Direction.VICTOR_TO_PC:
                self.handle_upload(start)
            else:
                self.handle_download(start)
        elif cmd == CMD_DELETE:
            self.handle_delete(self._req_path(payload))
        elif cmd == CMD_MKDIR:
            self.handle_mkdir(self._req_path(payload))
        elif cmd == CMD_RENAME:
            self.handle_rename(payload)
        elif cmd == CMD_QUIT:
            raise QuitServer()
        else:
            log(f"  <- (unknown request 0x{cmd:02X})")

    @staticmethod
    def _req_path(payload):
        plen = payload[1]
        return payload[2:2 + plen].decode('ascii', errors='replace')

    def _reply_ok(self):
        self.pkt.send_packet(bytes([CMD_OK, 0]))

    def _reply_error(self, code, message=''):
        self.pkt.send_packet(ErrorPacket(error_code=code, message=message).pack())

    # --- LIST --------------------------------------------------------------

    def handle_list(self, payload):
        req = ListPacket.unpack(payload)
        rel = req.path.replace('\\', '/').strip('/')
        log(f"  <- listdir {rel or '(root)'}")
        try:
            real_dir = self._resolve(rel)
            if not real_dir.is_dir():
                raise FileNotFoundError(rel)
        except Exception as e:
            log(f"  -> listdir failed: {e}")
            self._reply_error(Error.NOT_FOUND, str(e))
            return

        self.cwd = rel  # remember for basename-only uploads

        entries = []
        if rel:  # offer ".." for navigation when not at the root
            entries.append(DirEntry(name='..', attr=ATTR_DIRECTORY))
        mapping = build_dos_map(real_dir)
        for dosname, real in mapping.items():
            full = real_dir / real
            try:
                st = full.stat()
            except OSError:
                continue
            is_dir = full.is_dir()
            attr = ATTR_DIRECTORY if is_dir else ATTR_ARCHIVE
            if not is_dir and not os.access(full, os.W_OK):
                attr |= ATTR_READONLY
            date, tm = FileTransfer._unix_to_dos_datetime(st.st_mtime)
            entries.append(DirEntry(
                name=dosname, attr=attr, date=date, time=tm,
                size=0 if is_dir else min(st.st_size, 0xFFFFFFFF)))

        # Page the entries into LIST_RESP packets.
        if not entries:
            self.pkt.send_packet(ListRespPacket(entries=[], more_entries=False).pack())
        else:
            for i in range(0, len(entries), ENTRIES_PER_PACKET):
                page = entries[i:i + ENTRIES_PER_PACKET]
                more = (i + ENTRIES_PER_PACKET) < len(entries)
                self.pkt.send_packet(ListRespPacket(entries=page, more_entries=more).pack())
        log(f"  -> {len(entries)} entries")

    # --- download (Victor pulls a file from us) ----------------------------

    def handle_download(self, start):
        # The request START carries the file's relative path in its filename.
        rel = start.filename
        log(f"  <- getfile {rel}")
        try:
            real_dir, real_name = self._resolve_dir_and_name(rel)
            src = real_dir / real_name
            if not src.is_file():
                raise FileNotFoundError(rel)
        except Exception as e:
            log(f"  -> getfile failed: {e}")
            self._reply_error(Error.NOT_FOUND, str(e))
            return
        t0 = time.time()
        sent = self._send_file(src, os.path.basename(rel))
        log(f"  -> sent {sent} bytes{_rate(sent, time.time() - t0)}")

    def _send_file(self, src, remote_name):
        """START(metadata) -> READY -> DATA... -> END, the PC->Victor send the
        Victor's stock ftx_receive_file expects.

        Deliberately NOT FileTransfer.send_file: that ends with a trailing
        "wait for final ACK" receive which, against a back-to-back client, eats
        the *next* request packet. The corrected C ftx_send_file dropped exactly
        that trailing read (ftx_send.c) - we mirror it here.
        """
        src = Path(src)
        data = src.read_bytes()
        file_crc = crc32(data)
        total = max(1, (len(data) + CHUNK_SIZE - 1) // CHUNK_SIZE)
        date, tm = FileTransfer._unix_to_dos_datetime(src.stat().st_mtime)
        start = StartPacket(direction=Direction.PC_TO_VICTOR, file_size=len(data),
                            file_crc=file_crc, file_date=date, file_time=tm,
                            file_attr=ATTR_ARCHIVE, filename=_dos_remote_name(remote_name))
        self.pkt.send_packet(start.pack())
        ready = parse_packet(self.ft._recv_packet())
        if not isinstance(ready, ReadyPacket) or ready.status != 0:
            raise IOError("receiver not ready")
        for i in range(total):
            chunk = data[i * CHUNK_SIZE:(i + 1) * CHUNK_SIZE]
            try:
                self.pkt.send_packet(DataPacket(chunk_num=i, data=chunk).pack(),
                                     timeout=self.ft.DATA_ACK_TIMEOUT)
            except PacketError as e:
                log(f"  ! DATA chunk {i}/{total} (offset {i*CHUNK_SIZE}) failed: {e}")
                raise
            if self.frame_gap:
                time.sleep(self.frame_gap)
        self.pkt.send_packet(EndPacket(total_chunks=total, bytes_sent=len(data),
                                       file_crc=file_crc, status=Status.OK).pack())
        return len(data)

    # --- upload (Victor pushes a file to us) -------------------------------

    def handle_upload(self, start):
        # START carries only a basename; place it in the directory igc is
        # viewing (tracked from the last LIST).
        try:
            dest_dir = self._resolve(self.cwd)
            dest = dest_dir / start.filename
            if self.root not in dest.resolve().parents and dest.resolve() != self.root:
                # dest must be strictly inside root
                if self.root not in dest.parents:
                    raise PermissionError("path escapes root")
        except Exception as e:
            log(f"  -> putfile rejected: {e}")
            self.pkt.send_packet(ReadyPacket(status=1).pack())
            return
        log(f"  <- putfile {start.filename} ({start.file_size} bytes)")
        self._recv_after_start(start, dest)

    def _recv_after_start(self, start, dest_path):
        """Receive a file whose START we've already consumed. Ported from
        FileTransfer.receive_file (which reads the START itself)."""
        dest_path = Path(dest_path)
        t0 = time.time()
        self.pkt.send_packet(ReadyPacket(status=0).pack())  # READY
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            with open(dest_path, 'wb') as f:
                expected = 0
                while True:
                    pkt = self._parse(self.ft._recv_packet())
                    if isinstance(pkt, EndPacket):
                        if pkt.status != Status.OK:
                            raise IOError("sender reported error")
                        break
                    if not isinstance(pkt, DataPacket):
                        raise IOError("expected DATA")
                    if pkt.chunk_num != expected:
                        self.pkt.send_packet(ResendPacket(chunk_num=expected).pack())
                        continue
                    data = (rle_decompress(pkt.data)
                            if start.compression == Compression.RLE else pkt.data)
                    f.write(data)
                    expected += 1
        except Exception:
            if dest_path.exists():
                dest_path.unlink()
            raise
        # Verify whole-file CRC-32 and apply the original timestamp.
        if crc32_file(str(dest_path)) != start.file_crc:
            dest_path.unlink()
            log("  -> putfile CRC mismatch")
            return
        mtime = FileTransfer._dos_to_unix_datetime(start.file_date, start.file_time)
        if mtime:
            os.utime(dest_path, (mtime, mtime))
        nbytes = dest_path.stat().st_size
        log(f"  -> received {nbytes} bytes{_rate(nbytes, time.time() - t0)}")

    @staticmethod
    def _parse(data):
        from v9kfiletrx.protocol import parse_packet
        return parse_packet(data)

    # --- delete / mkdir / rename ------------------------------------------

    def handle_delete(self, rel):
        log(f"  <- delete {rel}")
        try:
            real_dir, real_name = self._resolve_dir_and_name(rel)
            target = real_dir / real_name
            if target.is_dir():
                target.rmdir()  # only removes empty dirs (matches DOS RMDIR)
            else:
                target.unlink()
            self._reply_ok()
        except Exception as e:
            log(f"  -> delete failed: {e}")
            self._reply_error(Error.FILE, str(e))

    def handle_mkdir(self, rel):
        log(f"  <- mkdir {rel}")
        try:
            # Resolve parent under root, then create the new (real-named) dir.
            rel = rel.replace('\\', '/').strip('/')
            parent_rel, new = (rel.rsplit('/', 1) if '/' in rel else ('', rel))
            parent = self._resolve(parent_rel)
            (parent / new).mkdir()
            self._reply_ok()
        except Exception as e:
            log(f"  -> mkdir failed: {e}")
            self._reply_error(Error.FILE, str(e))

    def handle_rename(self, payload):
        old_len = payload[1]
        old = payload[2:2 + old_len].decode('ascii', errors='replace')
        i = 2 + old_len
        new_len = payload[i]
        new = payload[i + 1:i + 1 + new_len].decode('ascii', errors='replace')
        log(f"  <- rename {old} -> {new}")
        try:
            old_dir, old_real = self._resolve_dir_and_name(old)
            # New name is a bare 8.3 name in the same directory.
            new = new.replace('\\', '/').strip('/')
            new_name = new.rsplit('/', 1)[-1]
            (old_dir / old_real).rename(old_dir / new_name)
            self._reply_ok()
        except Exception as e:
            log(f"  -> rename failed: {e}")
            self._reply_error(Error.FILE, str(e))


def main(argv=None):
    ap = argparse.ArgumentParser(description="igc serial file server")
    ap.add_argument('--root', required=True, help="directory to serve")
    ap.add_argument('--dev', default='/dev/ttyUSB0', help="serial device")
    ap.add_argument('--baud', type=int, default=BAUD_RATES[BAUD_38400],
                    help="baud rate (default 38400)")
    ap.add_argument('--no-rtscts', dest='rtscts', action='store_false',
                    help="disable RTS/CTS hardware flow control (default on)")
    ap.add_argument('--pace-bytes', dest='pace', action='store_true',
                    help="transmit one byte per write()+flush() so a slow polled "
                         "receiver keeps up at higher baud")
    ap.add_argument('--byte-delay', type=float, default=0.0, metavar='SECONDS',
                    help="sleep this long after each paced byte (implies "
                         "--pace-bytes); e.g. 0.001 for 1ms")
    ap.add_argument('--cts-gate', dest='cts_gate', action='store_true',
                    help="packet-granular software flow control: hold each "
                         "frame until the Victor asserts CTS (its RTS). Best "
                         "for higher baud over a USB-serial cable; forces "
                         "--no-rtscts")
    ap.add_argument('--gate-settle', type=float, default=0.003, metavar='SECONDS',
                    help="pre-send settle delay per frame in --cts-gate mode, "
                         "covering the request/response turnaround (default "
                         "0.003 = 3ms)")
    ap.add_argument('--frame-gap', type=float, default=0.0, metavar='SECONDS',
                    help="pause after each DATA frame on download so the polled "
                         "Victor can ACK and re-arm before the next chunk (e.g. "
                         "0.003 = 3ms); covers post-ACK turnaround without per-byte pacing")
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args(argv)
    if args.byte_delay > 0.0:
        args.pace = True
    if args.cts_gate:
        args.rtscts = False  # software gating owns CTS; don't let HW also gate

    root = Path(args.root)
    if not root.is_dir():
        ap.error(f"--root {args.root} is not a directory")

    # Open with rtscts set from the start: on a direct cable the host must honor
    # the Victor's polled-mode RTS gating, or the Victor's 3-byte receive FIFO
    # overflows on the request/response turnaround (esp. at 38400).
    conn = SerialConnection(args.dev, baudrate=args.baud, rtscts=args.rtscts,
                            pace=args.pace, byte_delay=args.byte_delay,
                            cts_gate=args.cts_gate, gate_settle=args.gate_settle)
    try:
        conn.connect()
    except Exception as e:
        log(f"failed to open {args.dev}: {e}")
        return 2
    # Read the flag back from the live port so the log reflects reality, not the
    # request - if the driver couldn't honor it we want to see that here.
    actual = bool(getattr(conn._serial, 'rtscts', False)) if conn._serial else False
    pace_desc = (f" pace=1B/flush"
                 f"{f'+{args.byte_delay*1000:.2f}ms' if args.byte_delay else ''}"
                 if args.pace else "")
    gate_desc = (f" cts-gate+{args.gate_settle*1000:.0f}ms"
                 if args.cts_gate else "")
    log(f"opened {args.dev} @ {args.baud} 8N1 "
        f"rtscts={'on' if actual else 'off'}"
        f"{'' if actual == args.rtscts else f' (requested {args.rtscts}!)'}"
        f"{pace_desc}{gate_desc}")

    server = IgcFileServer(root, conn, verbose=args.verbose, frame_gap=args.frame_gap)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("\ninterrupted")
    finally:
        conn.disconnect()
    return 0


if __name__ == '__main__':
    sys.exit(main())
