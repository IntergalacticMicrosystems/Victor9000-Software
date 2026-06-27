# MEM — Victor 9000 memory report

A small DOS console utility that reports conventional-memory usage on the
Victor 9000 (DOS 3.1, 8086). Plain-text output, so it can be redirected to a
file.

## Usage

```
MEM            Print the memory summary
MEM -M         Summary plus the DOS memory-control-block (MCB) chain
MEM -D         Summary plus the DOS device-driver chain
MEM -M -D      All of the above
MEM -?         Help
MEM > REPORT.TXT
```

Options accept either `-X` or `/X` and are case-insensitive.

### Example

```
Conventional memory:
  Total      : 838 KB
  Used       : 101 KB
  Free       : 736 KB
  Largest    : 736 KB
DOS version 3.10

MCB chain:
  SEG   OWNER  SIZE      TYPE/NAME
  0047  FFFF       10 KB (system)
  02C8  02CF        0 KB MEM.EXE
  02CE  02CF       78 KB MEM.EXE
  1658  0000      736 KB free
  CE5F  CE60        3 KB (program)
  CF53  0008        8 KB DOS

Device drivers:
  ADDRESS    ATTR  TYPE   NAME/UNITS
  D173:0048  8004  char   NUL
  D832:0264  C013  char   CON
  D832:02CC  C008  char   CLOCK
  D832:02E6  6800  block  3 unit(s)
```

(Example from a real Victor 9000; its conventional space extends above the
640 KB IBM-PC line, hence ~838 KB total. The Victor loads DOS and resident
code *high*, so the free block sits below them in the chain.)

## How it works

All figures come from DOS via `INT 21h` only — never the BIOS, because the
Victor 9000 BIOS is not IBM-compatible (so `INT 12h` is intentionally unused,
matching the approach in `../igc/src/mem.c`):

- `AH=30h` — DOS version
- `AH=48h, BX=FFFFh` — largest free block (the allocation fails and DOS
  reports the size)
- `AH=52h` — list-of-lists, from which the first MCB segment is read
  (`ES:[BX-2]`); the MCB chain is then walked block by block. The same
  list-of-lists holds the head of the device-driver chain (`-D`).

**Program names on DOS 3.1:** the MCB owner-name field (8 bytes at MCB offset
8) only exists on DOS 4+. To get real names on the Victor's DOS 3.1, `MEM`
instead reads each block's owner PSP: `PSP:[2Ch]` is the environment segment,
and DOS 3.0+ stores the program's full path right after the environment
strings. The 8.3 basename is shown (e.g. `MEM.EXE`). Blocks whose owner has no
recoverable path (some resident drivers, DOS system data) fall back to
`DOS` / `(system)` / `(program)`.

**Device chain (`-D`):** walks from the NUL device embedded in the
list-of-lists (offset `0x22` for DOS 3.1+) following each header's next-driver
pointer. Attribute bit 15 selects character vs. block device; character
devices carry an 8-char name, block devices a unit count.

## Build

Open Watcom v2 cross-compiler on Linux (same toolchain as `../igc`).

```
make           # build bin/mem.exe (+ bin/mem.map)
make clean
make deploy    # copy into the MAME disk image (set MAME_DIR=<path>)
```

Compiler flags match igc: `-0 -mc -os -bt=dos` (8086, compact model, size).
Unlike igc this tool links the Watcom 16-bit C library for `printf`.

## Source

| File        | Purpose                                              |
|-------------|------------------------------------------------------|
| `main.c`    | Argument parsing, report formatting/printing         |
| `dosmem.c/h`| DOS memory queries (INT 21h) and MCB-chain walk      |
