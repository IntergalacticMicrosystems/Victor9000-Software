# igcfs — igc serial file server

Serves a directory on a modern PC to the **igc** file manager running on a
Victor 9000, over a a simple 3-wire null-modem serial cable.
In igc, press **F1** on a pane and pick **Serial A (COM1)**;
the pane then browses the directory this program serves.

This is the PC-side *server* half of the vetted Victor file-transfer protocol
(viclibc `serial/` + `filetrx/`). The reliable packet layer and FTX protocol are
vendored here unchanged under `v9kserial/` and `v9kfiletrx/`; `igcfs.py` adds the
serve loop (the mirror of viclibc's C `ftx_serve_one`, from the PC's viewpoint).

## Topology

```
[ this PC /dev/ttyUSB0 ]  <--- RS-232 --->  [ Victor 9000 COM1 / Serial A ]
        igcfs.py (server)                          igc (client)
```

The link runs **8N1, 38400 baud over a 3-wire cable (no hardware flow control)** by default.

> **Why 38400 on 3 wires?** The Victor uses *polled* serial and never toggles
> RTS (the uPD7201's RTS is a static CR5 output), so RTS/CTS hardware flow
> control is inert and the link is effectively 3-wire (TX/RX/GND). Earlier the
> default was 9600 because at 38400 a USB-serial adapter's bursts overflowed the
> Victor's 3-byte receive FIFO and multi-byte packets were lost. A polled receive
> fast path in viclibc (`serial/packet.c`) now drains that FIFO tightly enough to
> keep up at 38400, so it is the default — and the packet layer's ACK round-trip
> brackets the Victor's between-packet work, so no line-level flow control is
> needed. If a particular USB-serial adapter still drops packets at 38400, fall
> back to `--baud 9600` (and set igc's pane to match), or use `--cts-gate` for
> per-frame software pacing. The Pico-based rig runs 38400 reliably with
> byte-exact timing.

## Run (standalone executable — no Python needed)

End users don't need Python installed. Build (or download) the single-file
executable and run it from a terminal:

```bat
REM Windows
igcfs.exe --root C:\victor --dev COM1 --baud 38400 -v
```

```sh
# Linux / macOS
./igcfs --root /srv/victor --dev /dev/ttyUSB0 --baud 38400 -v
```

### Building the executable

The build bundles the interpreter, `pyserial`, and the vendored `v9kserial/` +
`v9kfiletrx/` packages into one file via [PyInstaller](https://pyinstaller.org).
PyInstaller **cannot cross-compile**, so build on the OS you want a binary for:

```bat
REM Windows  ->  dist\igcfs.exe
build.bat
```

```sh
# Linux / macOS  ->  dist/igcfs
./build.sh
```

Both wrappers create a throwaway venv, install the build deps, and run
`pyinstaller igcfs.spec`. The file in `dist/` is self-contained and ships alone.

## Run from source (developers)

```sh
cd igc/tools/serialfs
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python igcfs.py --root /srv/victor --dev /dev/ttyUSB0 --baud 38400 -v
```

- `--root`       directory to serve (the server is sandboxed to it).
- `--dev`        serial device (default `/dev/ttyUSB0`).
- `--baud`       line rate (default 38400; must match igc's pane setting,
                 which is `SERIALFS_BAUD_DEFAULT` in `src/serialfs.h`).
- `-v`           verbose request log.

Files are presented to igc as uppercase 8.3 names (long/mixed-case names are
mangled `NAME~1.EXT` deterministically). Supported from igc: browse, view/edit,
copy in/out, delete, mkdir, rename. Directories copy recursively (single or
nested) in either direction: igc walks the tree and pushes/pulls each file. An
upload whose name carries a path separator is stored under that subdirectory
(parents auto-created); a bare name still lands in the last-listed directory.
The final name component is reverse-mapped through the 8.3 map, so writing to
`MYDOCU~1.TXT` updates *My Documents.txt* rather than creating a second file.

Uploads are stored atomically (temp file + rename after a whole-file CRC
check) and acknowledged to igc after the transfer — a failed store can't
destroy an existing file and is reported instead of silently vanishing.
Because igc waits for that acknowledgement, **igc and igcfs must be updated
together**: a new igc against an old igcfs reports every upload as failed.

Downloads (PC -> Victor) run with per-chunk credit: the START asks for it
(`FTX_FLAG_FLOWCTRL`), igc confirms in its READY, and the server holds each
chunk until igc has sent a READY for the previous one. The Victor polls a
3-byte RX FIFO and acknowledges a chunk before decoding and writing it, so
streaming on the ACK alone overruns the FIFO while it is busy. Every igc built
on viclibc2 already honours the flag, so updating igcfs alone fixes downloads;
an older igc answers a 2-byte READY and gets the old back-to-back stream.

The ~N mangle numbering is recomputed from the directory's sorted contents on
each request, so avoid adding/removing files in a served directory from the PC
side mid-session — a mangled name could remap to a different file.

## Disk images

igc can read a whole Victor floppy into an image file on the server, or write
an image file from the server onto a formatted Victor floppy (F6 More). The
server holds the image in memory between OPEN and CLOSE and moves it one track
at a time, as 1 KB fragments; when writing a disk, igc requests each fragment
separately (an igcfs older than this streams a whole track per request, which
a current igc cannot follow: update both together). The file name's extension picks the format: `.IMG` (raw) or
`.RLE` (compressed, below); anything else is refused. A new image (mode 0) is
named like an upload: a bare name lands in the last-listed directory, a
path-qualified one under root. An existing image (mode 1) is named like a
download, by its path from root. Either way the 8.3 components are
reverse-mapped, so `MYDISK~1.RLE` reaches a long-named file.

Geometry: 80 tracks per side, 512-byte sectors, zoned (19..12 sectors on side
0, 18..11 on side 1). Single-sided = 1224 sectors = 626,688 bytes,
double-sided = 2391 sectors = 1,224,192 bytes; any other size is refused.
Tracks are numbered linearly 0..159 (side 0 tracks 0..79, then side 1), and
the `.img` is in that logical-sector order. The table lives in `v9kimage.py`.

| Byte | Name | Direction | Payload |
|------|------|-----------|---------|
| 0x30 | `IMG_OPEN` | V->PC | `[cmd][mode][total_sectors u16][name_len][name]`. Mode 0 = create image (Victor reads its floppy, pushes tracks); mode 1 = read image (Victor pulls tracks, writes its floppy). `total_sectors` (1224 or 2391) only matters for mode 0. A new OPEN discards any unfinished job. |
| 0x31 | `IMG_INFO` | PC->V | `[cmd][status][total_sectors u16]`, the reply to OPEN. Status 0 ok, 1 not found / cannot open, 2 not a valid Victor disk image (extension, size, `.rle` header or CRC), 3 other. `total_sectors` = the image's size for mode 1, the echoed request for mode 0, 0 on refusal. In mode 1 an `.rle` is fully decoded and checked here, before the floppy is touched. |
| 0x32 | `IMG_GET` | V->PC | `[cmd][track][frag]`. Mode 1: the server answers with that one `IMG_TRACK` fragment; igc asks for the next only after decoding this one, so nothing arrives while the Victor is busy. The 2-byte `[cmd][track]` of an older igc gets the whole track's fragments back to back. No read job, bad track or bad fragment: ERROR. |
| 0x33 | `IMG_TRACK` | both | `[cmd][track][frag][nfrag][flags][len u16][data]`. `nfrag` = ceil(track bytes / 1024); fragment `i` covers track bytes `i*1024..` (the last one of a 19-sector track is 512 bytes). Flags bit 0 = data is RLE, set only when that saved bytes. Max 1031 bytes. In mode 0 the server replies once per track, after the fragment with `frag == nfrag-1`: `CMD_OK` if every fragment arrived in order with the right decoded length, else ERROR (the track counts as not received; resending it overwrites). |
| 0x34 | `IMG_CLOSE` | V->PC | `[cmd][status][crc32 u32]`. Status 0 = complete. Mode 0: every track must have arrived and the CRC-32 of the assembled image must match, then the file is written (temp file + rename) and `CMD_OK` is sent; otherwise ERROR and nothing is written. Mode 1: `CMD_OK` if the CRC matches the served image, ERROR (CRC) if not, which igc reports as a warning. Status 1 = abort: everything is dropped, `CMD_OK`. |

### `.rle` format

A 16-byte header, then the whole raw image encoded with the viclibc RLE scheme
(escape 0x90, runs of 4..258 as `[0x90][count-3][byte]`, a literal 0x90 as
`[0x90][0x00]`) as one continuous stream. igc writes it as independently
compressed 1 KB slices, which is still a valid continuous stream.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 8 | Magic `V9KRLE\x1a\x01` (last byte = format version 1) |
| 8 | 4 | Raw image size, little-endian (626688 or 1224192) |
| 12 | 4 | CRC-32 of the raw image, little-endian (zlib / FTX polynomial) |

### `v9kimg.py`

Converts and inspects images on the PC (it uses the same vendored codec):

```sh
python3 v9kimg.py info   DISK.img|DISK.rle    # sides (by size and by label), sectors, CRC, label bytes
python3 v9kimg.py pack   DISK.img [DISK.rle]  # img -> rle (default: same name, .rle)
python3 v9kimg.py unpack DISK.rle [DISK.img]  # rle -> img
```

The label's sidedness is bit 0 of the word at offset 32 of sector 0; `info`
flags an image whose size and label disagree.

## Self-test (no Victor required)

`selftest.py` bridges two in-process connection endpoints (a fake serial cable), runs the server on one end and
an igc-emulating client on the other, and exercises list / upload / download /
delete / mkdir / rename and disk-image push/pull end to end:

```sh
.venv/bin/python selftest.py
```
