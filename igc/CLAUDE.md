# Project: igc

Two-pane file manager for Victor 9000 DOS 3.1, written in C using Open Watcom v2.
Inspired by Midnight Commander.

Always update the documentation when changes are made.

## Key Features
- Dynamic memory management (scales from 128KB to 512KB+ systems)
- Direct VRAM access for fast screen updates
- Optimized cursor movement (only redraws affected rows, not entire panels)
- Victor 9000 keyboard translation (F-keys, arrow keys)
- Support for large directories (500+ files) and large files in editor
- INI configuration file for saving/loading panel state

## Build
```bash
make            # Build igc.exe
make clean      # Clean build artifacts
make deploy     # Deploy to MAME disk image
```

Compiler: Open Watcom v2, compact memory model (`-mc`), 8086 target
Output: bin/igc.exe (~25KB)

## Usage

### Keyboard Controls
- **Tab**: Switch between left/right panels
- **Arrows**: Navigate files
- **Enter**: Enter directory or view file
- **Backspace**: Go to parent directory
- **Space**: Toggle file selection (selected files marked with `*`)
- **ESC/Q**: Quit

### Function Keys
- **F1**: Drive selection
- **F2**: Make directory
- **F3**: View file (read-only)
- **F4**: Edit file
- **F5**: Copy files
- **F6**: Move files
- **F7**: Delete files
- **F8**: Rename file
- **F10**: Quit

## Source Files
```
src/
├── igc.h        - Core types, constants, structures
├── main.c       - Entry point, event loop
├── mem.h/c      - Memory management, dynamic allocation
├── screen.h/c   - VRAM access, text output
├── keyboard.h/c - DOS keyboard with V9K translation
├── dosapi.h/c   - DOS INT 21h wrappers
├── panel.h/c    - Panel structures, directory reading
├── ui.h/c       - Frame, headers, F-key bar
├── dialog.h/c   - Modal dialogs, confirmations
├── fileops.h/c  - File operations (copy, move, delete, mkdir, rename)
├── editor.h/c   - Text viewer/editor (F3/F4)
├── util.h/c     - String/path utilities
└── config.h/c   - INI configuration parser
```

## Memory Tiers
The program scales based on available memory:

| Resource          | Tiny (128KB) | Low (256KB) | Medium (384KB) | High (512KB+) |
|-------------------|--------------|-------------|----------------|---------------|
| Files per panel   | 64           | 256         | 512            | 1024+         |
| Editor buffer     | 4KB          | 16KB        | 32KB           | 64KB          |
| Copy buffer       | 256B         | 512B        | 2KB            | 8KB           |
| Editor max lines  | 128          | 512         | 1024           | 2048          |

## Reference Files
- `root/Victor9000-Development-Private/asm86lib` - Low-level library reference
- `root/Victor9000-Development-Private/old/igc` - Old nasm version
