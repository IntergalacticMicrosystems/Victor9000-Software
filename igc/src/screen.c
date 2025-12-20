/*
 * screen.c - IGC Screen/VRAM Access Implementation
 * Direct VRAM access for Victor 9000
 */

#include <i86.h>
#include "screen.h"

/*---------------------------------------------------------------------------
 * Global Screen State
 *---------------------------------------------------------------------------*/
ScreenState g_scr;

/*---------------------------------------------------------------------------
 * VRAM and CRTC Pointers (initialized once)
 *---------------------------------------------------------------------------*/
static uint16_t __far *g_vram = (uint16_t __far *)0;
static volatile uint8_t __far *g_crtc_sel = (uint8_t __far *)0;
static volatile uint8_t __far *g_crtc_data = (uint8_t __far *)0;

/*---------------------------------------------------------------------------
 * Pre-calculated line offset table
 * Avoids expensive multiplication at runtime
 *---------------------------------------------------------------------------*/
static const uint16_t line_offset[25] = {
       0,   80,  160,  240,  320,
     400,  480,  560,  640,  720,
     800,  880,  960, 1040, 1120,
    1200, 1280, 1360, 1440, 1520,
    1600, 1680, 1760, 1840, 1920
};

/*---------------------------------------------------------------------------
 * CRTC delay - required between register accesses
 *---------------------------------------------------------------------------*/
static void crtc_delay(void)
{
    volatile uint16_t i;
    for (i = 0; i < 50; i++) {
        /* Empty loop */
    }
}

/*---------------------------------------------------------------------------
 * CRTC register write
 *---------------------------------------------------------------------------*/
static void crtc_write(uint8_t reg, uint8_t value)
{
    *g_crtc_sel = reg;
    crtc_delay();
    *g_crtc_data = value;
}

/*---------------------------------------------------------------------------
 * CRTC register read
 *---------------------------------------------------------------------------*/
static uint8_t crtc_read(uint8_t reg)
{
    *g_crtc_sel = reg;
    crtc_delay();
    return *g_crtc_data;
}

/*---------------------------------------------------------------------------
 * Make a cell word from character and attribute
 * Cell format: bits 0-10 = glyph (char + CHAR_BASE), bits 11-15 = attr
 *---------------------------------------------------------------------------*/
static uint16_t make_cell(char c, uint8_t attr)
{
    uint16_t glyph = (uint16_t)(uint8_t)c + CHAR_BASE;
    return glyph | ((uint16_t)attr << 8);
}

/*---------------------------------------------------------------------------
 * Extract character from cell (subtract CHAR_BASE)
 *---------------------------------------------------------------------------*/
static char cell_to_char(uint16_t cell)
{
    uint16_t glyph = cell & 0x07FF;
    if (glyph >= CHAR_BASE) {
        return (char)(glyph - CHAR_BASE);
    }
    return 0;
}

/*---------------------------------------------------------------------------
 * Extract attribute from cell
 *---------------------------------------------------------------------------*/
static uint8_t cell_to_attr(uint16_t cell)
{
    return (uint8_t)(cell >> 8);
}

/*---------------------------------------------------------------------------
 * scr_init - Initialize screen system
 *---------------------------------------------------------------------------*/
void scr_init(void)
{
    /* Initialize VRAM pointer */
    g_vram = (uint16_t __far *)MK_FP(VRAM_SEG, 0);

    /* Initialize CRTC pointers */
    g_crtc_sel = (volatile uint8_t __far *)MK_FP(CRTC_SEG, CRTC_SEL_OFF);
    g_crtc_data = (volatile uint8_t __far *)MK_FP(CRTC_SEG, CRTC_DATA_OFF);

    /* Reset CRTC start address to 0 (in case DOS left it scrolled) */
    crtc_write(12, 0);  /* Start address high byte */
    crtc_write(13, 0);  /* Start address low byte */

    /* Initialize state */
    g_scr.cursor_x = 0;
    g_scr.cursor_y = 0;
    g_scr.attr = ATTR_NORMAL;
    g_scr.cursor_visible = TRUE;
}

/*---------------------------------------------------------------------------
 * scr_exit - Cleanup screen system
 *---------------------------------------------------------------------------*/
void scr_exit(void)
{
    /* Restore cursor */
    scr_cursor_on();
    scr_gotoxy(0, SCR_ROWS - 1);
}

/*---------------------------------------------------------------------------
 * scr_clear - Clear entire screen
 *---------------------------------------------------------------------------*/
void scr_clear(void)
{
    scr_fill_rect(0, 0, SCR_COLS, SCR_ROWS, ' ', g_scr.attr);
    scr_gotoxy(0, 0);
}

/*---------------------------------------------------------------------------
 * scr_clear_rect - Clear rectangle with spaces
 *---------------------------------------------------------------------------*/
void scr_clear_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    scr_fill_rect(x, y, w, h, ' ', g_scr.attr);
}

/*---------------------------------------------------------------------------
 * scr_gotoxy - Move cursor to position
 *---------------------------------------------------------------------------*/
void scr_gotoxy(uint8_t x, uint8_t y)
{
    uint16_t pos;

    if (x >= SCR_COLS) x = SCR_COLS - 1;
    if (y >= SCR_ROWS) y = SCR_ROWS - 1;

    g_scr.cursor_x = x;
    g_scr.cursor_y = y;

    /* Update hardware cursor position */
    pos = line_offset[y] + x;
    crtc_write(CRTC_CURSOR_MSB, (uint8_t)((pos >> 8) & 0x3F));
    crtc_write(CRTC_CURSOR_LSB, (uint8_t)(pos & 0xFF));
}

/*---------------------------------------------------------------------------
 * scr_getxy - Get current cursor position
 *---------------------------------------------------------------------------*/
void scr_getxy(uint8_t *x, uint8_t *y)
{
    *x = g_scr.cursor_x;
    *y = g_scr.cursor_y;
}

/*---------------------------------------------------------------------------
 * scr_cursor_on - Show hardware cursor
 *---------------------------------------------------------------------------*/
void scr_cursor_on(void)
{
    uint8_t reg10;

    reg10 = crtc_read(CRTC_CURSOR_START);
    reg10 &= 0x1F;              /* Keep raster bits */
    reg10 |= CURSOR_BLINK_NONE; /* Solid cursor */
    crtc_write(CRTC_CURSOR_START, reg10);

    g_scr.cursor_visible = TRUE;
}

/*---------------------------------------------------------------------------
 * scr_cursor_off - Hide hardware cursor
 *---------------------------------------------------------------------------*/
void scr_cursor_off(void)
{
    uint8_t reg10;

    reg10 = crtc_read(CRTC_CURSOR_START);
    reg10 &= 0x1F;          /* Keep raster bits */
    reg10 |= CURSOR_HIDDEN; /* Hidden mode */
    crtc_write(CRTC_CURSOR_START, reg10);

    g_scr.cursor_visible = FALSE;
}

/*---------------------------------------------------------------------------
 * scr_set_attr - Set current attribute
 *---------------------------------------------------------------------------*/
void scr_set_attr(uint8_t attr)
{
    g_scr.attr = attr;
}

/*---------------------------------------------------------------------------
 * scr_get_attr - Get current attribute
 *---------------------------------------------------------------------------*/
uint8_t scr_get_attr(void)
{
    return g_scr.attr;
}

/*---------------------------------------------------------------------------
 * scr_write_cell - Write raw cell at x,y
 *---------------------------------------------------------------------------*/
void scr_write_cell(uint8_t x, uint8_t y, uint16_t cell)
{
    uint16_t offset;

    if (x >= SCR_COLS || y >= SCR_ROWS) return;

    offset = line_offset[y] + x;
    g_vram[offset] = cell;
}

/*---------------------------------------------------------------------------
 * scr_read_cell - Read raw cell at x,y
 *---------------------------------------------------------------------------*/
uint16_t scr_read_cell(uint8_t x, uint8_t y)
{
    uint16_t offset;

    if (x >= SCR_COLS || y >= SCR_ROWS) return 0;

    offset = line_offset[y] + x;
    return g_vram[offset];
}

/*---------------------------------------------------------------------------
 * scr_read_char - Get character at x,y
 *---------------------------------------------------------------------------*/
char scr_read_char(uint8_t x, uint8_t y)
{
    return cell_to_char(scr_read_cell(x, y));
}

/*---------------------------------------------------------------------------
 * scr_read_attr - Get attribute at x,y
 *---------------------------------------------------------------------------*/
uint8_t scr_read_attr(uint8_t x, uint8_t y)
{
    return cell_to_attr(scr_read_cell(x, y));
}

/*---------------------------------------------------------------------------
 * scr_putc_xy - Write character at x,y with attribute
 *---------------------------------------------------------------------------*/
void scr_putc_xy(uint8_t x, uint8_t y, char c, uint8_t attr)
{
    scr_write_cell(x, y, make_cell(c, attr));
}

/*---------------------------------------------------------------------------
 * scr_putc - Write character at current position
 *---------------------------------------------------------------------------*/
void scr_putc(char c)
{
    scr_putc_xy(g_scr.cursor_x, g_scr.cursor_y, c, g_scr.attr);

    /* Advance cursor */
    g_scr.cursor_x++;
    if (g_scr.cursor_x >= SCR_COLS) {
        g_scr.cursor_x = 0;
        g_scr.cursor_y++;
        if (g_scr.cursor_y >= SCR_ROWS) {
            g_scr.cursor_y = SCR_ROWS - 1;
        }
    }
}

/*---------------------------------------------------------------------------
 * scr_puts_xy - Write string at x,y with attribute
 *---------------------------------------------------------------------------*/
void scr_puts_xy(uint8_t x, uint8_t y, const char *s, uint8_t attr)
{
    uint16_t offset;
    uint16_t cell;

    if (y >= SCR_ROWS) return;

    offset = line_offset[y] + x;

    while (*s && x < SCR_COLS) {
        cell = make_cell(*s, attr);
        g_vram[offset] = cell;
        offset++;
        x++;
        s++;
    }
}

/*---------------------------------------------------------------------------
 * scr_puts - Write string at current position
 *---------------------------------------------------------------------------*/
void scr_puts(const char *s)
{
    while (*s) {
        scr_putc(*s);
        s++;
    }
}

/*---------------------------------------------------------------------------
 * scr_puts_n_xy - Write string with max length at x,y
 *---------------------------------------------------------------------------*/
void scr_puts_n_xy(uint8_t x, uint8_t y, const char *s, uint8_t maxlen, uint8_t attr)
{
    uint16_t offset;
    uint16_t cell;
    uint8_t i;

    if (y >= SCR_ROWS) return;

    offset = line_offset[y] + x;

    for (i = 0; i < maxlen && x < SCR_COLS; i++) {
        if (*s) {
            cell = make_cell(*s, attr);
            s++;
        } else {
            cell = make_cell(' ', attr);
        }
        g_vram[offset] = cell;
        offset++;
        x++;
    }
}

/*---------------------------------------------------------------------------
 * scr_puts_n - Write string with max length at current position
 *---------------------------------------------------------------------------*/
void scr_puts_n(const char *s, uint8_t maxlen)
{
    scr_puts_n_xy(g_scr.cursor_x, g_scr.cursor_y, s, maxlen, g_scr.attr);
    g_scr.cursor_x += maxlen;
    if (g_scr.cursor_x >= SCR_COLS) {
        g_scr.cursor_x = SCR_COLS - 1;
    }
}

/*---------------------------------------------------------------------------
 * scr_fill_rect - Fill rectangle with character and attribute
 *---------------------------------------------------------------------------*/
void scr_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                   char c, uint8_t attr)
{
    uint16_t cell;
    uint16_t offset;
    uint8_t row, col;
    uint8_t x2, y2;

    cell = make_cell(c, attr);

    /* Clip to screen bounds */
    x2 = x + w;
    y2 = y + h;
    if (x2 > SCR_COLS) x2 = SCR_COLS;
    if (y2 > SCR_ROWS) y2 = SCR_ROWS;

    for (row = y; row < y2; row++) {
        offset = line_offset[row] + x;
        for (col = x; col < x2; col++) {
            g_vram[offset] = cell;
            offset++;
        }
    }
}

/*---------------------------------------------------------------------------
 * scr_hline - Draw horizontal line
 *---------------------------------------------------------------------------*/
void scr_hline(uint8_t x, uint8_t y, uint8_t len, char c, uint8_t attr)
{
    uint16_t cell;
    uint16_t offset;
    uint8_t i;

    if (y >= SCR_ROWS) return;

    cell = make_cell(c, attr);
    offset = line_offset[y] + x;

    for (i = 0; i < len && (x + i) < SCR_COLS; i++) {
        g_vram[offset] = cell;
        offset++;
    }
}

/*---------------------------------------------------------------------------
 * scr_vline - Draw vertical line
 *---------------------------------------------------------------------------*/
void scr_vline(uint8_t x, uint8_t y, uint8_t len, char c, uint8_t attr)
{
    uint16_t cell;
    uint8_t i;

    if (x >= SCR_COLS) return;

    cell = make_cell(c, attr);

    for (i = 0; i < len && (y + i) < SCR_ROWS; i++) {
        g_vram[line_offset[y + i] + x] = cell;
    }
}

/*---------------------------------------------------------------------------
 * scr_save_rect - Save rectangle to buffer
 *---------------------------------------------------------------------------*/
void scr_save_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                   uint16_t __far *buf)
{
    uint8_t row, col;
    uint16_t offset;

    for (row = 0; row < h && (y + row) < SCR_ROWS; row++) {
        offset = line_offset[y + row] + x;
        for (col = 0; col < w && (x + col) < SCR_COLS; col++) {
            *buf++ = g_vram[offset++];
        }
    }
}

/*---------------------------------------------------------------------------
 * scr_restore_rect - Restore rectangle from buffer
 *---------------------------------------------------------------------------*/
void scr_restore_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                      uint16_t __far *buf)
{
    uint8_t row, col;
    uint16_t offset;

    for (row = 0; row < h && (y + row) < SCR_ROWS; row++) {
        offset = line_offset[y + row] + x;
        for (col = 0; col < w && (x + col) < SCR_COLS; col++) {
            g_vram[offset++] = *buf++;
        }
    }
}

/*---------------------------------------------------------------------------
 * scr_put_uint16 - Write unsigned 16-bit number
 *---------------------------------------------------------------------------*/
void scr_put_uint16(uint16_t num)
{
    char buf[6];    /* Max 65535 = 5 digits + null */
    int i = 5;

    buf[5] = '\0';

    if (num == 0) {
        scr_putc('0');
        return;
    }

    while (num > 0 && i > 0) {
        i--;
        buf[i] = '0' + (num % 10);
        num /= 10;
    }

    scr_puts(&buf[i]);
}

/*---------------------------------------------------------------------------
 * scr_put_uint32 - Write unsigned 32-bit number
 *---------------------------------------------------------------------------*/
void scr_put_uint32(uint32_t num)
{
    char buf[11];   /* Max 4294967295 = 10 digits + null */
    int i = 10;

    buf[10] = '\0';

    if (num == 0) {
        scr_putc('0');
        return;
    }

    while (num > 0 && i > 0) {
        i--;
        buf[i] = '0' + (char)(num % 10L);
        num /= 10L;
    }

    scr_puts(&buf[i]);
}

/*---------------------------------------------------------------------------
 * scr_put_uint32_xy - Write number right-aligned at x,y
 *---------------------------------------------------------------------------*/
void scr_put_uint32_xy(uint8_t x, uint8_t y, uint32_t num, uint8_t width, uint8_t attr)
{
    char buf[11];
    int i = 10;
    int len;
    int pad;

    buf[10] = '\0';

    if (num == 0) {
        buf[9] = '0';
        i = 9;
    } else {
        while (num > 0 && i > 0) {
            i--;
            buf[i] = '0' + (char)(num % 10L);
            num /= 10L;
        }
    }

    len = 10 - i;
    pad = (width > len) ? width - len : 0;

    /* Output padding spaces */
    while (pad > 0 && x < SCR_COLS) {
        scr_putc_xy(x, y, ' ', attr);
        x++;
        pad--;
    }

    /* Output number */
    while (buf[i] && x < SCR_COLS) {
        scr_putc_xy(x, y, buf[i], attr);
        x++;
        i++;
    }
}
