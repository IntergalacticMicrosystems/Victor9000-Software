"""
v9kimage - Victor 9000 floppy image geometry and the .img / .rle file formats.

Shared by igcfs.py (the serial image job) and v9kimg.py (the converter CLI), so
the geometry and the .rle container are defined in exactly one place on the PC.

Geometry: 80 tracks per side, 512-byte sectors, zoned sector counts (MAME
victor9k_dsk.cpp). An .img is the DOS logical-sector order: side 0 tracks
0..79, then side 1 tracks 0..79. Linear track t (0..159) is side 0 track t for
t < 80, else side 1 track t-80; its first logical sector is the sum of the
sector counts of all earlier tracks.

.rle: a 16-byte header, then the whole raw image encoded with the viclibc RLE
scheme (v9kfiletrx.compress) as one continuous stream:

    offset 0   8 bytes  magic b"V9KRLE\\x1a\\x01" (last byte = format version)
    offset 8   u32 LE   raw image size (626688 or 1224192)
    offset 12  u32 LE   CRC-32 of the raw image (zlib / FTX polynomial)

The Victor writes the stream as independently compressed 1 KB slices; that is
still a valid continuous stream, since the decoder is stateless at token
boundaries.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from v9kfiletrx.compress import rle_compress, rle_decompress
from v9kfiletrx.crc32 import crc32

SECTOR_SIZE = 512
TRACKS_PER_SIDE = 80

# (sectors per track, number of tracks) runs, outermost track first.
ZONES_SIDE0 = [(19, 4), (18, 12), (17, 11), (16, 11), (15, 10), (14, 12), (13, 11), (12, 9)]
ZONES_SIDE1 = [(18, 8), (17, 11), (16, 11), (15, 10), (14, 12), (13, 11), (12, 12), (11, 5)]

SECTORS_SS = sum(s * n for s, n in ZONES_SIDE0)                 # 1224
SECTORS_DS = SECTORS_SS + sum(s * n for s, n in ZONES_SIDE1)    # 2391
SIZE_SS = SECTORS_SS * SECTOR_SIZE                              # 626,688
SIZE_DS = SECTORS_DS * SECTOR_SIZE                              # 1,224,192
VALID_SIZES = (SIZE_SS, SIZE_DS)
MAX_TRACK_BYTES = 19 * SECTOR_SIZE                              # 9728

RLE_MAGIC = b"V9KRLE\x1a\x01"
RLE_HEADER_SIZE = 16

assert SECTORS_SS == 1224 and SECTORS_DS == 2391


def _side_track(t):
    if not 0 <= t < 2 * TRACKS_PER_SIDE:
        raise ValueError(f"track {t} out of range")
    if t < TRACKS_PER_SIDE:
        return ZONES_SIDE0, t
    return ZONES_SIDE1, t - TRACKS_PER_SIDE


def track_sectors(t):
    """Sector count of linear track t (0..159)."""
    zones, trk = _side_track(t)
    for sectors, count in zones:
        if trk < count:
            return sectors
        trk -= count
    raise AssertionError("zone table does not cover 80 tracks")


def track_first_sector(t):
    """First logical sector of linear track t: the sectors of all earlier tracks."""
    zones, trk = _side_track(t)
    first = 0 if zones is ZONES_SIDE0 else SECTORS_SS
    for sectors, count in zones:
        n = min(trk, count)
        first += n * sectors
        trk -= n
        if trk == 0:
            break
    return first


def track_bytes(t):
    return track_sectors(t) * SECTOR_SIZE


def track_count(total_sectors):
    """80 tracks for a single-sided image, 160 for a double-sided one."""
    if total_sectors == SECTORS_SS:
        return TRACKS_PER_SIDE
    if total_sectors == SECTORS_DS:
        return 2 * TRACKS_PER_SIDE
    raise ValueError(f"not a Victor disk sector count: {total_sectors}")


def total_sectors_for_size(nbytes):
    if nbytes not in VALID_SIZES:
        raise ValueError(f"not a Victor disk image size: {nbytes} bytes "
                         f"(expected {SIZE_SS} or {SIZE_DS})")
    return nbytes // SECTOR_SIZE


def is_double_sided_label(img):
    """Sidedness from the disk label: bit 0 of the u16 at offset 32 of sector 0."""
    return bool(struct.unpack_from('<H', img, 32)[0] & 1)


def pack_rle_header(size, crc):
    return RLE_MAGIC + struct.pack('<II', size, crc & 0xFFFFFFFF)


def unpack_rle_header(buf):
    """Return (raw_size, crc). Raises ValueError on a bad magic, version or size."""
    if len(buf) < RLE_HEADER_SIZE:
        raise ValueError("file too short for an .rle header")
    if buf[:7] != RLE_MAGIC[:7]:
        raise ValueError("not a Victor .rle image (bad magic)")
    if buf[7] != RLE_MAGIC[7]:
        raise ValueError(f"unsupported .rle format version {buf[7]}")
    size, crc = struct.unpack_from('<II', buf, 8)
    total_sectors_for_size(size)
    return size, crc


def img_to_rle(img):
    total_sectors_for_size(len(img))
    return pack_rle_header(len(img), crc32(img)) + rle_compress(img)


def rle_to_img(rle):
    """Decode an .rle file, checking the decoded length and CRC against its header."""
    size, crc = unpack_rle_header(rle)
    img = rle_decompress(rle[RLE_HEADER_SIZE:])
    if len(img) != size:
        raise ValueError(f".rle decodes to {len(img)} bytes, header says {size}")
    if crc32(img) != crc:
        raise ValueError(".rle CRC-32 mismatch (corrupt file)")
    return img


def _ext(path):
    ext = os.path.splitext(str(path))[1].lower()
    if ext not in ('.img', '.rle'):
        raise ValueError(f"image file must end in .img or .rle: {path}")
    return ext


def load_image(path):
    """Raw image bytes from an .img or .rle file (by extension), validated."""
    ext = _ext(path)
    with open(path, 'rb') as f:
        data = f.read()
    if ext == '.rle':
        return rle_to_img(data)
    total_sectors_for_size(len(data))
    return data


def save_image(path, img):
    """Write raw image bytes as .img or .rle (by extension)."""
    ext = _ext(path)
    total_sectors_for_size(len(img))
    with open(path, 'wb') as f:
        f.write(img_to_rle(img) if ext == '.rle' else img)
