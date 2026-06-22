"""
File Transfer Protocol for Victor 9000.

Provides bidirectional file transfer between PC and Victor 9000
over serial connection using packet protocol.
"""

import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional, Union

from .protocol import (
    Command, Compression, Direction, Error, Status, Flags, Caps,
    StartPacket, DataPacket, EndPacket, ReadyPacket, ResendPacket,
    ListPacket, ListRespPacket, DirEntry, ErrorPacket, AbortPacket,
    DiskInfoPacket, DiskInfoRespPacket, SectorPacket,
    CHUNK_SIZE, MAX_PAYLOAD, DATA_FLAG_COMPRESSED, parse_packet
)
from .crc32 import crc32_file, crc32, crc32_update
from .compress import rle_compress, rle_decompress, compress_if_beneficial


def _dos_remote_name(name: str) -> str:
    """Uppercase a Victor file name and clamp only its 8.3 *filename* component
    to 12 chars (8 + '.' + 3), preserving any drive/directory prefix. A bare
    [:12] on the whole string corrupts qualified paths, e.g. 'B:\\COMMAND.COM'
    -> 'B:\\COMMAND.C' (then 'file not found'); the wire field holds 63 chars."""
    name = name.upper()
    idx = max(name.rfind('\\'), name.rfind('/'), name.rfind(':'))
    return name[:idx + 1] + name[idx + 1:][:12]


class TransferError(Exception):
    """File transfer error."""
    def __init__(self, message: str, error_code: int = Error.PROTOCOL):
        super().__init__(message)
        self.error_code = error_code


@dataclass
class TransferStats:
    """Transfer statistics."""
    bytes_transferred: int = 0
    total_bytes: int = 0
    chunks_sent: int = 0
    total_chunks: int = 0
    errors: int = 0
    retries: int = 0
    start_time: float = 0.0
    elapsed_seconds: float = 0.0
    compressed_bytes: int = 0

    @property
    def percent_complete(self) -> float:
        """Percentage complete (0-100)."""
        if self.total_bytes == 0:
            return 0.0
        return (self.bytes_transferred / self.total_bytes) * 100

    @property
    def bytes_per_second(self) -> float:
        """Transfer speed in bytes per second."""
        if self.elapsed_seconds == 0:
            return 0.0
        return self.bytes_transferred / self.elapsed_seconds

    @property
    def compression_ratio(self) -> float:
        """Compression ratio (compressed/original)."""
        if self.bytes_transferred == 0:
            return 1.0
        if self.compressed_bytes == 0:
            return 1.0
        return self.compressed_bytes / self.bytes_transferred


# Progress callback type: (stats) -> bool, return False to abort
ProgressCallback = Callable[[TransferStats], bool]


class FileTransfer:
    """
    File transfer protocol handler.

    Example:
        from v9kserial import MamePtyConnection, PacketProtocol

        conn = MamePtyConnection(baudrate=9600)
        conn.connect()
        pkt = PacketProtocol(conn)

        ftx = FileTransfer(pkt)

        # Send file to Victor
        stats = ftx.send_file("local_file.txt", "REMOTE.TXT",
                              compression=Compression.RLE)
        print(f"Sent {stats.bytes_transferred} bytes")

        # Receive file from Victor
        stats = ftx.receive_file("REMOTE.TXT", "local_copy.txt")
    """

    DEFAULT_TIMEOUT = 5.0
    MAX_RETRIES = 5

    # Per-chunk ACK timeout. Shorter than the overall timeout so a dropped
    # DATA chunk is retransmitted promptly (the receiver waits through it);
    # START/READY/END keep the longer default to tolerate slow file opens.
    DATA_ACK_TIMEOUT = 4.0

    # How long to wait for a per-chunk credit in flow-control mode. Must exceed
    # the receiver's worst-case between-chunk work (a floppy write can take
    # several seconds), since the credit is sent only after that write commits.
    CREDIT_TIMEOUT = 30.0

    def __init__(self, protocol, timeout: float = DEFAULT_TIMEOUT):
        """
        Initialize file transfer handler.

        Args:
            protocol: PacketProtocol instance from v9kserial
            timeout: Default timeout for operations (seconds)
        """
        self.protocol = protocol
        self.timeout = timeout
        self._stats = TransferStats()
        # Settle gap between back-to-back disk requests on one connection. After
        # a request the Victor briefly isn't back in its receive loop (and if its
        # final ACK was lost, the 8088 spends up to ~2 s on stop-and-wait ARQ
        # retransmit); a follow-up request sent into that window loses bytes and
        # NAKs. Space disk requests and RESET-resync sequence bits before each.
        self.disk_settle = 3.0
        self._last_disk_request = 0.0

    def send_file(
        self,
        local_path: Union[str, Path],
        remote_name: Optional[str] = None,
        compression: Compression = Compression.NONE,
        overwrite: bool = True,
        progress: Optional[ProgressCallback] = None
    ) -> TransferStats:
        """
        Send a file to Victor 9000.

        Args:
            local_path: Path to local file
            remote_name: Filename on Victor (8.3 format), defaults to basename
            compression: Compression mode
            overwrite: Overwrite if exists on Victor
            progress: Progress callback, return False to abort

        Returns:
            Transfer statistics

        Raises:
            TransferError: On failure
            FileNotFoundError: If local file doesn't exist
        """
        local_path = Path(local_path)

        if not local_path.exists():
            raise FileNotFoundError(f"File not found: {local_path}")

        if remote_name is None:
            remote_name = local_path.name

        # Ensure 8.3 format (uppercase); clamp only the filename, not the path.
        remote_name = _dos_remote_name(remote_name)

        # Initialize stats
        file_size = local_path.stat().st_size
        file_crc = crc32_file(str(local_path))
        total_chunks = (file_size + CHUNK_SIZE - 1) // CHUNK_SIZE

        self._stats = TransferStats(
            total_bytes=file_size,
            total_chunks=total_chunks,
            start_time=time.time()
        )

        # Get file modification time as DOS date/time
        mtime = local_path.stat().st_mtime
        file_date, file_time = self._unix_to_dos_datetime(mtime)

        # Build and send START packet. Always request per-chunk credit flow
        # control; whether it actually engages depends on the receiver echoing
        # FTX_CAP_FLOWCTRL in its READY (an old receiver ignores the flag, sends
        # no credits, and we fall back to fire-and-forget below).
        start = StartPacket(
            direction=Direction.PC_TO_VICTOR,
            compression=compression,
            flags=(Flags.OVERWRITE if overwrite else 0) | Flags.FLOWCTRL,
            file_size=file_size,
            compressed_size=0,
            file_date=file_date,
            file_time=file_time,
            file_attr=0x20,  # Archive
            filename=remote_name,
            file_crc=file_crc
        )

        self._send_packet(start.pack())

        # Wait for READY response
        resp_data = self._recv_packet()
        resp = parse_packet(resp_data)

        if isinstance(resp, ErrorPacket):
            raise TransferError(resp.message or "Transfer rejected",
                               resp.error_code)
        if isinstance(resp, AbortPacket):
            raise TransferError(resp.message or "Transfer aborted",
                               Error.ABORT)
        if not isinstance(resp, ReadyPacket):
            raise TransferError("Unexpected response to START")
        if resp.status != 0:
            raise TransferError(f"Receiver not ready: {resp.status}")

        # Did the receiver agree to credit flow control? Only then do we wait for
        # a credit after each chunk; otherwise the old fire-and-forget path runs.
        flow_control = bool(resp.caps & Caps.FLOWCTRL)
        self.flow_control = flow_control   # expose negotiated state for callers/tests

        # Send data chunks
        with open(local_path, 'rb') as f:
            for chunk_num in range(total_chunks):
                chunk_data = f.read(CHUNK_SIZE)

                # Compress if requested - set per-chunk flag
                chunk_flags = 0
                if compression == Compression.RLE:
                    compressed, was_compressed = compress_if_beneficial(chunk_data)
                    if was_compressed:
                        send_data = compressed
                        chunk_flags = DATA_FLAG_COMPRESSED
                        self._stats.compressed_bytes += len(compressed)
                    else:
                        send_data = chunk_data
                else:
                    send_data = chunk_data

                # Send with retries. In flow-control mode the chunk isn't done
                # until the receiver's credit arrives, so the credit wait is part
                # of the same attempt: a missing credit retries the whole chunk.
                for retry in range(self.MAX_RETRIES):
                    try:
                        data_pkt = DataPacket(chunk_num=chunk_num, data=send_data, flags=chunk_flags)
                        self._send_packet(data_pkt.pack(), timeout=self.DATA_ACK_TIMEOUT)
                        if flow_control:
                            self._wait_credit()
                        break
                    except Exception:
                        self._stats.retries += 1
                        if retry == self.MAX_RETRIES - 1:
                            raise TransferError("Max retries exceeded",
                                              Error.TIMEOUT)

                # Update stats
                self._stats.bytes_transferred += len(chunk_data)
                self._stats.chunks_sent += 1
                self._stats.elapsed_seconds = time.time() - self._stats.start_time

                # Progress callback
                if progress is not None:
                    if not progress(self._stats):
                        self._send_abort("User cancelled")
                        raise TransferError("Transfer cancelled by user",
                                          Error.ABORT)

        # Send END packet
        end = EndPacket(
            total_chunks=total_chunks,
            bytes_sent=file_size,
            file_crc=file_crc,
            status=Status.OK
        )
        self._send_packet(end.pack())

        # Wait for final ACK
        try:
            self._recv_packet()
        except Exception:
            pass  # Ignore timeout on final ACK

        self._stats.elapsed_seconds = time.time() - self._stats.start_time
        return self._stats

    def receive_file(
        self,
        remote_name: str,
        local_path: Union[str, Path],
        progress: Optional[ProgressCallback] = None
    ) -> TransferStats:
        """
        Receive a file from Victor 9000.

        This waits for Victor to initiate the transfer.

        Args:
            remote_name: Expected filename from Victor
            local_path: Path to save locally
            progress: Progress callback, return False to abort

        Returns:
            Transfer statistics

        Raises:
            TransferError: On failure
        """
        local_path = Path(local_path)

        # Wait for START packet
        start_data = self._recv_packet()
        start = parse_packet(start_data)

        if not isinstance(start, StartPacket):
            raise TransferError("Expected START packet")

        # Initialize stats
        self._stats = TransferStats(
            total_bytes=start.file_size,
            total_chunks=(start.file_size + CHUNK_SIZE - 1) // CHUNK_SIZE,
            start_time=time.time()
        )

        # Send READY
        ready = ReadyPacket(status=0)
        self._send_packet(ready.pack())

        # Create output file
        local_path.parent.mkdir(parents=True, exist_ok=True)

        try:
            with open(local_path, 'wb') as f:
                crc_accum = 0
                expected_chunk = 0
                # The authoritative file CRC arrives in the END packet (the sender
                # computes it incrementally as it transmits, rather than pre-reading
                # the whole file before START). Fall back to START's value for an
                # older sender that still fills it in.
                end_crc = start.file_crc

                while True:
                    pkt_data = self._recv_packet()
                    pkt = parse_packet(pkt_data)

                    if isinstance(pkt, EndPacket):
                        # Transfer complete
                        if pkt.status != Status.OK:
                            raise TransferError("Transfer failed on sender side")
                        end_crc = pkt.file_crc
                        break

                    if isinstance(pkt, AbortPacket):
                        raise TransferError(pkt.message or "Transfer aborted",
                                          Error.ABORT)

                    if not isinstance(pkt, DataPacket):
                        raise TransferError("Expected DATA packet")

                    if pkt.chunk_num != expected_chunk:
                        # Out of sequence - request resend
                        resend = ResendPacket(chunk_num=expected_chunk)
                        self._send_packet(resend.pack())
                        self._stats.retries += 1
                        continue

                    # Decompress if needed
                    if start.compression == Compression.RLE:
                        try:
                            chunk_data = rle_decompress(pkt.data)
                        except ValueError as e:
                            raise TransferError(f"Decompression error: {e}",
                                              Error.COMPRESS)
                    else:
                        chunk_data = pkt.data

                    # Write to file
                    f.write(chunk_data)
                    crc_accum = crc32(chunk_data)  # TODO: accumulate properly

                    # Update stats
                    self._stats.bytes_transferred += len(chunk_data)
                    self._stats.chunks_sent += 1
                    self._stats.elapsed_seconds = time.time() - self._stats.start_time
                    expected_chunk += 1

                    # Progress callback
                    if progress is not None:
                        if not progress(self._stats):
                            self._send_abort("User cancelled")
                            raise TransferError("Transfer cancelled by user",
                                              Error.ABORT)

        except Exception:
            # Clean up on error
            if local_path.exists():
                local_path.unlink()
            raise

        # Verify CRC against the value carried in END (see above).
        actual_crc = crc32_file(str(local_path))
        if actual_crc != end_crc:
            local_path.unlink()
            raise TransferError("CRC mismatch", Error.CRC)

        # Set file modification time
        mtime = self._dos_to_unix_datetime(start.file_date, start.file_time)
        os.utime(local_path, (mtime, mtime))

        self._stats.elapsed_seconds = time.time() - self._stats.start_time
        return self._stats

    def list_directory(self, path: str = "") -> list[dict]:
        """
        Get directory listing from Victor.

        Args:
            path: Directory path on Victor

        Returns:
            List of file entries with name, size, date, attr
        """
        # Send LIST request
        list_pkt = ListPacket(path=path)
        self._send_packet(list_pkt.pack())

        entries = []

        while True:
            resp_data = self._recv_packet()
            resp = parse_packet(resp_data)

            if isinstance(resp, ErrorPacket):
                raise TransferError(resp.message or "List failed",
                                   resp.error_code)

            if not isinstance(resp, ListRespPacket):
                raise TransferError("Expected LIST_RESP packet")

            for entry in resp.entries:
                entries.append({
                    'name': entry.name,
                    'size': entry.size,
                    'date': entry.date,
                    'time': entry.time,
                    'attr': entry.attr,
                    'is_dir': bool(entry.attr & 0x10)
                })

            if not resp.more_entries:
                break

        return entries

    # ------------------------------------------------------------------
    # Raw disk-sector transfer (whole-disk imaging)
    # ------------------------------------------------------------------

    def _disk_request_prep(self) -> None:
        """Space and resync before a back-to-back disk request: wait out the
        settle gap, then RESET to realign sequence bits with the server."""
        wait = self.disk_settle - (time.monotonic() - self._last_disk_request)
        if 0 < wait <= self.disk_settle:
            time.sleep(wait)
        try:
            self.protocol.reset()
        except Exception:
            pass

    def disk_info(self, drive: int = 0) -> tuple[int, int]:
        """Query a Victor drive's sector geometry.

        Args:
            drive: Drive number (0=A, 1=B, ...)

        Returns:
            (bytes_per_sector, total_sectors)

        Raises:
            TransferError: on an invalid drive or unexpected response
        """
        self._disk_request_prep()
        try:
            self._send_packet(DiskInfoPacket(drive=drive).pack())
            resp = parse_packet(self._recv_packet())
        finally:
            self._last_disk_request = time.monotonic()
        if isinstance(resp, ErrorPacket):
            raise TransferError(resp.message or "diskinfo failed", resp.error_code)
        if not isinstance(resp, DiskInfoRespPacket):
            raise TransferError("Expected DISKINFO_RESP")
        if resp.status != 0:
            raise TransferError(f"Invalid drive {drive}", Error.FILE)
        return resp.bytes_per_sector, resp.total_sectors

    def read_disk(
        self,
        drive: int,
        out_path: Union[str, Path],
        start_sector: int = 0,
        total_sectors: Optional[int] = None,
        segment_sectors: int = 0,
        compression: Compression = Compression.NONE,
        progress: Optional[ProgressCallback] = None
    ) -> TransferStats:
        """Image logical sectors from a Victor drive into a local file.

        Args:
            drive: Drive number (0=A, 1=B, ...)
            out_path: Local image file to create
            start_sector: First sector to read (default 0)
            total_sectors: How many sectors (default: to end of device)
            segment_sectors: Split the read into segments of this many sectors
                (0 = one transfer for the whole range). Smaller segments give
                coarser-grained restartability on a flaky link.
            compression: Per-chunk RLE (crushes blank/zeroed sectors)
            progress: Progress callback, return False to abort

        Returns:
            Transfer statistics
        """
        bps, dev_total = self.disk_info(drive)
        if start_sector > dev_total:
            start_sector = dev_total
        if total_sectors is None:
            total_sectors = dev_total - start_sector
        if total_sectors > dev_total - start_sector:
            total_sectors = dev_total - start_sector
        if total_sectors == 0:
            raise TransferError("Empty sector range", Error.FILE)

        out_path = Path(out_path)
        out_path.parent.mkdir(parents=True, exist_ok=True)

        self._stats = TransferStats(
            total_bytes=total_sectors * bps,
            total_chunks=(total_sectors * bps + CHUNK_SIZE - 1) // CHUNK_SIZE,
            start_time=time.time()
        )

        seg = segment_sectors if segment_sectors > 0 else total_sectors
        # Pre-size the image so segments can seek to their absolute offsets.
        with open(out_path, 'wb') as f:
            f.truncate(total_sectors * bps)

        with open(out_path, 'r+b') as f:
            s = start_sector
            end = start_sector + total_sectors
            while s < end:
                n = min(seg, end - s)
                self._recv_sector_segment(drive, s, n, bps, f,
                                          compression, progress)
                s += n

        self._stats.elapsed_seconds = time.time() - self._stats.start_time
        return self._stats

    def write_disk(
        self,
        drive: int,
        image_path: Union[str, Path],
        start_sector: int = 0,
        total_sectors: Optional[int] = None,
        segment_sectors: int = 0,
        compression: Compression = Compression.NONE,
        progress: Optional[ProgressCallback] = None
    ) -> TransferStats:
        """Write a local image's logical sectors onto a Victor drive. DESTRUCTIVE.

        The Victor server must have sector writes enabled (see FTXSERV /allowwrite).

        Args:
            drive: Drive number (0=A, 1=B, ...)
            image_path: Local image file to write from
            start_sector: First sector to write (default 0)
            total_sectors: How many sectors (default: derived from image size)
            segment_sectors: Split into segments of this many sectors (0 = one)
            compression: Per-chunk RLE
            progress: Progress callback, return False to abort

        Returns:
            Transfer statistics
        """
        bps, dev_total = self.disk_info(drive)
        image_path = Path(image_path)
        img_size = image_path.stat().st_size
        if img_size % bps != 0:
            raise TransferError(
                f"Image size {img_size} is not a multiple of the "
                f"{bps}-byte sector size", Error.PROTOCOL)

        avail = img_size // bps - start_sector
        if total_sectors is None:
            total_sectors = avail
        if total_sectors > avail:
            raise TransferError("Requested range exceeds the image", Error.PROTOCOL)
        if start_sector + total_sectors > dev_total:
            raise TransferError("Requested range exceeds the device", Error.PROTOCOL)
        if total_sectors == 0:
            raise TransferError("Empty sector range", Error.FILE)

        self._stats = TransferStats(
            total_bytes=total_sectors * bps,
            total_chunks=(total_sectors * bps + CHUNK_SIZE - 1) // CHUNK_SIZE,
            start_time=time.time()
        )

        seg = segment_sectors if segment_sectors > 0 else total_sectors
        with open(image_path, 'rb') as f:
            s = start_sector
            end = start_sector + total_sectors
            while s < end:
                n = min(seg, end - s)
                self._send_sector_segment(drive, s, n, bps, f,
                                          compression, progress)
                s += n

        self._stats.elapsed_seconds = time.time() - self._stats.start_time
        return self._stats

    def _recv_sector_segment(self, drive, start_sector, count, bps, out_file,
                             compression, progress):
        """Receive one sector range from the Victor and write it at its offset.

        Mirrors the getfile handshake: send the SECTOR request, receive the
        Victor's SECTOR metadata, send READY, then stream DATA until END. The
        segment CRC carried in END is verified against the bytes received.
        """
        self._disk_request_prep()
        req = SectorPacket(direction=Direction.VICTOR_TO_PC,
                           compression=compression, flags=0, drive=drive,
                           start_sector=start_sector, sector_count=count)
        self._send_packet(req.pack())

        resp = parse_packet(self._recv_packet())
        if isinstance(resp, ErrorPacket):
            raise TransferError(resp.message or "read rejected", resp.error_code)
        if isinstance(resp, AbortPacket):
            raise TransferError(resp.message or "read aborted", Error.ABORT)
        if not isinstance(resp, SectorPacket):
            raise TransferError("Expected SECTOR metadata")

        self._send_packet(ReadyPacket(status=0).pack())

        out_file.seek(start_sector * bps)
        expected_chunk = 0
        seg_crc = 0
        end_crc = 0
        while True:
            pkt = parse_packet(self._recv_packet())

            if isinstance(pkt, EndPacket):
                if pkt.status != Status.OK:
                    raise TransferError("Transfer failed on sender side")
                end_crc = pkt.file_crc
                break
            if isinstance(pkt, AbortPacket):
                raise TransferError(pkt.message or "Transfer aborted", Error.ABORT)
            if not isinstance(pkt, DataPacket):
                raise TransferError("Expected DATA packet")

            if pkt.chunk_num != expected_chunk:
                self._send_packet(ResendPacket(chunk_num=expected_chunk).pack())
                self._stats.retries += 1
                continue

            # Decompress per the chunk's own flag (the sender only sets it when
            # compression actually saved space, so a mixed stream is possible).
            if pkt.flags & DATA_FLAG_COMPRESSED:
                try:
                    chunk_data = rle_decompress(pkt.data)
                except ValueError as e:
                    raise TransferError(f"Decompression error: {e}", Error.COMPRESS)
            else:
                chunk_data = pkt.data

            out_file.write(chunk_data)
            seg_crc = crc32_update(seg_crc, chunk_data)
            self._stats.bytes_transferred += len(chunk_data)
            self._stats.chunks_sent += 1
            self._stats.elapsed_seconds = time.time() - self._stats.start_time
            expected_chunk += 1

            if progress is not None and not progress(self._stats):
                self._send_abort("User cancelled")
                raise TransferError("Transfer cancelled by user", Error.ABORT)

        self._last_disk_request = time.monotonic()
        if seg_crc != end_crc:
            raise TransferError("CRC mismatch on sector segment", Error.CRC)

    def _send_sector_segment(self, drive, start_sector, count, bps, in_file,
                             compression, progress):
        """Send one sector range to the Victor from the image file.

        Mirrors send_file: send the SECTOR request, wait for READY, stream DATA
        (requesting per-chunk credit flow control so the Victor's slow sector
        writes don't overrun the link), then END.
        """
        seg_bytes = count * bps
        total_chunks = (seg_bytes + CHUNK_SIZE - 1) // CHUNK_SIZE

        self._disk_request_prep()
        req = SectorPacket(direction=Direction.PC_TO_VICTOR,
                           compression=compression, flags=Flags.FLOWCTRL,
                           drive=drive, start_sector=start_sector,
                           sector_count=count)
        self._send_packet(req.pack())

        resp = parse_packet(self._recv_packet())
        if isinstance(resp, ErrorPacket):
            raise TransferError(resp.message or "write rejected", resp.error_code)
        if isinstance(resp, AbortPacket):
            raise TransferError(resp.message or "write aborted", Error.ABORT)
        if not isinstance(resp, ReadyPacket):
            raise TransferError("Unexpected response to SECTOR")
        if resp.status != 0:
            raise TransferError(f"Receiver not ready: {resp.status}")

        flow_control = bool(resp.caps & Caps.FLOWCTRL)
        self.flow_control = flow_control

        in_file.seek(start_sector * bps)
        seg_crc = 0
        for chunk_num in range(total_chunks):
            chunk_data = in_file.read(min(CHUNK_SIZE, seg_bytes - chunk_num * CHUNK_SIZE))
            if not chunk_data:
                raise TransferError("Image ended before the sector range did",
                                   Error.FILE)
            seg_crc = crc32_update(seg_crc, chunk_data)

            chunk_flags = 0
            send_data = chunk_data
            if compression == Compression.RLE:
                compressed, was_compressed = compress_if_beneficial(chunk_data)
                if was_compressed:
                    send_data = compressed
                    chunk_flags = DATA_FLAG_COMPRESSED

            for retry in range(self.MAX_RETRIES):
                try:
                    data_pkt = DataPacket(chunk_num=chunk_num, data=send_data,
                                         flags=chunk_flags)
                    self._send_packet(data_pkt.pack(), timeout=self.DATA_ACK_TIMEOUT)
                    if flow_control:
                        self._wait_credit()
                    break
                except Exception:
                    self._stats.retries += 1
                    if retry == self.MAX_RETRIES - 1:
                        raise TransferError("Max retries exceeded", Error.TIMEOUT)

            self._stats.bytes_transferred += len(chunk_data)
            self._stats.chunks_sent += 1
            self._stats.elapsed_seconds = time.time() - self._stats.start_time

            if progress is not None and not progress(self._stats):
                self._send_abort("User cancelled")
                raise TransferError("Transfer cancelled by user", Error.ABORT)

        end = EndPacket(total_chunks=total_chunks, bytes_sent=seg_bytes,
                       file_crc=seg_crc, status=Status.OK)
        self._send_packet(end.pack())
        # The packet layer already ACKed END; no app-level reply follows.
        self._last_disk_request = time.monotonic()

    @property
    def stats(self) -> TransferStats:
        """Get current transfer statistics."""
        return self._stats

    def _send_packet(self, data: bytes, timeout: Optional[float] = None) -> None:
        """Send packet via protocol layer."""
        self.protocol.send_packet(data, timeout)

    def _recv_packet(self) -> bytes:
        """Receive packet via protocol layer."""
        return self.protocol.receive_packet(timeout=self.timeout)

    def _wait_credit(self) -> None:
        """Wait for the receiver's per-chunk READY credit (flow-control mode).

        The credit is a reliable packet (the packet layer ACKs/retransmits it),
        so this only fails on a dead link or a receiver-side abort. The timeout
        is generous because the credit follows the receiver's disk write, which
        may take seconds. Raises on anything that isn't a clean READY.
        """
        data = self.protocol.receive_packet(timeout=self.CREDIT_TIMEOUT)
        pkt = parse_packet(data)
        if isinstance(pkt, AbortPacket):
            raise TransferError(pkt.message or "Transfer aborted", Error.ABORT)
        if isinstance(pkt, ErrorPacket):
            raise TransferError(pkt.message or "Receiver error", pkt.error_code)
        if not isinstance(pkt, ReadyPacket):
            raise TransferError("Expected per-chunk READY credit")
        if pkt.status != 0:
            raise TransferError(f"Receiver not ready: {pkt.status}")

    def _send_abort(self, message: str = "") -> None:
        """Send abort packet."""
        abort = AbortPacket(reason=Error.ABORT, message=message)
        try:
            self._send_packet(abort.pack())
        except Exception:
            pass

    @staticmethod
    def _unix_to_dos_datetime(unix_time: float) -> tuple[int, int]:
        """Convert Unix timestamp to DOS date/time."""
        t = time.localtime(unix_time)

        # DOS date: bits 0-4=day, 5-8=month, 9-15=year-1980
        date = (t.tm_mday & 0x1F) | ((t.tm_mon & 0x0F) << 5) | \
               (((t.tm_year - 1980) & 0x7F) << 9)

        # DOS time: bits 0-4=sec/2, 5-10=min, 11-15=hour
        time_val = ((t.tm_sec // 2) & 0x1F) | ((t.tm_min & 0x3F) << 5) | \
                   ((t.tm_hour & 0x1F) << 11)

        return date, time_val

    @staticmethod
    def _dos_to_unix_datetime(dos_date: int, dos_time: int) -> float:
        """Convert DOS date/time to Unix timestamp."""
        day = dos_date & 0x1F
        month = (dos_date >> 5) & 0x0F
        year = ((dos_date >> 9) & 0x7F) + 1980

        sec = ((dos_time & 0x1F) * 2)
        minute = (dos_time >> 5) & 0x3F
        hour = (dos_time >> 11) & 0x1F

        try:
            t = time.mktime((year, month, day, hour, minute, sec, 0, 0, -1))
        except (ValueError, OverflowError):
            t = 0

        return t
