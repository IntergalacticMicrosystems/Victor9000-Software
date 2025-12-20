/*
 * igc.h - IGC File Manager Core Types and Constants
 * Victor 9000 DOS 3.1 / Open Watcom v2
 */

#ifndef IGC_H
#define IGC_H

/*---------------------------------------------------------------------------
 * Fixed-width types for DOS/8086 compatibility
 *---------------------------------------------------------------------------*/
typedef unsigned char   uint8_t;
typedef signed char     int8_t;
typedef unsigned short  uint16_t;
typedef signed short    int16_t;
typedef unsigned long   uint32_t;
typedef signed long     int32_t;

typedef uint8_t bool_t;
#define TRUE    1
#define FALSE   0

/*---------------------------------------------------------------------------
 * Victor 9000 Screen Constants
 *---------------------------------------------------------------------------*/
#define SCR_WIDTH       80
#define SCR_HEIGHT      25
#define SCR_COLS        80
#define SCR_ROWS        25

/* Panel layout */
#define PANEL_WIDTH     40
#define PANEL_HEIGHT    19      /* Visible file rows (rows 3-21) */

/* Row assignments */
#define ROW_TITLE       0       /* Title bar with horizontal lines */
#define ROW_HEADER      1       /* Column headers */
#define ROW_FILES_START 2
#define ROW_FILES_END   20
#define ROW_BOT_BORDER  21
#define ROW_FKEYS       24
#define ROW_STATUS      23

/* Column positions */
#define LEFT_PANEL_X    0
#define RIGHT_PANEL_X   40
#define MID_COL         39

/*---------------------------------------------------------------------------
 * Victor 9000 VRAM Constants
 *---------------------------------------------------------------------------*/
#define VRAM_SEG        0xF000
#define VRAM_SIZE       8192        /* 8KB total VRAM */
#define VRAM_WORDS      4096        /* 4096 words */
#define PAGE_WORDS      2000        /* 80*25 = 2000 words per page */
#define CHAR_BASE       0x64        /* Character ROM offset (100 decimal) */

/* CRTC Controller (HD46505) */
#define CRTC_SEG        0xE800
#define CRTC_SEL_OFF    0x00
#define CRTC_DATA_OFF   0x01

/* CRTC Registers */
#define CRTC_CURSOR_START   10
#define CRTC_CURSOR_END     11
#define CRTC_START_MSB      12
#define CRTC_START_LSB      13
#define CRTC_CURSOR_MSB     14
#define CRTC_CURSOR_LSB     15

/* Cursor blink modes */
#define CURSOR_BLINK_NONE   0x00
#define CURSOR_HIDDEN       0x20
#define CURSOR_BLINK_FAST   0x40
#define CURSOR_BLINK_SLOW   0x60

/*---------------------------------------------------------------------------
 * Screen Attributes (Victor 9000)
 *---------------------------------------------------------------------------*/
#define ATTR_NORMAL     0x00
#define ATTR_REVERSE    0x80        /* Bit 7: Reverse video */
#define ATTR_DIM        0x40        /* Bit 6: Low intensity */
#define ATTR_UNDERLINE  0x20        /* Bit 5: Underline */
#define ATTR_DIM_REV    0xC0        /* Dim + Reverse combined */

/*---------------------------------------------------------------------------
 * Box Drawing Characters
 *---------------------------------------------------------------------------*/
#define BOX_TL          0xDA        /* Top-left corner */
#define BOX_TR          0xBF        /* Top-right corner */
#define BOX_BL          0xC0        /* Bottom-left corner */
#define BOX_BR          0xD9        /* Bottom-right corner */
#define BOX_HORIZ       0xC4        /* Horizontal line */
#define BOX_VERT        0xB3        /* Vertical line */
#define BOX_T_DN        0xC2        /* T pointing down */
#define BOX_T_UP        0xC1        /* T pointing up */
#define BOX_T_RT        0xC3        /* T pointing right */
#define BOX_T_LT        0xB4        /* T pointing left */
#define BOX_CROSS       0xC5        /* Cross/plus */

/*---------------------------------------------------------------------------
 * Keyboard Constants
 *---------------------------------------------------------------------------*/
/* Key types */
#define KEY_NONE        0
#define KEY_ASCII       1
#define KEY_EXTENDED    2

/* Extended key scan codes (IBM AT compatible) */
#define KEY_F1          0x3B
#define KEY_F2          0x3C
#define KEY_F3          0x3D
#define KEY_F4          0x3E
#define KEY_F5          0x3F
#define KEY_F6          0x40
#define KEY_F7          0x41
#define KEY_F8          0x42
#define KEY_F9          0x43
#define KEY_F10         0x44

#define KEY_HOME        0x47
#define KEY_UP          0x48
#define KEY_PGUP        0x49
#define KEY_LEFT        0x4B
#define KEY_RIGHT       0x4D
#define KEY_END         0x4F
#define KEY_DOWN        0x50
#define KEY_PGDN        0x51
#define KEY_INSERT      0x52
#define KEY_DELETE      0x53

/* ASCII key codes */
#define KEY_ESC         0x1B
#define KEY_ENTER       0x0D
#define KEY_TAB         0x09
#define KEY_BACKSPACE   0x08
#define KEY_SPACE       0x20

/* Victor 9000 F-key codes (need translation) */
#define V9K_F1          0xF1
#define V9K_F2          0xF2
#define V9K_F3          0xF3
#define V9K_F4          0xF4
#define V9K_F5          0xF5
#define V9K_F6          0xF6
#define V9K_F7          0xF7
#define V9K_F8          0xF8
#define V9K_F9          0xF9
#define V9K_F10         0xFA

/* Victor 9000 navigation keys */
#define V9K_PGUP        0xE4
#define V9K_PGDN        0xE5

/*---------------------------------------------------------------------------
 * DOS File Attributes
 *---------------------------------------------------------------------------*/
#define DOS_ATTR_READONLY   0x01
#define DOS_ATTR_HIDDEN     0x02
#define DOS_ATTR_SYSTEM     0x04
#define DOS_ATTR_VOLUME     0x08
#define DOS_ATTR_DIRECTORY  0x10
#define DOS_ATTR_ARCHIVE    0x20

/* File open modes */
#define DOS_OPEN_READ       0x00
#define DOS_OPEN_WRITE      0x01
#define DOS_OPEN_RW         0x02

/* Seek modes */
#define DOS_SEEK_SET        0x00
#define DOS_SEEK_CUR        0x01
#define DOS_SEEK_END        0x02

/*---------------------------------------------------------------------------
 * Memory Tiers
 *---------------------------------------------------------------------------*/
#define MEM_LOW         0       /* < 300KB: minimal config */
#define MEM_MEDIUM      1       /* 300-450KB: standard config */
#define MEM_HIGH        2       /* > 450KB: extended config */

/* Tier thresholds (in KB) */
#define MEM_LOW_THRESHOLD       300
#define MEM_MEDIUM_THRESHOLD    450

/*---------------------------------------------------------------------------
 * Buffer Size Limits by Memory Tier
 *---------------------------------------------------------------------------*/
/* Files per panel */
#define FILES_PER_PANEL_LOW     256
#define FILES_PER_PANEL_MEDIUM  512
#define FILES_PER_PANEL_HIGH    1024

/* Editor buffer (bytes) */
#define EDITOR_BUF_LOW          16384L      /* 16KB */
#define EDITOR_BUF_MEDIUM       32768L      /* 32KB */
#define EDITOR_BUF_HIGH         65536L      /* 64KB */

/* Copy buffer (bytes) */
#define COPY_BUF_LOW            512
#define COPY_BUF_MEDIUM         2048
#define COPY_BUF_HIGH           8192

/*---------------------------------------------------------------------------
 * Path and Filename Limits
 *---------------------------------------------------------------------------*/
#define MAX_PATH_LEN    65          /* DOS path limit */
#define MAX_FILENAME    13          /* 8.3 + null */
#define MAX_DRIVE       26          /* A-Z */

/*---------------------------------------------------------------------------
 * Key Event Structure
 *---------------------------------------------------------------------------*/
typedef struct {
    uint8_t type;       /* KEY_NONE, KEY_ASCII, KEY_EXTENDED */
    uint8_t code;       /* ASCII code or scan code */
} KeyEvent;

#endif /* IGC_H */
