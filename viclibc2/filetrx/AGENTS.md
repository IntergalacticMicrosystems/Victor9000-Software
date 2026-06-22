# Serial File Transfer Library

A file transfer library for transferring files between a modern PC (Python script) and a Victor 9000 PC.

# Rules - DO NOT REMOVE THIS SECTION

- Always update the documentation when changes are made
- Continue work uninterrupted unless you are stuck

# Features

- Built on the serial library in ../serial/
- Written in OpenWatcom C like the serial library
- Bidirectional transfers (PC <-> Victor)
- Robust transfers using CRC-32 file verification
- Packet-level CRC-16 via the underlying packet protocol
- Error detection, recovery with retries, graceful failure
- Optional RLE compression (fast decompression on Victor side)
- Directory listing support
- **Partial transfers (offset/segment) for large + resumable files.** The START
  packet carries an `offset` (the former unused `compressed_size` field) and the
  `FTX_FLAG_PARTIAL` flag: a putfile opens the existing file and seeks to `offset`
  (vs create/truncate at 0), and a getfile request seeks to `offset` and sends
  `file_size` bytes (0 = to EOF). `FTX_CMD_QUERY`/`FTX_CMD_HAVE` report a file's
  current size. The PC-side host (`victor9k_remote_control`) uses these to split
  any file into buffer-sized segments and to resume — the Victor library itself
  is stateless per request, so file size is bounded only by disk space.
- **Server dispatch** (`ftx_serve` / `ftx_serve_one`): one call handles a single
  inbound request — a putfile (receive), a getfile (send the requested file), a
  LIST, a QUERY, or a QUIT — and returns so the caller can poll the keyboard
  between requests. This is the heart of both `FTXSERV.EXE` and `FTXTEST.EXE`.
- **Application-owned disk staging buffer** (`ftx_set_io_buffer`): the app hands
  the library one buffer it uses for read-ahead while sending and write-batching
  while receiving. Bigger buffers mean fewer, larger disk operations — a big win
  on slow floppy media (FTXSERV/FTXTEST register 8 KB). A transfer fails with
  `FTX_ERR_MEMORY` if no buffer was registered.
- **Per-chunk credit flow control** (`FTX_FLAG_FLOWCTRL` / `FTX_CAP_FLOWCTRL`):
  the DATA sender can ask the receiver to send a READY credit after every chunk
  it commits, so the sender holds the next chunk while the receiver does slow
  between-chunk work (e.g. a multi-second floppy write) that would otherwise
  overrun the 3-byte RX FIFO. Negotiated in the START READY's `caps` byte; an old
  peer that ignores the flag never credits and the sender falls back to streaming.
- **Raw disk-sector transfer / whole-disk imaging** (`ftx_send_sectors` /
  `ftx_parse_sector` / `ftx_recv_sectors_after_sector`, server commands
  `FTX_CMD_DISKINFO` / `FTX_CMD_SECTOR`): the same chunk / RLE / CRC-32 /
  flow-control / retry engine that moves files can move **raw logical sectors**
  via DOS Absolute Disk Read/Write (**INT 25h/26h**, in `ftx_dosfile.c`). DOS
  hides the Victor GCR/zone floppy format, so a PC can image a whole Victor disk
  it could never read directly, and write an image back. The PC asks
  `FTX_CMD_DISKINFO` for a drive's geometry (`ftx_disk_geometry`, INT 21h AH=36h),
  then drives `FTX_CMD_SECTOR` transfers (the sector analog of START: drive +
  start-sector + count, host-segmentable like a large file). Implemented as a
  backing-store switch in the engine (`io_kind` = FILE | DISK): only the two leaf
  helpers `read_at` (send) and `flush_write_buf` (receive) became backend-aware;
  the hot loops are unchanged. **Track-at-a-time on the floppy:** a Victor floppy
  uses variable-zone recording (19→12 sectors/track, side 0 then side 1 with an
  8-track zone shift; full double-sided volume is **2391** logical sectors, not
  the smaller AH=36h data-area count). For a 512-byte floppy (drive A/B) the
  sender reads **one whole track per INT 25h** (`ftx_victor_track_bounds`) and
  caps each DATA chunk to a track boundary so nothing straddles - one revolution
  per track, no redundant adjacent-track re-reads (HW-verified ~2.3 KB/s,
  retries=0). The geometry is from the DOS driver `DIO.PLM` (see
  `../../disassembly/diskcopy_com`). Hard disks fall back to fixed-block reads.
  **Sector writes are destructive** (no file-system
  safety net), so the server rejects them unless opted in via
  `ftx_allow_sector_write()` (FTXSERV `/allowwrite`). The legacy INT 25h/26h form
  uses a 16-bit sector number, capping a device at 65535 sectors (~32 MB at 512
  B/sector) - ample for Victor floppies and small hard disks. PC side:
  `FileTransfer.disk_info` / `read_disk` / `write_disk` and the
  `read_disk.py` / `write_disk.py` CLIs.
- **Polled, 3-wire, up to 38400.** The underlying serial/packet library is now
  **polled-only** (`pkt_init`, with `pkt_init_polled` kept as an identical alias):
  a tight 8088 loop drains the µPD7201's 3-byte RX FIFO fast enough to stay clean
  all the way to **38400** — the async ceiling, and 4× the old interrupt path's
  reliable 9600 (see `../serial/AGENTS.md`, "Polled transfer speed"). The link is
  3-wire (no RTS/CTS); a lost byte just draws a NAK/retry from the packet layer,
  and the per-chunk credit flow control above brackets the slow floppy-write
  window so the sender doesn't outrun the FIFO between chunks.
- Test/server programs for Victor DOS (`FTXSERV.EXE`, `FTXTEST.EXE`) and a PC
  Python client (`v9kfiletrx`).

# Building

## Victor (C Library)

```bash
source /opt/watcom/owsetenv.sh
export LIB=$WATCOM/lib286/dos
cd ../serial && wmake     # Build serial library first
cd ../filetrx && wmake    # Build file transfer library
```

Outputs:
- `filetrx.lib` - Static library
- `ftxserv.exe` - File server (COM2, polled, default 38400) for the Pico
  remote-control bridge or any filetrx PC client. Serves
  putfile/getfile/listdir/query/diskinfo/sector-read until QUIT or ESC.
  `FTXSERV [9600|19200|38400] [/allowwrite]` - `/allowwrite` also permits
  destructive raw-sector writes (disk-image restore); off by default.
- `ftxtest.exe` - Hardware-in-the-loop receive test (COM1, polled 38400). Serves
  the same requests via `ftx_serve_one()`; used to exercise the putfile receive
  path (per-chunk credit flow control + write batching) against the Python client.

## PC (Python)

```bash
cd python
pip install -e .
```

# API Overview

## C API (Victor side)

```c
#include "filetrx.h"

// Initialize. The library is polled-only; pkt_init and pkt_init_polled are
// identical aliases. Pair with ser_init_port() at the desired baud (<=38400).
ftx_state_t ftx;
pkt_state_t pkt;
static uint8_t io_buf[8192];
pkt_init(&pkt, SER_PORT_B);
ftx_init(&ftx, &pkt);
ftx_set_io_buffer(&ftx, io_buf, sizeof(io_buf));  // required before any transfer

// --- Server model (what FTXSERV/FTXTEST do) ---
// Handle one request at a time so the caller can poll the keyboard between them.
for (;;) {
    if (kbhit() && getch() == 27) break;            // ESC to quit
    if (ftx_serve_one(&ftx, progress_cb) == FTX_SERVE_QUIT) break;
}
// Or block until the PC sends QUIT:  ftx_serve(&ftx, progress_cb);

// --- Direct client calls ---
ftx_receive_file(&ftx, "LOCAL.TXT", 1, progress_cb);          // receive from PC
ftx_send_file(&ftx, "LOCAL.TXT", FTX_COMP_RLE, progress_cb);  // send to PC

// --- Raw disk sectors (imaging) ---
// Reads/writes are normally driven by the server (ftx_serve_one dispatches
// DISKINFO + SECTOR). Sector writes are destructive, so enable them explicitly:
ftx_allow_sector_write(&ftx, 1);                              // off by default
// Direct: read 64 sectors from drive A (0) starting at sector 0 and send to PC.
ftx_send_sectors(&ftx, 0, 0, 64, FTX_COMP_NONE, progress_cb);

// Cleanup
ftx_shutdown(&ftx);
```

`ftx_serve_one()` returns `FTX_SERVE_IDLE` (nothing arrived before the receive
timed out), `FTX_SERVE_DONE` (handled one request), or `FTX_SERVE_QUIT`.

## Python API (PC side)

```python
from v9kserial import MamePtyConnection, PacketProtocol
from v9kfiletrx import FileTransfer, Compression

conn = MamePtyConnection(baudrate=9600)
conn.connect()
pkt = PacketProtocol(conn)
ftx = FileTransfer(pkt)

# Send file to Victor
stats = ftx.send_file("local.txt", "REMOTE.TXT", compression=Compression.RLE)

# Receive file from Victor
stats = ftx.receive_file("REMOTE.TXT", "local_copy.txt")

# List Victor directory
entries = ftx.list_directory("A:\\")

# Whole-disk imaging (drive 0 = A:). Reads are always allowed; writes need the
# server started with /allowwrite and are DESTRUCTIVE.
bps, total = ftx.disk_info(0)                 # geometry: bytes/sector, sectors
ftx.read_disk(0, "driveA.img")                # image disk -> local file
ftx.write_disk(0, "driveA.img")               # restore image -> disk (destructive)
```

The remote name passed to `send_file` may be drive/dir-qualified (e.g.
`B:\COMMAND.COM`). It is uppercased and clamped to 8.3, but only the *filename*
component is clamped — any drive/path prefix is preserved (a bare 12-char clamp
would corrupt `B:\COMMAND.COM` into `B:\COMMAND.C`).

# Protocol

## Commands

| Command | Value | Description |
|---------|-------|-------------|
| START   | 0x10  | File metadata (name, size, offset, CRC, flags) |
| DATA    | 0x11  | File data chunk (max 1024 bytes) |
| END     | 0x12  | Transfer complete, verify CRC |
| ABORT   | 0x13  | Abort transfer |
| STATUS  | 0x14  | Status/progress query |
| LIST    | 0x15  | Directory listing request |
| LIST_RESP | 0x16 | Directory listing response |
| ERROR   | 0x17  | Error message |
| READY   | 0x18  | Ready to receive (carries a `caps` byte, e.g. `FTX_CAP_FLOWCTRL`) |
| RESEND  | 0x19  | Request chunk resend |
| QUERY   | 0x1A  | Query a file's size/existence (PC→Victor) |
| HAVE    | 0x1B  | Query response: exists + current size (Victor→PC) |
| DISKINFO | 0x1C | Query a drive's sector geometry (PC→Victor) |
| DISKINFO_RESP | 0x1D | Geometry response: bytes/sector + total sectors (Victor→PC) |
| SECTOR  | 0x1E  | Start a raw logical-sector transfer (disk imaging; drive + start-sector + count + direction) |
| QUIT    | 0x1F  | Ask a running file server to exit |

START flags (`FTX_FLAG_*`): `OVERWRITE` 0x01, `CREATE_DIR` 0x02, `PARTIAL` 0x04
(segment starting at `offset`, for large/resumable transfers), `FLOWCTRL` 0x08
(request per-chunk credit flow control).

## Transfer Flow

```
PC (Sender)                 Victor (Receiver)
    |-- START (metadata) -->|
    |<------ READY ---------|
    |-- DATA (chunk 0) ---->|
    |<------ ACK -----------|
    |-- DATA (chunk 1) ---->|
    |<------ ACK -----------|
    ...
    |-- END (verify CRC) -->|
    |<------ ACK -----------|
```

## Compression

RLE with escape encoding:
- Escape byte: 0x90
- Run format: `[0x90][count][byte]` = repeat (count+3) times
- Literal 0x90: `[0x90][0x00]`
- Minimum run length: 4 bytes

# File Structure

```
filetrx/
├── include/
│   ├── filetrx.h           # Main API
│   ├── ftx_protocol.h      # Protocol constants
│   ├── ftx_crc32.h         # CRC-32
│   ├── ftx_compress.h      # RLE compression
│   └── ftx_dosfile.h       # DOS file I/O
├── src/
│   ├── filetrx.c           # Init, stats, packet I/O, io-buffer registration
│   ├── ftx_send.c          # Send logic (getfile, read-ahead)
│   ├── ftx_recv.c          # Receive logic (putfile, flow control, write batching)
│   ├── ftx_server.c        # Server dispatch (ftx_serve / ftx_serve_one)
│   ├── ftx_compress.c      # RLE implementation
│   ├── ftx_crc32.c         # CRC-32 implementation
│   └── ftx_dosfile.c       # DOS file wrapper + raw sector I/O (INT 25h/26h)
├── test/
│   ├── ftxserv.c           # FTXSERV.EXE - file server (COM2, polled; /allowwrite)
│   └── ftxtest.c           # FTXTEST.EXE - receive test (COM1, polled 38400)
├── python/
│   ├── v9kfiletrx/         # Python client package (FileTransfer, Compression)
│   ├── send_file.py        # CLI send tool
│   ├── receive_file.py     # CLI receive tool
│   ├── read_disk.py        # CLI disk-image read (sector imaging)
│   ├── write_disk.py       # CLI disk-image write (destructive restore)
│   ├── ftx_test.py         # Interactive test
│   └── test_suite.py       # Legacy automated suite (see Testing notes below)
└── Makefile
```

# Performance

The library is polled-only and runs clean to 38400 (the async ceiling).
Throughput is essentially line rate (baud/10):

| Baud  | Effective    | 64 KB file (uncompressed) |
|-------|--------------|---------------------------|
| 9600  | ~0.9 KB/sec  | ~70 s                     |
| 19200 | ~1.9 KB/sec  | ~34 s                     |
| 38400 | ~3.8 KB/sec  | ~17 s                     |

- 64 KB and 256 KB putfile + getfile verified byte-identical both ways on real
  hardware at 38400 (see `../serial/AGENTS.md`, "Polled packet layer + FTXSERV").
- RLE roughly halves wall-clock on compressible data; CRC-32 of the whole file
  before a getfile adds a fixed startup cost.

# Testing

## Current model: hardware-in-the-loop

The Victor side runs a polled server and the PC drives it with the `v9kfiletrx`
Python client over the real serial link (`/dev/ttyUSB0`). No `FTX_CMD_TEST_*`
indirection is involved — the client just issues real putfile/getfile/listdir
requests.

- **`FTXTEST.EXE`** — receive-path test. Listens on COM1 (SER_PORT_A), polled
  38400, and serves requests via `ftx_serve_one()`. Run it from `C:\` so an
  incoming putfile lands on C:. Exercises the per-chunk credit flow control and
  the 4 KB write batching in `ftx_recv.c`. getfile/listdir/query/quit also work,
  so the same console can pull a file back to verify a round trip.
- **`FTXSERV.EXE`** — the production server (COM2/SER_PORT_B, polled, default
  38400) used by the Pico remote-control bridge. Delivery of `FTXTEST.EXE`
  itself to the Victor is typically done with the Pico over COM2/FTXSERV; then
  drive a putfile from the PC over ttyUSB0 (COM1). NOTE: FTXSERV needs a short
  settle gap between back-to-back requests.

Drive transfers directly with the client:

```python
from v9kserial import SerialConnection, PacketProtocol
from v9kfiletrx import FileTransfer, Compression

conn = SerialConnection("/dev/ttyUSB0", baudrate=38400); conn.connect()
pkt = PacketProtocol(conn)
ftx = FileTransfer(pkt)
ftx.send_file("local.bin", "REMOTE.BIN", compression=Compression.NONE)
```

### Disk imaging (sectors)

Verify against the same hardware loop. Start with geometry (`ftx.disk_info(0)` →
`(512, total_sectors)`), which exercises the wire path before any INT 25h call.
Then `ftx.read_disk(0, "driveA.img")` and confirm the image is
`total_sectors * 512` bytes (a blank/zeroed disk RLE-compresses heavily and gives
a stable CRC across two reads). For the write path, start the server with
`FTXSERV /allowwrite`, `ftx.write_disk(0, "driveA.img")` to a **scratch** disk,
then `read_disk` it back and compare byte-for-byte. Sector writes are destructive
with no file-system safety net — never target a disk whose contents matter, and
never sector-write over media holding a running EXE.

`send_file.py` / `receive_file.py` / `ftx_test.py` wrap this for the command line.

## Legacy automated suite

`python/test_suite.py` predates the polled server and drives an older
test-command orchestration (control commands `0x20-0x23`: `TEST_CMD`,
`TEST_RESULT`, `TEST_PING`, `TEST_PONG`; sub-commands `SEND_FILE`, `RECV_FILE`,
`SET_COMPRESS`, `SET_BAUD`, `DELETE_FILE`, `QUIT`). The current `FTXTEST.EXE` no
longer implements that test-command server, so the orchestrated/error-injection
modes do not apply to it; the suite's plain putfile/getfile paths still exercise
the library. The `FTX_CMD_TEST_*` constants remain in `ftx_protocol.h` for
reference.

