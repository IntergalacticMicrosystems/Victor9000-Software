# MEM Development Guide

Console memory-report utility for the Victor 9000 (DOS 3.1, 8086), Open Watcom v2.

This file is for LLM use; README.md is for humans. Keep both updated.

## Build

```bash
make            # bin/mem.exe (+ bin/mem.map)
make clean
make deploy     # copy into MAME disk image (set MAME_DIR=<path>)
```

Flags mirror igc (`-0 -mc -s -os -bt=dos -w4`). Difference from igc: this tool
links the Watcom C library (uses `printf`); igc does direct VRAM/DOS only.

## Architecture

All data is from DOS `INT 21h`, never the BIOS — the Victor BIOS is not
IBM-compatible, so `INT 12h` is intentionally avoided (same stance as
`../igc/src/mem.c`, which uses only `AH=48h`).

| File         | Purpose                                                |
|--------------|--------------------------------------------------------|
| `main.c`     | Arg parse (`-M`, `-D`, `-?`), formatting, printing      |
| `dosmem.c/h` | INT 21h queries + MCB/device walks, PSP name resolution |

### DOS calls used
- `AH=30h` — version
- `AH=48h, BX=FFFFh` — largest free block (failed alloc returns size in BX)
- `AH=52h` — list-of-lists; first MCB segment = word at `ES:[BX-2]`; also the
  head of the device-driver chain (`dosmem_sysvars` returns the full `seg:off`)

### MCB walk (`dosmem_walk`)
Far-peek each MCB header (no packed struct needed):
- off 0: marker `'M'` (0x4D, more) / `'Z'` (0x5A, last) — else chain corrupt
- off 1: owner PSP (word); 0 = free, 8 = DOS
- off 3: size in paragraphs (word)
- off 8: 8-char owner name — **DOS 4+ only**; garbage on 3.1
- next MCB = `seg + 1 + size`

Totals count each block's 1-paragraph MCB header. Memory below the first MCB
(IVT + DOS + BIOS data) is added to the total as `first_mcb` paragraphs.

### Program names (`dosmem_psp_name`)
The MCB owner-name field is DOS 4+ only, so on 3.1 we read the name from the
owner PSP's environment: verify `PSP:[0]==CD20`, read env seg at `PSP:[2Ch]`,
skip env strings to the empty-string terminator, then a word count, then the
ASCIIZ program path; show its basename. `owner_label()` order: PSP env name →
MCB name (DOS 4+) → `(system)`/`(program)` heuristic. Verified on hardware:
`MEM.EXE` resolved on DOS 3.10.

### Device chain (`dosmem_walk_devices`)
NUL device offset within SysVars is version-dependent (2.x `0x17`, 3.0 `0x28`,
3.1+ `0x22`). Header: `[0]`/`[2]` next off/seg (`0xFFFF` off = end), `[4]` attr
(bit 15 = char), `[0Ah]` 8-char name (char) or unit count (block). Verified on
hardware: NUL/CON/LST/PRN/AUX/CLOCK + a 3-unit block driver.

## Gotchas
- MCB/PSP/device access must be far (`MK_FP`) — they live in arbitrary segments.
- `para_to_kb` rounds to nearest KB; paragraphs are 16 bytes.
- `-M` lists up to `MAX_BLOCKS` (128); extra blocks still count in totals.
- **Big arrays go in BSS, not the stack.** `blocks[128]` + `devices[64]` are
  `static` locals in `main()`; as plain autos they overran the linker stack
  and silently smashed memory (`-s` disables the overflow check). Stack is 8 KB.
```
