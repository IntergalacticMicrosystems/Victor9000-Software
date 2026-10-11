# MOUSE.DEV: Victor 9000 serial mouse driver

`MOUSE.DEV` is a 3,294-byte MS-DOS 3.1 character-device driver for the Victor 9000 / Sirius 1.
It drives a Microsoft-protocol serial mouse on serial port A or B and gives programs a
Microsoft-style INT 33h API, plus a small private INT FBh API. It came on the RAT mouse disk
(`../RAT/`) with `MOUSE.DOC` (wiring notes) and `PICASSO.EXE`, a paint program. Section 11
describes `MOUSE.COM`, a rebuild of the same code as a TSR that does not need CONFIG.SYS.
The sign-on reads `Device MOUSE v1.0, (C) Issue 1990 installed for serial port A.`

Every statement here was checked against the code in `MOUSE.asm`. Labels in `code font` are
the names used in that file. Offsets are hexadecimal driver offsets.

## 1. Image layout

| Offset | Contents |
|---|---|
| 0000-0011 | DOS device header |
| 0012-0015 | `request_ptr`, the request header saved by the strategy routine |
| 0016-0052 | `strategy`, `interrupt`, `request_done` |
| 0053-0084 | `int_fb_entry`, the private API |
| 0085-0089 | `'MOUSE'` signature, just before the INT 33h entry |
| 008A-0389 | `int33_entry`, `int33_dispatch` and functions 0-16 |
| 038A-05C9 | `serial_isr` (packet decoder), `accelerate` |
| 05CA-0891 | cursor draw/erase, text cell address, clamping, mode detection, `init_serial_hw` |
| 0892-08BF | `serial_poll_rx`, `serial_eoi` |
| 08C0-0ADA | `draw_graphics_cursor`, `restore_graphics_background` |
| 0ADB-0C1F | resident variables (section 9) |
| 0C20-0CDD | `device_init` and the sign-on strings. DOS discards this part because the break address is 0C20h. |

## 2. Device header and DOS protocol

| Offset | Value | Meaning |
|---|---|---|
| 0000 | FFFF:FFFF | next-driver link |
| 0004 | 8000h | attribute: character device, no other bits |
| 0006 | 0016h | strategy entry |
| 0008 | 0021h | interrupt entry |
| 000A | `MOUSE`,0,0,0 | device name. It is padded with NULs, not blanks. |
| 0012 | dword | `request_ptr` |

- **Strategy** (`0016`) stores ES:BX in `request_ptr` and does a far return.
- **Interrupt** (`0021`) saves all registers and reads command byte `[rq+2]`.
  - Command 0 (INIT) jumps to `device_init`.
  - Every other command gets status **8103h** (error, done, code 3 "unknown command"). The driver
    does nothing through the DOS file interface. Programs reach it only through INT 33h and INT FBh.
- `request_done` (`003F`) sets AH = 01 (done), writes AX to `[rq+3]` and returns. AL is not
  cleared, so INIT returns status **0124h**: AL still holds `'$'` from the last INT 21h AH=09h
  call. This is harmless because the error bit is clear.

### INIT (`device_init`, 0C20)

1. `lds si,[rq+12h]` loads a pointer to the CONFIG.SYS text after `device=`. With the shipped
   CONFIG.SYS this text is `mouse.dev a`.
2. The routine scans that text up to CR or LF. The first `a`/`A` sets `port_index` = 0 (serial A).
   The first `b`/`B` sets it to 1 (serial B).
   - The scan covers the **whole line, file name included**. `mouse.dev` happens to contain
     neither letter. A path such as `b:mouse.dev a` would select port B.
   - With no letter, the built-in default in `port_index` applies: **1 = port B**. MOUSE.DOC says
     the default is serial A. The shipped CONFIG.SYS passes `a`, so the mismatch never shows.
3. It calls `reset_driver` with BX = 0, which includes the hardware initialisation (section 4).
4. It sets INT 33h to `int33_entry` and INT FBh to `int_fb_entry` with INT 21h AH=25h.
   INT 41h was already hooked inside `init_serial_hw`.
5. It prints the sign-on (AH=09h), the letter `'A'+port_index` (AH=02h) and `".\r\n"`.
6. It sets the break address `[rq+0Eh]` to CS:0C20h and exits through `request_done`.

## 3. Hardware used

All Victor I/O is memory-mapped. The driver reaches every device with ES = E000h. The sources are
MAME `docs/vic-src/mame-v9k/victor9k.cpp` and DOS 3.1 `docs/vic-src/dos31/SYSPAR.PLM`.

| Address | Device | Use |
|---|---|---|
| E000:0000 | 8259A, A0=0 | OCW2 = 61h, a specific EOI for IR1 |
| E000:0001 | 8259A, A0=1 | IMR. The driver ANDs it with FDh to unmask IR1. |
| E000:0020 / 0021 | 8253 counter 0 / 1 | baud clocks of 7201 channel A / B (victor9k.cpp lines 786-792) |
| E000:0023 | 8253 control | mode word |
| E000:0040 / 0041 | uPD7201 data A / B | `cd_ba` mapping (victor9k.cpp line 235) |
| E000:0042 / 0043 | uPD7201 control A / B | `port_index` is added to select the channel |
| E000:8000 / 8001 | HD46505 CRTC address / data | the same location as E800:0000 |
| F000:0000 | screen RAM | 2048 words. Bits 10-0 are the font-cell number, bits 15-11 the attributes. |

The 8259 interrupt vectors start at 40h. IR1 is "COMM", the 7201 (victor9k.cpp lines 370-382), so
the receive interrupt is **INT 41h**.

## 4. Hardware initialisation (`init_serial_hw`, 07F3)

This routine runs at INIT, and also on INT 33h AX=0 when the caller's BX is 0 (see section 6).
The 7201 registers are written as a pointer byte followed by a value byte, on the control port of
the selected channel.

| Step | Write | Meaning |
|---|---|---|
| 1 | WR0 = 18h | command 3: channel reset |
| 2 | INT 21h AH=25h, AL=41h | INT 41h set to `serial_isr`. The old vector is **not saved or chained**. |
| 3 | 8259 OCW2 = 61h; IMR &= FDh | clear a pending IR1 and enable IR1 |
| 4 | 8253 control 36h (port A) or 76h (port B) | counter 0 or 1, LSB then MSB, mode 3 (square wave), binary |
| 5 | count = 0041h = 65 | 1.25 MHz / 65 = 19,231 Hz. With the x16 clock mode this is **1,202 baud**. |
| 6 | WR2 (channel A) = 14h | Bits 1:0 = 00: both channels are interrupt-driven, no DMA. Bit 4 is the same as DOS, which writes 10h. Bit 2 is the interrupt-priority select. The bit meanings are from the uPD7201 data sheet and were not checked against a copy here. |
| 7 | WR4 = 44h | x16 clock (bits 7:6 = 01), 1 stop bit (bits 3:2 = 01), no parity |
| 8 | WR3 = 41h | Rx 7 bits (bits 7:6 = 01) and Rx enable. Auto-enables (bit 5) are off, so the receiver does not need DCD. |
| 9 | WR5 = E8h | DTR on, Tx 8 bits, Tx enable, **RTS off** |
| 10 | WR0 = 10h | command 2: reset external/status interrupts |
| 11 | read the data register 65,535 times | drains the receiver and gives a delay with the mouse unpowered |
| 12 | WR5 = EAh | same as step 9 but with **RTS on** |
| 13 | WR1 = 10h | Rx interrupt on every character (bits 4:3 = 10). Tx and external interrupts are off. |

The result is **1200 baud, 7 data bits, no parity, 1 stop bit**, the Microsoft serial mouse format.
The DOS 3.1 defaults in SYSPAR.PLM use the same count (41h) but CR4=47h, CR3=61h and CR5=AAh, which
is 7E1 with auto-enables. The driver reprograms the port to 7N1.

The mouse is powered from the port. MOUSE.DOC wires only TD, RD, RTS and ground, with RTS on
Sirius pin 4. Holding RTS low through the delay and then raising it power-cycles the mouse. A
Microsoft mouse then sends `'M'` (4Dh). Bit 6 of that byte is set, so the decoder takes it as a
sync byte and resynchronises on the first real packet.

`serial_eoi` writes WR0A = 38h (command 7, "end of interrupt", which is only valid on channel A)
and then the 8259 specific EOI 61h.

## 5. Packet decoding (`serial_isr`, 038A)

```
byte 1   1  L  R  Y7 Y6 X7 X6      (bit 6 = sync)
byte 2   0  X5 X4 X3 X2 X1 X0
byte 3   0  Y5 Y4 Y3 Y2 Y1 Y0      X, Y: 8-bit two's complement, +X right, +Y down
```

1. The ISR reads RR0 bit 0 (Rx character available). If no byte is waiting, it only sends the EOI.
2. **Sync byte** (bit 6 set):
   - Clear `event_flags`.
   - Merge L (bit 5) into `button_state` bit 0 and R (bit 4) into bit 1.
   - For each button, compare the new state word with the old one. "New > old" means pressed.
   - On a change, increment the count of the matching record and store the current
     `cursor_x`/`cursor_y` in it. This is the position before this packet's motion.
   - Set the event bit: 02h left pressed, 04h left released, 08h right pressed, 10h right released.
   - Save Y7:6 in `pkt_y` bits 7:6 and X7:6 in `pkt_x` bits 7:6. Set `pkt_state` = 1.
3. **Byte 2** (`pkt_state` = 1): OR the low 6 bits into `pkt_x` and set `pkt_state` = 2.
   A byte without the sync bit while `pkt_state` = 0 is dropped.
4. **Byte 3**: complete Y, sign-extend X and Y with CBW, set `pkt_state` = 0. If both deltas are
   0, skip to the event handling. Otherwise:
   - `raw_x += dX`, `raw_y += dY`. These are unaccelerated mickeys.
   - **Acceleration** (`accelerate`): if |dX| > 2, double both dX and dY. Otherwise, if |dX| < 2
     and |dY| > 2, double both.
   - **Scaling**, per axis with a non-zero delta:
     `q, r = (delta*8 + frac) / mickey_ratio` (signed IDIV). `frac = r` carries the remainder
     to the next packet. Then `virt += q`.
   - Clamp `virt` to [min-16, max+16]. Clamp a copy to [min, max] and round it down to the
     granularity (10/16 in text mode, 1 in graphics mode). The result is `cursor_x`/`cursor_y`.
   - If the new cursor position differs from the drawn position (`drawn_x`/`drawn_y`) on an axis
     that moved, set event bit 01h.
5. Send the EOI. Then, unless the ISR has re-entered (`isr_not_busy` = 0), mark busy and continue:
   - If `event_flags & user_mask` is non-zero, enable interrupts and far-call the fn 12 handler with
     AX = events, BX = buttons, CX/DX = cursor position and SI/DI = `raw_x`/`raw_y`. DS = driver CS.
   - If the cursor moved and is visible (`cursor_level` = 0), call `draw_cursor`.
   - Clear the busy flag.

   Interrupts are enabled during this tail, so a nested packet still updates the position but does
   not call the handler or redraw.

## 6. INT 33h API (`int33_entry` 008A, `int33_dispatch` 008F)

`int33_entry` is `sti` / `call int33_dispatch` / `iret`. The dispatcher pushes
AX BX CX DX DI SI BP DS ES, sets BP = SP and DS = CS, and applies **CBW to AL**, so AH is ignored.
An if-chain on AX selects the function. Values above 16 are ignored; this includes AL = 80h-FFh,
which CBW makes negative and the unsigned `ja` rejects. Handlers return results by writing the
saved registers: AX at [bp+10h], BX at [bp+0Eh], CX at [bp+0Ch], DX at [bp+0Ah].

| AX | Routine | Inputs → outputs | Deviation from the Microsoft API |
|---|---|---|---|
| 0 | `fn00_reset` → `reset_driver` | → AX = FFFFh, BX = 2 | If the caller's BX = 0, the serial hardware is re-initialised (section 4), including INT 21h from inside INT 33h, the delay and a mouse power cycle. Reset re-detects text/graphics mode. Defaults: hardware text cursor, ratio 8/8, range 0-799 × 0-399, user mask 0, counters cleared, arrow cursor with hot spot (-1,-1), exclusion off, cursor hidden, position (400,200) rounded to the cell grid. |
| 1 | `fn01_show_cursor` | — | Cancels the fn 16 area by setting `excl_top` = 431. The counter stops at 0. |
| 2 | `fn02_hide_cursor` | — | The **hardware** text cursor is not removed. |
| 3 | `fn03_get_status` | → BX buttons, CX X, DX Y | Positions are always pixels (0-799 × 0-399). In text mode they are multiples of 10 × 16. |
| 4 | `fn04_set_position` | CX X, DX Y | Clamped and rounded but **not redrawn**. The cursor jumps on the next mouse motion. |
| 5 | `fn05_press_info` | BX bit 0: 0 = left, 1 = right → AX state, BX count (then cleared), CX/DX position at the last press | Only bit 0 of BX is tested. |
| 6 | `fn06_release_info` | BX: 0 = left, any other value = right → same as fn 5 | Tests the whole of BX. |
| 7 | `fn07_set_x_range` | CX, DX: swapped if CX ≥ DX | Re-clamps the position, no redraw. |
| 8 | `fn08_set_y_range` | same as fn 7 | same as fn 7 |
| 9 | `fn09_set_graphics_cursor` | BX/CX hot spot, ES:DX 32 mask words | Masks are stored bit-reversed (section 8). The cursor is redrawn if visible. |
| 10 | `fn10_set_text_cursor` | BX = 0: CX AND mask, DX XOR mask. BX ≠ 0: 6845 R10 = CL & 0Fh, R11 = DL & 0Fh | The type byte is BL, but the branch tests the whole of BX. Changing from a visible software cursor to the hardware cursor leaves the software image on screen. |
| 11 | `fn11_read_motion` | → CX, DX raw mickeys, then cleared | Shares its counters with INT FBh. |
| 12 | `fn12_set_user_handler` | CX mask, ES:DX handler | Patches the `CALL FAR` in the ISR (self-modifying code). |
| 13, 14 | — | no-op | Light pen is not supported. |
| 15 | `fn15_set_mickey_ratio` | CX, DX = mickeys per 8 pixels | Not validated. A value of 0 causes a divide fault inside the ISR. |
| 16 | `fn16_conditional_off` | CX left, DX top, SI right, DI bottom | The overlap is tested whenever the cursor is drawn. On overlap the counter drops to -1. |
| ≥ 17 | — | ignored | |

Functions 0, 1, 2, 9, 10 and 16 manage the visibility counter `cursor_level` (a signed byte:
0 = shown, negative = hidden).
- Functions 9, 10 and 16 decrement the counter and then jump to `show_cursor_tail`. The tail
  draws when the counter is -1 and then increments it, so the cursor is redrawn only if it was
  visible before the call.
- `check_exclusion` decrements the counter again when the cursor lands in the fn 16 area. That
  leaves the cursor hidden until fn 1 is called.

## 7. INT FBh private API (`int_fb_entry`, 0053)

| CX | Result |
|---|---|
| 1 | AX = 1 (presence check) |
| 2 | AX = `button_state`, BX = `raw_x`, CX = -`raw_y` (+ = up) |
| other | AX unchanged |

Every call clears `raw_x` and `raw_y`. No registers are saved and DS is not used. PICASSO.EXE does
not use it (its disassembly in `../picasso/` shows INT 33h only); the client is unknown.

## 8. Cursor drawing

`draw_cursor` (05CA) is called from `show_cursor_tail` and from the ISR. It picks one of three
cases. Graphics mode is detected by `gran_y` = 1. The text cursor type is `text_cursor_type`, set
to 1 (hardware) by reset.

### Mode detection (`detect_video_mode`, 078B, called only by reset)

The routine reads the 6845 start address R12:R13. The MAME `crtc_update_row` confirms the meaning
of the bits:
- **R12 bit 5 (MA13)** set selects hi-res mode: 16-pixel-wide character cells, which make the
  800×400 graphics screen.
- **R12 bit 4 (MA12)** selects the second 64 KB of font-cell RAM.

Each screen-RAM word selects a font cell of 16 words (32 bytes) at physical address
`(MA12 << 16) + (word & 7FFh) * 32`. Bit 0 of each font word is the **leftmost** pixel.

- **Graphics mode:** `gran_x = gran_y = 1`.
  `bitmap_seg = MA12 * 1000h + 2 * (F000:0000 & 7FFh)`. This is the segment of the font cell
  used by screen word 0. The driver reads screen word 0, not the word at the start address.
- **Text mode:** `gran_x = 10`, `gran_y = 16` (one character cell).

A program that switches mode must call fn 0 again.

### Text mode, hardware cursor

`text_cell_address` computes `2*start + (Y/16)*160 + (X/10)*2`. Halved, this is the word address.
The driver writes it to R14/R15, the cursor address registers.
- There is no erase step and no exclusion check.
- This is the 6845's only cursor, the same one the DOS console uses.

### Text mode, software cursor

1. The driver saves the cell's screen-RAM word in `save_buffer`.
2. It writes `(word AND text_screen_mask) XOR text_cursor_mask` back to the cell.
3. Erasing writes the saved word back at `drawn_x`/`drawn_y`.

The defaults are AND = FFFFh and XOR = 7700h. These look like the PC default text masks. On the
Victor, XOR 7700h toggles:
- attribute bits 14 (low intensity), 13 (underline) and 12 (**secret / non-display**);
- bits 10-8 of the font-cell number, which shows a different glyph.

A cell with the secret bit set draws blank in MAME's renderer. The default software cursor is
therefore most likely a blank or underlined block.

`text_cell_address` neither masks the start address nor wraps at the end of the 2048-word screen
RAM. The hardware wraps (MAME: `aa &= 0xfff`). Once the console has scrolled by moving the start
address, the software cursor can therefore write outside screen RAM.

### Graphics mode

The cursor is drawn at `gc_x = cursor_x - hotspot_x`, `gc_y = cursor_y - hotspot_y`.

**Memory layout the driver assumes:**
- The 800×400 screen is 50 × 25 hi-res cells.
- Their font-cell numbers run **column-major**, consecutive from the cell of screen word 0.
  Cell (column c, row r) = base + c*25 + r.
- So each 16-pixel-wide column strip is one contiguous block of 25 × 32 = **800 bytes (320h)**:
  one word per pixel row.

```
pixel (x, y)  ->  bitmap_seg : (x/16)*800 + y*2 ,  bit (x mod 16)     (bit 0 = leftmost)
```

PICASSO sets up the screen exactly this way (confirmed from its disassembly in `../picasso/`,
`qg_screen_pixel_addr`, and from PDUMP.COM's screen reader). `load_cursor_masks` (02DE)
mirrors every Microsoft mask word (bit 15 = leftmost) to match this bit order.

**Steps in `draw_graphics_cursor` (08C0):**
1. **Clip.** Exit if `gc_x` ≥ 800 or `gc_y` ≥ 400. Clamp the right edge to 799 and the bottom
   edge to 399. Rows above the top are skipped through `gc_skip_rows`.
2. **Shift.** `s = gc_x mod 16`. `gc_strip_left` and `gc_strip_right` become strip byte offsets
   `(x & ~15) * 50`.
3. **Two strips** (unaligned cursor), for each row:
   - Left word at `[bp]`: `(bg AND (screen<<s | low-s-bits)) XOR (cursor<<s)`. The low s bits
     keep the background pixels to the left of the cursor.
   - Right word at `[bp+320h]`: the same with `>> (16-s)` and the high bits kept.
   - Both background words go into `save_buffer`, two words per row.
4. **One strip** (aligned, or clipped at the right edge): only the left word, with the
   save-buffer stride kept at 4 bytes.
5. `restore_graphics_background` (0A7C) writes the buffer back using the `gc_*` values of the
   last draw.

The fn 16 test in `draw_cursor` uses the whole strip pair: left = `gc_x & ~15`, right = left+31,
bottom = top+15.

## 9. Variable map

| Offset | Size | Name | Meaning |
|---|---|---|---|
| 0012 | 4 | `request_ptr` | DOS request header (far pointer) |
| 058F | 2 | `user_handler_ofs` | operand of the `CALL FAR` in the ISR, patched by fn 12 |
| 0591 | 2 | `user_handler_seg` | the same, segment |
| 0ADB | 1 | `pkt_x` | X delta being assembled |
| 0ADC | 1 | `pkt_y` | Y delta being assembled |
| 0ADD | 2 | `raw_x` | raw X mickeys since the last fn 11 / INT FBh call |
| 0ADF | 2 | `raw_y` | raw Y mickeys (+ = down) |
| 0AE1 | 2 | `button_state` | bit 0 left, bit 1 right |
| 0AE3 | 6 | `left_press` | count, X, Y |
| 0AE9 | 6 | `left_release` | count, X, Y |
| 0AEF | 6 | `right_press` | count, X, Y |
| 0AF5 | 6 | `right_release` | count, X, Y. Reset clears 15 words, 0ADD-0AFA. |
| 0AFB | 2 | `x_min` | fn 7 range |
| 0AFD | 2 | `x_max` | |
| 0AFF | 2 | `y_min` | fn 8 range |
| 0B01 | 2 | `y_max` | |
| 0B03 | 2 | `excl_left` | fn 16 area |
| 0B05 | 2 | `excl_top` | 431 = disabled |
| 0B07 | 2 | `excl_right` | |
| 0B09 | 2 | `excl_bottom` | |
| 0B0B | 2 | `mickey_ratio_x` | mickeys per 8 pixels (8) |
| 0B0D | 2 | `mickey_ratio_y` | mickeys per 8 pixels (8) |
| 0B0F | 2 | `user_mask` | fn 12 event mask |
| 0B11 | 2 | `event_flags` | events of the current packet (only the low byte is written) |
| 0B13 | 1 | `isr_not_busy` | FFh = idle, 00h = handler or redraw running |
| 0B14 | 2 | `screen_width` | 800 |
| 0B16 | 2 | `screen_height` | 400 |
| 0B18 | 1 | `text_columns` | 80 |
| 0B19 | 2 | `cell_width` | 10. DIV reads it as a byte, ADD as a word. |
| 0B1B | 2 | `cell_height` | 16 |
| 0B1D | 2 | `gran_x` | 10 in text mode, 1 in graphics mode |
| 0B1F | 2 | `gran_y` | 16 in text mode, 1 in graphics mode. The value 1 is the "graphics mode" test. |
| 0B21 | 2 | `bitmap_seg` | segment of the font cell of screen word 0 |
| 0B23 | 2 | `reset_status` | FFFFh, returned by fn 0 |
| 0B25 | 1 | `pkt_state` | 0 = wait for sync, 1 = wait for byte 2, 2 = wait for byte 3 |
| 0B26 | 2 | `port_index` | 0 = A, 1 = B (default 1) |
| 0B28 | 32 | `cursor_screen_mask` | active AND mask, bit-reversed |
| 0B48 | 32 | `cursor_image_mask` | active XOR mask, bit-reversed |
| 0B68 | 2 | `hotspot_x` | -1 |
| 0B6A | 2 | `hotspot_y` | -1 |
| 0B6C | 32 | `default_screen_mask` | Microsoft arrow |
| 0B8C | 32 | `default_cursor_mask` | Microsoft arrow |
| 0BAC | 2 | `text_screen_mask` | FFFFh |
| 0BAE | 2 | `text_cursor_mask` | 7700h |
| 0BB0 | 1 | `text_cursor_type` | 0 = software; anything else = hardware. Reset sets 1. |
| 0BB1 | 2 | `frac_x` | scaling remainder |
| 0BB3 | 2 | `frac_y` | |
| 0BB5 | 2 | `virt_x` | virtual position, range ±16 |
| 0BB7 | 2 | `virt_y` | |
| 0BB9 | 2 | `cursor_x` | reported position |
| 0BBB | 2 | `cursor_y` | |
| 0BBD | 2 | `drawn_x` | last drawn position |
| 0BBF | 2 | `drawn_y` | |
| 0BC1 | 1 | `cursor_level` | visibility counter. The file holds FFh; reset sets 0, then hide makes it -1. |
| 0BC2 | 1 | `cursor_drawn` | FFh = an image is on screen |
| 0BC3 | 64 | `save_buffer` | background under the cursor |
| 0C03 | 2 | `gc_x` | graphics cursor left edge |
| 0C05 | 2 | `gc_y` | graphics cursor top edge |
| 0C07 | 2 | `gc_skip_rows` | rows clipped at the top, ×2 after setup |
| 0C09 | 2 | `gc_skip_cols` | columns clipped at the left |
| 0C0B | 2 | `gc_row_first` | first row, ×2 after setup |
| 0C0D | 2 | `gc_row_last` | last row, ×2 after setup |
| 0C0F | 2 | `gc_strip_left` | left X, then the strip byte offset |
| 0C11 | 2 | `gc_strip_right` | right X, then the strip byte offset |
| 0C13 | 1 | `gc_shift` | X mod 16 |
| 0C14 | 12 | `unused_0C14` | never referenced |

## 10. Surprises and bugs

1. **INT 33h AX=0 with BX=0 re-initialises the hardware.** `fn00_reset` falls into
   `reset_driver` with the caller's BX. BX=0 triggers the full serial setup: INT 21h AH=25h from
   inside INT 33h, the 65,535-read delay and a mouse power cycle.
2. **Acceleration bug.** `accelerate` returns CF=0 when |dX| = 2, so a packet with exactly 2
   mickeys of X motion never has its dY tested for acceleration.
3. **Left-edge clipping of the graphics cursor is wrong.** For `gc_x` < 0, the shift is
   `gc_x mod 16` and the masks are still shifted left inside strip 0, so the wrong columns are
   drawn. This only happens with a hot spot X larger than the cursor X. The "right/bottom edge
   < 0" exits are dead code: `cmp ax,0` / `jnc` is always taken.
4. **Hide does not hide the hardware text cursor**, which is the default after reset.
   Switching from a visible software cursor to the hardware cursor leaves a stale software image.
5. **fn 4 (set position) and fn 7/8 (range) do not redraw.** The cursor moves on the next mouse
   motion.
6. **Motion events are relative to the drawn position.** While the cursor is hidden, `drawn_x`
   and `drawn_y` do not change, so any non-zero packet that leaves the position away from the last
   drawn place reports "moved".
7. **The software text cursor ignores screen-RAM wrap-around** and the R12 mode bits
   (`text_cell_address`).
8. **Graphics mode is detected only at reset.** The bitmap base comes from screen word 0 and
   assumes the column-major cell layout.
9. **INT 41h is taken over without chaining.** The ISR does not check which 7201 channel raised
   the interrupt. Interrupt-driven use of the other serial port (PORTA.DEV/PORTB.DEV are loaded
   before MOUSE.DEV in CONFIG.SYS) would break.
10. **Default port B versus MOUSE.DOC's "default SERIAL A".** The command line scan also reads
    the driver's file name.
11. **fn 15 with a ratio of 0** causes a divide fault inside the ISR. fn 4 with a negative
    coordinate can overflow the unsigned DIV in the rounding step.
12. **INT FBh and fn 11 share the raw counters.** Each clears them for the other.
13. **INIT status is 0124h** (AL = `'$'`). This is harmless.
14. **The device name is NUL-padded**, not blank-padded.

## 11. MOUSE.COM: the same driver as a TSR

Nothing in the resident code depends on the DOS device-driver mechanism. Every DOS request except
INIT is rejected (section 2), all traffic goes through INT 33h, FBh and 41h, and the code
addresses its data through CS. The .DEV form is used for two things only, both in the discarded
INIT part: reading the port letter from the CONFIG.SYS line and staying resident through the
break address. `MOUSE_COM.asm` replaces those with their .COM equivalents and adds the checks
the .DEV never made.

```
MOUSE A              mouse on serial port A
MOUSE B              mouse on serial port B
MOUSE                usage screen, nothing installed (the .DEV silently defaults to B)
```

| | MOUSE.DEV | MOUSE.COM |
|---|---|---|
| Build | `nasm -f bin MOUSE.asm` | `nasm -f bin -I./ MOUSE_COM.asm` (defines `BUILD_COM`, includes `MOUSE.asm`) |
| Origin | `org 0` | `org 100h`. Every offset in this document is valid as offset + 100h. |
| 0000-0052 | device header, strategy, interrupt, `request_done` | `jmp com_install` at 100h, then zero padding to 153h |
| 0053-0C1F | resident code and variables | identical bytes, apart from absolute offsets being 100h higher |
| Port letter | first a/A/b/B of the CONFIG.SYS line, file name included; none = B | first a/A/b/B of the PSP command tail at 81h; none = usage screen and exit |
| Staying resident | break address = `device_init` | INT 21h AH=31h with D2h paragraphs = PSP + 0C20h bytes, 3,360 bytes in all |
| Load time | boot, before COMMAND.COM | any time. INT 41h is still taken over without chaining (bug 9), so whatever owned the serial interrupt before is cut off, exactly as the .DEV does to PORTA/PORTB.DEV. |
| No mouse | installs anyway | does not install: channel reset, INT 41h and the 8259 mask restored, errorlevel 2 |

`com_install` otherwise repeats `device_init` step for step, but in a safer order: `reset_driver`
with BX = 0 (full hardware initialisation and the INT 41h hook), then `detect_mouse`, and only
if a mouse answered the INT 33h and INT FBh vectors, the sign-on text and the TSR exit. Before
`reset_driver` it saves the INT 41h vector and the 8259 mask so a failed install can put them
back. Memory cost is the 256-byte PSP more than the .DEV.

### What can go wrong, and what the installer says

| Condition | Detection | Message | Effect |
|---|---|---|---|
| No port letter | command tail has no a/A/b/B | usage screen (`Usage:  MOUSE A ...`) | exit, errorlevel 1, nothing changed |
| Already loaded | the current INT 33h vector has `MOUSE` in the 5 bytes before it (the test PICASSO uses) | `MOUSE is already installed.` | exit, errorlevel 1, nothing changed |
| Another mouse driver present | INT 33h vector non-null, no signature | `Note: another INT 33h mouse handler is installed and will be replaced.` (printed only when the install goes ahead) | install continues |
| No mouse, wrong port, bad cable, mouse not powered from RTS | `detect_mouse`: after the reset has power-cycled the mouse (RTS off, 65,535-read delay, RTS on), a Microsoft mouse sends `'M'`. The ISR takes it as a sync byte and leaves `pkt_state` = 1. The installer polls `pkt_state` (and, as a fallback, RR0 bit 0 "Rx character available") for up to 2 s on the DOS clock (INT 21h AH=2Ch). | `No mouse detected on serial port A: nothing was received after a power cycle.` + `Check the cable and the port letter (MOUSE A or MOUSE B). MOUSE is not installed.` | not installed: WR1 = 0 and a channel reset quiet the port (RTS drops, so the mouse is unpowered), the 8259 mask and INT 41h go back to their previous values, exit with errorlevel 2. INT 33h / FBh were never hooked. The channel is left reset, not restored to DOS's 7E1 setup. |
| Not a Victor 9000 | not detected. The E000h writes land in whatever is there; on a PC the result is the "no mouse" warning. | | |
| Mouse-Systems or other non-Microsoft mouse | not detected. Bytes arrive, so detection passes, but packets will not decode. | | |
| Another program using the same serial port's interrupt | not detected (bug 9) | | |
| Out of memory | DOS refuses to load the program before `com_install` runs | DOS's own message | |

The detection itself adds no hardware writes: it only reads `pkt_state` and RR0, so a false
"no mouse" can only come from a mouse that stays silent after a power cycle. If that happens with
a real mouse, moving it during the 2 s window also counts as detected (packet bytes cycle
`pkt_state` through 1, 2, 0 at 8 ms per byte, which the polling loop cannot miss). A mouse
plugged in later therefore needs MOUSE run again; the .DEV form still installs unconditionally.

**Test status.** `make test-dosbox` runs `dosbox_test/harness.asm` under headless DOSBox 0.74-3.
DOSBox is a PC, so E000h is plain memory and the 7201 code is exercised only as memory writes.
Results on 2026-10-11: no letter prints the usage screen and returns errorlevel 1 with the
vectors untouched; `MOUSE A` on the empty port reports no mouse after 2.03 s, returns errorlevel 2
and leaves INT 33h and INT 41h exactly as they were; a run where the harness's INT 08h hook sets
RR0 bit 0 after 0.5 s prints the replacement notice (DOSBox's own INT 33h), signs on for port A
and goes resident (return code 0300h) in 0.49 s, with INT 33h, INT FBh and INT 41h all pointing
into the TSR and the `MOUSE` signature in front of the INT 33h entry; a second run is refused with
errorlevel 1. It has not been run on a Victor 9000 or in MAME's `victor9k`, so the hardware path
(the unchanged .DEV code) and the real `'M'` response are untested in this form.
