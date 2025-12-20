/*
 * dialog.c - IGC Modal Dialog System Implementation
 */

#include "dialog.h"
#include "screen.h"
#include "keyboard.h"
#include "mem.h"
#include "util.h"
#include "dosapi.h"

/*---------------------------------------------------------------------------
 * dlg_open - Open a dialog window
 *---------------------------------------------------------------------------*/
bool_t dlg_open(DialogWindow *win, uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                const char *title)
{
    uint32_t save_size;
    uint8_t i;
    uint16_t title_len;
    uint8_t title_x;

    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;

    /* Allocate save buffer */
    save_size = (uint32_t)w * h * 2;
    win->save = (uint16_t __far *)mem_alloc(save_size);

    if (win->save == (uint16_t __far *)0) {
        return FALSE;
    }

    /* Save background */
    scr_save_rect(x, y, w, h, win->save);

    /* Draw window border */
    /* Top border */
    scr_putc_xy(x, y, BOX_TL, ATTR_DIM);
    scr_hline(x + 1, y, w - 2, BOX_HORIZ, ATTR_DIM);
    scr_putc_xy(x + w - 1, y, BOX_TR, ATTR_DIM);

    /* Side borders */
    for (i = 1; i < h - 1; i++) {
        scr_putc_xy(x, y + i, BOX_VERT, ATTR_DIM);
        scr_fill_rect(x + 1, y + i, w - 2, 1, ' ', ATTR_DIM);
        scr_putc_xy(x + w - 1, y + i, BOX_VERT, ATTR_DIM);
    }

    /* Bottom border */
    scr_putc_xy(x, y + h - 1, BOX_BL, ATTR_DIM);
    scr_hline(x + 1, y + h - 1, w - 2, BOX_HORIZ, ATTR_DIM);
    scr_putc_xy(x + w - 1, y + h - 1, BOX_BR, ATTR_DIM);

    /* Draw title if provided */
    if (title != (const char *)0 && title[0] != '\0') {
        title_len = str_len(title);
        if (title_len > w - 4) title_len = w - 4;
        title_x = x + (w - title_len - 2) / 2;
        scr_putc_xy(title_x, y, ' ', ATTR_DIM_REV);
        scr_puts_n_xy(title_x + 1, y, title, title_len, ATTR_DIM_REV);
        scr_putc_xy(title_x + 1 + title_len, y, ' ', ATTR_DIM_REV);
    }

    return TRUE;
}

/*---------------------------------------------------------------------------
 * dlg_close - Close dialog window
 *---------------------------------------------------------------------------*/
void dlg_close(DialogWindow *win)
{
    if (win->save != (uint16_t __far *)0) {
        scr_restore_rect(win->x, win->y, win->w, win->h, win->save);
        mem_free(win->save);
        win->save = (uint16_t __far *)0;
    }
}

/*---------------------------------------------------------------------------
 * dlg_print - Draw text inside dialog
 *---------------------------------------------------------------------------*/
void dlg_print(DialogWindow *win, uint8_t x, uint8_t y, const char *text)
{
    scr_puts_xy(win->x + 1 + x, win->y + 1 + y, text, ATTR_DIM);
}

/*---------------------------------------------------------------------------
 * dlg_print_center - Draw text centered
 *---------------------------------------------------------------------------*/
void dlg_print_center(DialogWindow *win, uint8_t y, const char *text)
{
    uint16_t len = str_len(text);
    uint8_t x;

    if (len > win->w - 2) len = win->w - 2;
    x = (win->w - 2 - len) / 2;
    scr_puts_n_xy(win->x + 1 + x, win->y + 1 + y, text, len, ATTR_DIM);
}

/*---------------------------------------------------------------------------
 * draw_button - Draw a button
 *---------------------------------------------------------------------------*/
static void draw_button(uint8_t x, uint8_t y, const char *label, bool_t selected)
{
    uint8_t attr = selected ? ATTR_DIM_REV : ATTR_DIM;
    scr_putc_xy(x, y, '[', attr);
    scr_puts_xy(x + 1, y, label, attr);
    scr_putc_xy(x + 1 + str_len(label), y, ']', attr);
}

/*---------------------------------------------------------------------------
 * dlg_alert - Show alert with OK button
 *---------------------------------------------------------------------------*/
void dlg_alert(const char *title, const char *message)
{
    DialogWindow win;
    uint16_t msg_len = str_len(message);
    uint8_t w, h;

    /* Calculate window size */
    w = msg_len + 4;
    if (w < 20) w = 20;
    if (w > 60) w = 60;
    h = 6;

    /* Center window */
    if (!dlg_open(&win, (80 - w) / 2, (25 - h) / 2, w, h, title)) {
        return;
    }

    /* Draw message */
    dlg_print_center(&win, 1, message);

    /* Draw OK button */
    draw_button(win.x + (w - 4) / 2, win.y + 3, "OK", TRUE);

    /* Wait for any key */
    kbd_flush();
    kbd_wait();

    dlg_close(&win);
}

/*---------------------------------------------------------------------------
 * dlg_confirm - Show Yes/No confirmation
 *---------------------------------------------------------------------------*/
int dlg_confirm(const char *title, const char *message)
{
    DialogWindow win;
    uint16_t msg_len = str_len(message);
    uint8_t w, h;
    KeyEvent key;
    int selected = 1;  /* 0=Yes, 1=No - default to No */

    /* Calculate window size */
    w = msg_len + 4;
    if (w < 24) w = 24;
    if (w > 60) w = 60;
    h = 6;

    /* Center window */
    if (!dlg_open(&win, (80 - w) / 2, (25 - h) / 2, w, h, title)) {
        return DLG_NO;
    }

    /* Draw message */
    dlg_print_center(&win, 1, message);

    kbd_flush();

    while (1) {
        /* Draw buttons - centered: "[Yes]" (5) + 2 spaces + "[No]" (4) = 11 chars */
        {
            uint8_t btn_start = win.x + (w - 11) / 2;
            draw_button(btn_start, win.y + 3, "Yes", (selected == 0));
            draw_button(btn_start + 7, win.y + 3, "No", (selected == 1));
        }

        key = kbd_wait();

        if (key.type == KEY_ASCII) {
            switch (key.code) {
                case 'y':
                case 'Y':
                    dlg_close(&win);
                    return DLG_YES;
                case 'n':
                case 'N':
                case KEY_ESC:
                    dlg_close(&win);
                    return DLG_NO;
                case KEY_ENTER:
                    dlg_close(&win);
                    return (selected == 0) ? DLG_YES : DLG_NO;
                case KEY_TAB:
                    selected = 1 - selected;
                    break;
            }
        } else if (key.type == KEY_EXTENDED) {
            if (key.code == KEY_LEFT) {
                selected = 0;
            } else if (key.code == KEY_RIGHT) {
                selected = 1;
            } else if (key.code == KEY_F7 || key.code == KEY_F10) {
                dlg_close(&win);
                return DLG_NO;
            }
        }
    }
}

/*---------------------------------------------------------------------------
 * dlg_input - Input dialog
 *---------------------------------------------------------------------------*/
int dlg_input(const char *title, const char *prompt, char *buf, uint16_t maxlen)
{
    DialogWindow win;
    uint16_t prompt_len = str_len(prompt);
    uint8_t w, h;
    uint8_t input_x, input_w;
    uint16_t cursor = str_len(buf);
    KeyEvent key;
    int btn_selected = 0;  /* 0=OK, 1=Cancel */
    int in_buttons = 0;    /* 0=editing text, 1=in button area */

    /* Calculate window size */
    w = prompt_len + maxlen + 6;
    if (w < 30) w = 30;
    if (w > 70) w = 70;
    h = 6;

    input_w = w - 4;
    if (input_w > maxlen) input_w = maxlen;

    /* Center window */
    if (!dlg_open(&win, (80 - w) / 2, (25 - h) / 2, w, h, title)) {
        return DLG_CANCEL;
    }

    /* Draw prompt */
    dlg_print(&win, 1, 1, prompt);

    input_x = win.x + 2;

    kbd_flush();
    scr_cursor_on();

    while (1) {
        /* Draw input field */
        scr_fill_rect(input_x, win.y + 2, input_w, 1, ' ', ATTR_DIM_REV);
        scr_puts_n_xy(input_x, win.y + 2, buf, input_w, ATTR_DIM_REV);

        /* Draw buttons - centered: "[OK]" (4) + 2 spaces + "[Cancel]" (8) = 14 chars */
        {
            uint8_t btn_start = win.x + (w - 14) / 2;
            draw_button(btn_start, win.y + 4, "OK", (in_buttons && btn_selected == 0));
            draw_button(btn_start + 6, win.y + 4, "Cancel", (in_buttons && btn_selected == 1));
        }

        /* Position cursor */
        if (in_buttons) {
            scr_cursor_off();
        } else {
            scr_cursor_on();
            scr_gotoxy(input_x + cursor, win.y + 2);
        }

        key = kbd_wait();

        if (key.type == KEY_ASCII) {
            switch (key.code) {
                case KEY_ENTER:
                    scr_cursor_off();
                    dlg_close(&win);
                    if (in_buttons && btn_selected == 1) {
                        return DLG_CANCEL;
                    }
                    return DLG_OK;

                case KEY_ESC:
                    scr_cursor_off();
                    dlg_close(&win);
                    return DLG_CANCEL;

                case KEY_TAB:
                    if (in_buttons) {
                        btn_selected = 1 - btn_selected;
                    } else {
                        in_buttons = 1;
                        btn_selected = 0;
                    }
                    break;

                case KEY_BACKSPACE:
                    if (!in_buttons && cursor > 0) {
                        cursor--;
                        /* Shift remaining chars left */
                        {
                            uint16_t i;
                            for (i = cursor; buf[i]; i++) {
                                buf[i] = buf[i + 1];
                            }
                        }
                    }
                    break;

                default:
                    /* Printable character - return to text editing */
                    if (key.code >= 32 && key.code < 127) {
                        in_buttons = 0;
                        if (str_len(buf) < maxlen - 1) {
                            /* Insert character */
                            uint16_t i;
                            uint16_t len = str_len(buf);
                            for (i = len + 1; i > cursor; i--) {
                                buf[i] = buf[i - 1];
                            }
                            buf[cursor] = (char)key.code;
                            cursor++;
                        }
                    }
                    break;
            }
        } else if (key.type == KEY_EXTENDED) {
            switch (key.code) {
                case KEY_F7:
                case KEY_F10:
                    scr_cursor_off();
                    dlg_close(&win);
                    return DLG_CANCEL;
                case KEY_UP:
                    if (in_buttons) {
                        in_buttons = 0;
                    }
                    break;
                case KEY_DOWN:
                    if (!in_buttons) {
                        in_buttons = 1;
                        btn_selected = 0;
                    }
                    break;
                case KEY_LEFT:
                    if (in_buttons) {
                        btn_selected = 0;
                    } else if (cursor > 0) {
                        cursor--;
                    }
                    break;
                case KEY_RIGHT:
                    if (in_buttons) {
                        btn_selected = 1;
                    } else if (cursor < str_len(buf)) {
                        cursor++;
                    }
                    break;
                case KEY_HOME:
                    if (!in_buttons) cursor = 0;
                    break;
                case KEY_END:
                    if (!in_buttons) cursor = str_len(buf);
                    break;
                case KEY_DELETE:
                    if (!in_buttons && buf[cursor]) {
                        uint16_t i;
                        for (i = cursor; buf[i]; i++) {
                            buf[i] = buf[i + 1];
                        }
                    }
                    break;
            }
        }
    }
}

/*---------------------------------------------------------------------------
 * dlg_drive_select - Drive selection dialog
 *---------------------------------------------------------------------------*/
int dlg_drive_select(uint8_t current_drive)
{
    DialogWindow win;
    uint8_t drives[26];
    uint8_t drive_count = 0;
    uint8_t i;
    uint8_t selected = 0;
    KeyEvent key;

    /* Find valid drives */
    for (i = 0; i < 26; i++) {
        if (dos_is_drive_valid(i)) {
            if (i == current_drive) {
                selected = drive_count;
            }
            drives[drive_count++] = i;
        }
    }

    if (drive_count == 0) {
        return -1;
    }

    /* Open dialog */
    if (!dlg_open(&win, 30, 8, 20, drive_count + 4, "Select Drive")) {
        return -1;
    }

    kbd_flush();

    while (1) {
        /* Draw drive list */
        for (i = 0; i < drive_count; i++) {
            uint8_t attr = (i == selected) ? ATTR_DIM_REV : ATTR_DIM;
            char label[4];
            label[0] = ' ';
            label[1] = 'A' + drives[i];
            label[2] = ':';
            label[3] = '\0';
            scr_puts_n_xy(win.x + 2, win.y + 1 + i, label, 16, attr);
        }

        key = kbd_wait();

        if (key.type == KEY_ASCII) {
            switch (key.code) {
                case KEY_ENTER:
                    /* Check if drive is ready before selecting */
                    if (!dos_is_drive_ready(drives[selected])) {
                        dlg_alert("Error", "Drive not ready");
                        break;
                    }
                    dlg_close(&win);
                    return drives[selected];

                case KEY_ESC:
                    dlg_close(&win);
                    return -1;

                default:
                    /* Check for drive letter */
                    if ((key.code >= 'A' && key.code <= 'Z') ||
                        (key.code >= 'a' && key.code <= 'z')) {
                        uint8_t drive = char_upper((char)key.code) - 'A';
                        for (i = 0; i < drive_count; i++) {
                            if (drives[i] == drive) {
                                /* Check if drive is ready before selecting */
                                if (!dos_is_drive_ready(drive)) {
                                    dlg_alert("Error", "Drive not ready");
                                    break;
                                }
                                dlg_close(&win);
                                return drive;
                            }
                        }
                    }
                    break;
            }
        } else if (key.type == KEY_EXTENDED) {
            switch (key.code) {
                case KEY_UP:
                    if (selected > 0) selected--;
                    break;
                case KEY_DOWN:
                    if (selected < drive_count - 1) selected++;
                    break;
                case KEY_HOME:
                    selected = 0;
                    break;
                case KEY_END:
                    selected = drive_count - 1;
                    break;
                case KEY_F7:
                case KEY_F10:
                    dlg_close(&win);
                    return -1;
            }
        }
    }
}

/*---------------------------------------------------------------------------
 * dlg_copy_or_move - Copy/Move dialog
 *---------------------------------------------------------------------------*/
int dlg_copy_or_move(const char *filename)
{
    DialogWindow win;
    KeyEvent key;

    if (!dlg_open(&win, 20, 9, 40, 7, "File Operation")) {
        return 0;
    }

    dlg_print_center(&win, 1, filename);
    dlg_print_center(&win, 3, "(C)opy or (M)ove?");

    kbd_flush();

    while (1) {
        key = kbd_wait();

        if (key.type == KEY_ASCII) {
            switch (key.code) {
                case 'c':
                case 'C':
                    dlg_close(&win);
                    return 'C';
                case 'm':
                case 'M':
                    dlg_close(&win);
                    return 'M';
                case KEY_ESC:
                    dlg_close(&win);
                    return 0;
            }
        } else if (key.type == KEY_EXTENDED) {
            if (key.code == KEY_F7 || key.code == KEY_F10) {
                dlg_close(&win);
                return 0;
            }
        }
    }
}

/*---------------------------------------------------------------------------
 * dlg_delete_confirm - Delete confirmation dialog
 *---------------------------------------------------------------------------*/
int dlg_delete_confirm(const char *filename, bool_t is_dir)
{
    char msg[60];

    if (is_dir) {
        str_copy(msg, "Delete directory ");
    } else {
        str_copy(msg, "Delete ");
    }
    str_copy_n(msg + str_len(msg), filename, 30);
    str_copy(msg + str_len(msg), "?");

    return dlg_confirm("Confirm Delete", msg);
}

/*---------------------------------------------------------------------------
 * dlg_overwrite - Overwrite confirmation dialog
 *---------------------------------------------------------------------------*/
int dlg_overwrite(const char *filename)
{
    DialogWindow win;
    KeyEvent key;
    char msg[60];

    str_copy(msg, "Overwrite ");
    str_copy_n(msg + str_len(msg), filename, 30);
    str_copy(msg + str_len(msg), "?");

    if (!dlg_open(&win, 15, 9, 50, 7, "File Exists")) {
        return DLG_NO;
    }

    dlg_print_center(&win, 1, msg);
    dlg_print_center(&win, 3, "(Y)es / (N)o / (A)ll");

    kbd_flush();

    while (1) {
        key = kbd_wait();

        if (key.type == KEY_ASCII) {
            switch (key.code) {
                case 'y':
                case 'Y':
                    dlg_close(&win);
                    return DLG_YES;
                case 'n':
                case 'N':
                case KEY_ESC:
                    dlg_close(&win);
                    return DLG_NO;
                case 'a':
                case 'A':
                    dlg_close(&win);
                    return 'A';
            }
        } else if (key.type == KEY_EXTENDED) {
            if (key.code == KEY_F7 || key.code == KEY_F10) {
                dlg_close(&win);
                return DLG_NO;
            }
        }
    }
}

/*---------------------------------------------------------------------------
 * dlg_exit_confirm - Exit confirmation dialog
 *---------------------------------------------------------------------------*/
int dlg_exit_confirm(void)
{
    return dlg_confirm("Exit", "Are you sure you want to exit?");
}
