# igcfs — igc serial file server

Serves a directory on a modern PC to the **igc** file manager running on a
Victor 9000, over a direct serial cable. In igc, press **F1** on a pane and pick
**Serial A (COM1)**; the pane then browses the directory this program serves.

This is the PC-side *server* half of the vetted Victor file-transfer protocol
(viclibc `serial/` + `filetrx/`). The reliable packet layer and FTX protocol are
vendored here unchanged under `v9kserial/` and `v9kfiletrx/`; `igcfs.py` adds the
serve loop (the mirror of viclibc's C `ftx_serve_one`, from the PC's viewpoint).

## Topology

```
[ this PC /dev/ttyUSB0 ]  <--- RS-232 --->  [ Victor 9000 COM1 / Serial A ]
        igcfs.py (server)                          igc (client)
```

The link runs **8N1, 38400 baud with RTS/CTS hardware flow control** by default.

> **Why 38400 + RTS/CTS?** The Victor uses *polled* serial and gates the sender
> with RTS during its between-packet work; the sender must honor CTS. Earlier the
> default was 9600 because a USB-serial adapter (e.g. PL2303) buffers internally
> and can't honor that gating with byte precision — at 38400 its bursts overflowed
> the Victor's 3-byte receive FIFO and multi-byte packets were lost. A polled
> receive fast path in viclibc (`serial/packet.c`) now drains that FIFO tightly
> enough to keep up at 38400, so it is the default; RTS/CTS still covers the
> multi-packet transfers. If your cable carries only TX/RX/GND (no handshake
> lines), pass `--no-rtscts`; single-packet listings still work, but file
> transfers may not. If a particular USB-serial adapter still drops packets at
> 38400, fall back to `--baud 9600` (and set igc's pane to match). The Pico-based
> rig runs 38400 reliably because it does byte-exact hardware flow control.

## Setup

```sh
cd igc/tools/serialfs
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

## Run

```sh
.venv/bin/python igcfs.py --root /srv/victor --dev /dev/ttyUSB0 --baud 38400 -v
```

- `--root`       directory to serve (the server is sandboxed to it).
- `--dev`        serial device (default `/dev/ttyUSB0`).
- `--baud`       line rate (default 38400; must match igc's pane setting,
                 which is `SERIALFS_BAUD_DEFAULT` in `src/serialfs.h`).
- `--no-rtscts`  disable RTS/CTS flow control (default on; see note above).
- `-v`           verbose request log.

Files are presented to igc as uppercase 8.3 names (long/mixed-case names are
mangled `NAME~1.EXT` deterministically). Supported from igc: browse, view/edit,
copy in/out, delete, mkdir, rename.

## Self-test (no Victor required)

`selftest.py` bridges two local PTYs with `socat`, runs the server on one end and
an igc-emulating client on the other, and exercises list / upload / download /
delete / mkdir / rename end to end:

```sh
.venv/bin/python selftest.py
```
