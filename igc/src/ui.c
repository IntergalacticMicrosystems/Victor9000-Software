/*
 * ui.c - IGC User Interface Drawing Implementation
 */

#include "ui.h"
#include "screen.h"
#include "util.h"
#include "mem.h"
#include "dosapi.h"
#include "serialfs.h"
#include "mouse.h"

/*---------------------------------------------------------------------------
 * Constants
 *---------------------------------------------------------------------------*/
#define LEFT_X      0
#define RIGHT_X     40
#define INNER_WIDTH 38      /* Width inside panel border */

/* Activity box: a bright window in the middle of the screen, growing down
 * from ACT_Y. Row 0 = top border with the title, row 1 = line 0 (message or
 * file name), then the progress bar (if active), then the non-empty extra
 * lines (hints), then the bottom border. One global box, no nesting. */
#define ACT_X         12
#define ACT_Y         8
#define ACT_W         56
#define ACT_MAXH      6
#define ACT_BAR_Y     (ACT_Y + 2)
#define ACT_TITLE_CAP 24    /* buffer sizes, including the NUL */
#define ACT_LINE_CAP  53

/* Progress-bar geometry, relative to ACT_X: "[20 cells] NNN%" centred in the
 * 54-cell interior. Shared by the full draw and the lightweight tick. */
#define PROG_BAR_X   15     /* first of the 20 bar cells */
#define PROG_BAR_W   20
#define PROG_PCT_X   37     /* "100%" field */

/* Last bar fill / percentage actually painted, so ui_progress_tick can repaint
 * only what changed. Set by ui_show_progress (the full draw establishes them). */
static uint8_t g_prog_bar_len;
static uint8_t g_prog_pct;

static uint8_t g_act_open;          /* box drawn, background in g_act_save */
static uint8_t g_act_bar;           /* 1 = progress bar row present */
static uint8_t g_act_h;             /* current height in rows */
static char g_act_title[ACT_TITLE_CAP];
static char g_act_line[3][ACT_LINE_CAP];
static uint16_t g_act_save[ACT_W * ACT_MAXH];

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

    /* Build stamp in bottom border, right-aligned before the corner:
     * "Mmm dd yyyy hh:mm" (17 chars, cols 61-77; __TIME__ is 24-hour, its
     * ":ss" is cut off by the length). The right panel's file count only
     * occupies cols 41-55, so this area is always clear. */
    {
        static const char build_date[] = __DATE__ " " __TIME__;
        scr_puts_n_xy(61, ROW_BOT_BORDER, build_date, 17, ATTR_DIM);
    }

    /* Clear rows 22-23 */
    scr_fill_rect(0, 22, 80, 1, ' ', ATTR_DIM);
    scr_fill_rect(0, ROW_STATUS, 80, 1, ' ', ATTR_DIM);

    /* F-key row */
    scr_fill_rect(0, ROW_FKEYS, 80, 1, ' ', ATTR_DIM);
}

/*---------------------------------------------------------------------------
 * ui_panel_path - Display form of a panel's location, at most 23 chars
 *
 * A serial panel shows the link ("SERA:\path" or "SERB:\path") rather than
 * a drive letter.
 *---------------------------------------------------------------------------*/
void ui_panel_path(const Panel *p, char *buf)
{
    const char *path = (p->path[0] == '\\') ? &p->path[1] : p->path;

    if (p->backend == PANEL_SERIAL) {
        buf[0] = 'S';
        buf[1] = 'E';
        buf[2] = 'R';
        buf[3] = 'A' + serialfs_port();
        buf[4] = ':';
        buf[5] = '\\';
        str_copy_n(&buf[6], path, 17);
    } else {
        buf[0] = 'A' + p->drive;
        buf[1] = ':';
        buf[2] = '\\';
        str_copy_n(&buf[3], path, 20);
    }
}

/*---------------------------------------------------------------------------
 * ui_draw_panel_path - Draw panel path in title bar
 *---------------------------------------------------------------------------*/
void ui_draw_panel_path(Panel *p, uint8_t x_offset, bool_t active)
{
    char buf[40];
    uint32_t free_kb;
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

    ui_panel_path(p, buf);
    path_len = str_len(buf);

    /* Draw path (one position after the border/separator) */
    scr_puts_n_xy(start_col + 1, ROW_TITLE, buf, path_len, path_attr);

    /* Serial panels have no local free-space figure (and querying drive 0
       would hit an empty floppy A: with a "not ready" critical error). */
    if (p->backend == PANEL_SERIAL) {
        return;
    }

    /* Draw free space (right-justified before panel separator) */
    free_kb = dos_get_free_space(p->drive);
    if (free_kb > 0) {
        /* Format as "NNNNk free", switching to "NNM free" for large drives */
        if (free_kb >= 10000L) {
            num_format_simple(buf, free_kb / 1024L);
            str_copy(buf + str_len(buf), "M free");
        } else {
            num_format_simple(buf, free_kb);
            str_copy(buf + str_len(buf), "k free");
        }
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
 * ui_draw_fkey_bar - Draw F-key bar (F1-F7: dim number, 8-char label)
 *
 * F8 is unassigned: its slot stays the dim-filled blank row.
 *---------------------------------------------------------------------------*/
void ui_draw_fkey_bar(void)
{
    static const char *labels[7] = {
        "Drive   ", "Mkdir   ", "View    ", "Edit    ",
        "File Ops", "More    ", "Quit    "
    };
    uint8_t i;
    uint8_t x;

    /* Clear row with dim attribute */
    scr_fill_rect(0, ROW_FKEYS, 80, 1, ' ', ATTR_DIM);

    for (i = 0; i < 7; i++) {
        x = i * 10;
        scr_putc_xy(x, ROW_FKEYS, '1' + i, ATTR_DIM);
        scr_puts_xy(x + 1, ROW_FKEYS, labels[i], ATTR_DIM_REV);
    }
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
 * Row 23: notices and errors
 *---------------------------------------------------------------------------*/
static void status_blank(void)
{
    scr_fill_rect(0, ROW_STATUS, 80, 1, ' ', ATTR_DIM);
}

void ui_notice(const char *msg)
{
    status_blank();
    scr_puts_xy(1, ROW_STATUS, msg, ATTR_DIM);
}

void ui_error(const char *msg)
{
    scr_fill_rect(0, ROW_STATUS, 80, 1, ' ', ATTR_DIM_REV);
    scr_puts_xy(1, ROW_STATUS, "ERROR: ", ATTR_DIM_REV);
    scr_puts_xy(8, ROW_STATUS, msg, ATTR_DIM_REV);
}

/*---------------------------------------------------------------------------
 * Activity box internals
 *---------------------------------------------------------------------------*/
static uint8_t calc_pct(uint16_t current, uint16_t total)
{
    uint32_t p;

    if (total == 0) return 0;
    p = (uint32_t)current * 100L / total;
    return (p > 100L) ? 100 : (uint8_t)p;
}

/* "NNN%" at the percent field of the bar row */
static void prog_pct(uint8_t pct)
{
    char buf[5];

    buf[0] = '0' + pct / 100;
    buf[1] = '0' + (pct / 10) % 10;
    buf[2] = '0' + pct % 10;
    buf[3] = '%';
    buf[4] = '\0';
    if (buf[0] == '0') {
        buf[0] = ' ';
        if (buf[1] == '0') buf[1] = ' ';
    }
    scr_puts_xy(ACT_X + PROG_PCT_X, ACT_BAR_Y, buf, ATTR_NORMAL);
}

/* Whole bar from the cached fill and percent (never drawn empty) */
static void act_bar(void)
{
    uint8_t i;

    scr_putc_xy(ACT_X + PROG_BAR_X - 1, ACT_BAR_Y, '[', ATTR_NORMAL);
    for (i = 0; i < PROG_BAR_W; i++) {
        scr_putc_xy(ACT_X + PROG_BAR_X + i, ACT_BAR_Y,
                    (i < g_prog_bar_len) ? 0xDB : 0xB0, ATTR_NORMAL);
    }
    scr_putc_xy(ACT_X + PROG_BAR_X + PROG_BAR_W, ACT_BAR_Y, ']', ATTR_NORMAL);
    prog_pct(g_prog_pct);
}

static void act_line(uint8_t n, uint8_t y)
{
    scr_puts_n_xy(ACT_X + 2, y, g_act_line[n], ACT_LINE_CAP - 1, ATTR_NORMAL);
}

/* Title area of the top border: every cell a title can occupy is reset to
 * border first, so a shorter or removed title leaves nothing behind. */
static void act_title(void)
{
    uint8_t len = (uint8_t)str_len(g_act_title);
    uint8_t x = ACT_X + (ACT_W - len - 2) / 2;

    scr_hline(ACT_X + (ACT_W - ACT_TITLE_CAP - 1) / 2, ACT_Y,
              ACT_TITLE_CAP + 2, BOX_HORIZ, ATTR_NORMAL);
    if (len) {
        scr_putc_xy(x, ACT_Y, ' ', ATTR_REVERSE);
        scr_puts_xy(x + 1, ACT_Y, g_act_title, ATTR_REVERSE);
        scr_putc_xy(x + 1 + len, ACT_Y, ' ', ATTR_REVERSE);
    }
}

/* Open, grow/shrink or refresh the box. The frame is redrawn only when the
 * height changes; otherwise title, line 0 and the bar are repainted, plus the
 * extra lines when 'extras' is set. */
static void act_show(bool_t extras)
{
    uint8_t h = 3 + g_act_bar + (g_act_line[1][0] != 0) + (g_act_line[2][0] != 0);
    uint8_t y, n;

    if (!g_act_open) {
        scr_save_rect(ACT_X, ACT_Y, ACT_W, ACT_MAXH, g_act_save);
        g_act_open = 1;
        g_act_h = 0;
    }
    if (h != g_act_h) {
        if (h < g_act_h) {      /* give the freed rows back first */
            scr_restore_rect(ACT_X, ACT_Y + h, ACT_W, g_act_h - h,
                             g_act_save + h * ACT_W);
        }
        g_act_h = h;
        scr_putc_xy(ACT_X, ACT_Y, BOX_TL, ATTR_NORMAL);
        scr_hline(ACT_X + 1, ACT_Y, ACT_W - 2, BOX_HORIZ, ATTR_NORMAL);
        scr_putc_xy(ACT_X + ACT_W - 1, ACT_Y, BOX_TR, ATTR_NORMAL);
        for (y = ACT_Y + 1; y < ACT_Y + h - 1; y++) {
            scr_putc_xy(ACT_X, y, BOX_VERT, ATTR_NORMAL);
            scr_fill_rect(ACT_X + 1, y, ACT_W - 2, 1, ' ', ATTR_NORMAL);
            scr_putc_xy(ACT_X + ACT_W - 1, y, BOX_VERT, ATTR_NORMAL);
        }
        scr_putc_xy(ACT_X, y, BOX_BL, ATTR_NORMAL);
        scr_hline(ACT_X + 1, y, ACT_W - 2, BOX_HORIZ, ATTR_NORMAL);
        scr_putc_xy(ACT_X + ACT_W - 1, y, BOX_BR, ATTR_NORMAL);
        extras = TRUE;
    }
    act_title();
    act_line(0, ACT_Y + 1);
    y = ACT_Y + 2;
    if (g_act_bar) {
        act_bar();
        y++;
    }
    if (extras) {
        for (n = 1; n < 3; n++) {
            if (g_act_line[n][0]) act_line(n, y++);
        }
    }
}

/*---------------------------------------------------------------------------
 * ui_status - Open/refresh the activity box with a plain message
 *---------------------------------------------------------------------------*/
void ui_status(const char *msg)
{
    status_blank();             /* an earlier ui_error must not linger */
    g_act_title[0] = '\0';
    g_act_bar = 0;
    str_copy_n(g_act_line[0], msg, ACT_LINE_CAP);
    act_show(FALSE);
}

/*---------------------------------------------------------------------------
 * ui_status_line - Set (or with NULL/"" remove) extra line n = 1..2
 *---------------------------------------------------------------------------*/
void ui_status_line(uint8_t n, const char *text)
{
    if (n < 1 || n > 2) return;
    str_copy_n(g_act_line[n], text ? text : "", ACT_LINE_CAP);
    if (g_act_open) act_show(TRUE);
}

/*---------------------------------------------------------------------------
 * ui_clear_status - Close and reset the activity box, blank row 23
 *---------------------------------------------------------------------------*/
void ui_clear_status(void)
{
    if (g_act_open) {
        scr_restore_rect(ACT_X, ACT_Y, ACT_W, g_act_h, g_act_save);
        mouse_new_context();
    }
    g_act_open = 0;
    g_act_bar = 0;
    g_act_h = 0;
    g_act_title[0] = '\0';
    g_act_line[0][0] = g_act_line[1][0] = g_act_line[2][0] = '\0';
    status_blank();
}

/*---------------------------------------------------------------------------
 * ui_show_progress - Title, file name and bar in the activity box
 *---------------------------------------------------------------------------*/
void ui_show_progress(const char *title, const char *filename,
                      uint16_t current, uint16_t total)
{
    status_blank();             /* an earlier ui_error must not linger */
    g_prog_pct = calc_pct(current, total);
    g_prog_bar_len = (g_prog_pct * PROG_BAR_W) / 100;
    str_copy_n(g_act_title, title, ACT_TITLE_CAP);
    str_copy_n(g_act_line[0], filename ? filename : "", ACT_LINE_CAP);
    g_act_bar = 1;
    act_show(FALSE);
}

/*---------------------------------------------------------------------------
 * ui_progress_tick - Cheap in-place progress update
 *
 * Updates only the bar cells that flipped since the last paint plus the
 * percentage digits: at most 20 + 4 video writes. The caller must have drawn
 * the frame once with ui_show_progress first (which sets the title, filename
 * and the delta baseline). This is deliberately tiny: it runs in the gap
 * between serial packets, and a full redraw there starves the Victor's polled
 * receive long enough to overrun its 3-byte FIFO at 38400.
 *---------------------------------------------------------------------------*/
void ui_progress_tick(uint16_t current, uint16_t total)
{
    uint8_t pct;
    uint8_t bar_len;
    uint8_t i;

    if (!g_act_bar || total == 0) {
        return;
    }
    pct = calc_pct(current, total);
    if (pct == g_prog_pct) {
        return;                 /* nothing visible changed - stay off the wire */
    }

    bar_len = (pct * PROG_BAR_W) / 100;
    for (i = g_prog_bar_len; i < bar_len; i++) {
        scr_putc_xy(ACT_X + PROG_BAR_X + i, ACT_BAR_Y, 0xDB, ATTR_NORMAL);
    }
    for (i = bar_len; i < g_prog_bar_len; i++) {
        scr_putc_xy(ACT_X + PROG_BAR_X + i, ACT_BAR_Y, 0xB0, ATTR_NORMAL);
    }
    g_prog_bar_len = bar_len;
    g_prog_pct = pct;
    prog_pct(pct);
}

/*---------------------------------------------------------------------------
 * ui_hide_progress - Close the activity box
 *---------------------------------------------------------------------------*/
void ui_hide_progress(void)
{
    ui_clear_status();
}
