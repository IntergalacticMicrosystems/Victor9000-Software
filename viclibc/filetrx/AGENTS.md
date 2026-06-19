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
- Test programs for Victor DOS and PC Python
- `FTXSERV.EXE` enables the serial RX-side RTS throttle
  (`ser_set_rx_flow(SER_PORT_A, TRUE)`) for hardware flow control on inbound
  transfers; it leaves `flow_ctrl = SER_FLOW_NONE` (the µPD7201 has no automatic
  RTS, so RTS is the software watermark throttle, and TX-side CTS gating is not
  needed). NOTE: file packets only work at **9600** — at 19200 the 8088 can't
  drain the 7201's 3-byte RX FIFO fast enough and ~86-byte packets hit
  `SER_ERR_OVERRUN`; the ring-level throttle can't prevent a per-FIFO overrun.

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
- `ftxtest.exe` - Automated test server (no interaction required)

## PC (Python)

```bash
cd python
pip install -e .
```

# API Overview

## C API (Victor side)

```c
#include "filetrx.h"

// Initialize
ftx_state_t ftx;
pkt_state_t pkt;
pkt_init(&pkt, SER_PORT_A);
ftx_init(&ftx, &pkt);

// Receive file from PC
ftx_receive_file(&ftx, "LOCAL.TXT", 1, progress_callback);

// Send file to PC
ftx_send_file(&ftx, "LOCAL.TXT", FTX_COMP_RLE, progress_callback);

// Cleanup
ftx_shutdown(&ftx);
```

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
```

# Protocol

## Commands

| Command | Value | Description |
|---------|-------|-------------|
| START   | 0x10  | File metadata (name, size, CRC) |
| DATA    | 0x11  | File data chunk (max 1016 bytes) |
| END     | 0x12  | Transfer complete, verify CRC |
| ABORT   | 0x13  | Abort transfer |
| LIST    | 0x15  | Directory listing request |
| LIST_RESP | 0x16 | Directory listing response |
| READY   | 0x18  | Ready to receive |
| RESEND  | 0x19  | Request chunk resend |

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
│   ├── filetrx.c           # Main implementation
│   ├── ftx_send.c          # Send logic
│   ├── ftx_recv.c          # Receive logic
│   ├── ftx_compress.c      # RLE implementation
│   ├── ftx_crc32.c         # CRC-32 implementation
│   └── ftx_dosfile.c       # DOS file wrapper
├── test/
│   └── ftxtest.c           # Victor test program
├── python/
│   ├── v9kfiletrx/         # Python package
│   ├── send_file.py        # CLI send tool
│   ├── receive_file.py     # CLI receive tool
│   └── ftx_test.py         # Interactive test
└── Makefile
```

# Performance

At 9600 baud:
- Effective: ~700-800 bytes/sec
- 64KB file: ~90 seconds uncompressed, ~45 seconds with RLE

At 19200 baud:
- Effective: ~1400-1600 bytes/sec
- 64KB file: ~45 seconds uncompressed

# Automated Test Suite

The test suite provides fully automated testing with no Victor-side user interaction beyond starting `FTXTEST.EXE`.

## Running Tests

1. Build and copy to Victor:
```bash
cd /root/Victor9000-Development-Private/viclibc/filetrx
source /opt/watcom/owsetenv.sh && wmake
cd /root/disks && ./venv/bin/python -m vtg_image_util copy \
  /root/Victor9000-Development-Private/viclibc/filetrx/ftxtest.exe \
  '/root/disks/victor9k/vichd31.img:0:\FTXTEST.EXE'
```

2. Start MAME:
```bash
cd /root/bldmame1 && ./mame victor9k -w -nomaximize -bios univ \
  -plugin mcpplugin -rs232a pty -ramsize 512k \
  -harddisk /root/disks/victor9k/vichd31.img
```

3. Run `FTXTEST.EXE` on Victor (auto-starts test server mode)

4. Run Python test suite:
```bash
cd /root/Victor9000-Development-Private/viclibc/filetrx/python
python test_suite.py --iterations 3 --baud 9600
```

## Test Suite Options

```
python test_suite.py [OPTIONS]

Options:
  --iterations N, -n N    Number of iterations per test (default: 3)
  --baud RATE, -b RATE    Baud rate: 9600 or 19200 (default: 9600)
  --connection TYPE, -c   Connection: mame or real (default: mame)
  --port PORT, -p PORT    Serial port for real connection (default: /dev/ttyUSB0)
  --errors, -e            Include error injection tests
  --compression MODE      Test modes: none, rle, both (default: both)
  --generate-only         Only generate test files
  --no-quit               Don't send quit command at end
```

## Test Cases

- **File sizes**: 500B, 5KB, 20KB, 50KB, 5KB compressible
- **Directions**: PC->Victor (Victor->PC available but not in default suite)
- **Compression**: None and RLE
- **Error injection** (with --errors flag):
  - Single packet drop
  - Single packet corruption
  - Multiple packet drops
  - Light random errors (5% drop, 2% corrupt)

## Test Protocol

The test suite uses a control protocol (commands 0x20-0x23) to orchestrate tests:

| Command | Value | Description |
|---------|-------|-------------|
| TEST_CMD | 0x20 | Test command from PC |
| TEST_RESULT | 0x21 | Result from Victor |
| TEST_PING | 0x22 | Ping request |
| TEST_PONG | 0x23 | Ping response |

Test sub-commands:
- `SEND_FILE (0x01)` - Victor sends file to PC
- `RECV_FILE (0x02)` - Victor receives file from PC
- `SET_COMPRESS (0x03)` - Set compression mode
- `SET_BAUD (0x04)` - Set baud rate
- `DELETE_FILE (0x05)` - Delete a file
- `QUIT (0xFF)` - Exit test server

## Example Output

```
Generating test files...
Connecting (MAME_PTY)...
  Connected to PTY: /dev/pts/5
Pinging Victor test server...
Victor test server ready!

============================================================
Compression: None
============================================================
  Compression: None
[1/30] test_small.bin (iter 1)... PASS (720 B/s, retries=0)
[2/30] test_small.bin (iter 2)... PASS (715 B/s, retries=0)
...

======================================================================
SUMMARY
======================================================================
  Total Tests: 30
  Passed: 30
  Failed: 0
  Pass Rate: 100.0%
  Average Speed: 725 B/s
```



