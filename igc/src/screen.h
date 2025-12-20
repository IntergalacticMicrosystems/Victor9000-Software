/*
 * screen.h - IGC Screen/VRAM Access
 * Direct VRAM access for Victor 9000
 */

#ifndef SCREEN_H
#define SCREEN_H

#include "igc.h"

/*---------------------------------------------------------------------------
 * Screen State
 *---------------------------------------------------------------------------*/
typedef struct {
    uint8_t cursor_x;       /* Current cursor X position */
    uint8_t cursor_y;       /* Current cursor Y position */
    uint8_t attr;           /* Current attribute */
    bool_t  cursor_visible; /* Cursor visibility state */
} ScreenState;

extern ScreenState g_scr;

/*---------------------------------------------------------------------------
 * Initialization
 *---------------------------------------------------------------------------*/

/* Initialize screen system */
void scr_init(void);

/* Cleanup screen system */
void scr_exit(void);

/*---------------------------------------------------------------------------
 * Screen Clear
 *---------------------------------------------------------------------------*/

/* Clear entire screen with current attribute */
void scr_clear(void);

/* Clear rectangle with spaces */
void scr_clear_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h);

/*---------------------------------------------------------------------------
 * Cursor Control
 *---------------------------------------------------------------------------*/

/* Move cursor to x,y position */
void scr_gotoxy(uint8_t x, uint8_t y);

/* Get current cursor position */
void scr_getxy(uint8_t *x, uint8_t *y);

/* Show hardware cursor */
void scr_cursor_on(void);

/* Hide hardware cursor */
void scr_cursor_off(void);

/*---------------------------------------------------------------------------
 * Attribute Control
 *---------------------------------------------------------------------------*/

/* Set current attribute for subsequent output */
void scr_set_attr(uint8_t attr);

/* Get current attribute */
uint8_t scr_get_attr(void);

/*---------------------------------------------------------------------------
 * Character Output
 *---------------------------------------------------------------------------*/

/* Write character at current position with current attribute */
void scr_putc(char c);

/* Write character at x,y with specified attribute */
void scr_putc_xy(uint8_t x, uint8_t y, char c, uint8_t attr);

/* Write string at current position with current attribute */
void scr_puts(const char *s);

/* Write string at x,y with specified attribute */
void scr_puts_xy(uint8_t x, uint8_t y, const char *s, uint8_t attr);

/* Write string with maximum length (truncates or pads with spaces) */
void scr_puts_n(const char *s, uint8_t maxlen);

/* Write string at x,y with max length */
void scr_puts_n_xy(uint8_t x, uint8_t y, const char *s, uint8_t maxlen, uint8_t attr);

/*---------------------------------------------------------------------------
 * Number Output
 *---------------------------------------------------------------------------*/

/* Write unsigned 16-bit decimal number */
void scr_put_uint16(uint16_t num);

/* Write unsigned 32-bit decimal number */
void scr_put_uint32(uint32_t num);

/* Write number at x,y right-aligned in field of given width */
void scr_put_uint32_xy(uint8_t x, uint8_t y, uint32_t num, uint8_t width, uint8_t attr);

/*---------------------------------------------------------------------------
 * Rectangle Operations
 *---------------------------------------------------------------------------*/

/* Fill rectangle with character and attribute */
void scr_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                   char c, uint8_t attr);

/* Draw horizontal line */
void scr_hline(uint8_t x, uint8_t y, uint8_t len, char c, uint8_t attr);

/* Draw vertical line */
void scr_vline(uint8_t x, uint8_t y, uint8_t len, char c, uint8_t attr);

/*---------------------------------------------------------------------------
 * Save/Restore (for dialogs)
 *---------------------------------------------------------------------------*/

/* Save rectangle to buffer (caller provides buffer) */
void scr_save_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                   uint16_t __far *buf);

/* Restore rectangle from buffer */
void scr_restore_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                      uint16_t __far *buf);

/*---------------------------------------------------------------------------
 * Direct VRAM Access (low-level)
 *---------------------------------------------------------------------------*/

/* Write raw cell (character + attribute combined) at x,y */
void scr_write_cell(uint8_t x, uint8_t y, uint16_t cell);

/* Read raw cell at x,y */
uint16_t scr_read_cell(uint8_t x, uint8_t y);

/* Get character at x,y (extracts from cell) */
char scr_read_char(uint8_t x, uint8_t y);

/* Get attribute at x,y (extracts from cell) */
uint8_t scr_read_attr(uint8_t x, uint8_t y);

#endif /* SCREEN_H */
