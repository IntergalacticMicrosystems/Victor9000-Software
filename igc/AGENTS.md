# IGC Development Guide

Two-pane file manager for Victor 9000 DOS 3.1, written in C using Open Watcom v2.

This file is for LLM use, README.md is for human use.
Please update the documentation when changes are made.

## Build

```bash
make              # Build bin/igc.exe
make clean        # Remove build artifacts
make deploy       # Deploy to MAME disk image
make keytest      # Build keyboard test utility
```

## Architecture

### Target Platform
- Victor 9000 / Sirius 1 computer
- DOS 3.1, 8086 processor (16-bit real mode)
- Minimum 128KB RAM, scales to 512KB+
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
| `keyboard.c/h` | DOS INT 9h, Victor 9000 key translation |
| `dosapi.c/h` | DOS INT 21h wrappers (files, directories, memory) |
| `panel.c/h` | File list management, directory reading |
| `ui.c/h` | Frame drawing, headers, F-key bar |
| `dialog.c/h` | Modal dialogs with screen save/restore |
| `fileops.c/h` | Copy, move, delete, mkdir, rename |
| `editor.c/h` | Text viewer (F3) and editor (F4) |
| `util.c/h` | String/path utilities |
| `config.c/h` | INI file persistence |

## Key Data Structures

```c
// Key event (keyboard.h)
typedef struct {
    uint8_t type;   // KEY_NONE, KEY_ASCII, KEY_EXTENDED
    uint8_t code;   // ASCII or scan code
} KeyEvent;

// File entry (panel.h) - 24 bytes
typedef struct {
    char name[13];      // 8.3 filename + null
    uint8_t attr;       // DOS attributes
    uint16_t date;      // DOS date format
    uint16_t time;      // DOS time format
    uint32_t size;      // File size
    bool_t selected;    // Selection flag
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

```c
// VRAM (screen.h)
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
Always use incremental updates via `ui_draw_panel_row()` rather than full redraws:
```c
// Good: Update only affected row
ui_draw_panel_row(panel, old_cursor);
ui_draw_panel_row(panel, panel->cursor);

// Avoid: Full panel redraw (slow)
ui_draw_panel(panel);
```

### Far Pointer Usage
Large arrays must use far pointers:
```c
FileEntry __far *files = (FileEntry __far *)mem_alloc_far(count * sizeof(FileEntry));
// Access via far pointer
files[i].name;
```

### Dialog Overlay Pattern
Save screen region before dialog, restore after:
```c
uint16_t __far *saved = dialog_save_region(x, y, w, h);
// Draw dialog...
// Handle input...
dialog_restore_region(saved, x, y, w, h);
mem_free_far(saved);
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
