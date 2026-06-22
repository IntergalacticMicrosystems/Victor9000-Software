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
copy in/out, delete, mkdir, rename.

## Self-test (no Victor required)

`selftest.py` bridges two local PTYs with `socat`, runs the server on one end and
an igc-emulating client on the other, and exercises list / upload / download /
delete / mkdir / rename end to end:

```sh
.venv/bin/python selftest.py
```
