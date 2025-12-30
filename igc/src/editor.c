/*
 * editor.c - IGC Text Editor/Viewer Implementation
 */

#include "editor.h"
#include "screen.h"
#include "keyboard.h"
#include "dosapi.h"
#include "mem.h"
#include "ui.h"
#include "util.h"
#include "dialog.h"

/*---------------------------------------------------------------------------
 * Static Variables
 *---------------------------------------------------------------------------*/
static Editor g_editor;
static uint16_t __far *g_screen_save = (uint16_t __far *)0;

/* Cut buffer for Ctrl-K/Ctrl-U (multi-line cut/paste) */
#define CUT_BUF_SIZE 4096
static char g_cut_buffer[CUT_BUF_SIZE];
static uint16_t g_cut_len = 0;
static bool_t g_last_was_cut = FALSE;  /* Track consecutive cuts */

/*---------------------------------------------------------------------------
 * editor_init - Initialize editor module
 *---------------------------------------------------------------------------*/
bool_t editor_init(void)
{
    uint8_t tier = mem_get_tier();

    /* Set buffer sizes based on memory tier */
    switch (tier) {
        case MEM_HIGH:
            g_editor.buf_size = EDIT_BUF_HIGH;
            g_editor.max_lines = EDIT_LINES_HIGH;
            break;
        case MEM_MEDIUM:
            g_editor.buf_size = EDIT_BUF_MEDIUM;
            g_editor.max_lines = EDIT_LINES_MEDIUM;
            break;
        case MEM_LOW:
            g_editor.buf_size = EDIT_BUF_LOW;
            g_editor.max_lines = EDIT_LINES_LOW;
            break;
        case MEM_TINY:
        default:
            g_editor.buf_size = EDIT_BUF_TINY;
            g_editor.max_lines = EDIT_LINES_TINY;
            break;
    }

    /* Allocate text buffer */
    g_editor.buffer = (char __far *)mem_alloc(g_editor.buf_size);
    if (g_editor.buffer == (char __far *)0) {
        /* Try smallest buffer */
        g_editor.buf_size = EDIT_BUF_TINY;
        g_editor.max_lines = EDIT_LINES_TINY;
        g_editor.buffer = (char __far *)mem_alloc(g_editor.buf_size);
        if (g_editor.buffer == (char __far *)0) {
            return FALSE;
        }
    }

    /* Allocate line offset table */
    g_editor.line_offs = (uint16_t __far *)mem_alloc(
        (uint32_t)g_editor.max_lines * sizeof(uint16_t));
    if (g_editor.line_offs == (uint16_t __far *)0) {
        mem_free(g_editor.buffer);
        g_editor.buffer = (char __far *)0;
        return FALSE;
    }

    /* Allocate screen save buffer */
    g_screen_save = (uint16_t __far *)mem_alloc(80 * 25 * 2);
    if (g_screen_save == (uint16_t __far *)0) {
        mem_free(g_editor.line_offs);
        mem_free(g_editor.buffer);
        return FALSE;
    }

    return TRUE;
}

/*---------------------------------------------------------------------------
 * editor_shutdown - Shutdown editor module
 *---------------------------------------------------------------------------*/
void editor_shutdown(void)
{
    if (g_screen_save != (uint16_t __far *)0) {
        mem_free(g_screen_save);
        g_screen_save = (uint16_t __far *)0;
    }
    if (g_editor.line_offs != (uint16_t __far *)0) {
        mem_free(g_editor.line_offs);
        g_editor.line_offs = (uint16_t __far *)0;
    }
    if (g_editor.buffer != (char __far *)0) {
        mem_free(g_editor.buffer);
        g_editor.buffer = (char __far *)0;
    }
}

/*---------------------------------------------------------------------------
 * parse_lines - Build line offset table
 *---------------------------------------------------------------------------*/
static void parse_lines(void)
{
    uint32_t i;
    uint16_t line = 0;

    g_editor.line_offs[0] = 0;
    line = 1;

    for (i = 0; i < g_editor.buf_used && line < g_editor.max_lines; i++) {
        if (g_editor.buffer[i] == '\n') {
            if (line < g_editor.max_lines) {
                g_editor.line_offs[line] = (uint16_t)(i + 1);
                line++;
            }
        }
    }

    g_editor.total_lines = line;
}

/*---------------------------------------------------------------------------
 * load_file - Load file into buffer
 *---------------------------------------------------------------------------*/
static bool_t load_file(const char *filename)
{
    dos_handle_t h;
    int16_t bytes;
    uint32_t total = 0;

    str_copy(g_editor.filename, filename);

    h = dos_open(filename, DOS_OPEN_READ);
    if (h < 0) {
        return FALSE;
    }

    /* Read file in chunks */
    while (total < g_editor.buf_size) {
        uint16_t to_read = (g_editor.buf_size - total > 4096) ?
                           4096 : (uint16_t)(g_editor.buf_size - total);
        bytes = dos_read(h, &g_editor.buffer[total], to_read);
        if (bytes <= 0) break;
        total += bytes;
    }

    dos_close(h);

    g_editor.buf_used = total;
    g_editor.modified = FALSE;
    g_editor.top_line = 0;
    g_editor.cursor_line = 0;
    g_editor.cursor_col = 0;
    g_editor.left_col = 0;

    /* Reset cut buffer state for new file */
    g_last_was_cut = FALSE;

    parse_lines();

    return TRUE;
}

/*---------------------------------------------------------------------------
 * save_file - Save buffer to file
 *---------------------------------------------------------------------------*/
static bool_t save_file(void)
{
    dos_handle_t h;
    int16_t bytes;
    uint32_t written = 0;

    h = dos_create(g_editor.filename, 0);
    if (h < 0) {
        return FALSE;
    }

    /* Write in chunks */
    while (written < g_editor.buf_used) {
        uint16_t to_write = (g_editor.buf_used - written > 4096) ?
                            4096 : (uint16_t)(g_editor.buf_used - written);
        bytes = dos_write(h, &g_editor.buffer[written], to_write);
        if (bytes < 0) {
            dos_close(h);
            return FALSE;
        }
        written += bytes;
    }

    dos_close(h);
    g_editor.modified = FALSE;

    return TRUE;
}

/*---------------------------------------------------------------------------
 * get_line_len - Get length of a line (excluding newline)
 *---------------------------------------------------------------------------*/
static uint16_t get_line_len(uint16_t line)
{
    uint16_t start, end;

    if (line >= g_editor.total_lines) return 0;

    start = g_editor.line_offs[line];
    if (line + 1 < g_editor.total_lines) {
        end = g_editor.line_offs[line + 1];
    } else {
        end = (uint16_t)g_editor.buf_used;
    }

    /* Exclude newline/CR */
    while (end > start &&
           (g_editor.buffer[end - 1] == '\n' ||
            g_editor.buffer[end - 1] == '\r')) {
        end--;
    }

    return end - start;
}

/*---------------------------------------------------------------------------
 * draw_status_bar - Draw status bar at top
 *---------------------------------------------------------------------------*/
static void draw_status_bar(void)
{
    char buf[80];
    uint16_t i;

    /* Clear status bar */
    scr_fill_rect(0, 0, 80, 1, ' ', ATTR_DIM_REV);

    /* Left: filename and modified indicator */
    str_copy(buf, " ");
    str_copy_n(buf + 1, g_editor.filename, 40);
    if (g_editor.modified) {
        str_copy(buf + str_len(buf), " [Modified]");
    }
    if (g_editor.readonly) {
        str_copy(buf + str_len(buf), " [View]");
    }
    scr_puts_xy(0, 0, buf, ATTR_DIM_REV);

    /* Right: line/col position */
    buf[0] = 'L';
    buf[1] = ':';
    num_format(&buf[2], g_editor.cursor_line + 1);
    i = str_len(buf);
    buf[i++] = ' ';
    buf[i++] = 'C';
    buf[i++] = ':';
    num_format(&buf[i], g_editor.cursor_col + 1);
    scr_puts_xy(80 - str_len(buf) - 1, 0, buf, ATTR_DIM_REV);
}

/*---------------------------------------------------------------------------
 * draw_help_bar - Draw help bar at bottom
 *---------------------------------------------------------------------------*/
static void draw_help_bar(void)
{
    scr_fill_rect(0, 24, 80, 1, ' ', ATTR_DIM);

    /* Entire label in dim+reverse, matching F-key bar style */
    if (g_editor.readonly) {
        scr_puts_xy(0, 24, "7Exit   ", ATTR_DIM_REV);
    } else {
        scr_puts_xy(0, 24, "2Save   ", ATTR_DIM_REV);
        scr_puts_xy(11, 24, "^K Cut  ", ATTR_DIM_REV);
        scr_puts_xy(22, 24, "^U Paste", ATTR_DIM_REV);
        scr_puts_xy(33, 24, "7Exit   ", ATTR_DIM_REV);
    }
}

/*---------------------------------------------------------------------------
 * draw_line - Draw a single line
 *---------------------------------------------------------------------------*/
static void draw_line(uint16_t screen_row, uint16_t line)
{
    uint8_t row = EDIT_TOP_ROW + screen_row;
    uint16_t start, end, len;
    uint16_t i;
    char c;

    /* Clear line */
    scr_fill_rect(0, row, 80, 1, ' ', ATTR_DIM);

    if (line >= g_editor.total_lines) {
        scr_putc_xy(0, row, '~', ATTR_DIM);
        return;
    }

    /* Get line start and end */
    start = g_editor.line_offs[line];
    if (line + 1 < g_editor.total_lines) {
        end = g_editor.line_offs[line + 1];
    } else {
        end = (uint16_t)g_editor.buf_used;
    }

    /* Remove trailing newline for display */
    while (end > start &&
           (g_editor.buffer[end - 1] == '\n' ||
            g_editor.buffer[end - 1] == '\r')) {
        end--;
    }

    len = end - start;

    /* Draw visible portion */
    for (i = 0; i < EDIT_COLS && (g_editor.left_col + i) < len; i++) {
        c = g_editor.buffer[start + g_editor.left_col + i];
        if (c == '\t') c = ' ';
        if (c < 32) c = '.';
        scr_putc_xy(EDIT_LEFT_COL + i, row, c, ATTR_DIM);
    }
}

/*---------------------------------------------------------------------------
 * draw_screen - Draw entire edit area
 *---------------------------------------------------------------------------*/
static void draw_screen(void)
{
    uint16_t i;

    for (i = 0; i < EDIT_ROWS; i++) {
        draw_line(i, g_editor.top_line + i);
    }

    draw_status_bar();
    draw_help_bar();
}

/*---------------------------------------------------------------------------
 * update_cursor - Update cursor position on screen
 *---------------------------------------------------------------------------*/
static void update_cursor(void)
{
    uint16_t screen_row = g_editor.cursor_line - g_editor.top_line;
    uint16_t screen_col = g_editor.cursor_col - g_editor.left_col;

    scr_gotoxy(EDIT_LEFT_COL + screen_col, EDIT_TOP_ROW + screen_row);
}

/*---------------------------------------------------------------------------
 * scroll_if_needed - Adjust viewport if cursor moved off-screen
 *---------------------------------------------------------------------------*/
static void scroll_if_needed(void)
{
    bool_t need_redraw = FALSE;

    /* Vertical scrolling */
    if (g_editor.cursor_line < g_editor.top_line) {
        g_editor.top_line = g_editor.cursor_line;
        need_redraw = TRUE;
    }
    if (g_editor.cursor_line >= g_editor.top_line + EDIT_ROWS) {
        g_editor.top_line = g_editor.cursor_line - EDIT_ROWS + 1;
        need_redraw = TRUE;
    }

    /* Horizontal scrolling */
    if (g_editor.cursor_col < g_editor.left_col) {
        g_editor.left_col = g_editor.cursor_col;
        need_redraw = TRUE;
    }
    if (g_editor.cursor_col >= g_editor.left_col + EDIT_COLS) {
        g_editor.left_col = g_editor.cursor_col - EDIT_COLS + 1;
        need_redraw = TRUE;
    }

    if (need_redraw) {
        draw_screen();
    }
}

/*---------------------------------------------------------------------------
 * clamp_cursor_col - Keep cursor within line bounds
 *---------------------------------------------------------------------------*/
static void clamp_cursor_col(void)
{
    uint16_t line_len = get_line_len(g_editor.cursor_line);
    if (g_editor.cursor_col > line_len) {
        g_editor.cursor_col = line_len;
    }
}

/*---------------------------------------------------------------------------
 * cursor_up - Move cursor up
 *---------------------------------------------------------------------------*/
static void cursor_up(void)
{
    if (g_editor.cursor_line > 0) {
        g_editor.cursor_line--;
        clamp_cursor_col();
        scroll_if_needed();
    }
}

/*---------------------------------------------------------------------------
 * cursor_down - Move cursor down
 *---------------------------------------------------------------------------*/
static void cursor_down(void)
{
    if (g_editor.cursor_line < g_editor.total_lines - 1) {
        g_editor.cursor_line++;
        clamp_cursor_col();
        scroll_if_needed();
    }
}

/*---------------------------------------------------------------------------
 * cursor_left - Move cursor left
 *---------------------------------------------------------------------------*/
static void cursor_left(void)
{
    if (g_editor.cursor_col > 0) {
        g_editor.cursor_col--;
        scroll_if_needed();
    } else if (g_editor.cursor_line > 0) {
        g_editor.cursor_line--;
        g_editor.cursor_col = get_line_len(g_editor.cursor_line);
        scroll_if_needed();
    }
}

/*---------------------------------------------------------------------------
 * cursor_right - Move cursor right
 *---------------------------------------------------------------------------*/
static void cursor_right(void)
{
    uint16_t line_len = get_line_len(g_editor.cursor_line);

    if (g_editor.cursor_col < line_len) {
        g_editor.cursor_col++;
        scroll_if_needed();
    } else if (g_editor.cursor_line < g_editor.total_lines - 1) {
        g_editor.cursor_line++;
        g_editor.cursor_col = 0;
        scroll_if_needed();
    }
}

/*---------------------------------------------------------------------------
 * cursor_home - Move cursor to start of line
 *---------------------------------------------------------------------------*/
static void cursor_home(void)
{
    g_editor.cursor_col = 0;
    scroll_if_needed();
}

/*---------------------------------------------------------------------------
 * cursor_end - Move cursor to end of line
 *---------------------------------------------------------------------------*/
static void cursor_end(void)
{
    g_editor.cursor_col = get_line_len(g_editor.cursor_line);
    scroll_if_needed();
}

/*---------------------------------------------------------------------------
 * page_up - Move up one page
 *---------------------------------------------------------------------------*/
static void page_up(void)
{
    if (g_editor.cursor_line >= EDIT_ROWS) {
        g_editor.cursor_line -= EDIT_ROWS;
    } else {
        g_editor.cursor_line = 0;
    }
    clamp_cursor_col();
    scroll_if_needed();
}

/*---------------------------------------------------------------------------
 * page_down - Move down one page
 *---------------------------------------------------------------------------*/
static void page_down(void)
{
    g_editor.cursor_line += EDIT_ROWS;
    if (g_editor.cursor_line >= g_editor.total_lines) {
        g_editor.cursor_line = g_editor.total_lines - 1;
    }
    clamp_cursor_col();
    scroll_if_needed();
}

/*---------------------------------------------------------------------------
 * get_cursor_offset - Get buffer offset for cursor position
 *---------------------------------------------------------------------------*/
static uint16_t get_cursor_offset(void)
{
    return g_editor.line_offs[g_editor.cursor_line] + g_editor.cursor_col;
}

/*---------------------------------------------------------------------------
 * insert_char - Insert character at cursor
 *---------------------------------------------------------------------------*/
static void insert_char(char c)
{
    uint16_t offset;
    uint32_t i;

    if (g_editor.readonly) return;
    if (g_editor.buf_used >= g_editor.buf_size - 1) return;

    offset = get_cursor_offset();

    /* Shift buffer contents right */
    for (i = g_editor.buf_used; i > offset; i--) {
        g_editor.buffer[i] = g_editor.buffer[i - 1];
    }
    g_editor.buffer[offset] = c;
    g_editor.buf_used++;

    /* Update line offsets */
    parse_lines();

    g_editor.cursor_col++;
    g_editor.modified = TRUE;

    /* Redraw current line */
    draw_line(g_editor.cursor_line - g_editor.top_line, g_editor.cursor_line);
    draw_status_bar();
}

/*---------------------------------------------------------------------------
 * insert_newline - Insert newline at cursor
 *---------------------------------------------------------------------------*/
static void insert_newline(void)
{
    if (g_editor.readonly) return;
    if (g_editor.buf_used >= g_editor.buf_size - 2) return;
    if (g_editor.total_lines >= g_editor.max_lines - 1) return;

    insert_char('\r');
    insert_char('\n');

    g_editor.cursor_line++;
    g_editor.cursor_col = 0;

    scroll_if_needed();
    draw_screen();
}

/*---------------------------------------------------------------------------
 * delete_char - Delete character at cursor
 *---------------------------------------------------------------------------*/
static void delete_char(void)
{
    uint16_t offset;
    uint32_t i;
    uint16_t del_count = 1;

    if (g_editor.readonly) return;

    offset = get_cursor_offset();
    if (offset >= g_editor.buf_used) return;

    /* Handle CRLF as a unit - if deleting CR followed by LF, delete both */
    if (g_editor.buffer[offset] == '\r' &&
        offset + 1 < g_editor.buf_used &&
        g_editor.buffer[offset + 1] == '\n') {
        del_count = 2;
    }

    /* Shift buffer contents left */
    for (i = offset; i < g_editor.buf_used - del_count; i++) {
        g_editor.buffer[i] = g_editor.buffer[i + del_count];
    }
    g_editor.buf_used -= del_count;

    /* Update line offsets */
    parse_lines();

    g_editor.modified = TRUE;
    clamp_cursor_col();
    draw_screen();
}

/*---------------------------------------------------------------------------
 * backspace - Delete character before cursor
 *---------------------------------------------------------------------------*/
static void backspace(void)
{
    if (g_editor.readonly) return;
    if (g_editor.cursor_col == 0 && g_editor.cursor_line == 0) return;

    cursor_left();
    delete_char();
}

/*---------------------------------------------------------------------------
 * cut_line - Cut current line to cut buffer (Ctrl-K)
 * Consecutive cuts append to the buffer for multi-line cut/paste.
 *---------------------------------------------------------------------------*/
static void cut_line(void)
{
    uint16_t start, end, len;
    uint32_t i;
    uint16_t copy_len;

    if (g_editor.readonly) return;
    if (g_editor.total_lines == 0) return;

    /* Get line boundaries */
    start = g_editor.line_offs[g_editor.cursor_line];
    if (g_editor.cursor_line + 1 < g_editor.total_lines) {
        end = g_editor.line_offs[g_editor.cursor_line + 1];
    } else {
        end = (uint16_t)g_editor.buf_used;
    }

    len = end - start;
    if (len == 0) return;

    /* If last action wasn't a cut, start fresh */
    if (!g_last_was_cut) {
        g_cut_len = 0;
    }

    /* Calculate how much we can copy (don't overflow cut buffer) */
    copy_len = len;
    if (g_cut_len + copy_len > CUT_BUF_SIZE - 1) {
        copy_len = CUT_BUF_SIZE - 1 - g_cut_len;
    }

    if (copy_len > 0) {
        /* Append line to cut buffer (including newline if present) */
        for (i = 0; i < copy_len; i++) {
            g_cut_buffer[g_cut_len + i] = g_editor.buffer[start + i];
        }
        g_cut_len += copy_len;
    }

    /* Delete line from buffer */
    for (i = start; i < g_editor.buf_used - len; i++) {
        g_editor.buffer[i] = g_editor.buffer[i + len];
    }
    g_editor.buf_used -= len;

    /* Update line offsets */
    parse_lines();

    /* Adjust cursor if needed */
    if (g_editor.cursor_line >= g_editor.total_lines && g_editor.total_lines > 0) {
        g_editor.cursor_line = g_editor.total_lines - 1;
    }
    g_editor.cursor_col = 0;

    g_editor.modified = TRUE;
    g_last_was_cut = TRUE;  /* Mark that last action was cut */
    draw_screen();
}

/*---------------------------------------------------------------------------
 * paste_line - Paste cut buffer at cursor (Ctrl-U)
 *---------------------------------------------------------------------------*/
static void paste_line(void)
{
    uint16_t offset;
    uint32_t i;

    if (g_editor.readonly) return;
    if (g_cut_len == 0) return;
    if (g_editor.buf_used + g_cut_len >= g_editor.buf_size) return;

    /* Insert at start of current line */
    offset = g_editor.line_offs[g_editor.cursor_line];

    /* Shift buffer contents right to make room */
    for (i = g_editor.buf_used; i > offset; i--) {
        g_editor.buffer[i + g_cut_len - 1] = g_editor.buffer[i - 1];
    }

    /* Copy cut buffer into position */
    for (i = 0; i < g_cut_len; i++) {
        g_editor.buffer[offset + i] = g_cut_buffer[i];
    }
    g_editor.buf_used += g_cut_len;

    /* Update line offsets */
    parse_lines();

    g_editor.modified = TRUE;
    draw_screen();
}

/*---------------------------------------------------------------------------
 * confirm_exit - Confirm exit if modified
 *---------------------------------------------------------------------------*/
static bool_t confirm_exit(void)
{
    if (!g_editor.modified) return TRUE;

    return (dlg_confirm("Exit", "Discard changes?") == DLG_YES);
}

/*---------------------------------------------------------------------------
 * editor_run - Main editor loop
 *---------------------------------------------------------------------------*/
static void editor_run(void)
{
    KeyEvent key;
    bool_t running = TRUE;

    draw_screen();
    scr_cursor_on();
    update_cursor();

    while (running) {
        key = kbd_wait();

        if (key.type == KEY_ASCII) {
            switch (key.code) {
                case KEY_ENTER:
                    g_last_was_cut = FALSE;
                    insert_newline();
                    break;

                case KEY_BACKSPACE:
                    g_last_was_cut = FALSE;
                    backspace();
                    break;

                case KEY_TAB:
                    g_last_was_cut = FALSE;
                    /* Insert spaces for tab */
                    insert_char(' ');
                    insert_char(' ');
                    insert_char(' ');
                    insert_char(' ');
                    break;

                case 11:    /* Ctrl-K: cut line (consecutive cuts accumulate) */
                    cut_line();
                    break;

                case 21:    /* Ctrl-U: paste line(s) */
                    g_last_was_cut = FALSE;
                    paste_line();
                    break;

                default:
                    g_last_was_cut = FALSE;
                    if (key.code >= 32 && key.code < 127) {
                        insert_char((char)key.code);
                    }
                    break;
            }
        } else if (key.type == KEY_EXTENDED) {
            /* Any navigation or other action breaks cut accumulation */
            g_last_was_cut = FALSE;

            switch (key.code) {
                case KEY_UP:
                    cursor_up();
                    break;
                case KEY_DOWN:
                    cursor_down();
                    break;
                case KEY_LEFT:
                    cursor_left();
                    break;
                case KEY_RIGHT:
                    cursor_right();
                    break;
                case KEY_HOME:
                    cursor_home();
                    break;
                case KEY_END:
                    cursor_end();
                    break;
                case KEY_PGUP:
                    page_up();
                    break;
                case KEY_PGDN:
                    page_down();
                    break;
                case KEY_DELETE:
                    delete_char();
                    break;
                case KEY_F2:
                    if (!g_editor.readonly) {
                        if (save_file()) {
                            ui_status("File saved");
                        } else {
                            ui_error("Save failed");
                        }
                        draw_screen();
                    }
                    break;
                case KEY_F7:
                case KEY_F10:
                    if (confirm_exit()) {
                        running = FALSE;
                    } else {
                        draw_screen();
                    }
                    break;
            }
        }

        update_cursor();
    }

    scr_cursor_off();
}

/*---------------------------------------------------------------------------
 * editor_view - View file (F3)
 *---------------------------------------------------------------------------*/
void editor_view(const char *filename)
{
    /* Save screen */
    scr_save_rect(0, 0, 80, 25, g_screen_save);
    scr_clear();

    g_editor.readonly = TRUE;

    if (load_file(filename)) {
        editor_run();
    } else {
        dlg_alert("Error", "Cannot open file");
    }

    /* Restore screen */
    scr_restore_rect(0, 0, 80, 25, g_screen_save);
}

/*---------------------------------------------------------------------------
 * editor_edit - Edit file (F4)
 *---------------------------------------------------------------------------*/
void editor_edit(const char *filename)
{
    /* Save screen */
    scr_save_rect(0, 0, 80, 25, g_screen_save);
    scr_clear();

    g_editor.readonly = FALSE;

    if (load_file(filename)) {
        editor_run();
    } else {
        /* New file */
        str_copy(g_editor.filename, filename);
        g_editor.buf_used = 0;
        g_editor.total_lines = 1;
        g_editor.line_offs[0] = 0;
        g_editor.modified = FALSE;
        g_editor.top_line = 0;
        g_editor.cursor_line = 0;
        g_editor.cursor_col = 0;
        g_editor.left_col = 0;
        editor_run();
    }

    /* Restore screen */
    scr_restore_rect(0, 0, 80, 25, g_screen_save);
}
