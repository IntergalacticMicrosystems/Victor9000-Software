"""
File Transfer Protocol Constants and Structures.

Matches the C definitions in ftx_protocol.h for interoperability.
"""

from dataclasses import dataclass
from enum import IntEnum
import struct
from typing import Optional


class Command(IntEnum):
    """File transfer command types."""
    START = 0x10
    DATA = 0x11
    END = 0x12
    ABORT = 0x13
    STATUS = 0x14
    LIST = 0x15
    LIST_RESP = 0x16
    ERROR = 0x17
    READY = 0x18
    RESEND = 0x19
    DISKINFO = 0x1C       # Query a drive's sector geometry (PC->Victor)
    DISKINFO_RESP = 0x1D  # Geometry response: bytes/sector + total sectors
    SECTOR = 0x1E         # Start a raw logical-sector transfer (disk imaging)


class Direction(IntEnum):
    """Transfer direction."""
    PC_TO_VICTOR = 0
    VICTOR_TO_PC = 1


class Compression(IntEnum):
    """Compression modes."""
    NONE = 0
    RLE = 1


class Flags(IntEnum):
    """Flags for START packet."""
    OVERWRITE = 0x01
    CREATE_DIR = 0x02
    PARTIAL = 0x04
    FLOWCTRL = 0x08   # Request per-chunk credit flow control (see ReadyPacket.caps)


class Caps(IntEnum):
    """Capability bits reported in the caps byte of the READY answering a START."""
    FLOWCTRL = 0x01   # Receiver will send a per-chunk credit after each write


class Status(IntEnum):
    """Status codes for END packet."""
    OK = 0
    ERROR = 1
    ABORTED = 2


class Error(IntEnum):
    """Error codes."""
    OK = 0
    TIMEOUT = -1
    CRC = -2
    PROTOCOL = -3
    FILE = -4
    DISK_FULL = -5
    ABORT = -6
    COMPRESS = -7
    MEMORY = -8
    NOT_FOUND = -9
    EXISTS = -10
    PACKET = -11


# Size limits (match ftx_protocol.h: FTX_CHUNK_SIZE / FTX_MAX_PAYLOAD). A 1024-
# byte chunk is two full 512-byte floppy sectors, so the Victor's batched writes
# land on sector boundaries. The resulting DATA payload (1024 + 6-byte header =
# 1030) needs the packet layer's PKT_MAX_PAYLOAD >= 1030 (now 2000).
MAX_FILENAME = 64
CHUNK_SIZE = 1024
MAX_PAYLOAD = 1040


@dataclass
class StartPacket:
    """FTX_CMD_START - File metadata packet."""
    direction: int = Direction.PC_TO_VICTOR
    compression: int = Compression.NONE
    flags: int = 0
    file_size: int = 0
    compressed_size: int = 0
    file_date: int = 0
    file_time: int = 0
    file_attr: int = 0
    filename: str = ""
    file_crc: int = 0

    def pack(self) -> bytes:
        """Pack into bytes for transmission.

        Matches C struct ftx_start_t layout:
        - Fixed header (18 bytes)
        - filename[64] (64 bytes, null-terminated, padded)
        - file_crc (4 bytes)
        Total: 86 bytes
        """
        filename_bytes = self.filename.encode('ascii')[:MAX_FILENAME - 1]
        name_len = len(filename_bytes)

        # Pad filename to MAX_FILENAME bytes (64) to match C struct layout
        # The C code casts buffer directly to ftx_start_t, so file_crc
        # must be at offset 82 (18 + 64)
        padded_filename = filename_bytes + b'\x00' * (MAX_FILENAME - name_len)

        return struct.pack(
            '<BBBBIIHHBB',
            Command.START,
            self.direction,
            self.compression,
            self.flags,
            self.file_size,
            self.compressed_size,
            self.file_date,
            self.file_time,
            self.file_attr,
            name_len
        ) + padded_filename + struct.pack('<I', self.file_crc)

    @classmethod
    def unpack(cls, data: bytes) -> 'StartPacket':
        """Unpack from received bytes.

        Matches C struct ftx_start_t layout:
        - cmd at offset 0 (1 byte)
        - direction at offset 1 (1 byte)
        - compression at offset 2 (1 byte)
        - flags at offset 3 (1 byte)
        - file_size at offset 4 (4 bytes)
        - compressed_size at offset 8 (4 bytes)
        - file_date at offset 12 (2 bytes)
        - file_time at offset 14 (2 bytes)
        - file_attr at offset 16 (1 byte)
        - name_len at offset 17 (1 byte)
        - filename at offset 18 (64 bytes)
        - file_crc at offset 82 (4 bytes)
        """
        if data[0] != Command.START:
            raise ValueError("Not a START packet")

        # Unpack fixed header starting after cmd byte (17 bytes)
        # Format: direction, compression, flags, file_size, compressed_size,
        #         file_date, file_time, file_attr, name_len
        header = struct.unpack('<BBBIIHHBB', data[1:18])
        # header indices: 0=direction, 1=compression, 2=flags, 3=file_size,
        #                 4=compressed_size, 5=file_date, 6=file_time,
        #                 7=file_attr, 8=name_len
        name_len = header[8]
        # Filename starts at offset 18
        filename = data[18:18 + name_len].decode('ascii', errors='replace')
        # CRC is at fixed offset 82 (18 + 64)
        file_crc = struct.unpack('<I', data[82:86])[0]

        return cls(
            direction=header[0],
            compression=header[1],
            flags=header[2],
            file_size=header[3],
            compressed_size=header[4],
            file_date=header[5],
            file_time=header[6],
            file_attr=header[7],
            filename=filename,
            file_crc=file_crc
        )


# DATA packet flags
DATA_FLAG_COMPRESSED = 0x01  # This chunk's data is compressed


@dataclass
class DataPacket:
    """FTX_CMD_DATA - File data chunk packet."""
    chunk_num: int = 0
    data: bytes = b''
    flags: int = 0  # Chunk flags: bit 0 = compressed

    def pack(self) -> bytes:
        """Pack into bytes for transmission."""
        return struct.pack(
            '<BBHH',
            Command.DATA,
            self.flags,
            self.chunk_num,
            len(self.data)
        ) + self.data

    @classmethod
    def unpack(cls, data: bytes) -> 'DataPacket':
        """Unpack from received bytes."""
        if data[0] != Command.DATA:
            raise ValueError("Not a DATA packet")

        flags, chunk_num, chunk_size = struct.unpack('<BHH', data[1:6])
        return cls(chunk_num=chunk_num, data=data[6:6 + chunk_size], flags=flags)


@dataclass
class EndPacket:
    """FTX_CMD_END - End of transfer packet."""
    total_chunks: int = 0
    bytes_sent: int = 0
    file_crc: int = 0
    status: int = Status.OK

    def pack(self) -> bytes:
        """Pack into bytes for transmission."""
        return struct.pack(
            '<BHIIB',
            Command.END,
            self.total_chunks,
            self.bytes_sent,
            self.file_crc,
            self.status
        )

    @classmethod
    def unpack(cls, data: bytes) -> 'EndPacket':
        """Unpack from received bytes."""
        if data[0] != Command.END:
            raise ValueError("Not an END packet")

        total_chunks, bytes_sent, file_crc, status = struct.unpack(
            '<HIIB', data[1:12]
        )
        return cls(
            total_chunks=total_chunks,
            bytes_sent=bytes_sent,
            file_crc=file_crc,
            status=status
        )


@dataclass
class ReadyPacket:
    """FTX_CMD_READY - Ready to receive packet.

    Doubles as the per-chunk credit in flow-control mode. The `caps` byte (3rd
    byte) reports negotiated capabilities on the READY that answers a START; a
    pre-caps peer sends only 2 bytes, so unpack treats a missing caps as 0.
    """
    status: int = 0
    caps: int = 0

    def pack(self) -> bytes:
        """Pack into bytes for transmission."""
        return struct.pack('<BBB', Command.READY, self.status, self.caps)

    @classmethod
    def unpack(cls, data: bytes) -> 'ReadyPacket':
        """Unpack from received bytes (tolerates the old 2-byte form)."""
        if data[0] != Command.READY:
            raise ValueError("Not a READY packet")
        caps = data[2] if len(data) > 2 else 0
        return cls(status=data[1], caps=caps)


@dataclass
class ResendPacket:
    """FTX_CMD_RESEND - Request chunk resend."""
    chunk_num: int = 0

    def pack(self) -> bytes:
        """Pack into bytes for transmission."""
        return struct.pack('<BH', Command.RESEND, self.chunk_num)

    @classmethod
    def unpack(cls, data: bytes) -> 'ResendPacket':
        """Unpack from received bytes."""
        if data[0] != Command.RESEND:
            raise ValueError("Not a RESEND packet")
        return cls(chunk_num=struct.unpack('<H', data[1:3])[0])


@dataclass
class ListPacket:
    """FTX_CMD_LIST - Directory listing request."""
    path: str = ""

    def pack(self) -> bytes:
        """Pack into bytes for transmission."""
        path_bytes = self.path.encode('ascii')[:MAX_FILENAME - 1]
        return struct.pack('<BB', Command.LIST, len(path_bytes)) + path_bytes

    @classmethod
    def unpack(cls, data: bytes) -> 'ListPacket':
        """Unpack from received bytes."""
        if data[0] != Command.LIST:
            raise ValueError("Not a LIST packet")
        path_len = data[1]
        path = data[2:2 + path_len].decode('ascii', errors='replace')
        return cls(path=path)


@dataclass
class DirEntry:
    """Directory entry in listing response."""
    name: str = ""
    attr: int = 0
    date: int = 0
    time: int = 0
    size: int = 0

    ENTRY_SIZE = 24  # 13 + 1 + 2 + 2 + 4 + 2 padding

    def pack(self) -> bytes:
        """Pack into bytes."""
        name_bytes = self.name.encode('ascii')[:12].ljust(13, b'\x00')
        return name_bytes + struct.pack('<BHHI', self.attr, self.date,
                                        self.time, self.size)

    @classmethod
    def unpack(cls, data: bytes) -> 'DirEntry':
        """Unpack from bytes."""
        name = data[:13].rstrip(b'\x00').decode('ascii', errors='replace')
        attr, date, time, size = struct.unpack('<BHHI', data[13:24])
        return cls(name=name, attr=attr, date=date, time=time, size=size)


@dataclass
class ListRespPacket:
    """FTX_CMD_LIST_RESP - Directory listing response."""
    entries: list = None
    more_entries: bool = False

    def __post_init__(self):
        if self.entries is None:
            self.entries = []

    def pack(self) -> bytes:
        """Pack into bytes for transmission."""
        data = struct.pack(
            '<BBB',
            Command.LIST_RESP,
            len(self.entries),
            1 if self.more_entries else 0
        )
        for entry in self.entries:
            data += entry.pack()
        return data

    @classmethod
    def unpack(cls, data: bytes) -> 'ListRespPacket':
        """Unpack from received bytes."""
        if data[0] != Command.LIST_RESP:
            raise ValueError("Not a LIST_RESP packet")

        entry_count = data[1]
        more_entries = data[2] != 0
        entries = []

        offset = 3
        for _ in range(entry_count):
            entries.append(DirEntry.unpack(data[offset:offset + DirEntry.ENTRY_SIZE]))
            offset += DirEntry.ENTRY_SIZE

        return cls(entries=entries, more_entries=more_entries)


@dataclass
class ErrorPacket:
    """FTX_CMD_ERROR - Error message packet."""
    error_code: int = 0
    message: str = ""

    def pack(self) -> bytes:
        """Pack into bytes for transmission."""
        msg_bytes = self.message.encode('ascii')[:63]
        return struct.pack('<Bb', Command.ERROR, self.error_code) + msg_bytes + b'\x00'

    @classmethod
    def unpack(cls, data: bytes) -> 'ErrorPacket':
        """Unpack from received bytes."""
        if data[0] != Command.ERROR:
            raise ValueError("Not an ERROR packet")
        error_code = struct.unpack('<b', data[1:2])[0]
        message = data[2:].rstrip(b'\x00').decode('ascii', errors='replace')
        return cls(error_code=error_code, message=message)


@dataclass
class AbortPacket:
    """FTX_CMD_ABORT - Abort transfer packet."""
    reason: int = 0
    message: str = ""

    def pack(self) -> bytes:
        """Pack into bytes for transmission."""
        msg_bytes = self.message.encode('ascii')[:63]
        return struct.pack('<BB', Command.ABORT, self.reason) + msg_bytes + b'\x00'

    @classmethod
    def unpack(cls, data: bytes) -> 'AbortPacket':
        """Unpack from received bytes."""
        if data[0] != Command.ABORT:
            raise ValueError("Not an ABORT packet")
        reason = data[1]
        message = data[2:].rstrip(b'\x00').decode('ascii', errors='replace')
        return cls(reason=reason, message=message)


@dataclass
class DiskInfoPacket:
    """FTX_CMD_DISKINFO - Ask the Victor for a drive's sector geometry."""
    drive: int = 0   # 0=A, 1=B, ...

    def pack(self) -> bytes:
        """Pack into bytes for transmission (matches ftx_diskinfo_t)."""
        return struct.pack('<BB', Command.DISKINFO, self.drive)

    @classmethod
    def unpack(cls, data: bytes) -> 'DiskInfoPacket':
        if data[0] != Command.DISKINFO:
            raise ValueError("Not a DISKINFO packet")
        return cls(drive=data[1])


@dataclass
class DiskInfoRespPacket:
    """FTX_CMD_DISKINFO_RESP - Response to DISKINFO (matches ftx_diskinfo_resp_t)."""
    status: int = 0            # 0 = ok, non-zero = error (e.g. invalid drive)
    bytes_per_sector: int = 0
    total_sectors: int = 0

    def pack(self) -> bytes:
        """Pack into bytes for transmission."""
        return struct.pack('<BBHI', Command.DISKINFO_RESP, self.status,
                           self.bytes_per_sector, self.total_sectors)

    @classmethod
    def unpack(cls, data: bytes) -> 'DiskInfoRespPacket':
        if data[0] != Command.DISKINFO_RESP:
            raise ValueError("Not a DISKINFO_RESP packet")
        status, bps, total = struct.unpack('<BHI', data[1:8])
        return cls(status=status, bytes_per_sector=bps, total_sectors=total)


@dataclass
class SectorPacket:
    """FTX_CMD_SECTOR - Start a raw logical-sector transfer (matches ftx_sector_t).

    The sector analog of StartPacket: names a sector range + direction, then the
    bulk payload moves over the same DATA/END/READY machinery. data_crc rides in
    END (computed incrementally), so it is 0 in the request/metadata packet.
    """
    direction: int = Direction.VICTOR_TO_PC
    compression: int = Compression.NONE
    flags: int = 0
    drive: int = 0          # 0=A, 1=B, ...
    start_sector: int = 0
    sector_count: int = 0
    data_crc: int = 0

    def pack(self) -> bytes:
        """Pack into bytes for transmission (18 bytes, 1-byte packed)."""
        return struct.pack(
            '<BBBBBBIII',
            Command.SECTOR,
            self.direction,
            self.compression,
            self.flags,
            self.drive,
            0,                  # reserved
            self.start_sector,
            self.sector_count,
            self.data_crc
        )

    @classmethod
    def unpack(cls, data: bytes) -> 'SectorPacket':
        if data[0] != Command.SECTOR:
            raise ValueError("Not a SECTOR packet")
        (direction, compression, flags, drive, _reserved,
         start_sector, sector_count, data_crc) = struct.unpack('<BBBBBIII', data[1:18])
        return cls(direction=direction, compression=compression, flags=flags,
                   drive=drive, start_sector=start_sector,
                   sector_count=sector_count, data_crc=data_crc)


def parse_packet(data: bytes):
    """Parse a packet based on its command type."""
    if not data:
        raise ValueError("Empty packet")

    cmd = data[0]

    parsers = {
        Command.START: StartPacket.unpack,
        Command.DATA: DataPacket.unpack,
        Command.END: EndPacket.unpack,
        Command.READY: ReadyPacket.unpack,
        Command.RESEND: ResendPacket.unpack,
        Command.LIST: ListPacket.unpack,
        Command.LIST_RESP: ListRespPacket.unpack,
        Command.ERROR: ErrorPacket.unpack,
        Command.ABORT: AbortPacket.unpack,
        Command.DISKINFO: DiskInfoPacket.unpack,
        Command.DISKINFO_RESP: DiskInfoRespPacket.unpack,
        Command.SECTOR: SectorPacket.unpack,
    }

    parser = parsers.get(cmd)
    if parser is None:
        raise ValueError(f"Unknown command: 0x{cmd:02X}")

    return parser(data)
