/*
 * ui.c - IGC User Interface Drawing Implementation
 */

#include "ui.h"
#include "screen.h"
#include "util.h"
#include "mem.h"
#include "dosapi.h"

/*---------------------------------------------------------------------------
 * Constants
 *---------------------------------------------------------------------------*/
#define LEFT_X      0
#define RIGHT_X     40
#define INNER_WIDTH 38      /* Width inside panel border */

/*---------------------------------------------------------------------------
 * ui_draw_frame - Draw complete UI frame
 *---------------------------------------------------------------------------*/
void ui_draw_frame(void)
{
    uint8_t i;

    /* Title bar corners (row 0) */
    scr_putc_xy(0, ROW_TITLE, BOX_TL, ATTR_DIM);
    scr_putc_xy(39, ROW_TITLE, BOX_T_DN, ATTR_DIM);
    scr_putc_xy(79, ROW_TITLE, BOX_TR, ATTR_DIM);

    /* Header row - vertical borders */
    scr_putc_xy(0, ROW_HEADER, BOX_VERT, ATTR_DIM);
    scr_putc_xy(39, ROW_HEADER, BOX_VERT, ATTR_DIM);
    scr_putc_xy(79, ROW_HEADER, BOX_VERT, ATTR_DIM);

    /* Side borders for file area */
    for (i = ROW_FILES_START; i <= ROW_FILES_END; i++) {
        scr_putc_xy(0, i, BOX_VERT, ATTR_DIM);
        scr_putc_xy(39, i, BOX_VERT, ATTR_DIM);
        scr_putc_xy(79, i, BOX_VERT, ATTR_DIM);
    }

    /* Bottom border */
    scr_putc_xy(0, ROW_BOT_BORDER, BOX_BL, ATTR_DIM);
    scr_hline(1, ROW_BOT_BORDER, 38, BOX_HORIZ, ATTR_DIM);
    scr_putc_xy(39, ROW_BOT_BORDER, BOX_T_UP, ATTR_DIM);
    scr_hline(40, ROW_BOT_BORDER, 39, BOX_HORIZ, ATTR_DIM);
    scr_putc_xy(79, ROW_BOT_BORDER, BOX_BR, ATTR_DIM);

    /* Clear rows 22-23 */
    scr_fill_rect(0, 22, 80, 1, ' ', ATTR_DIM);
    scr_fill_rect(0, ROW_STATUS, 80, 1, ' ', ATTR_DIM);

    /* F-key row */
    scr_fill_rect(0, ROW_FKEYS, 80, 1, ' ', ATTR_DIM);
}

/*---------------------------------------------------------------------------
 * ui_draw_panel_path - Draw panel path in title bar
 *---------------------------------------------------------------------------*/
void ui_draw_panel_path(Panel *p, uint8_t x_offset, bool_t active)
{
    char buf[40];
    uint16_t free_kb;
    uint8_t path_len, free_len;
    uint8_t free_start;
    uint8_t path_attr;
    uint8_t start_col, end_col;

    /* Path is only reverse on active panel */
    path_attr = active ? ATTR_DIM_REV : ATTR_DIM;

    /* Calculate column positions based on panel */
    if (x_offset == 0) {
        /* Left panel: columns 1-38 (after TL, before T-DN) */
        start_col = 1;
        end_col = 38;
    } else {
        /* Right panel: columns 40-78 (after T-DN, before TR) */
        start_col = 40;
        end_col = 78;
    }

    /* Fill title area with horizontal lines */
    scr_hline(start_col, ROW_TITLE, end_col - start_col + 1, BOX_HORIZ, ATTR_DIM);

    /* Build path string: "D:\path" */
    buf[0] = 'A' + p->drive;
    buf[1] = ':';
    buf[2] = '\\';

    if (p->path[0] == '\\') {
        str_copy_n(&buf[3], &p->path[1], 20);
    } else {
        str_copy_n(&buf[3], p->path, 20);
    }
    path_len = str_len(buf);

    /* Draw path (one position after the border/separator) */
    scr_puts_n_xy(start_col + 1, ROW_TITLE, buf, path_len, path_attr);

    /* Draw free space (right-justified before panel separator) */
    free_kb = (uint16_t)(dos_get_free_space(p->drive) & 0xFFFF);
    if (free_kb > 0) {
        /* Format as "NNNNk" without comma */
        num_format_simple(buf, free_kb);
        str_copy(buf + str_len(buf), "K");
        free_len = str_len(buf);
        free_start = end_col - free_len;
        scr_puts_xy(free_start, ROW_TITLE, buf, ATTR_DIM);
    }
}

/*---------------------------------------------------------------------------
 * ui_draw_title_bar - Draw title bar with panel paths
 *---------------------------------------------------------------------------*/
void ui_draw_title_bar(void)
{
    ui_draw_panel_path(&g_left_panel, LEFT_X, (g_active_panel == 0));
    ui_draw_panel_path(&g_right_panel, RIGHT_X, (g_active_panel == 1));
}

/*---------------------------------------------------------------------------
 * ui_draw_headers - Draw column headers
 *---------------------------------------------------------------------------*/
void ui_draw_headers(void)
{
    /* Clear header row */
    scr_fill_rect(1, ROW_HEADER, 38, 1, ' ', ATTR_DIM);
    scr_fill_rect(40, ROW_HEADER, 39, 1, ' ', ATTR_DIM);

    /* Left panel header - aligned with data columns */
    scr_puts_xy(2, ROW_HEADER, "Name", ATTR_DIM);
    scr_puts_xy(17, ROW_HEADER, "Size", ATTR_DIM);
    scr_puts_xy(26, ROW_HEADER, "Date", ATTR_DIM);

    /* Right panel header */
    scr_puts_xy(42, ROW_HEADER, "Name", ATTR_DIM);
    scr_puts_xy(57, ROW_HEADER, "Size", ATTR_DIM);
    scr_puts_xy(66, ROW_HEADER, "Date", ATTR_DIM);
}

/*---------------------------------------------------------------------------
 * ui_draw_fkey_bar - Draw F-key bar (F1-F7 layout, centered)
 *---------------------------------------------------------------------------*/
void ui_draw_fkey_bar(void)
{
    /* Clear row with dim attribute */
    scr_fill_rect(0, ROW_FKEYS, 80, 1, ' ', ATTR_DIM);

    /* F1-F7: dim number, 9-char reverse label, centered (starts at col 2) */
    scr_putc_xy(2, ROW_FKEYS, '1', ATTR_DIM);
    scr_puts_xy(3, ROW_FKEYS, "Drive    ", ATTR_DIM_REV);

    scr_putc_xy(13, ROW_FKEYS, '2', ATTR_DIM);
    scr_puts_xy(14, ROW_FKEYS, "Mkdir    ", ATTR_DIM_REV);

    scr_putc_xy(24, ROW_FKEYS, '3', ATTR_DIM);
    scr_puts_xy(25, ROW_FKEYS, "View     ", ATTR_DIM_REV);

    scr_putc_xy(35, ROW_FKEYS, '4', ATTR_DIM);
    scr_puts_xy(36, ROW_FKEYS, "Edit     ", ATTR_DIM_REV);

    scr_putc_xy(46, ROW_FKEYS, '5', ATTR_DIM);
    scr_puts_xy(47, ROW_FKEYS, "CpyMov   ", ATTR_DIM_REV);

    scr_putc_xy(57, ROW_FKEYS, '6', ATTR_DIM);
    scr_puts_xy(58, ROW_FKEYS, "Delete   ", ATTR_DIM_REV);

    scr_putc_xy(68, ROW_FKEYS, '7', ATTR_DIM);
    scr_puts_xy(69, ROW_FKEYS, "Quit     ", ATTR_DIM_REV);
}

/*---------------------------------------------------------------------------
 * ui_draw_panel_row - Draw a single row in a panel
 *---------------------------------------------------------------------------*/
void ui_draw_panel_row(Panel *p, uint8_t x_offset, bool_t active, uint16_t file_idx)
{
    uint8_t row;
    FileEntry __far *f;
    char size_buf[10];
    char date_buf[10];
    uint8_t attr;
    bool_t is_cursor;

    /* Check if file_idx is visible */
    if (file_idx < p->top || file_idx >= p->top + PANEL_HEIGHT) {
        return;
    }

    row = ROW_FILES_START + (file_idx - p->top);

    /* Clear the row first */
    scr_fill_rect(x_offset + 1, row, INNER_WIDTH, 1, ' ', ATTR_DIM);

    if (file_idx >= p->files.count) {
        return;
    }

    f = panel_get_file(p, file_idx);
    if (f == (FileEntry __far *)0) return;

    is_cursor = (file_idx == p->cursor && active);

    /* Only cursor row gets reverse video */
    if (is_cursor) {
        attr = ATTR_DIM_REV;
        scr_fill_rect(x_offset + 1, row, INNER_WIDTH, 1, ' ', attr);
    } else {
        attr = ATTR_DIM;
    }

    /* Draw selection star (if selected and not cursor) */
    if (f->selected && !is_cursor) {
        scr_putc_xy(x_offset + 1, row, '*', ATTR_DIM);
    }

    /* Draw name (directories with angle brackets) */
    if (file_is_dir(f)) {
        char dir_name[16];
        dir_name[0] = '<';
        str_copy_n(&dir_name[1], f->name, 12);
        str_copy(dir_name + str_len(dir_name), ">");
        scr_puts_n_xy(x_offset + 2, row, dir_name, 14, attr);
    } else {
        scr_puts_n_xy(x_offset + 2, row, f->name, 14, attr);
    }

    /* Draw size */
    file_format_size(f, size_buf);
    scr_puts_n_xy(x_offset + 17, row, size_buf, 8, attr);

    /* Draw date */
    file_format_date(f, date_buf);
    scr_puts_n_xy(x_offset + 26, row, date_buf, 12, attr);
}

/*---------------------------------------------------------------------------
 * ui_draw_panel - Draw a single panel
 *---------------------------------------------------------------------------*/
void ui_draw_panel(Panel *p, uint8_t x_offset, bool_t active)
{
    uint16_t i;
    uint16_t file_idx;
    uint8_t row;

    /* Clear panel area */
    for (row = ROW_FILES_START; row <= ROW_FILES_END; row++) {
        scr_fill_rect(x_offset + 1, row, INNER_WIDTH, 1, ' ', ATTR_DIM);
    }

    /* Draw files */
    for (i = 0; i < PANEL_HEIGHT; i++) {
        file_idx = p->top + i;

        if (file_idx >= p->files.count) {
            break;
        }

        ui_draw_panel_row(p, x_offset, active, file_idx);
    }

    /* Show truncation indicator */
    if (p->files.truncated) {
        scr_puts_xy(x_offset + 2, ROW_FILES_END, "[...more files]", ATTR_DIM);
    }

    /* Show file count on bottom border */
    {
        char count_buf[20];
        /* Clear area with horizontal lines first to remove old text */
        scr_hline(x_offset + 1, ROW_BOT_BORDER, 15, BOX_HORIZ, ATTR_DIM);
        /* Format and draw the count */
        num_format(count_buf, p->files.count);
        str_copy(count_buf + str_len(count_buf), " files");
        scr_puts_xy(x_offset + 2, ROW_BOT_BORDER, count_buf, ATTR_DIM);
    }
}

/*---------------------------------------------------------------------------
 * ui_draw_panels - Draw both panels
 *---------------------------------------------------------------------------*/
void ui_draw_panels(void)
{
    ui_draw_panel(&g_left_panel, LEFT_X, (g_active_panel == 0));
    ui_draw_panel(&g_right_panel, RIGHT_X, (g_active_panel == 1));
}

/*---------------------------------------------------------------------------
 * ui_update_cursor - Efficient cursor update (redraws only affected rows)
 *---------------------------------------------------------------------------*/
void ui_update_cursor(uint16_t old_cursor, uint16_t old_top)
{
    Panel *p = panel_get_active();
    uint8_t x_offset = (g_active_panel == 0) ? LEFT_X : RIGHT_X;

    /* If view scrolled, need full panel redraw */
    if (old_top != p->top) {
        ui_draw_panel(p, x_offset, TRUE);
        return;
    }

    /* Just redraw old and new cursor rows */
    if (old_cursor != p->cursor) {
        ui_draw_panel_row(p, x_offset, TRUE, old_cursor);
        ui_draw_panel_row(p, x_offset, TRUE, p->cursor);
    }
}

/*---------------------------------------------------------------------------
 * ui_status - Show status message
 *---------------------------------------------------------------------------*/
void ui_status(const char *msg)
{
    scr_fill_rect(0, ROW_STATUS, 80, 1, ' ', ATTR_DIM);
    scr_puts_xy(1, ROW_STATUS, msg, ATTR_DIM);
}

/*---------------------------------------------------------------------------
 * ui_error - Show error message
 *---------------------------------------------------------------------------*/
void ui_error(const char *msg)
{
    scr_fill_rect(0, ROW_STATUS, 80, 1, ' ', ATTR_DIM_REV);
    scr_puts_xy(1, ROW_STATUS, "ERROR: ", ATTR_DIM_REV);
    scr_puts(msg);
}

/*---------------------------------------------------------------------------
 * ui_clear_status - Clear status line
 *---------------------------------------------------------------------------*/
void ui_clear_status(void)
{
    scr_fill_rect(0, ROW_STATUS, 80, 1, ' ', ATTR_DIM);
}

/*---------------------------------------------------------------------------
 * ui_show_loading - Show loading message
 *---------------------------------------------------------------------------*/
void ui_show_loading(void)
{
    ui_status("Loading...");
}

/*---------------------------------------------------------------------------
 * ui_hide_loading - Hide loading message
 *---------------------------------------------------------------------------*/
void ui_hide_loading(void)
{
    ui_clear_status();
}

/*---------------------------------------------------------------------------
 * ui_show_progress - Show progress display
 *---------------------------------------------------------------------------*/
void ui_show_progress(const char *title, const char *filename,
                      uint16_t current, uint16_t total)
{
    char buf[80];
    uint8_t pct;
    uint8_t bar_len;
    uint8_t i;

    /* Calculate percentage */
    if (total > 0) {
        pct = (uint8_t)((uint32_t)current * 100L / total);
    } else {
        pct = 0;
    }

    /* Build status line */
    str_copy(buf, title);
    str_copy(buf + str_len(buf), ": ");
    if (filename != (const char *)0) {
        str_copy_n(buf + str_len(buf), filename, 30);
        str_copy(buf + str_len(buf), " ");
    }

    /* Show on status line */
    scr_fill_rect(0, ROW_STATUS, 80, 1, ' ', ATTR_DIM);
    scr_puts_xy(1, ROW_STATUS, buf, ATTR_DIM);

    /* Draw progress bar */
    bar_len = (pct * 20) / 100;
    scr_putc_xy(55, ROW_STATUS, '[', ATTR_DIM);
    for (i = 0; i < 20; i++) {
        scr_putc_xy(56 + i, ROW_STATUS, (i < bar_len) ? 0xDB : 0xB0, ATTR_DIM);
    }
    scr_putc_xy(76, ROW_STATUS, ']', ATTR_DIM);

    /* Show percentage */
    buf[0] = '0' + (pct / 100) % 10;
    buf[1] = '0' + (pct / 10) % 10;
    buf[2] = '0' + pct % 10;
    buf[3] = '%';
    buf[4] = '\0';
    if (buf[0] == '0') {
        buf[0] = ' ';
        if (buf[1] == '0') buf[1] = ' ';
    }
    scr_puts_xy(78, ROW_STATUS, buf, ATTR_DIM);
}

/*---------------------------------------------------------------------------
 * ui_hide_progress - Hide progress display
 *---------------------------------------------------------------------------*/
void ui_hide_progress(void)
{
    ui_clear_status();
}
