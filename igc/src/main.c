/*
 * main.c - IGC File Manager Entry Point
 * Victor 9000 DOS 3.1 / Open Watcom v2
 *
 * A two-pane file manager inspired by Midnight Commander
 */

#include "igc.h"
#include "mem.h"
#include "screen.h"
#include "keyboard.h"
#include "dosapi.h"
#include "panel.h"
#include "ui.h"
#include "util.h"
#include "dialog.h"
#include "fileops.h"
#include "editor.h"
#include "config.h"
#include "serialfs.h"

/*---------------------------------------------------------------------------
 * Forward declarations
 *---------------------------------------------------------------------------*/
static void handle_key(KeyEvent *key);
static void handle_navigation(uint8_t code);
static void handle_enter(void);
static void serial_view_edit(Panel *p, FileEntry __far *f, bool_t edit);

/*---------------------------------------------------------------------------
 * Global state
 *---------------------------------------------------------------------------*/
static bool_t g_running = TRUE;
static bool_t g_need_redraw = TRUE;

/*---------------------------------------------------------------------------
 * handle_enter - Handle Enter key (enter directory or action)
 *---------------------------------------------------------------------------*/
static void handle_enter(void)
{
    Panel *p = panel_get_active();
    FileEntry __far *f = panel_get_cursor_file(p);

    if (f == (FileEntry __far *)0) return;

    if (file_is_dir(f)) {
        if (file_is_parent(f)) {
            panel_go_parent(p);
        } else {
            panel_change_dir(p, f->name);
        }
        g_need_redraw = TRUE;
    }
    /* TODO: For files, could launch viewer/editor */
}

/*---------------------------------------------------------------------------
 * handle_navigation - Handle navigation keys
 *---------------------------------------------------------------------------*/
static void handle_navigation(uint8_t code)
{
    Panel *p = panel_get_active();
    uint16_t old_cursor = p->cursor;
    uint16_t old_top = p->top;

    switch (code) {
        case KEY_UP:
            panel_cursor_up(p);
            ui_update_cursor(old_cursor, old_top);
            return;
        case KEY_DOWN:
            panel_cursor_down(p);
            ui_update_cursor(old_cursor, old_top);
            return;
        case KEY_HOME:
            panel_cursor_home(p);
            ui_update_cursor(old_cursor, old_top);
            return;
        case KEY_END:
            panel_cursor_end(p);
            ui_update_cursor(old_cursor, old_top);
            return;
        case KEY_PGUP:
            panel_page_up(p);
            ui_update_cursor(old_cursor, old_top);
            return;
        case KEY_PGDN:
            panel_page_down(p);
            ui_update_cursor(old_cursor, old_top);
            return;
        case KEY_LEFT:
            panel_go_parent(p);
            g_need_redraw = TRUE;
            return;
        case KEY_RIGHT:
            handle_enter();
            return;
    }
}

/*---------------------------------------------------------------------------
 * serial_view_edit - View/edit a file on a serial panel
 *
 * Downloads the remote file to a local temp, opens it in the viewer/editor,
 * and (when editing) uploads the result back under its original name.
 *---------------------------------------------------------------------------*/
static void serial_view_edit(Panel *p, FileEntry __far *f, bool_t edit)
{
    char remote[MAX_FULL_PATH];
    char name[14];
    char temp[16];
    uint8_t cur;

    /* Copy the 8.3 name out of far storage, form the remote relative path. */
    str_copy_n(name, f->name, 13);
    name[13] = '\0';
    serialfs_build_rel(p, name, remote);

    /* Temp file on the current local drive's root. */
    cur = dos_get_drive();
    temp[0] = (char)('A' + cur);
    temp[1] = ':';
    temp[2] = '\\';
    str_copy(&temp[3], "IGCVIEW.TMP");

    ui_status("Fetching from server...");
    if (serialfs_get_file(remote, temp) != 0) {
        ui_clear_status();
        ui_error("Cannot fetch remote file");
        kbd_wait();
        return;
    }
    ui_clear_status();

    if (edit) {
        editor_edit(temp);
        ui_status("Saving to server...");
        if (serialfs_put_file(temp, remote) != 0) {
            ui_clear_status();
            ui_error("Cannot save to server");
            kbd_wait();
        } else {
            ui_clear_status();
        }
        panel_read_dir(p);
    } else {
        editor_view(temp);
    }

    dos_delete(temp);
}

/*---------------------------------------------------------------------------
 * handle_fkey - Handle function keys
 *---------------------------------------------------------------------------*/
static void handle_fkey(uint8_t fkey_num)
{
    Panel *p;
    int drive;

    switch (fkey_num) {
        case 1:     /* F1: Drive */
            p = panel_get_active();
            drive = dlg_drive_select(p->drive);
            if (drive == DLG_DRIVE_SERIAL) {
                ui_status("Connecting to serial server...");
                if (serialfs_connect(SERIALFS_PORT_A, SERIALFS_BAUD_DEFAULT)) {
                    ui_clear_status();
                    p->backend = PANEL_SERIAL;
                    p->drive = 0;
                    p->path[0] = '\0';
                    p->cursor = 0;
                    p->top = 0;
                    panel_read_dir(p);      /* routes to the serial backend */
                    g_need_redraw = TRUE;
                } else {
                    ui_clear_status();
                    ui_error("No serial server on COM1");
                    kbd_wait();
                }
            } else if (drive >= 0) {
                panel_set_drive(p, (uint8_t)drive);
                g_need_redraw = TRUE;
            }
            ui_draw_fkey_bar();
            break;

        case 2:     /* F2: MkDir */
            fops_mkdir();
            g_need_redraw = TRUE;
            ui_draw_fkey_bar();
            break;

        case 3:     /* F3: View */
            {
                Panel *vp = panel_get_active();
                FileEntry __far *vf = panel_get_cursor_file(vp);
                if (vf != (FileEntry __far *)0 && !file_is_dir(vf)) {
                    if (vp->backend == PANEL_SERIAL) {
                        serial_view_edit(vp, vf, FALSE);
                    } else {
                        char vpath[MAX_FULL_PATH];
                        path_build(vpath, vp->drive, vp->path, vf->name);
                        editor_view(vpath);
                    }
                    g_need_redraw = TRUE;
                    ui_draw_frame();
                    ui_draw_headers();
                    ui_draw_fkey_bar();
                }
            }
            break;

        case 4:     /* F4: Edit */
            {
                Panel *ep = panel_get_active();
                FileEntry __far *ef = panel_get_cursor_file(ep);
                if (ef != (FileEntry __far *)0 && !file_is_dir(ef)) {
                    if (ep->backend == PANEL_SERIAL) {
                        serial_view_edit(ep, ef, TRUE);
                    } else {
                        char epath[MAX_FULL_PATH];
                        path_build(epath, ep->drive, ep->path, ef->name);
                        editor_edit(epath);
                    }
                    g_need_redraw = TRUE;
                    ui_draw_frame();
                    ui_draw_headers();
                    ui_draw_fkey_bar();
                }
            }
            break;

        case 5:     /* F5: Copy/Move */
            fops_copy_or_move();
            g_need_redraw = TRUE;
            ui_draw_fkey_bar();
            break;

        case 6:     /* F6: Delete */
            fops_delete();
            g_need_redraw = TRUE;
            ui_draw_fkey_bar();
            break;

        case 7:     /* F7: Quit */
        case 10:    /* F10: Quit (not shown on bar) */
            if (dlg_exit_confirm() == DLG_YES) {
                g_running = FALSE;
            }
            ui_draw_fkey_bar();
            break;

        case 8:     /* F8: Rename */
            fops_rename();
            g_need_redraw = TRUE;
            ui_draw_fkey_bar();
            break;
    }
}

/*---------------------------------------------------------------------------
 * handle_key - Process a key event
 *---------------------------------------------------------------------------*/
static void handle_key(KeyEvent *key)
{
    Panel *p;

    if (key->type == KEY_EXTENDED) {
        /* Navigation keys */
        if (kbd_is_nav(key)) {
            handle_navigation(key->code);
            return;
        }

        /* Function keys */
        if (kbd_is_fkey(key)) {
            handle_fkey(kbd_get_fkey_num(key));
            return;
        }
    }

    if (key->type == KEY_ASCII) {
        switch (key->code) {
            case KEY_TAB:
                panel_switch();
                ui_draw_title_bar();
                g_need_redraw = TRUE;
                break;

            case KEY_ENTER:
                handle_enter();
                break;

            case KEY_BACKSPACE:
                p = panel_get_active();
                panel_go_parent(p);
                g_need_redraw = TRUE;
                break;

            case KEY_SPACE:
                {
                    uint16_t old_cursor, old_top;
                    p = panel_get_active();
                    old_cursor = p->cursor;
                    old_top = p->top;
                    panel_toggle_selection(p);
                    panel_cursor_down(p);
                    ui_update_cursor(old_cursor, old_top);
                }
                break;

            case KEY_ESC:
            case 'q':
            case 'Q':
                if (dlg_exit_confirm() == DLG_YES) {
                    g_running = FALSE;
                }
                ui_draw_fkey_bar();
                break;

            default:
                break;
        }
    }
}

/*---------------------------------------------------------------------------
 * main_loop - Main event loop
 *---------------------------------------------------------------------------*/
static void main_loop(void)
{
    KeyEvent key;

    while (g_running) {
        /* Redraw if needed */
        if (g_need_redraw) {
            ui_draw_panels();
            ui_draw_title_bar();
            g_need_redraw = FALSE;
        }

        /* Wait for and handle key */
        key = kbd_wait();
        handle_key(&key);
    }
}

/*---------------------------------------------------------------------------
 * fatal_msg - Report a startup failure after the screen is restored
 *---------------------------------------------------------------------------*/
static void fatal_msg(const char *msg)
{
    dos_write(1, msg, str_len(msg));    /* handle 1 = stdout */
}

/*---------------------------------------------------------------------------
 * Main entry point
 *---------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
    /* Initialize memory system */
    mem_init();

    /* Point IGC.INI at the executable's directory (argv[0] is the full
     * program path on DOS 3.0+); falls back to the CWD otherwise. */
    config_init_path(argc > 0 ? argv[0] : (char *)0);

    /* Initialize screen */
    scr_init();
    scr_clear();
    scr_cursor_off();

    /* Initialize keyboard */
    kbd_init();

    /* Initialize panels */
    if (!panels_init()) {
        scr_cursor_on();
        scr_exit();
        fatal_msg("IGC: not enough memory for file lists\r\n");
        return 1;
    }

    /* Initialize file operations */
    if (!fops_init()) {
        panels_free();
        scr_cursor_on();
        scr_exit();
        fatal_msg("IGC: not enough memory for copy buffer\r\n");
        return 1;
    }

    /* Initialize editor */
    if (!editor_init()) {
        fops_shutdown();
        panels_free();
        scr_cursor_on();
        scr_exit();
        fatal_msg("IGC: not enough memory for editor\r\n");
        return 1;
    }

    /* Load configuration */
    {
        Config cfg;
        if (config_load(&cfg)) {
            config_apply(&cfg);
        }
    }

    /* Draw initial UI */
    ui_draw_frame();
    ui_draw_headers();
    ui_draw_fkey_bar();

    /* Load initial directories */
    panel_read_dir(&g_left_panel);
    panel_read_dir(&g_right_panel);

    /* Run main event loop */
    main_loop();

    /* Save configuration */
    {
        Config cfg;
        config_build(&cfg);
        config_save(&cfg);
    }

    /* Cleanup */
    editor_shutdown();
    fops_shutdown();
    panels_free();
    scr_clear();
    scr_cursor_on();
    scr_exit();
    mem_shutdown();

    return 0;
}
