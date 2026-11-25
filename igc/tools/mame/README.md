# igc in MAME

End-to-end tests of igc in MAME's `victor9k`, with the real `igcfs.py` on
Serial A and, optionally, a scripted Microsoft serial mouse on Serial B.

```sh
python3 test_igc.py                       # all tests, work in /srv/prime/work/v9kmame-tests
python3 test_igc.py /path/work view mouse # chosen tests, other work dir
python3 v9kmame.py                        # smoke test: boot, start igc, open F1
```

A test takes 20-60 s: MAME runs headless and unthrottled (about 13x real time).

## Needs

| What | Default | Override |
|---|---|---|
| MAME with the `victor9k` driver (0.285 used) | `/root/tools/bin/mame` | `V9K_MAME` |
| `victor9k` ROM set (7 files) | `/root/tools/mame-victor/roms` | `V9K_ROMPATH` |
| Bootable DOS 3.1 floppy with `CTMOUSEV.EXE` | `/root/tools/mame-victor/V9K31.orig.IMG` | `V9K_BOOT_IMG` |
| `vtg_image_util` checkout | `/root/tools/vtg_image_util` | `V9K_VTG` |
| ctmouse_v9k `tools/` (mouse feeder, mouse tests only) | `/srv/prime/repos/ctmouse_v9k/tools` | `V9K_MOUSE_TOOLS` |
| igc.exe to test | `igc/bin/igc.exe` | `V9K_IGC_EXE` |

The boot image is never written: every run boots a copy.

## How a run is set up

- A: is a copy of the boot floppy. B: is a blank single-sided floppy from
  `vtg_image_util create -t victor-ss` (needs vtg_image_util 5620839 or
  later: older versions write a label Victor DOS cannot read), plus IGC.EXE
  and the test's files.
- MAME gets `-ramsize 896K` (the 128K default fails with "Configuration too
  large for memory") and `-rs232a pty`. Baud and data bits go in the
  machine's cfg file before launch: changing them while MAME runs makes it
  reopen the pty, and igcfs then fails with EIO.
- igcfs serves `<work>/<test>/root` on Serial A at 38400. With a mouse,
  Serial B runs at 1200 7N1 and `CTMOUSEV /SB` is loaded before igc.
- igc runs from B:, so its temp files (`IGCVIEW.TMP`) land there.

## Driver (`v9kmame.py`, `v9k.lua`)

`v9k.lua` is MAME's autoboot script; `v9kmame.py` talks to it through
`cmd.N` / `ack.N` files in `<work>/ctl`.

- `post(text)` types through MAME's natural keyboard (`\r` = Return). It
  sometimes drops a doubled letter or keeps Shift after a capital, so type
  commands in lower case.
- `press(key)` holds a key by its MAME field name or an alias (`F1`..`F10`,
  `UP`, `DOWN`, `LEFT`, `RIGHT`, `HOME`, `PGUP`, `PGDN`, `TAB`, `ENTER`,
  `SPACE`, `ESC`, ...); all names are in `<work>/ctl/keys.txt`.
- **Esc is Alt + the RVS key** (its front legend). MAME's natural keyboard
  maps chr(27) to the bare key, which Victor DOS does not read as Esc, so
  `press("ESC")` sends the chord.
- `screen()` decodes the 80x25 text screen from VRAM at F000:0, starting at
  the CRTC display start (DOS scrolls by moving it; igc resets it to 0).
  Glyph = character + 64h; box-drawing cells show as `#`.
- `wait_text`, `wait_log` (igcfs's log), `snap` (PNG), `get_b` / `put_b`
  (B: files, only while MAME is stopped).
- Mouse: `load_mouse()`, `point(col, row)`, `click(col, row)`. Moves are
  sent as steps of at most 4 mickeys, which CTMOUSEV's acceleration leaves
  alone, so with its 1:1 ratio the pointer lands on the cell exactly.

## Gotchas

- Quit igc (F7, `y`) before checking B:: stopping MAME right after a copy can
  catch DOS between deleting the old file and renaming the new one.
- igcfs marks a file read-only when it cannot write it. As root it can write
  a 0444 file, so the read-only test sets `chattr +i` (and clears it after).
- MAME ignores SIGTERM in this mode; `stop()` asks it to exit, then kills it.
- Not covered: real serial timing (MAME is not cycle-exact for the 7201
  FIFO), the midnight wrap of the double-click timer.
