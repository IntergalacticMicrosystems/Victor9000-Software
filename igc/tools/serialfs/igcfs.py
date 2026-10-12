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
            A bare-basename filename lands in the last-LISTed dir (cwd); a
            path-qualified filename (contains a separator) is stored under root,
            which is how igc copies whole directory trees. The final component
            is reverse-mapped through the directory's 8.3 map, so writing to
            MYDOCU~1.TXT updates 'My Documents.txt' rather than forking a new
            literal-8.3 file. The store is atomic (temp file + rename after the
            CRC check), and after the client's END we reply CMD_OK on success
            or ERROR on failure - igc waits for this, so igc and igcfs must be
            updated together.
        direction PC_TO_VICTOR  -> Victor is pulling a file: we SEND it.
  - DELETE (0x1A) / MKDIR (0x1B) / RENAME (0x1C): filesystem op, reply OK/ERROR.
  - QUIT   (0x1F): stop serving.
  - IMG_OPEN/IMG_GET/IMG_TRACK/IMG_CLOSE (0x30-0x34): floppy disk imaging, one
    track per exchange. The image is held in memory (ImageJob) between OPEN and
    CLOSE; see the command block below and README.md "Disk images".

Run:
  python3 igcfs.py --root /srv/victor --dev /dev/ttyUSB0 --baud 38400 -v
"""

import argparse
import math
import os
import struct
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
from v9kfiletrx.compress import rle_decompress, compress_if_beneficial
from v9kfiletrx.protocol import (CHUNK_SIZE, DATA_FLAG_COMPRESSED, ResendPacket,
                                 ErrorPacket, parse_packet)
import v9kimage

# --- igc protocol extension (must match igc/src/serialfs_proto.h) -------------
# The vetted FTX command space uses 0x10-0x1F (0x1F = QUIT); 0x1A-0x1D are free.
CMD_DELETE = 0x1A   # payload: [cmd][path_len][path]
CMD_MKDIR  = 0x1B   # payload: [cmd][path_len][path]
CMD_RENAME = 0x1C   # payload: [cmd][old_len][old][new_len][new]
CMD_OK     = 0x1D   # reply:   [cmd][status]  (status 0 = ok)
CMD_QUIT   = 0x1F   # FTX_CMD_QUIT

# Disk imaging (0x30 block; 0x20-0x2F is reserved for FTX tests). Linear track
# t is 0..159 (side 0 tracks 0..79, then side 1); geometry lives in v9kimage.
CMD_IMG_OPEN  = 0x30  # V->PC: [cmd][mode][total_sectors u16][name_len][name]
                      #        mode 0 = create image (Victor pushes tracks),
                      #        mode 1 = read image (Victor pulls tracks)
CMD_IMG_INFO  = 0x31  # PC->V: [cmd][status][total_sectors u16]  reply to OPEN
                      #        status 0 ok, 1 not found / cannot open,
                      #        2 not a valid Victor disk image, 3 other error
CMD_IMG_GET   = 0x32  # V->PC: [cmd][track]  -> that track's IMG_TRACK fragments
CMD_IMG_TRACK = 0x33  # both:  [cmd][track][frag][nfrag][flags][len u16][data]
                      #        fragment i = track bytes i*1024.., flags bit 0 = RLE.
                      #        Mode 0: server replies CMD_OK/ERROR once per track,
                      #        after the fragment with frag == nfrag-1.
CMD_IMG_CLOSE = 0x34  # V->PC: [cmd][status][crc32 u32]  status 0 done, 1 abort
                      #        reply CMD_OK / ERROR
IMG_FRAG_SIZE = 1024
IMG_FLAG_RLE = 0x01
IMG_ST_OK, IMG_ST_NOT_FOUND, IMG_ST_INVALID, IMG_ST_OTHER = 0, 1, 2, 3

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


class ImageJob:
    """State of one disk-image transfer between IMG_OPEN and IMG_CLOSE.

    The whole image lives in memory (at most 1.2 MB): mode 0 assembles it track
    by track and writes the file only at CLOSE (which is what makes .rle output
    a single compress call); mode 1 holds the fully decoded file so IMG_GET is
    a plain slice."""

    def __init__(self, mode, dest, name, image):
        self.mode = mode
        self.dest = dest            # real Path of the .img/.rle file
        self.name = name            # name as the Victor sent it (for logs)
        self.image = image          # bytearray (mode 0) / bytes (mode 1)
        self.ntracks = v9kimage.track_count(len(image) // v9kimage.SECTOR_SIZE)
        self.received = [False] * self.ntracks
        self.t0 = time.time()
        # Mode-0 track being assembled from fragments.
        self.cur_track = None
        self.cur_frags = []
        self.cur_bad = None         # reason the current track was dropped

    def track_span(self, t):
        start = v9kimage.track_first_sector(t) * v9kimage.SECTOR_SIZE
        return start, start + v9kimage.track_bytes(t)


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
    """Return {DOSNAME: real_name} for dirpath, deterministic & collision-free.

    Caveat: the ~N collision numbering is derived from the directory's sorted
    real names at call time. If files are added or removed server-side between
    igc's LIST and a later operation, a mangled name can remap to a different
    real file. Don't reshuffle a served directory mid-session."""
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
        self.img_job = None   # ImageJob between IMG_OPEN and IMG_CLOSE

    # --- path helpers ------------------------------------------------------

    def _resolve(self, rel):
        """Map a Victor relative path to a real path under root (sandboxed).

        Every component is reverse-mapped through its parent directory's 8.3 DOS
        map, so a mangled name (e.g. MYDOCU~1) resolves to its real on-disk name
        (e.g. 'My Documents') at *each* level - not just the last. This is what
        lets igc browse into and copy long-named directories. A component that
        doesn't match a real entry (e.g. a not-yet-created dir) passes through
        literally; the DOS map is only consulted while still inside the sandbox."""
        rel = (rel or '').replace('\\', '/').strip('/')
        target = self.root
        for part in (rel.split('/') if rel else []):
            if part in ('', '.'):
                continue
            if part == '..':
                target = target.parent
                continue
            inside = (target == self.root or self.root in target.parents)
            real = build_dos_map(target).get(part.upper(), part) if inside else part
            target = target / real
        target = target.resolve()
        # Reject escapes outside the sandbox root.
        if target != self.root and self.root not in target.parents:
            raise PermissionError(f"path escapes root: {rel!r}")
        return target

    def _resolve_dir_and_name(self, rel):
        """Split a relative path into (real_dir, real_basename), mapping the
        parent components via _resolve and the final 8.3 component against the
        parent directory's DOS map."""
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
        elif cmd == CMD_IMG_OPEN:
            self.handle_img_open(payload)
        elif cmd == CMD_IMG_GET:
            self.handle_img_get(payload)
        elif cmd == CMD_IMG_TRACK:
            self.handle_img_track(payload)
        elif cmd == CMD_IMG_CLOSE:
            self.handle_img_close(payload)
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
        sent = self._send_file(src, os.path.basename(rel), start.compression)
        log(f"  -> sent {sent} bytes{_rate(sent, time.time() - t0)}")

    def _send_file(self, src, remote_name, compression=Compression.NONE):
        """START(metadata) -> READY -> DATA... -> END, the PC->Victor send the
        Victor's stock ftx_receive_file expects.

        compression is whatever the request START asked for: with RLE each chunk
        is compressed when that saves bytes and flagged per chunk, exactly like
        the C sender; incompressible chunks go raw and unflagged.

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
        start = StartPacket(direction=Direction.PC_TO_VICTOR, compression=compression,
                            file_size=len(data), file_crc=file_crc, file_date=date,
                            file_time=tm, file_attr=ATTR_ARCHIVE,
                            filename=_dos_remote_name(remote_name))
        self.pkt.send_packet(start.pack())
        ready = parse_packet(self.ft._recv_packet())
        if not isinstance(ready, ReadyPacket) or ready.status != 0:
            raise IOError("receiver not ready")
        for i in range(total):
            chunk = data[i * CHUNK_SIZE:(i + 1) * CHUNK_SIZE]
            flags = 0
            if compression == Compression.RLE:
                chunk, packed = compress_if_beneficial(chunk)
                flags = DATA_FLAG_COMPRESSED if packed else 0
            try:
                self.pkt.send_packet(DataPacket(chunk_num=i, data=chunk, flags=flags).pack(),
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

    def _resolve_upload_dest(self, name):
        """Real destination path for a file the Victor is pushing.

        A bare basename lands in the directory igc is viewing (tracked from the
        last LIST); a path-qualified name (it contains a separator, as in
        directory-tree copies) is resolved under root. The final 8.3 component
        is reverse-mapped like the download path does, so writing to
        MYDOCU~1.TXT updates 'My Documents.txt' instead of creating a second,
        literally-named file beside it. A name with no mapping passes through
        (a genuinely new file)."""
        if '\\' in name or '/' in name:
            rel = name.replace('\\', '/').strip('/')
            parent_rel, base = (rel.rsplit('/', 1) if '/' in rel else ('', rel))
            dest_dir = self._resolve(parent_rel)
        else:
            dest_dir = self._resolve(self.cwd)
            base = name
        base = build_dos_map(dest_dir).get(base.upper(), base)
        dest = dest_dir / base
        # dest_dir is sandbox-checked by _resolve, but the final component
        # may be a symlink pointing outside the root - resolve and re-check.
        resolved = dest.resolve()
        if resolved != self.root and self.root not in resolved.parents:
            raise PermissionError("path escapes root")
        return dest

    def handle_upload(self, start):
        # Parents of a path-qualified name are auto-created by _recv_after_start.
        try:
            dest = self._resolve_upload_dest(start.filename)
        except Exception as e:
            log(f"  -> putfile rejected: {e}")
            self.pkt.send_packet(ReadyPacket(status=1).pack())
            return
        log(f"  <- putfile {start.filename} ({start.file_size} bytes)")
        self._recv_after_start(start, dest)

    def _recv_after_start(self, start, dest_path):
        """Receive a file whose START we've already consumed. Ported from
        FileTransfer.receive_file (which reads the START itself).

        The data lands in a temp file that replaces dest_path only after the
        whole-file CRC checks out, so a failed transfer can never destroy an
        existing file. Every exit reports its outcome: pre-transfer failures
        via the READY status, everything after via the post-END CMD_OK/ERROR
        reply that igc's serialfs_put_named waits for."""
        dest_path = Path(dest_path)
        tmp_path = dest_path.with_name(dest_path.name + '.igctmp')
        t0 = time.time()

        # Anything that fails before the data flows is a READY(status=1)
        # rejection, which the client already understands.
        try:
            dest_path.parent.mkdir(parents=True, exist_ok=True)
            f = open(tmp_path, 'wb')
        except Exception as e:
            log(f"  -> putfile rejected: {e}")
            self.pkt.send_packet(ReadyPacket(status=1).pack())
            return
        self.pkt.send_packet(ReadyPacket(status=0).pack())  # READY

        try:
            with f:
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
                    # The per-chunk flag, not the START mode, gates expansion:
                    # the sender leaves incompressible chunks raw and unflagged.
                    data = (rle_decompress(pkt.data)
                            if pkt.flags & DATA_FLAG_COMPRESSED else pkt.data)
                    f.write(data)
                    expected += 1
            # Verify the whole-file CRC-32, stamp the original timestamp, and
            # only then move the temp into place.
            if crc32_file(str(tmp_path)) != start.file_crc:
                raise IOError("CRC mismatch")
            mtime = FileTransfer._dos_to_unix_datetime(start.file_date, start.file_time)
            if mtime:
                os.utime(tmp_path, (mtime, mtime))
            os.replace(tmp_path, dest_path)
        except Exception as e:
            if tmp_path.exists():
                tmp_path.unlink()
            log(f"  -> putfile failed: {e}")
            self._reply_error(Error.FILE, str(e)[:60])
            return
        nbytes = dest_path.stat().st_size
        self._reply_ok()
        log(f"  -> received {nbytes} bytes{_rate(nbytes, time.time() - t0)}")

    @staticmethod
    def _parse(data):
        from v9kfiletrx.protocol import parse_packet
        return parse_packet(data)

    # --- disk images (IMG_OPEN / IMG_GET / IMG_TRACK / IMG_CLOSE) -----------

    def _img_info(self, status, total_sectors=0):
        self.pkt.send_packet(struct.pack('<BBH', CMD_IMG_INFO, status, total_sectors))

    def handle_img_open(self, payload):
        self.img_job = None  # a new OPEN discards any unfinished job
        if len(payload) < 5 or len(payload) < 5 + payload[4]:
            log("  <- imgopen refused: short packet")
            self._img_info(IMG_ST_OTHER)
            return
        mode = payload[1]
        total_sectors = struct.unpack_from('<H', payload, 2)[0]
        name = payload[5:5 + payload[4]].decode('ascii', errors='replace')
        log(f"  <- imgopen mode={mode} {name} {total_sectors} sectors")
        status = IMG_ST_OTHER
        try:
            status = IMG_ST_INVALID
            if os.path.splitext(name)[1].upper() not in ('.IMG', '.RLE') or mode not in (0, 1):
                raise ValueError("not an .IMG/.RLE name or bad mode")
            status = IMG_ST_NOT_FOUND
            # Mode 0 names a new file like an upload (bare name -> the listed
            # dir); mode 1 names an existing one like a download (path from root).
            if mode == 0:
                dest = self._resolve_upload_dest(name)
            else:
                dest = Path(*self._resolve_dir_and_name(name))
                # The parent is sandbox-checked; the file itself may be a
                # symlink leading outside the root (same check as uploads).
                resolved = dest.resolve()
                if resolved != self.root and self.root not in resolved.parents:
                    raise PermissionError("path escapes root")
            if mode == 0:
                status = IMG_ST_INVALID
                v9kimage.track_count(total_sectors)
                image = bytearray(total_sectors * v9kimage.SECTOR_SIZE)
            else:
                if not dest.is_file():
                    raise FileNotFoundError(name)
                status = IMG_ST_INVALID
                image = v9kimage.load_image(dest)  # .rle: header/size/CRC checked here
                status = IMG_ST_OTHER
            self.img_job = ImageJob(mode, dest, name, image)
        except Exception as e:
            log(f"  -> imgopen refused ({status}): {e}")
            self._img_info(status)
            return
        sectors = len(image) // v9kimage.SECTOR_SIZE
        self._img_info(IMG_ST_OK, sectors)
        log(f"  -> {'creating' if mode == 0 else 'serving'} {dest.name}: "
            f"{sectors} sectors, {self.img_job.ntracks} tracks")

    def handle_img_get(self, payload):
        job = self.img_job
        track = payload[1] if len(payload) > 1 else -1
        if job is None or job.mode != 1 or not 0 <= track < job.ntracks:
            log(f"  -> imgget {track} refused (no read job or bad track)")
            self._reply_error(Error.PROTOCOL, "bad IMG_GET")
            return
        start, end = job.track_span(track)
        data = job.image[start:end]
        nfrag = math.ceil(len(data) / IMG_FRAG_SIZE)
        for i in range(nfrag):
            frag, packed = compress_if_beneficial(data[i * IMG_FRAG_SIZE:(i + 1) * IMG_FRAG_SIZE])
            self.pkt.send_packet(struct.pack('<BBBBBH', CMD_IMG_TRACK, track, i, nfrag,
                                             IMG_FLAG_RLE if packed else 0, len(frag)) + frag,
                                 timeout=self.ft.DATA_ACK_TIMEOUT)
            if self.frame_gap:
                time.sleep(self.frame_gap)
        job.received[track] = True
        if track % 10 == 9 or track == job.ntracks - 1:
            log(f"  -> imgget track {track + 1}/{job.ntracks}")

    def handle_img_track(self, payload):
        """One fragment of a track the Victor is pushing (mode 0).

        Only the fragment that says it is the last (frag == nfrag-1) gets a
        reply - CMD_OK if the whole track arrived intact, else ERROR - because
        the Victor sends a track's fragments back to back and waits for one
        reply per track. A bad earlier fragment just poisons the track, so the
        error surfaces at that per-track checkpoint instead of as a stray
        packet in the middle of the Victor's send loop."""
        job = self.img_job
        if len(payload) < 7:
            log("  -> imgtrack dropped: short packet")
            self._reply_error(Error.PROTOCOL, "short IMG_TRACK")
            return
        track, frag, nfrag, flags, dlen = struct.unpack_from('<BBBBH', payload, 1)
        data = payload[7:7 + dlen]
        bad = None
        if job is None or job.mode != 0:
            bad = "no image being created"
        elif not 0 <= track < job.ntracks:
            bad = f"track {track} out of range"
        else:
            if frag == 0:
                job.cur_track, job.cur_frags, job.cur_bad = track, [], None
            tbytes = v9kimage.track_bytes(track)
            if job.cur_bad:
                bad = job.cur_bad
            elif (track != job.cur_track or frag != len(job.cur_frags)
                  or nfrag != math.ceil(tbytes / IMG_FRAG_SIZE) or len(data) != dlen):
                bad = f"track {track} fragment {frag}/{nfrag} out of order"
            else:
                try:
                    if flags & IMG_FLAG_RLE:
                        data = rle_decompress(data)
                except ValueError as e:
                    data, bad = b'', f"track {track} fragment {frag}: {e}"
                want = min(IMG_FRAG_SIZE, tbytes - frag * IMG_FRAG_SIZE)
                if not bad and len(data) != want:
                    bad = f"track {track} fragment {frag}: {len(data)} bytes, expected {want}"
            if bad:
                job.cur_bad = bad
                if 0 <= track < job.ntracks:
                    job.received[track] = False
            else:
                job.cur_frags.append(data)
        if frag != nfrag - 1:
            return  # more fragments of this track to come; reply at the last one
        if bad:
            log(f"  -> imgtrack dropped: {bad}")
            if job is not None:
                job.cur_track, job.cur_frags, job.cur_bad = None, [], None
            self._reply_error(Error.PROTOCOL, bad[:60])
            return
        start, end = job.track_span(track)
        job.image[start:end] = b''.join(job.cur_frags)
        job.received[track] = True
        job.cur_track, job.cur_frags = None, []
        self._reply_ok()
        if track % 10 == 9 or track == job.ntracks - 1:
            log(f"  <- imgtrack {track + 1}/{job.ntracks}")

    def handle_img_close(self, payload):
        if len(payload) < 6:
            self._reply_error(Error.PROTOCOL, "short IMG_CLOSE")
            return  # the job stays open for a well-formed CLOSE
        job, self.img_job = self.img_job, None  # the job ends here, whatever happens
        status = payload[1]
        crc = struct.unpack_from('<I', payload, 2)[0]
        log(f"  <- imgclose status={status} crc={crc:08X}")
        if job is None:
            self._reply_error(Error.PROTOCOL, "no image open")
            return
        if status != 0:
            log(f"  -> aborted, {job.name} discarded")
            self._reply_ok()
            return
        image_crc = crc32(bytes(job.image))
        if job.mode == 1:
            if image_crc != crc:
                log(f"  -> CRC MISMATCH: Victor {crc:08X}, image {image_crc:08X}")
                self._reply_error(Error.CRC, "image CRC mismatch")
            else:
                log(f"  -> served {job.dest.name} CRC ok{_rate(len(job.image), time.time() - job.t0)}")
                self._reply_ok()
            return
        missing = job.received.count(False)
        if missing:
            log(f"  -> {missing} tracks missing, nothing written")
            self._reply_error(Error.PROTOCOL, f"{missing} tracks missing")
            return
        if image_crc != crc:
            log(f"  -> CRC MISMATCH: Victor {crc:08X}, received {image_crc:08X}; nothing written")
            self._reply_error(Error.CRC, "image CRC mismatch")
            return
        tmp_path = job.dest.with_name(job.dest.name + '.igctmp')
        try:
            job.dest.parent.mkdir(parents=True, exist_ok=True)
            data = (v9kimage.img_to_rle(bytes(job.image))
                    if job.dest.suffix.lower() == '.rle' else bytes(job.image))
            tmp_path.write_bytes(data)
            os.replace(tmp_path, job.dest)
        except Exception as e:
            if tmp_path.exists():
                tmp_path.unlink()
            log(f"  -> image write failed: {e}")
            self._reply_error(Error.FILE, str(e)[:60])
            return
        self._reply_ok()
        kind = 'rle' if job.dest.suffix.lower() == '.rle' else 'img'
        log(f"  -> wrote {job.dest.name} ({len(job.image)} bytes raw, {len(data)} bytes {kind}, "
            f"{time.time() - job.t0:.1f} s)")

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
    ap.add_argument('--pace-bytes', dest='pace', action='store_true',
                    help="transmit one byte per write()+flush() so a slow polled "
                         "receiver keeps up at higher baud")
    ap.add_argument('--byte-delay', type=float, default=0.0, metavar='SECONDS',
                    help="sleep this long after each paced byte (implies "
                         "--pace-bytes); e.g. 0.001 for 1ms")
    ap.add_argument('--cts-gate', dest='cts_gate', action='store_true',
                    help="packet-granular software flow control: hold each "
                         "frame until the Victor asserts CTS (its RTS). Best "
                         "for higher baud over a USB-serial cable")
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

    root = Path(args.root)
    if not root.is_dir():
        ap.error(f"--root {args.root} is not a directory")

    # The Victor link is 3-wire and the uPD7201 never deasserts RTS (it's a static
    # CR5 output), so hardware flow control would be inert; the port runs without
    # it. Reliable multi-packet transfers at 38400 come from the Victor's lean
    # polled receive path. --cts-gate offers optional software gating.
    conn = SerialConnection(args.dev, baudrate=args.baud,
                            pace=args.pace, byte_delay=args.byte_delay,
                            cts_gate=args.cts_gate, gate_settle=args.gate_settle)
    try:
        conn.connect()
    except Exception as e:
        log(f"failed to open {args.dev}: {e}")
        return 2
    pace_desc = (f" pace=1B/flush"
                 f"{f'+{args.byte_delay*1000:.2f}ms' if args.byte_delay else ''}"
                 if args.pace else "")
    gate_desc = (f" cts-gate+{args.gate_settle*1000:.0f}ms"
                 if args.cts_gate else "")
    log(f"opened {args.dev} @ {args.baud} 8N1{pace_desc}{gate_desc}")

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
