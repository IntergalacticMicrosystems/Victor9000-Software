# Victor 9000 RAM Test ROM

## ~ Initial Version, haven't tested on hardware yet ~

A bare-metal 4K diagnostic ROM for the Victor 9000 (8088). It replaces the
**FF000 system ROM chip** and does nothing but continuously test main DRAM,
showing live results on the built-in CRT. No DOS, no BIOS, no interrupts — the
ROM owns the machine from reset.

Drop in the EPROM (or swap the ROM in MAME), power on, and the screen shows a
burn-in RAM test with a pass counter and the exact address/bit of any failure.

## Display

```
VICTOR 9000 RAM TEST            (reverse-video title bar)
MEMORY TOP SEG: xxxx            first absent 64K segment (top of RAM)
PASS: xxxxxxxx     ERRORS: xxxx pass counter (32-bit) and error counter
TESTING SEG: xxxx  |           segment under test + a spinner ( | / - \ )
FAIL  SEG: xxxx  OFF: xxxx      latched failure (reverse video) ...
EXP: xxxx  GOT: xxxx  BIT: xxxx expected / actual / failing-bit mask
```

All numbers are hexadecimal. `BIT` is `EXP XOR GOT` — the data lines that failed.

## What it tests

Per 64K segment, in order:

1. **Pattern sweep** — fill + verify with `0000, FFFF, 5555, AAAA, 55AA, AA55`
   (catches stuck/shorted data bits and decay).
2. **Address-in-address** — write each word's own offset, then verify (catches
   stuck/shorted address lines and aliasing).
3. **March C-** — `{↑(w0); ↑(r0,w1); ↑(r1,w0); ↓(r0,w1); ↓(r1,w0); ↓(r0)}`,
   reading-then-writing each cell in ascending and descending address order
   (catches **transition faults**, **address-decoder faults** and **coupling
   faults** the fill-then-verify sweep cannot — a write everywhere followed by a
   read everywhere can't see a cell that won't flip or one disturbed by a
   neighbor in a specific order).

Then, **once every `RETENTION_EVERY` passes** (default 4), a **DRAM data-
retention test** runs over all high RAM: write a background (`AAAA`/`5555`,
alternating), **hold for ~1s without touching DRAM**, then verify. With the CPU
not refreshing via access, this catches **refresh failures** (cells decay in
milliseconds) and **leaky/marginal cells**. A `RETENTION: xxxx` status shows on
row 9 during the hold; the spinner keeps moving so a stall is still visible.
Tune cadence/hold with `RETENTION_EVERY` / `RETENTION_OUTER` near the top of
`ram_test.asm`. (Retention covers high RAM only; the low 64K holds the
font/stack/vars.)

The burn-in loop repeats forever, incrementing the pass counter. Failures are
**latched and displayed**, the error counter increments, and testing continues
(so intermittent/thermal faults accumulate).

### Coverage

- **Phase H** tests all high RAM: segment `0x1000` up to the sized top, below
  the I/O space at `0xE000`.
- **Phase L** (when `FULL_COVERAGE=1`, the default) tests the **low 64K** too.
  The downloadable font, the stack, and the variable block all normally live in
  segment 0, so phase L relocates the stack+variables into verified high RAM and
  uses two font homes (`0x0400` and `0x8000`): it tests everything except the
  active font window, then moves the font (redrawing the screen) and tests the
  window it vacated. Net result: every DRAM byte in `0x00000–0xDFFFF` is tested.

Never tested (not DRAM): `0xE0000–0xEFFFF` (I/O), `0xF0000–0xFFFFF` (video RAM +
ROM). Video RAM at `0xF0000` is exercised implicitly by drawing. Phase L also
skips the first 16 bytes of RAM (`0:0000–0:000F`, CPU exception vectors 0–3) so
that the NMI vector at `0:0008` — which we point at a ROM `IRET` — stays valid
while the rest of the IVT region is overwritten by the test.

A full thorough pass tests every word of all RAM and takes ~10-30s of emulated
8088 time, so `PASS` ticks slowly; the **spinner** next to `TESTING SEG` is
advanced from the test inner loop (once per pattern) so it spins at a few Hz —
independent of RAM size — and freezes if the test ever stalls. (During phase L
the font relocates between its two homes; on-screen cells are re-based in place,
so there is no screen flicker.) For a quick smoke test set
`%define FAST 1` at the top of `ram_test.asm` — only the first 1KB of each region
is tested and `PASS` ticks several times per second.

To fall back to the proven high-RAM-only tester, set `%define FULL_COVERAGE 0`
at the top of `ram_test.asm`.

> **Limitation:** if the machine has only 64K total (memtop == `0x1000`), there
> is no verified high RAM to host the relocated stack/vars, so phase L is
> skipped. The Victor 9000 ships with ≥128K, so this is a corner case.

## Building

```sh
./build.sh
```

Produces `ram_test.rom` (exactly 4096 bytes). Requires `nasm`. The build runs
`nasm -f bin -O0 ram_test.asm -o ram_test.rom` and checks the image is exactly
4096 bytes.

The font is extracted from the stock Victor 9000 character set
(`V9K_NORMAL_FONT.bin`) by `extractfont.py` into `font.inc` (ASCII-indexed,
glyph index = ASCII − 0x20). Regenerate with `python3 extractfont.py`. The
older `genfont.py` (hand-drawn 5×7 glyphs) is kept for reference.

### ROM image layout

`org 0`, 4096 bytes loaded at FF000:

| Offset | Contents |
|---|---|
| `0x020` | font data (low ROM, so MAME's `0x1AB` patch hits an unused glyph) |
| `0x820` | `cold_start` and all code + strings/tables |
| `0xD51` | (must stay padding — MAME's checksum-NOP patch lands here) |
| `0xFF0` | reset vector: `jmp 0xFF00:cold_start` (CPU fetches here from FFFF0) |
| `0xFFA` | version word (`F3F7`) |
| `0xFFE` | PC-compat flag (`FFFF`) |

## Running in MAME (verified)

The ROM is verified end-to-end on both stock and non-patching MAME: full-coverage
burn-in runs stably (pass counter climbs, zero errors), and injected stuck-RAM
faults are reported with the correct segment / offset / expected / got /
failing-bit.

> ### Stock MAME patches the victor9k ROM — and this ROM dodges it
> Stock MAME's `victor9k` driver overwrites two spots in the FF000 (`.8j`) chip
> after load (from `victor9k.cpp`): `0x1AB`→`0xC3` ("patch out SCP self test")
> and `0xD51-0xD54`→`0x90` ("patch out ROM checksum error"). These don't exist on
> real hardware. This ROM is laid out so both land in **harmless bytes**: the font
> occupies low ROM so `0x1AB` falls inside the unused `,` glyph (never displayed),
> and `0xD51-0xD54` falls in tail padding. So it runs correctly on stock MAME *and*
> on real hardware. (Build-time `%if`/`TIMES` guards in `ram_test.asm` fail the
> assembly if a future edit moves either patch point onto live code.)

The Victor 9000 BIOS is **two separate 4K chips**. In MAME's `victor9k` romset:

- `v9000 univ. fe f3f7 13db.7j` — the **FE000** chip (leave as-is)
- `v9000 univ. ff f3f7 39fe.8j` — the **FF000** chip → **replace with our image**

Use `install-to-mame.sh` to build a patched `victor9k` romset (it replaces the
`.8j` member with our image and writes `victor9k.zip` into `./mame-roms`, which
you put first on the rompath). It needs the original romset present:

```sh
VICTOR9K_ZIP=/path/to/victor9k.zip ./install-to-mame.sh
# then: mame victor9k -rompath "./mame-roms;<original rompath>"   (stock MAME is fine)
```

For headless verification, run with a time limit and read the screen from a
snapshot, or drive the CPU via a Lua `-autoboot_script` (the maincpu device tag
is `:8l`). Example stuck-RAM fault injection that the ROM correctly reports:

```lua
-- force physical 0x15000 to read back 0xDEAD -> FAIL SEG:1000 OFF:5000
manager.machine.devices[":8l"].spaces["program"]
  :install_read_tap(0x15000, 0x15001, "stuck", function() return 0xDEAD end)
```

## Known risks (real hardware)

- **DRAM refresh:** the stock ROM contains no explicit refresh setup; the
  mechanism (dedicated circuit / timer) is unconfirmed. The test loops are
  continuously memory-bound, so they self-refresh in practice, but confirm the
  refresh mechanism before trusting results on real hardware, and replicate any
  required timer/6522 init in `cold_start` if needed.
- **Font addressing:** phase L assumes the character generator is read from
  segment 0 (`glyph << 5`), constraining font homes to the low 64K. This matches
  the stock ROM (`dot_ram` at `0:0400`); verify `0:8000` works as a font home on
  real hardware.

## Files

| File | Purpose |
|---|---|
| `ram_test.asm` | the ROM source (entry, display, test engine, burn-in, relocation) |
| `font.inc` | generated ASCII-indexed font (do not hand-edit) |
| `extractfont.py` | extracts `font.inc` glyphs from `V9K_NORMAL_FONT.bin` |
| `genfont.py` | legacy font generator (hand-drawn 5×7 glyphs) |
| `V9K_NORMAL_FONT.bin` | stock Victor 9000 NORMAL character set (.CHR) |
| `build.sh` | assemble + size-check |
| `install-to-mame.sh` | build a patched `victor9k` romset for MAME |
