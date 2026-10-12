#!/usr/bin/env python3
"""
v9kimg - inspect and convert Victor 9000 floppy images (.img <-> .rle).

    v9kimg.py info   DISK.img|DISK.rle
    v9kimg.py pack   DISK.img [DISK.rle]
    v9kimg.py unpack DISK.rle [DISK.img]

.img is the raw logical-sector image (626,688 or 1,224,192 bytes), as MAME and
igc use it; .rle is the compressed container igc can also read and write (see
v9kimage.py for the format).
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import v9kimage
from v9kfiletrx.crc32 import crc32


def cmd_info(args):
    img = v9kimage.load_image(args.image)
    sectors = v9kimage.total_sectors_for_size(len(img))
    ds_size = sectors == v9kimage.SECTORS_DS
    ds_label = v9kimage.is_double_sided_label(img)
    sides = lambda ds: "double-sided" if ds else "single-sided"
    print(f"file:        {args.image}")
    print(f"sides:       {sides(ds_size)} (by size), {sides(ds_label)} (by label)"
          + ("  ** MISMATCH **" if ds_size != ds_label else ""))
    print(f"sectors:     {sectors} ({v9kimage.track_count(sectors)} tracks)")
    print(f"raw size:    {len(img)} bytes")
    print(f"CRC-32:      {crc32(img):08X}")
    if args.image.lower().endswith('.rle'):
        n = os.path.getsize(args.image)
        print(f"rle size:    {n} bytes ({100.0 * n / len(img):.1f}% of raw)")
    print("label (sector 0, bytes 0..63):")
    for off in range(0, 64, 16):
        print(f"  {off:04X}  " + ' '.join(f"{b:02X}" for b in img[off:off + 16]))


def _convert(args, src_ext, dst_ext):
    if not args.src.lower().endswith(src_ext):
        raise ValueError(f"input must be a {src_ext} file: {args.src}")
    dst = args.dst or os.path.splitext(args.src)[0] + dst_ext
    if not dst.lower().endswith(dst_ext):
        raise ValueError(f"output must be a {dst_ext} file: {dst}")
    img = v9kimage.load_image(args.src)
    v9kimage.save_image(dst, img)
    print(f"{args.src} -> {dst} ({len(img)} bytes raw, {os.path.getsize(dst)} bytes written)")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Victor 9000 floppy image tool (.img <-> .rle)")
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('info', help="show sides, size, CRC and label of an image")
    p.add_argument('image')
    p = sub.add_parser('pack', help="compress DISK.img to DISK.rle")
    p.add_argument('src')
    p.add_argument('dst', nargs='?')
    p = sub.add_parser('unpack', help="expand DISK.rle to DISK.img")
    p.add_argument('src')
    p.add_argument('dst', nargs='?')
    args = ap.parse_args(argv)
    try:
        if args.cmd == 'info':
            cmd_info(args)
        elif args.cmd == 'pack':
            _convert(args, '.img', '.rle')
        else:
            _convert(args, '.rle', '.img')
    except (OSError, ValueError) as e:
        print(f"v9kimg: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
