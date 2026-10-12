# Plan: floppy disk imaging in IGC (.img / .rle)

Status: proposal, not started. Written 2026-10-11.

## Goal

From IGC, read a Victor floppy into an image file, or write an image file back
onto a formatted Victor floppy. The image can live on a local DOS drive or on
the serial file server (igcfs). Disk I/O is always a whole track per DOS call;
serial transfers are always one track per exchange, RLE-compressed. A PC-side
Python tool converts between `.img` and `.rle`.

## Facts the design rests on

**Geometry** (MAME `victor9k_dsk.cpp`, matches `vtg_image_util` data_start /
cluster counts). 80 tracks per side, 512-byte sectors, zoned sector counts:

| Side | Sectors per track, by run of tracks | Sectors |
|------|-------------------------------------|---------|
| 0 | 19x4, 18x12, 17x11, 16x11, 15x10, 14x12, 13x11, 12x9 | 1224 |
| 1 | 18x8, 17x11, 16x11, 15x10, 14x12, 13x11, 12x12, 11x5 | 1167 |

| Disk | Sectors | `.img` size (bytes) |
|------|---------|---------------------|
| Single-sided | 1224 | 626,688 |
| Double-sided | 2391 | 1,224,192 |

The biggest track is 19 sectors = 9728 bytes. The sidedness of a disk is bit 0
of the word at offset 32 in logical sector 0 (the Victor disk label).

**Image layout.** `.img` is the DOS logical-sector order: side 0 tracks 0..79,
then side 1 tracks 0..79. This is what MAME loads and writes, so DOS logical
sector N is byte offset N*512 in the file. Track-at-a-time means: for linear
track index `t` (0..159), first logical sector = sum of the sectors of all
earlier tracks, count = that track's sector count. Two tiny zone tables (eight
run lengths per side) give both; no 160-entry table needed.

**DOS access.** INT 25h (read) / INT 26h (write): AL = drive (0 = A), CX =
sector count, DX = first logical sector, DS:BX = buffer. These do not raise
INT 24h; errors come back in AX with the carry flag. Both leave a word on the
stack after return, so they cannot go through `int86x()` like the rest of
`dosapi.c`: they need a small inline-asm wrapper that does `int 25h` / `int 26h`
then `popf`. Call INT 21h AH=0Dh (disk reset) before imaging and after writing
so DOS does not later flush stale buffers over the new contents.

**Existing pieces reused.** `rle_compress`/`rle_decompress` (viclibc
`ftx_compress.c`, escape 0x90, min run 4, max run 258; the Python twin is
`tools/serialfs/v9kfiletrx/compress.py`), `ftx_crc32_update`, the polled
packet layer (`pkt_send` is stop-and-wait, so each packet is ACKed before the
next is sent), the progress bar (`ui_show_progress` / `ui_progress_tick`), the
ESC-polling pattern (`sfs_tree_cancelled` in `serialfs.c`), `dlg_drive_select`
as the model for list dialogs.

## File formats

- `.img`: raw image, exactly 626,688 or 1,224,192 bytes. Any other size is
  rejected on both sides.
- `.rle`: a 16-byte header followed by the whole raw image encoded with the
  viclibc RLE scheme as one continuous stream (runs may cross track and sector
  boundaries).

| Offset | Size | Field |
|--------|------|-------|
| 0 | 8 | Magic `V9KRLE\x1a\x01` (last byte = format version) |
| 8 | 4 | Raw image size, little-endian (626688 or 1224192) |
| 12 | 4 | CRC-32 of the raw image (same polynomial as FTX) |

The header is the one addition beyond "an rle-compressed img": it lets IGC know
the sidedness before reading a single track, and lets a truncated or corrupt
file be refused before a floppy is touched. Dropping it is a one-line change on
each side if we decide against it.

On the Victor, an `.rle` is produced track by track: compress each 1 KB slice
of the track buffer into a 2 KB scratch buffer and append to the file.
Concatenated slice encodings are a valid continuous stream, because the decoder
is stateless at token boundaries. Reading an `.rle` uses a small streaming
decoder (about 30 lines) that emits exactly one track's bytes per call and
carries a partial run across calls, so files compressed in one go by Python
decode correctly.

## User interface

F-key bar becomes:

| Key | Label | Action |
|-----|-------|--------|
| F1 | Drive | unchanged |
| F2 | Mkdir | unchanged |
| F3 | View | unchanged |
| F4 | Edit | unchanged |
| F5 | Files | `(C)opy (M)ove (D)elete (R)ename` dialog, then the existing `fops_*` entry points |
| F6 | More | menu: `Read disk to image file`, `Write image file to disk` |
| F7 | Quit | unchanged |
| F8 | (blank) | freed; Rename moved under F5 |

`dlg_copy_or_move` grows into `dlg_file_op` returning 'C', 'M', 'D', 'R' or 0.
F6 opens a `dlg_menu` (a generic list picker factored out of
`dlg_drive_select`: rows, Up/Down/Enter/Esc, mouse rows, like the drive list).

**Read disk to image file**
1. Pick the source drive: A: or B: only (floppies).
2. Prompt for the file name (`dlg_input`), default `DISK.IMG`; the extension
   chooses the format (`.IMG` or `.RLE`, anything else is refused).
3. Destination is the active panel's current directory, local or serial, the
   same rule copy uses for the "other panel".
4. Read track 0, decide sidedness from the label, show the progress bar as
   `Track n/80` or `n/160`, loop over tracks. ESC between tracks aborts and
   deletes the partial file (local) or tells the server to discard it (serial).
5. Refresh the destination panel.

**Write image file to disk**
1. Source is the file under the cursor in the active panel (local or serial).
   It must end in `.IMG` or `.RLE` and have a valid size / header, or the menu
   item reports "not a Victor disk image".
2. Pick the target drive: A: or B:.
3. Read the target's track 0 (it must already be a formatted Victor disk; the
   GCR format cannot be laid down through DOS) and check its sidedness matches
   the image's. Mismatch or unreadable: refuse with a clear message.
4. Confirm: `Overwrite everything on A:?` with Yes/No.
5. Loop over tracks: fetch track (file or server), decompress if needed, INT
   26h. Per-track error dialog: `Retry / Abort`. ESC aborts.
6. INT 21h disk reset; if a panel shows that drive, re-read it.

Both operations run with a single far-heap track buffer (9728 bytes) plus, for
`.rle` files, a 2 KB scratch, allocated for the duration of the operation. On
the TINY memory tier the allocation may fail; report "Not enough memory" and
do nothing. Imaging the disk IGC itself runs from is allowed for reading and
refused for writing (compare with the drive letter of the current exe path).

## Serial protocol additions

New request bytes in the unused 0x30 block of the command space (0x10-0x1F is
FTX plus the existing SFS commands; 0x20-0x2F is reserved for FTX tests). The
model is a pull by the Victor, one track per exchange, so the Victor is never
mid-floppy-write while packets arrive: it either sits in a tight receive loop
for one track's fragments or is doing disk I/O with nothing in flight. That is
why this is a new mini-protocol instead of a START/DATA/END download: the
server's download path has no per-chunk credit flow control, and a 1 KB chunk
stream does not align with 9728-byte tracks anyway.

| Byte | Name | Direction | Payload |
|------|------|-----------|---------|
| 0x30 | `IMG_OPEN` | V->PC | `[cmd][mode][total_sectors u16][name_len][name]`; mode 0 = create image (Victor reads disk), 1 = read image (Victor writes disk). `total_sectors` is meaningful for mode 0. |
| 0x31 | `IMG_INFO` | PC->V | `[cmd][status][total_sectors u16]`. Reply to OPEN. For mode 1 the server has opened and, for `.rle`, decoded the file into memory, and reports its size; status non-zero refuses (bad size, missing file, bad header). |
| 0x32 | `IMG_GET` | V->PC | `[cmd][track u8]` (linear 0..159). Server answers with that track's `IMG_TRACK` fragments back to back. |
| 0x33 | `IMG_TRACK` | both | `[cmd][track u8][frag u8][nfrag u8][flags u8][len u16][data]`. Fragment `i` covers bytes `i*1024..` of the track. Flags bit 0 = data is RLE (set only when it saved bytes, exactly like file chunks). Max payload 1031 bytes, under `FTX_MAX_PAYLOAD`. |
| 0x34 | `IMG_CLOSE` | V->PC | `[cmd][status][crc32 u32]`. Status 0 = complete: server checks the CRC against what it received, then writes the `.img`, or RLE-compresses and writes the `.rle`, temp file plus rename. Status 1 = aborted: server drops the temp data. Reply is the existing `SFS_CMD_OK` / `FTX_CMD_ERROR`. |

Mode 0 (Victor -> PC): after sending a track's `nfrag` fragments the Victor
waits for `SFS_CMD_OK` before reading the next track. That is the per-track
checkpoint: the server confirms it holds exactly the expected bytes for that
track, and a server-side problem stops the job at the track where it happened.

Mode 1 (PC -> Victor): `IMG_INFO` carries the image size (so the Victor knows
how many tracks and can check the target disk). The Victor then alternates
`IMG_GET` / receive fragments / INT 26h. `IMG_CLOSE` with status 0 and the CRC
the Victor accumulated lets the server log a match; a mismatch is reported to
the user as a warning after the write (the floppy is already written).

Server side the image is held in memory (1.2 MB) until CLOSE, which is also
what makes `.rle` output trivial: compress the whole buffer once. Both `.IMG`
and `.RLE` names map through the existing 8.3 handling, so `MYDISK~1.RLE`
style names keep working.

## Victor-side code layout

| File | Change |
|------|--------|
| `src/diskimg.c/.h` (new) | Zone tables, `trk_first_sector(t)` / `trk_sectors(t)`, `disk_read_track` / `disk_write_track` (INT 25h/26h wrappers with the `popf` fix and error-code to message mapping), sidedness probe, the streaming RLE track decoder, the two top-level operations `diskimg_read_to_file()` / `diskimg_write_from_file()` including the dialogs, progress and abort handling. Source/destination abstraction: a small struct with `open / put_track / get_track / close` function pointers, with a local-file implementation here and the serial one in `serialfs.c`. |
| `src/serialfs.c/.h` | `serialfs_img_open / put_track / get_track / close` implementing the table above with `g_sfs_buf` and `g_ftx.pkt_buf`, mirroring `serialfs_put_named`'s slice-compress loop. |
| `src/serialfs_proto.h` | The five new command bytes, comment block kept in sync with `igcfs.py`. |
| `src/dosapi.c/.h` | `dos_disk_reset()` (AH=0Dh). The INT 25h/26h wrappers stay in `diskimg.c` because of their non-standard calling convention. |
| `src/dialog.c/.h` | `dlg_file_op` (replaces `dlg_copy_or_move`), generic `dlg_menu`, drive picker variant limited to A:/B:. |
| `src/main.c` | F5 -> `dlg_file_op` dispatch, F6 -> More menu, F8 no-op, Del key (if bound) unchanged. |
| `src/ui.c` | New F-key labels. Progress title "Reading A:" / "Writing A:". |
| `Makefile` | Add `diskimg.c`. |
| `README.md`, `AGENTS.md` | Key table, Disk images section, module table, protocol note. |

Estimated size: about 600 lines of new C, well under the remaining code budget
(the exe is ~50 KB in a compact-model near code segment; check `bin/igc.map`).

## PC side

**`tools/serialfs/v9kimage.py` (new module).** Geometry constants, the two
valid sizes, `.rle` header pack/unpack, `img_to_rle(bytes) -> bytes`,
`rle_to_img(bytes) -> bytes` (validating header, size and CRC), and
`load_image(path)` which returns raw bytes for either extension. Uses the
vendored `v9kfiletrx.compress` and `v9kfiletrx.crc32` so the codec is the one
the wire already uses.

**`tools/serialfs/v9kimg.py` (new CLI).**

```
v9kimg.py info  DISK.img|DISK.rle      # sides, sectors, size, CRC, label bytes
v9kimg.py pack  DISK.img [DISK.rle]    # img -> rle
v9kimg.py unpack DISK.rle [DISK.img]   # rle -> img
```

Lives next to `igcfs.py` so it shares the vendored packages; added to
`igcfs.spec` as a second entry point or simply shipped as a script (decide at
build time; the plain script is enough for now).

**`igcfs.py`.** Dispatch entries for 0x30/0x32/0x33/0x34, an `ImageJob` holding
mode, path, expected size, a `bytearray` and the received-track bookkeeping.
Sandboxing and 8.3 reverse mapping reuse `_resolve_dir_and_name` and
`build_dos_map`. Write uses the same temp-plus-rename as uploads.

**`selftest.py`.** Two new checks over the PTY bridge: a fake Victor pushes a
synthetic 1224-sector image track by track and the server's `.img` and `.rle`
outputs round-trip through `v9kimage`; then the fake Victor pulls the `.rle`
back track by track and the reassembled bytes match.

## Verification

1. **Unit, PC:** `v9kimg.py pack` then `unpack` reproduces a known SS and DS
   image byte for byte; `info` reports the right sidedness.
2. **Protocol, PC only:** extended `selftest.py` passes.
3. **MAME** (use `/root/sync/bldmame1/mame`, the stock build patches the ROM):
   mount a known DS image as A:, image it from IGC to the hard disk image, pull
   the file out with `vtg_image_util`, `cmp` against the source. This checks the
   logical-sector-order assumption and the INT 25h path. Then write that image
   to a freshly formatted second floppy image in B: and `cmp` the MAME floppy
   file after exit. Serial variants over MAME's PTY (`v9kserial/mame_pty.py`).
4. **Real hardware** via the remote-control rig in `TESTING.md`: SS and DS
   disks, both directions, local and serial; a write-protected target; ESC
   mid-operation; a deliberately short `.img` (must be refused).

## Open points to settle during implementation

- Does Victor DOS 3.1 INT 26h accept a 19-sector write in one call, and does
  INT 25h require the buffer not to cross a 64 KB physical boundary? If either
  bites, split the call per sector inside `disk_read_track` (the "track at a
  time" unit is kept at the file and protocol level either way). The MAME
  step above answers both before real disks are involved.
- Keep or drop the 16-byte `.rle` header (see File formats).
- Whether F8 should pick up something useful (for example "Select all") or
  stay blank for now.

## Suggested implementation order

1. `v9kimage.py` + `v9kimg.py` + their unit check (no Victor needed, defines the formats).
2. `diskimg.c` DOS layer: track tables, INT 25h/26h wrappers, sidedness probe; a
   hidden test entry (read disk to local `.img`) verified in MAME against the source image.
3. Local `.rle` write and read paths, write-to-disk flow, dialogs, F-key changes.
4. Protocol: `igcfs.py` image job + `serialfs.c` client + `selftest.py` cases.
5. Docs (README key table and Disk images section, AGENTS module table), rebuild
   `igcfs.exe` on Windows, final hardware pass.
