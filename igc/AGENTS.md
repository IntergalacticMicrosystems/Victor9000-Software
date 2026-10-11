# IGC Development Guide

Two-pane file manager for Victor 9000 DOS 3.1, written in C using Open Watcom v2.

This file is for LLM use, README.md is for human use.
Please update the documentation when changes are made.

## Build

```bash
make              # Build bin/igc.exe (also writes bin/igc.map)
make clean        # Remove build artifacts
make deploy       # Deploy to MAME disk image (set MAME_DIR=<path>)
make keytest      # Build keyboard test utility
make deploy-keytest  # Deploy keytest.exe to the MAME disk image
```

## Architecture

### Target Platform
- Victor 9000 / Sirius 1 computer
- DOS 3.1, 8086 processor (16-bit real mode)
- Minimum 256KB RAM, scales to 512KB+
- 80x25 text mode with direct VRAM access

### Compiler Settings
- Open Watcom v2 cross-compiler (Linux host)
- Compact memory model (`-mc`): near code, far data
- 8086-only instructions (`-0`)
- Optimized for size (`-os`)

### Memory Model
Uses far pointers (`__far`) for data larger than 64KB. Memory tier detection at startup scales all buffers:
- `MEM_TINY` (<64KB free): 64 files/panel, 4KB editor
- `MEM_LOW` (64-128KB): 256 files/panel, 16KB editor
- `MEM_MEDIUM` (128-200KB): 512 files/panel, 32KB editor
- `MEM_HIGH` (>200KB): 1024 files/panel, 64KB editor

## Source Files

| File | Purpose |
|------|---------|
| `igc.h` | Core types, constants, screen/keyboard codes |
| `main.c` | Entry point, event loop, key dispatch |
| `mem.c/h` | Far heap allocation, tier detection |
| `screen.c/h` | VRAM access (0xF000), CRTC cursor control |
| `keyboard.c/h` | DOS INT 9h, Victor 9000 key translation; `kbd_wait()` also polls the mouse and returns `KEY_MOUSE` events |
| `mouse.c/h` | INT 33h driver wrapper (Victor MOUSE.DEV / MOUSE.COM): detect (incl. which serial port it owns), software text pointer, click polling with double-click detection |
| `dosapi.c/h` | DOS INT 21h wrappers (files, directories, memory) |
| `panel.c/h` | File list management, directory reading |
| `ui.c/h` | Frame drawing, headers, F-key bar |
| `dialog.c/h` | Modal dialogs with screen save/restore |
| `fileops.c/h` | Copy, move, delete, mkdir, rename (local + serial, incl. recursive directory trees) |
| `editor.c/h` | Text viewer (F3) and editor (F4) |
| `util.c/h` | String/path utilities |
| `serialfs.c/h` | Serial file-server backend (igcfs over viclibc) on Serial A or B; file + directory-tree transfer, list, delete, mkdir, rename |
| `serialfs_proto.h` | igc↔igcfs protocol extension command bytes |

## Key Data Structures

```c
// Key event (igc.h)
typedef struct {
    uint8_t type;   // KEY_NONE, KEY_ASCII, KEY_EXTENDED, KEY_MOUSE
    uint8_t code;   // ASCII or scan code; for KEY_MOUSE: MOUSE_LEFT / MOUSE_RIGHT /
                    // MOUSE_LEFT_DBL, with the clicked cell in g_mouse_x / g_mouse_y
} KeyEvent;

// File entry (panel.h) - 26 bytes with default -zp2 packing
typedef struct {
    uint8_t  attr;          // DOS attributes
    uint16_t time;          // DOS time format
    uint16_t date;          // DOS date format
    uint32_t size;          // File size
    char     name[13];      // 8.3 filename + null
    uint8_t  selected;      // Selection flag
    uint8_t  padding[2];    // Reserved
} FileEntry;

// Panel state (panel.h)
typedef struct {
    uint8_t drive;          // Current drive (0=A, 1=B, etc)
    char path[MAX_PATH_LEN]; // Current directory
    uint16_t top;           // First visible file index
    uint16_t cursor;        // Selected file index
    uint16_t sel_count;     // Number of selected files
    FileList files;         // Dynamic array of FileEntry
} Panel;
```

## Hardware Constants

All in `igc.h`:

```c
// VRAM
#define VRAM_SEG    0xF000   // Video RAM segment
#define CRTC_SEG    0xE800   // CRT controller segment

// Screen layout
#define SCR_WIDTH   80
#define SCR_HEIGHT  25
#define PANEL_WIDTH 40
#define PANEL_HEIGHT 19      // Visible file rows

// Attributes
#define ATTR_NORMAL     0x00
#define ATTR_REVERSE    0x80  // Highlight
#define ATTR_DIM        0x40
#define ATTR_UNDERLINE  0x20
```

## Victor 9000 Key Translation

The Victor 9000 uses different scan codes than IBM PC. Translation in `keyboard.c`:

```c
// Victor F-keys (0xF1-0xFA) -> IBM AT codes (0x3B-0x44)
// Victor PgUp (0xE4) -> IBM PgUp (0x49)
// Victor PgDn (0xE5) -> IBM PgDn (0x51)
```

## Common Patterns

### Screen Updates
Prefer incremental updates over full redraws. `ui_update_cursor()` redraws
only the old and new cursor rows (or the panel if the view scrolled):
```c
// Good: update only affected rows (see handle_navigation in main.c)
uint16_t old_cursor = p->cursor, old_top = p->top;
panel_cursor_down(p);
ui_update_cursor(old_cursor, old_top);

// Avoid: full panel redraw (slow)
ui_draw_panel(p, x_offset, active);   // x_offset 0=left, 40=right
```

### Far Pointer Usage
Large arrays must use far pointers (allocated from the DOS far heap):
```c
FileEntry __far *files =
    (FileEntry __far *)mem_alloc((uint32_t)count * sizeof(FileEntry));
// Access via far pointer
files[i].name;
// Release with:
mem_free(files);
```

### Mouse Input
The mouse is optional: `mouse_init()` (main.c, after `scr_init()`) looks for an
INT 33h driver and IGC runs unchanged without one. Rules that keep it simple:
- Only `kbd_wait()` talks to the mouse. It shows the pointer, polls keyboard and
  mouse, hides the pointer and returns. All drawing happens between waits, so
  the driver's save/restore of the cell under the pointer cannot corrupt the
  screen. Never draw while the pointer is shown.
- The driver is reset with `BX = 1`. `BX = 0` would make it re-initialise the
  serial port and power-cycle the mouse (slow, and it hooks INT 41h again).
- The pointer is the driver's software text cursor (fn 10: AND `FFFF`, XOR
  `8000` = flip reverse video). The driver's default is the 6845 hardware
  cursor, which the editor and input fields use for their own cursor.
- Clicks come from fn 5 (press counts), so a click is not lost if it happens
  while IGC is busy; `kbd_flush()` also drops pending clicks.
- Input loops that do not understand `KEY_MOUSE` just ignore it (they switch
  on `KEY_ASCII` / `KEY_EXTENDED`). Loops that do: `handle_mouse()` in main.c
  (panels, title bar, F-key bar), `dlg_confirm`, `dlg_input`,
  `dlg_drive_select` (button/row hit tests against the same coordinates the
  draw code uses), and `mouse_to_key()` in editor.c (text click, help bar).
- Positions: the driver reports pixels on an 800x400 grid; text cells are
  10x16, so cell = x/10, y/16.
- Detection checks for the text `MOUSE` in the 5 bytes before the INT 33h
  entry (the Victor driver's signature, also what PICASSO tests) before the
  vector is ever called, because Victor DOS leaves INT 33h as the boot ROM
  set it.
- `mouse_port()` reports the serial port the driver was loaded on. The driver
  has no API for this, so `mouse_init()` reads its `port_index` word at a
  fixed offset (0A9Ch) past the INT 33h entry; the offset is the same in
  MOUSE.DEV and MOUSE.COM (same bytes, shifted by 100h). The source is in
  `../mouse/` (MOUSE_DRIVER.md, section 9).
  Values other than 0/1 are reported as `MOUSE_PORT_NONE`. The drive dialog
  lists that port as `Serial x (mouse)` and refuses it: the driver owns the
  channel and INT 41h, and a channel reset there would silence the mouse.
- Serial file server and mouse coexist on different ports only because
  `serialfs_connect()` calls viclibc's `ser_init_one(port)`, which resets and
  programs just that SIO channel. Never call `ser_init()` / `ser_shutdown()`
  from igc: both touch the other channel too.
- Stale clicks: the second press of a double-click on a button or label
  usually arrives while the first click's action is running, and would land
  on whatever the next screen draws there. `mouse_new_context()` (called by
  `dlg_close()` and at editor open/close, and implied by `kbd_flush()`) marks
  a screen change; the next `kbd_wait()` then drains presses queued since
  the previous wait before showing the pointer. Call it from any new
  screen-changing code that a single click can trigger. This is deliberately
  independent of timing: a slow floppy operation can delay the next wait by
  seconds.
- Any click dismisses "press a key" pauses and `dlg_alert`, like a key.
- Polling uses DOS calls that run INT 23h on Ctrl-C, so main.c installs a
  do-nothing INT 23h handler at startup. Without it Ctrl-C would exit IGC
  mid-screen with the pointer still live.

### Dialog Overlay Pattern
`dlg_open()` saves the background and draws the frame; `dlg_close()`
restores it and frees the save buffer:
```c
DialogWindow win;
if (dlg_open(&win, x, y, w, h, "Title")) {
    dlg_print(&win, 1, 1, "text");
    // Handle input...
    dlg_close(&win);
}
```

## Testing

Run in MAME Victor 9000 emulator:
```bash
make deploy   # Copy to disk image
# Then launch MAME with victor9k driver
```

Use `keytest.exe` to debug keyboard input issues.

## Common Issues

1. **Linker errors**: Check far pointer usage - compact model requires explicit `__far`
2. **Screen corruption**: Verify VRAM segment (0xF000) and attribute bytes
3. **Keyboard not responding**: Check Victor 9000 key translation in `keyboard.c`
4. **Out of memory**: Reduce buffer sizes or check tier detection in `mem.c`
5. **Files not showing**: Verify DOS INT 21h calls in `dosapi.c`, check error returns

## Reference

- `../asm86lib` - Low-level library reference (if available)
- Victor 9000 Technical Reference Manual
- DOS INT 21h function reference
- Open Watcom v2 C Library Reference (16-bit DOS)
