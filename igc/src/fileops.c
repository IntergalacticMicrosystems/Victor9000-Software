/*
 * fileops.c - IGC File Operations Implementation
 */

#include "fileops.h"
#include "dialog.h"
#include "dosapi.h"
#include "mem.h"
#include "ui.h"
#include "util.h"
#include "screen.h"
#include "keyboard.h"

/*---------------------------------------------------------------------------
 * Static variables
 *---------------------------------------------------------------------------*/
static uint8_t __far *g_copy_buf = (uint8_t __far *)0;
static uint16_t g_copy_buf_size = 0;
static int g_overwrite_all = 0;     /* 1 = overwrite all without asking */
static uint16_t g_file_count = 0;   /* For progress display */
static uint16_t g_file_current = 0;

/*---------------------------------------------------------------------------
 * fops_init - Initialize file operations module
 *---------------------------------------------------------------------------*/
bool_t fops_init(void)
{
    uint8_t tier = mem_get_tier();

    /* Allocate copy buffer based on memory tier */
    switch (tier) {
        case MEM_HIGH:
            g_copy_buf_size = COPY_BUF_HIGH;
            break;
        case MEM_MEDIUM:
            g_copy_buf_size = COPY_BUF_MEDIUM;
            break;
        case MEM_LOW:
            g_copy_buf_size = COPY_BUF_LOW;
            break;
        case MEM_TINY:
        default:
            g_copy_buf_size = COPY_BUF_TINY;
            break;
    }

    g_copy_buf = (uint8_t __far *)mem_alloc(g_copy_buf_size);
    if (g_copy_buf == (uint8_t __far *)0) {
        /* Try smallest buffer */
        g_copy_buf_size = COPY_BUF_TINY;
        g_copy_buf = (uint8_t __far *)mem_alloc(g_copy_buf_size);
    }

    return (g_copy_buf != (uint8_t __far *)0) ? TRUE : FALSE;
}

/*---------------------------------------------------------------------------
 * fops_shutdown - Shutdown file operations module
 *---------------------------------------------------------------------------*/
void fops_shutdown(void)
{
    if (g_copy_buf != (uint8_t __far *)0) {
        mem_free(g_copy_buf);
        g_copy_buf = (uint8_t __far *)0;
    }
}

/*---------------------------------------------------------------------------
 * build_src_path - Build source path for current file
 *---------------------------------------------------------------------------*/
static void build_src_path(Panel *p, FileEntry __far *f, char *buf)
{
    buf[0] = 'A' + p->drive;
    buf[1] = ':';
    buf[2] = '\\';
    if (p->path[0] == '\\') {
        str_copy(&buf[3], &p->path[1]);
    } else if (p->path[0] != '\0') {
        str_copy(&buf[3], p->path);
    } else {
        buf[3] = '\0';
    }
    path_append(buf, f->name);
}

/*---------------------------------------------------------------------------
 * build_dst_path - Build destination path for target panel
 *---------------------------------------------------------------------------*/
static void build_dst_path(Panel *p, const char *filename, char *buf)
{
    buf[0] = 'A' + p->drive;
    buf[1] = ':';
    buf[2] = '\\';
    if (p->path[0] == '\\') {
        str_copy(&buf[3], &p->path[1]);
    } else if (p->path[0] != '\0') {
        str_copy(&buf[3], p->path);
    } else {
        buf[3] = '\0';
    }
    path_append(buf, filename);
}

/*---------------------------------------------------------------------------
 * fops_copy_file - Copy a single file
 *---------------------------------------------------------------------------*/
int fops_copy_file(const char *src, const char *dst)
{
    dos_handle_t src_h, dst_h;
    int16_t bytes_read, bytes_written;
    int result = FOPS_OK;

    /* Check if destination exists */
    if (dos_exists(dst)) {
        if (!g_overwrite_all) {
            int ow = dlg_overwrite(path_basename(dst));
            if (ow == DLG_NO) {
                return FOPS_SKIP;
            } else if (ow == 'A') {
                g_overwrite_all = 1;
            } else if (ow != DLG_YES) {
                return FOPS_CANCEL;
            }
        }
        /* Delete existing file */
        dos_delete(dst);
    }

    /* Open source file */
    src_h = dos_open(src, DOS_OPEN_READ);
    if (src_h < 0) {
        ui_error("Cannot open source file");
        kbd_wait();
        return FOPS_ERROR;
    }

    /* Create destination file */
    dst_h = dos_create(dst, 0);
    if (dst_h < 0) {
        dos_close(src_h);
        ui_error("Cannot create destination file");
        kbd_wait();
        return FOPS_ERROR;
    }

    /* Copy data */
    while (1) {
        bytes_read = dos_read(src_h, g_copy_buf, g_copy_buf_size);
        if (bytes_read < 0) {
            result = FOPS_ERROR;
            break;
        }
        if (bytes_read == 0) {
            break;  /* EOF */
        }

        bytes_written = dos_write(dst_h, g_copy_buf, (uint16_t)bytes_read);
        if (bytes_written != bytes_read) {
            result = FOPS_ERROR;
            break;
        }

        /* Check for user cancel (ESC) */
        if (kbd_check()) {
            KeyEvent key = kbd_get();
            if (key.code == KEY_ESC) {
                result = FOPS_CANCEL;
                break;
            }
        }
    }

    dos_close(src_h);
    dos_close(dst_h);

    if (result != FOPS_OK) {
        dos_delete(dst);  /* Clean up partial file */
    }

    return result;
}

/*---------------------------------------------------------------------------
 * fops_copy_dir - Copy directory recursively
 *---------------------------------------------------------------------------*/
int fops_copy_dir(const char *src, const char *dst)
{
    char src_path[80];
    char dst_path[80];
    DTA dta;
    DTA __far *old_dta;
    int result = FOPS_OK;

    /* Create destination directory */
    if (dos_mkdir(dst) != 0) {
        if (!dos_exists(dst)) {
            ui_error("Cannot create directory");
            kbd_wait();
            return FOPS_ERROR;
        }
    }

    /* Build search pattern */
    str_copy(src_path, src);
    path_append(src_path, "*.*");

    /* Save and set DTA */
    old_dta = dos_get_dta();
    dos_set_dta(&dta);

    /* Find first file */
    if (dos_find_first(src_path, 0x37) == 0) {
        do {
            /* Skip . and .. */
            if (dta.name[0] == '.') {
                if (dta.name[1] == '\0' ||
                    (dta.name[1] == '.' && dta.name[2] == '\0')) {
                    continue;
                }
            }

            /* Build full paths */
            str_copy(src_path, src);
            path_append(src_path, dta.name);
            str_copy(dst_path, dst);
            path_append(dst_path, dta.name);

            /* Update progress */
            g_file_current++;
            ui_show_progress("Copying", dta.name, g_file_current, g_file_count);

            if (dta.attr & DOS_ATTR_DIRECTORY) {
                /* Recurse into subdirectory */
                result = fops_copy_dir(src_path, dst_path);
            } else {
                /* Copy file */
                result = fops_copy_file(src_path, dst_path);
            }

            if (result == FOPS_CANCEL) {
                break;
            }
            /* Continue on SKIP or ERROR for single files */
            if (result == FOPS_SKIP) {
                result = FOPS_OK;
            }

        } while (dos_find_next() == 0 && result != FOPS_CANCEL);
    }

    /* Restore DTA */
    dos_set_dta(old_dta);

    return result;
}

/*---------------------------------------------------------------------------
 * fops_delete_file - Delete a single file
 *---------------------------------------------------------------------------*/
int fops_delete_file(const char *path)
{
    /* Clear read-only attribute if set */
    int16_t attr = dos_get_attr(path);
    if (attr >= 0 && (attr & 0x01)) {
        dos_set_attr(path, attr & ~0x01);
    }

    if (dos_delete(path) != 0) {
        ui_error("Cannot delete file");
        kbd_wait();
        return FOPS_ERROR;
    }

    return FOPS_OK;
}

/*---------------------------------------------------------------------------
 * fops_delete_dir - Delete directory recursively
 *---------------------------------------------------------------------------*/
int fops_delete_dir(const char *path)
{
    char full_path[80];
    DTA dta;
    DTA __far *old_dta;
    int result = FOPS_OK;

    /* Build search pattern */
    str_copy(full_path, path);
    path_append(full_path, "*.*");

    /* Save and set DTA */
    old_dta = dos_get_dta();
    dos_set_dta(&dta);

    /* Find first file */
    if (dos_find_first(full_path, 0x37) == 0) {
        do {
            /* Skip . and .. */
            if (dta.name[0] == '.') {
                if (dta.name[1] == '\0' ||
                    (dta.name[1] == '.' && dta.name[2] == '\0')) {
                    continue;
                }
            }

            /* Build full path */
            str_copy(full_path, path);
            path_append(full_path, dta.name);

            /* Update progress */
            g_file_current++;
            ui_show_progress("Deleting", dta.name, g_file_current, g_file_count);

            if (dta.attr & DOS_ATTR_DIRECTORY) {
                /* Recurse into subdirectory */
                result = fops_delete_dir(full_path);
            } else {
                /* Delete file */
                result = fops_delete_file(full_path);
            }

            if (result == FOPS_CANCEL) {
                break;
            }

            /* Check for user cancel (ESC) */
            if (kbd_check()) {
                KeyEvent key = kbd_get();
                if (key.code == KEY_ESC) {
                    result = FOPS_CANCEL;
                    break;
                }
            }

        } while (dos_find_next() == 0 && result != FOPS_CANCEL);
    }

    /* Restore DTA */
    dos_set_dta(old_dta);

    /* Remove the now-empty directory */
    if (result == FOPS_OK) {
        if (dos_rmdir(path) != 0) {
            ui_error("Cannot remove directory");
            kbd_wait();
            return FOPS_ERROR;
        }
    }

    return result;
}

/*---------------------------------------------------------------------------
 * count_selected_files - Count selected files in panel
 *---------------------------------------------------------------------------*/
static uint16_t count_selected_files(Panel *p)
{
    uint16_t count = 0;
    uint16_t i;

    for (i = 0; i < p->files.count; i++) {
        FileEntry __far *f = panel_get_file(p, i);
        if (f != (FileEntry __far *)0 && f->selected) {
            count++;
        }
    }

    return count;
}

/*---------------------------------------------------------------------------
 * fops_copy - Copy selected files to other panel
 *---------------------------------------------------------------------------*/
int fops_copy(void)
{
    Panel *src_panel = panel_get_active();
    Panel *dst_panel = panel_get_other();
    FileEntry __far *f;
    char src_path[80];
    char dst_path[80];
    uint16_t i;
    uint16_t selected;
    int result = FOPS_OK;

    /* Reset state */
    g_overwrite_all = 0;
    g_file_current = 0;

    /* Count selected files */
    selected = count_selected_files(src_panel);

    if (selected == 0) {
        /* No selection - copy cursor item */
        f = panel_get_cursor_file(src_panel);
        if (f == (FileEntry __far *)0) return FOPS_CANCEL;

        /* Don't copy . or .. */
        if (file_is_parent(f) || (f->name[0] == '.' && f->name[1] == '\0')) {
            return FOPS_CANCEL;
        }

        /* Confirm single file copy */
        if (dlg_copy_or_move(f->name) != 'C') {
            return FOPS_CANCEL;
        }

        build_src_path(src_panel, f, src_path);
        build_dst_path(dst_panel, f->name, dst_path);

        g_file_count = 1;
        ui_show_progress("Copying", f->name, 0, 1);

        if (file_is_dir(f)) {
            result = fops_copy_dir(src_path, dst_path);
        } else {
            result = fops_copy_file(src_path, dst_path);
        }
    } else {
        /* Copy all selected files */
        char msg[40];
        str_copy(msg, "Copy ");
        num_format(msg + str_len(msg), selected);
        str_copy(msg + str_len(msg), " files?");

        if (dlg_confirm("Confirm Copy", msg) != DLG_YES) {
            return FOPS_CANCEL;
        }

        g_file_count = selected;

        for (i = 0; i < src_panel->files.count && result != FOPS_CANCEL; i++) {
            f = panel_get_file(src_panel, i);
            if (f == (FileEntry __far *)0 || !f->selected) continue;

            build_src_path(src_panel, f, src_path);
            build_dst_path(dst_panel, f->name, dst_path);

            g_file_current++;
            ui_show_progress("Copying", f->name, g_file_current, g_file_count);

            if (file_is_dir(f)) {
                result = fops_copy_dir(src_path, dst_path);
            } else {
                result = fops_copy_file(src_path, dst_path);
            }

            if (result == FOPS_SKIP) {
                result = FOPS_OK;
            }
        }
    }

    ui_hide_progress();

    /* Refresh destination panel */
    panel_read_dir(dst_panel);

    return result;
}

/*---------------------------------------------------------------------------
 * fops_move - Move selected files to other panel
 *---------------------------------------------------------------------------*/
int fops_move(void)
{
    Panel *src_panel = panel_get_active();
    Panel *dst_panel = panel_get_other();
    FileEntry __far *f;
    char src_path[80];
    char dst_path[80];
    uint16_t i;
    uint16_t selected;
    int result = FOPS_OK;

    /* Reset state */
    g_overwrite_all = 0;
    g_file_current = 0;

    /* Count selected files */
    selected = count_selected_files(src_panel);

    if (selected == 0) {
        /* No selection - move cursor item */
        f = panel_get_cursor_file(src_panel);
        if (f == (FileEntry __far *)0) return FOPS_CANCEL;

        /* Don't move . or .. */
        if (file_is_parent(f) || (f->name[0] == '.' && f->name[1] == '\0')) {
            return FOPS_CANCEL;
        }

        /* Confirm single file move */
        if (dlg_copy_or_move(f->name) != 'M') {
            return FOPS_CANCEL;
        }

        build_src_path(src_panel, f, src_path);
        build_dst_path(dst_panel, f->name, dst_path);

        g_file_count = 1;
        ui_show_progress("Moving", f->name, 0, 1);

        /* Try rename first (fast if same drive) */
        if (src_panel->drive == dst_panel->drive) {
            if (dos_rename(src_path, dst_path) == 0) {
                result = FOPS_OK;
            } else {
                /* Rename failed, fall back to copy+delete */
                if (file_is_dir(f)) {
                    result = fops_copy_dir(src_path, dst_path);
                } else {
                    result = fops_copy_file(src_path, dst_path);
                }
                if (result == FOPS_OK) {
                    if (file_is_dir(f)) {
                        fops_delete_dir(src_path);
                    } else {
                        fops_delete_file(src_path);
                    }
                }
            }
        } else {
            /* Different drives - must copy+delete */
            if (file_is_dir(f)) {
                result = fops_copy_dir(src_path, dst_path);
            } else {
                result = fops_copy_file(src_path, dst_path);
            }
            if (result == FOPS_OK) {
                if (file_is_dir(f)) {
                    fops_delete_dir(src_path);
                } else {
                    fops_delete_file(src_path);
                }
            }
        }
    } else {
        /* Move all selected files */
        char msg[40];
        str_copy(msg, "Move ");
        num_format(msg + str_len(msg), selected);
        str_copy(msg + str_len(msg), " files?");

        if (dlg_confirm("Confirm Move", msg) != DLG_YES) {
            return FOPS_CANCEL;
        }

        g_file_count = selected;

        for (i = 0; i < src_panel->files.count && result != FOPS_CANCEL; i++) {
            f = panel_get_file(src_panel, i);
            if (f == (FileEntry __far *)0 || !f->selected) continue;

            build_src_path(src_panel, f, src_path);
            build_dst_path(dst_panel, f->name, dst_path);

            g_file_current++;
            ui_show_progress("Moving", f->name, g_file_current, g_file_count);

            /* Try rename first */
            if (src_panel->drive == dst_panel->drive &&
                dos_rename(src_path, dst_path) == 0) {
                continue;
            }

            /* Fall back to copy+delete */
            if (file_is_dir(f)) {
                result = fops_copy_dir(src_path, dst_path);
            } else {
                result = fops_copy_file(src_path, dst_path);
            }

            if (result == FOPS_OK) {
                if (file_is_dir(f)) {
                    fops_delete_dir(src_path);
                } else {
                    fops_delete_file(src_path);
                }
            } else if (result == FOPS_SKIP) {
                result = FOPS_OK;
            }
        }
    }

    ui_hide_progress();

    /* Refresh both panels */
    panel_read_dir(src_panel);
    panel_read_dir(dst_panel);

    return result;
}

/*---------------------------------------------------------------------------
 * fops_delete - Delete selected files
 *---------------------------------------------------------------------------*/
int fops_delete(void)
{
    Panel *panel = panel_get_active();
    FileEntry __far *f;
    char path[80];
    uint16_t i;
    uint16_t selected;
    int result = FOPS_OK;

    /* Reset state */
    g_file_current = 0;

    /* Count selected files */
    selected = count_selected_files(panel);

    if (selected == 0) {
        /* No selection - delete cursor item */
        f = panel_get_cursor_file(panel);
        if (f == (FileEntry __far *)0) return FOPS_CANCEL;

        /* Don't delete . or .. */
        if (file_is_parent(f) || (f->name[0] == '.' && f->name[1] == '\0')) {
            return FOPS_CANCEL;
        }

        /* Confirm deletion */
        if (dlg_delete_confirm(f->name, file_is_dir(f)) != DLG_YES) {
            return FOPS_CANCEL;
        }

        build_src_path(panel, f, path);

        g_file_count = 1;
        ui_show_progress("Deleting", f->name, 0, 1);

        if (file_is_dir(f)) {
            result = fops_delete_dir(path);
        } else {
            result = fops_delete_file(path);
        }
    } else {
        /* Delete all selected files */
        char msg[40];
        str_copy(msg, "Delete ");
        num_format(msg + str_len(msg), selected);
        str_copy(msg + str_len(msg), " files?");

        if (dlg_confirm("Confirm Delete", msg) != DLG_YES) {
            return FOPS_CANCEL;
        }

        g_file_count = selected;

        for (i = 0; i < panel->files.count && result != FOPS_CANCEL; i++) {
            f = panel_get_file(panel, i);
            if (f == (FileEntry __far *)0 || !f->selected) continue;

            build_src_path(panel, f, path);

            g_file_current++;
            ui_show_progress("Deleting", f->name, g_file_current, g_file_count);

            if (file_is_dir(f)) {
                result = fops_delete_dir(path);
            } else {
                result = fops_delete_file(path);
            }
        }
    }

    ui_hide_progress();

    /* Refresh panel */
    panel_read_dir(panel);

    return result;
}

/*---------------------------------------------------------------------------
 * fops_mkdir - Create directory
 *---------------------------------------------------------------------------*/
int fops_mkdir(void)
{
    Panel *panel = panel_get_active();
    char name[14];
    char path[80];

    name[0] = '\0';

    if (dlg_input("Make Directory", "Name:", name, 13) != DLG_OK) {
        return FOPS_CANCEL;
    }

    if (name[0] == '\0') {
        return FOPS_CANCEL;
    }

    /* Build full path */
    path[0] = 'A' + panel->drive;
    path[1] = ':';
    path[2] = '\\';
    if (panel->path[0] == '\\') {
        str_copy(&path[3], &panel->path[1]);
    } else if (panel->path[0] != '\0') {
        str_copy(&path[3], panel->path);
    } else {
        path[3] = '\0';
    }
    path_append(path, name);

    if (dos_mkdir(path) != 0) {
        ui_error("Cannot create directory");
        kbd_wait();
        return FOPS_ERROR;
    }

    /* Refresh panel */
    panel_read_dir(panel);

    return FOPS_OK;
}

/*---------------------------------------------------------------------------
 * fops_rename - Rename file/directory
 *---------------------------------------------------------------------------*/
int fops_rename(void)
{
    Panel *panel = panel_get_active();
    FileEntry __far *f;
    char old_name[14];
    char new_name[14];
    char old_path[80];
    char new_path[80];

    f = panel_get_cursor_file(panel);
    if (f == (FileEntry __far *)0) return FOPS_CANCEL;

    /* Don't rename . or .. */
    if (file_is_parent(f) || (f->name[0] == '.' && f->name[1] == '\0')) {
        return FOPS_CANCEL;
    }

    /* Copy current name */
    str_copy(old_name, f->name);
    str_copy(new_name, f->name);

    if (dlg_input("Rename", "New name:", new_name, 13) != DLG_OK) {
        return FOPS_CANCEL;
    }

    if (new_name[0] == '\0' || str_cmp(old_name, new_name) == 0) {
        return FOPS_CANCEL;
    }

    /* Build full paths */
    old_path[0] = 'A' + panel->drive;
    old_path[1] = ':';
    old_path[2] = '\\';
    if (panel->path[0] == '\\') {
        str_copy(&old_path[3], &panel->path[1]);
    } else if (panel->path[0] != '\0') {
        str_copy(&old_path[3], panel->path);
    } else {
        old_path[3] = '\0';
    }
    path_append(old_path, old_name);

    new_path[0] = 'A' + panel->drive;
    new_path[1] = ':';
    new_path[2] = '\\';
    if (panel->path[0] == '\\') {
        str_copy(&new_path[3], &panel->path[1]);
    } else if (panel->path[0] != '\0') {
        str_copy(&new_path[3], panel->path);
    } else {
        new_path[3] = '\0';
    }
    path_append(new_path, new_name);

    if (dos_rename(old_path, new_path) != 0) {
        ui_error("Cannot rename file");
        kbd_wait();
        return FOPS_ERROR;
    }

    /* Refresh panel */
    panel_read_dir(panel);

    return FOPS_OK;
}
