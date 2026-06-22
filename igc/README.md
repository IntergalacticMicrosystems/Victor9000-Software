# Intergalactic Commander ~ File Manager for Victor 9000

**A two-pane file manager for Victor 9000 DOS 3.1, inspired by Volkov Commander.**  

![IGC Screenshot](igc-screenshot.png)

## Known issues / Limitations

- **DOS < 2.11** - Untested
- **DOS 2.11** - Selecting an empty floppy drive falls back to DOS
- **Speed issues** - Uses DOS file access, some things are slow
- **Keys** - Non-Standard/American keyboard configs may not map 100%

## Features

- **Dual-pane interface** - Navigate two directories simultaneously
- **File operations** - Copy, move, delete, rename files and directories
- **Built-in editor** - View and edit text files
- **Dynamic memory scaling** - Works on systems from 128KB to 512KB+ RAM
- **Fast display** - Direct VRAM access for responsive UI
- **Session persistence** - Remembers your last directory locations
- **Serial file transfer** - Browse a directory on a modern PC over a serial cable and copy files to/from the Victor

## Keyboard Controls

### Navigation
| Key | Action | Vic Key |
|-----|--------|---------|
| Tab | Switch between panels | |
| Up/Down | Move cursor | |
| Left/Right | Parent directory / enter directory | |
| PgUp/PgDn | Scroll page | Word arrow keys |
| Home/End | Jump to first/last file | |
| Enter | Open directory or view file | |
| Backspace | Go to parent directory | |
| Space | Select/deselect file | |
| Esc or Q | Quit | |

### Function Keys
| Key | Action |
|-----|--------|
| F1 | Change drive |
| F2 | Create directory |
| F3 | View file (read-only) |
| F4 | Edit file |
| F5 | Copy/Move |
| F6 | Delete |
| F7 | Quit (F10 also quits) |
| F8 | Rename |

## Serial File Transfer

IGC can browse a directory on a modern PC over a direct RS-232 cable and copy
files in both directions — handy for moving programs and data on/off the Victor
without floppies.

### On the Victor (client)

Press **F1** (Change drive) on a pane and select **Serial A** at the bottom of
the drive list. The pane then browses the directory served by the PC and
supports the usual operations: browse, view/edit, copy in/out (F5), delete (F6),
mkdir (F2), and rename (F8). The path is shown as `SER:\...`. Remote files are
presented as uppercase 8.3 names (long/mixed-case names are mangled to
`NAME~1.EXT`).

The link runs **8N1 at 38400 baud over a 3-wire cable (no hardware flow control)**.

### On the PC (server)

The PC-side server, `igcfs.py`, lives in [`tools/serialfs/`](tools/serialfs/).
It serves one directory (sandboxed to it) to the Victor.

```sh
cd tools/serialfs
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt

# Serve /srv/victor over /dev/ttyUSB0 at 38400 baud
.venv/bin/python igcfs.py --root /srv/victor --dev /dev/ttyUSB0 --baud 38400 -v
```

| Option | Meaning |
|--------|---------|
| `--root` | Directory to serve (server is sandboxed to it) |
| `--dev` | Serial device (default `/dev/ttyUSB0`) |
| `--baud` | Line rate (default 38400; must match the Victor's pane setting) |
| `-v` | Verbose request log |

> **Cable note:** the link is 3-wire (TX/RX/GND). The Victor uses *polled* serial
> and never toggles RTS, so hardware flow control does nothing; reliable
> multi-packet transfers come from the Victor's lean polled-receive path. A plain
> TX/RX/GND cable is all you need — see [`tools/serialfs/README.md`](tools/serialfs/README.md)
> for the full protocol and baud-rate notes.

## Installation

Copy `IGC.EXE` to your Victor 9000 hard drive or floppy disk.

## Building from Source

Requires Open Watcom v2 compiler.

```bash
make            # Build igc.exe
make clean      # Remove build artifacts
make deploy     # Deploy to MAME disk image
```

Output: `bin/igc.exe` (~30KB)

## System Requirements

- Victor 9000 / Sirius 1 computer
- DOS 3.1 or compatible
- Minimum 128KB RAM (scales with available memory)

### Memory Scaling

The program automatically adapts to the free RAM detected at startup:

| Free Memory | Files per Panel | Editor Buffer |
|-------------|-----------------|---------------|
| < 64KB | 64 | 4KB |
| 64-128KB | 256 | 16KB |
| 128-200KB | 512 | 32KB |
| > 200KB | 1024 | 64KB |


