# MOUSE.DEV: Victor 9000 serial mouse driver (mouse)

This directory is a copy of `reverse8086/platforms/victor9000/mouse_dev` (the reverse-engineering
repo), kept here because `igc/` depends on the driver's layout (`igc/src/mouse.c` reads its
`port_index` word at a fixed offset past the INT 33h entry). `../RAT/` and `../picasso/` below
refer to sibling folders in that repo, not to this one.

Always update this file and `MOUSE_DRIVER.md` when you change `MOUSE.asm` or `MOUSE_COM.asm`.

**Status (2026-10-09): byte-exact and complete.** `MOUSE.asm` reassembles to `MOUSE.DEV`. Every
routine, variable and hardware address has a name. Every routine has a header and the interesting
lines have comments. `MOUSE_COM.asm` builds the same resident code as `MOUSE.COM`, a TSR that
runs from the command line instead of CONFIG.SYS, shows a usage screen without a port letter and
refuses to stay resident when no mouse answers (section 11 of `MOUSE_DRIVER.md`).
`make test-dosbox` exercises its install path (needs `dosbox` and `nasm`).

## Rebuild / verify

```bash
make verify          # nasm -f bin MOUSE.asm -o MOUSE_rebuilt.bin && cmp -> "byte-exact: 3294 bytes"
                     # also builds MOUSE.COM and prints its size (4036 bytes)
make MOUSE.COM       # nasm -f bin -I./ MOUSE_COM.asm -o MOUSE.COM
make test-dosbox     # headless DOSBox run of the installer (dosbox_test/run.sh), 8 checks
make clean
```

`MOUSE.asm` has three `%ifdef`/`%ifndef BUILD_COM` blocks (org, device header + strategy/interrupt,
`device_init`). Without the define it is the byte-exact .DEV. `MOUSE_COM.asm` defines `BUILD_COM`,
includes it and appends the installer, so the resident code is shared and the .COM's resident
offsets are the .DEV offsets + 100h. Check that after any change:

```bash
sed 's/^        org     0$/        org     0x100/' MOUSE.asm > /tmp/dev100.asm
nasm -f bin -I./ /tmp/dev100.asm -o /tmp/dev100.bin
cmp <(dd if=/tmp/dev100.bin bs=1 skip=83 count=3021) <(dd if=MOUSE.COM bs=1 skip=83 count=3021)
```

The only tool needed is `nasm` (2.16 was used). jwasm is not needed.

## Read first

1. `MOUSE_DRIVER.md`, the human-readable write-up. It covers the DOS protocol, the hardware init,
   the packet protocol, the INT 33h and INT FBh APIs, cursor drawing, the variable map and the
   bugs.
2. `MOUSE.asm`, the annotated source. Its file header has the memory map.

## Files

| File | What |
|---|---|
| `MOUSE.DEV` | The original binary, 3,294 bytes, MD5 `c85874432617f3b6c2f0a277ab24f12a`. Copied from the RAT mouse disk folder `../RAT/`, which also holds `MOUSE.DOC` (wiring notes) and `PICASSO.EXE` (the paint program it shipped with). Do not modify anything in `../RAT/`. |
| `MOUSE.asm` | Annotated NASM source (`org 0`, `cpu 8086`); with `BUILD_COM` defined it supplies the resident code for the .COM |
| `MOUSE_COM.asm` | `MOUSE.COM` build: `%define BUILD_COM`, `%include "MOUSE.asm"`, then the TSR installer `com_install` |
| `MOUSE.COM` | Built TSR, 4,036 bytes on disk (resident part 0C20h + PSP, 3,360 bytes). Not an original file. Install path tested in DOSBox; not yet tried on a Victor or in MAME. |
| `dosbox_test/` | `run.sh` + `harness.asm`: a DOS program that EXECs MOUSE.COM four times under headless DOSBox (no letter, no mouse, simulated mouse, already installed) and checks the messages, return codes and the INT 33h/FBh/41h vectors (`make test-dosbox`) |
| `MOUSE_raw.txt` | Raw `ndisasm -b16` listing, with sync points at 0016/008A/0ADB/0C20/0C9B so the header and data do not desynchronise the listing |
| `MOUSE_DRIVER.md` | Driver write-up |
| `Makefile` | `all`, `verify`, `MOUSE.COM`, `clean` |

## Encoding notes

The original was assembled with MASM. NASM differs from it in two ways:
- **reg,reg forms.** MASM uses the "reg ← r/m" opcodes (8B/03/0B/23/2B/33/3B/8A/0A/22). The local
  macro `rr op, dst, src` emits them, for example `rr mov, bp,sp` is `8B EC`.
- **Full imm16 forms.** `cmp ax,0` is `3D 00 00`, and `add ax,` / `and ax,` / `and bx,` with
  small values use the long form. These are written `strict word`.

All other instructions are plain NASM. MASM's padding `nop` after forward short jumps is kept
as literal `nop` lines. The patched `CALL FAR` in the ISR is `db 0x9A` + two `dw 0`, named
`user_handler_ofs` and `user_handler_seg` with `equ`.

## Corrections to the first-pass notes (code wins)

- The device name is `MOUSE` followed by three NULs, not blanks.
- `bitmap_seg` = `(R12 bit 4 ? 1000h : 0) + 2*(F000:0000 & 7FFh)`. It does **not** add F000h:
  font cells live in system RAM at segment 0000h/1000h (MAME: `ab = (dot_addr<<15) | (dc&0x7ff)<<4`).
- The graphics test in the drawing code is `gran_y == 1`. `text_cursor_type` (0BB0) selects a
  software (0) or hardware text cursor, and reset sets it to **1 (hardware)**.
- fn 0 runs `init_serial_hw` only when the **caller's** BX is 0. The routine falls into
  `reset_driver` with BX unchanged.
- fn 10 with BX≠0 sets R10/R11 (cursor start/end raster). The 6845 is reached as E000:8000/8001,
  which is the same as E800:0000/0001.
- `save_buffer` holds 64 bytes, 0BC3-0C02. 0C14-0C1F is never referenced.
- INT FBh clears the raw counters on every call, whatever CX is.
- The INIT status word is 0124h, because AL = `'$'` is left over from INT 21h AH=09h.
- The text cursor XOR default is 7700h, which toggles attributes 12-14 **and** font-cell bits 8-10.
  It is not a plain attribute flip.

## Open questions

- The meaning of uPD7201 WR2A = 14h bit 2 (priority) and bits 3-5 (interrupt mode; DOS writes
  10h) comes from memory of the uPD7201 data sheet and has not been checked against a copy.
- PICASSO.EXE has since been disassembled (`../picasso/`): it uses INT 33h only (functions 0, 1,
  2, 3, 7, 8, 9) after checking the `'MOUSE'` signature before the INT 33h entry, never INT FBh,
  and its bitmap is exactly the column-major layout `draw_graphics_cursor` assumes
  (pixel (x,y) at (x/16)*320h + 2*y, bit x mod 16, bit 0 leftmost; PDUMP.COM reads it the same
  way). The INT FBh client is still unknown.
- The left-edge clipping bug, the |dX| = 2 acceleration bug and the stale hardware cursor were
  found by static reading only. They have not been reproduced in MAME.
- MOUSE.DOC says the default port is serial A, but the code default is B. The binary may be a
  later build than the document.
