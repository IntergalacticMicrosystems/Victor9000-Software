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

/*---------------------------------------------------------------------------
 * Forward declarations
 *---------------------------------------------------------------------------*/
static void handle_key(KeyEvent *key);
static void handle_navigation(uint8_t code);
static void handle_enter(void);

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
            if (drive >= 0) {
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
                    char vpath[80];
                    vpath[0] = 'A' + vp->drive;
                    vpath[1] = ':';
                    vpath[2] = '\\';
                    if (vp->path[0] == '\\') {
                        str_copy(&vpath[3], &vp->path[1]);
                    } else if (vp->path[0] != '\0') {
                        str_copy(&vpath[3], vp->path);
                    } else {
                        vpath[3] = '\0';
                    }
                    path_append(vpath, vf->name);
                    editor_view(vpath);
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
                    char epath[80];
                    epath[0] = 'A' + ep->drive;
                    epath[1] = ':';
                    epath[2] = '\\';
                    if (ep->path[0] == '\\') {
                        str_copy(&epath[3], &ep->path[1]);
                    } else if (ep->path[0] != '\0') {
                        str_copy(&epath[3], ep->path);
                    } else {
                        epath[3] = '\0';
                    }
                    path_append(epath, ef->name);
                    editor_edit(epath);
                    g_need_redraw = TRUE;
                    ui_draw_frame();
                    ui_draw_headers();
                    ui_draw_fkey_bar();
                }
            }
            break;

        case 5:     /* F5: Copy/Move */
            {
                Panel *cp = panel_get_active();
                FileEntry __far *cf = panel_get_cursor_file(cp);
                if (cf != (FileEntry __far *)0 &&
                    !file_is_parent(cf) &&
                    !(cf->name[0] == '.' && cf->name[1] == '\0')) {
                    int op = dlg_copy_or_move(cf->name);
                    if (op == 'C') {
                        fops_copy();
                    } else if (op == 'M') {
                        fops_move();
                    }
                }
            }
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
 * Main entry point
 *---------------------------------------------------------------------------*/
int main(void)
{
    /* Initialize memory system */
    mem_init();

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
        dos_exit(1);
        return 1;
    }

    /* Initialize file operations */
    if (!fops_init()) {
        panels_free();
        scr_cursor_on();
        scr_exit();
        dos_exit(1);
        return 1;
    }

    /* Initialize editor */
    if (!editor_init()) {
        fops_shutdown();
        panels_free();
        scr_cursor_on();
        scr_exit();
        dos_exit(1);
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
    dos_exit(0);

    return 0;
}
