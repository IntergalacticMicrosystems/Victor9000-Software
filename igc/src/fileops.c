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
#include "serialfs.h"

/*---------------------------------------------------------------------------
 * Static variables
 *---------------------------------------------------------------------------*/
/* Recursion guard for copy/delete - each level uses ~250 bytes of stack */
#define MAX_DIR_DEPTH 24

static uint8_t __far *g_copy_buf = (uint8_t __far *)0;
static uint16_t g_copy_buf_size = 0;
static int g_overwrite_all = 0;     /* 1 = overwrite all without asking */
static uint16_t g_file_count = 0;   /* For progress display */
static uint16_t g_file_current = 0;
static uint8_t g_dir_depth = 0;

/*---------------------------------------------------------------------------
 * fops_init - Initialize file operations module
 *---------------------------------------------------------------------------*/
bool_t fops_init(void)
{
    /* The copy buffer is now allocated on demand around an actual copy/move
     * (copybuf_acquire) and freed afterwards, so an idle IGC holds no copy
     * heap. Nothing to allocate at startup. */
    g_copy_buf = (uint8_t __far *)0;
    g_copy_buf_size = 0;
    return TRUE;
}

/*---------------------------------------------------------------------------
 * copybuf_acquire - Allocate the copy buffer for a copy/move operation
 * Idempotent: a no-op if the buffer is already held, so the recursive copy
 * paths can call it per file yet only allocate once per batch.
 *---------------------------------------------------------------------------*/
static bool_t copybuf_acquire(void)
{
    if (g_copy_buf != (uint8_t __far *)0) {
        return TRUE;
    }

    /* Copy buffer size for the current tier, computed by mem_init */
    g_copy_buf_size = mem_get_copy_buf_size();
    g_copy_buf = (uint8_t __far *)mem_alloc(g_copy_buf_size);
    if (g_copy_buf == (uint8_t __far *)0) {
        /* Try smallest buffer */
        g_copy_buf_size = COPY_BUF_TINY;
        g_copy_buf = (uint8_t __far *)mem_alloc(g_copy_buf_size);
    }
    if (g_copy_buf == (uint8_t __far *)0) {
        g_copy_buf_size = 0;
        return FALSE;
    }
    return TRUE;
}

/*---------------------------------------------------------------------------
 * copybuf_release - Free the copy buffer after a copy/move operation
 *---------------------------------------------------------------------------*/
static void copybuf_release(void)
{
    if (g_copy_buf != (uint8_t __far *)0) {
        mem_free(g_copy_buf);
        g_copy_buf = (uint8_t __far *)0;
        g_copy_buf_size = 0;
    }
}

/*---------------------------------------------------------------------------
 * fops_shutdown - Shutdown file operations module
 *---------------------------------------------------------------------------*/
void fops_shutdown(void)
{
    copybuf_release();
}

/*---------------------------------------------------------------------------
 * build_src_path - Build source path for current file
 *---------------------------------------------------------------------------*/
static void build_src_path(Panel *p, FileEntry __far *f, char *buf)
{
    path_build(buf, p->drive, p->path, f->name);
}

/*---------------------------------------------------------------------------
 * build_dst_path - Build destination path for target panel
 *---------------------------------------------------------------------------*/
static void build_dst_path(Panel *p, const char *filename, char *buf)
{
    path_build(buf, p->drive, p->path, filename);
}

/*---------------------------------------------------------------------------
 * path_is_inside - TRUE if dst equals src or lies inside it
 *---------------------------------------------------------------------------*/
static bool_t path_is_inside(const char *src, const char *dst)
{
    uint16_t len = str_len(src);
    uint16_t i;

    for (i = 0; i < len; i++) {
        if (char_upper(src[i]) != char_upper(dst[i])) {
            return FALSE;
        }
    }

    return (dst[len] == '\0' || dst[len] == '\\') ? TRUE : FALSE;
}

/*---------------------------------------------------------------------------
 * same_location - TRUE if both panels show the same drive and directory
 *---------------------------------------------------------------------------*/
static bool_t same_location(Panel *a, Panel *b)
{
    const char *pa = a->path;
    const char *pb = b->path;

    if (a->drive != b->drive) {
        return FALSE;
    }

    /* Normalize the leading backslash, as build_src_path does */
    if (pa[0] == '\\') pa++;
    if (pb[0] == '\\') pb++;

    return (str_cmp_i(pa, pb) == 0) ? TRUE : FALSE;
}

/*---------------------------------------------------------------------------
 * fops_copy_file - Copy a single file
 *---------------------------------------------------------------------------*/
int fops_copy_file(const char *src, const char *dst)
{
    dos_handle_t src_h, dst_h;
    int16_t bytes_read, bytes_written;
    int16_t src_attr;
    uint16_t src_date = 0, src_time = 0;
    bool_t overwrite = FALSE;
    int result = FOPS_OK;

    /* Ensure the copy buffer is allocated (no-op if a batch already holds it). */
    if (!copybuf_acquire()) {
        ui_error("Not enough memory for copy buffer");
        kbd_wait();
        return FOPS_ERROR;
    }

    /* Copying a file onto itself would delete the source in the
       overwrite path below before it is ever opened */
    if (str_cmp_i(src, dst) == 0) {
        ui_error("Source and destination are the same file");
        kbd_wait();
        return FOPS_ERROR;
    }

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
        overwrite = TRUE;
    }

    src_attr = dos_get_attr(src);

    /* Open source file */
    src_h = dos_open(src, DOS_OPEN_READ);
    if (src_h < 0) {
        ui_error("Cannot open source file");
        kbd_wait();
        return FOPS_ERROR;
    }

    /* Source timestamp, preserved on the copy */
    if (dos_get_ftime(src_h, &src_date, &src_time) != 0) {
        src_date = 0;
    }

    /* Delete the existing destination only now that the source is open, so a
       failed source open can't destroy the destination with nothing to replace
       it. The delete also clears a read-only attribute dos_create trips on. */
    if (overwrite && fops_delete_file(dst) != FOPS_OK) {
        dos_close(src_h);
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
            ui_error("Read error");
            kbd_wait();
            result = FOPS_ERROR;
            break;
        }
        if (bytes_read == 0) {
            break;  /* EOF */
        }

        /* DOS reports a full disk as a short write, not an error */
        bytes_written = dos_write(dst_h, g_copy_buf, (uint16_t)bytes_read);
        if (bytes_written != bytes_read) {
            ui_error("Write error - disk full?");
            kbd_wait();
            result = FOPS_ERROR;
            break;
        }

        /* Check for user cancel (ESC) */
        if (kbd_check()) {
            KeyEvent key = kbd_get();
            if (key.type == KEY_ASCII && key.code == KEY_ESC) {
                result = FOPS_CANCEL;
                break;
            }
        }
    }

    /* Stamp the copy with the source's date/time (applied at close) */
    if (result == FOPS_OK && src_date != 0) {
        dos_set_ftime(dst_h, src_date, src_time);
    }

    dos_close(src_h);
    dos_close(dst_h);

    if (result != FOPS_OK) {
        dos_delete(dst);  /* Clean up partial file */
    } else if (src_attr > 0) {
        dos_set_attr(dst, (uint8_t)src_attr);
    }

    return result;
}

/*---------------------------------------------------------------------------
 * fops_copy_dir - Copy directory recursively
 *---------------------------------------------------------------------------*/
int fops_copy_dir(const char *src, const char *dst)
{
    char src_path[MAX_FULL_PATH];
    char dst_path[MAX_FULL_PATH];
    DTA dta;
    DTA __far *old_dta;
    bool_t failed = FALSE;
    int result = FOPS_OK;

    /* Copying a directory into itself would recurse endlessly: the new
       destination shows up in the live enumeration of the source */
    if (path_is_inside(src, dst)) {
        ui_error("Cannot copy directory into itself");
        kbd_wait();
        return FOPS_ERROR;
    }

    /* Guard the stack against runaway recursion */
    if (g_dir_depth >= MAX_DIR_DEPTH) {
        ui_error("Directory tree too deep");
        kbd_wait();
        return FOPS_ERROR;
    }

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
    if (!path_join(src_path, "*.*", MAX_FULL_PATH)) {
        ui_error("Path too long");
        kbd_wait();
        return FOPS_ERROR;
    }

    /* Save and set DTA */
    g_dir_depth++;
    old_dta = dos_get_dta();
    dos_set_dta(&dta);

    /* Find first file */
    if (dos_find_first(src_path, 0x37) == 0) {
        do {
            int rc;

            /* Skip . and .. */
            if (dta.name[0] == '.') {
                if (dta.name[1] == '\0' ||
                    (dta.name[1] == '.' && dta.name[2] == '\0')) {
                    continue;
                }
            }

            /* Build full paths */
            str_copy(src_path, src);
            str_copy(dst_path, dst);
            if (!path_join(src_path, dta.name, MAX_FULL_PATH) ||
                !path_join(dst_path, dta.name, MAX_FULL_PATH)) {
                ui_error("Path too long");
                kbd_wait();
                result = FOPS_ERROR;
                break;
            }

            /* Show the nested name against the top-level counters */
            ui_show_progress("Copying", dta.name, g_file_current, g_file_count);

            if (dta.attr & DOS_ATTR_DIRECTORY) {
                /* Recurse into subdirectory */
                rc = fops_copy_dir(src_path, dst_path);
            } else {
                /* Copy file */
                rc = fops_copy_file(src_path, dst_path);
            }

            if (rc == FOPS_CANCEL) {
                result = FOPS_CANCEL;
                break;
            }
            /* Keep copying the remaining entries on a per-file error or a
               user skip, but remember it: the tree is only complete if every
               file made it, and a move must never delete a source whose copy
               failed or was skipped. */
            if (rc != FOPS_OK) {
                failed = TRUE;
            }

        } while (dos_find_next() == 0);
    }

    /* Restore DTA */
    dos_set_dta(old_dta);
    g_dir_depth--;

    if (result == FOPS_OK && failed) {
        result = FOPS_ERROR;
    }
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
    char full_path[MAX_FULL_PATH];
    DTA dta;
    DTA __far *old_dta;
    bool_t failed = FALSE;
    int result = FOPS_OK;

    /* Guard the stack against runaway recursion */
    if (g_dir_depth >= MAX_DIR_DEPTH) {
        ui_error("Directory tree too deep");
        kbd_wait();
        return FOPS_ERROR;
    }

    /* Build search pattern */
    str_copy(full_path, path);
    if (!path_join(full_path, "*.*", MAX_FULL_PATH)) {
        ui_error("Path too long");
        kbd_wait();
        return FOPS_ERROR;
    }

    /* Save and set DTA */
    g_dir_depth++;
    old_dta = dos_get_dta();
    dos_set_dta(&dta);

    /* Find first file */
    if (dos_find_first(full_path, 0x37) == 0) {
        do {
            int rc;

            /* Skip . and .. */
            if (dta.name[0] == '.') {
                if (dta.name[1] == '\0' ||
                    (dta.name[1] == '.' && dta.name[2] == '\0')) {
                    continue;
                }
            }

            /* Build full path */
            str_copy(full_path, path);
            if (!path_join(full_path, dta.name, MAX_FULL_PATH)) {
                ui_error("Path too long");
                kbd_wait();
                result = FOPS_ERROR;
                break;
            }

            /* Show the nested name against the top-level counters */
            ui_show_progress("Deleting", dta.name, g_file_current, g_file_count);

            if (dta.attr & DOS_ATTR_DIRECTORY) {
                /* Recurse into subdirectory */
                rc = fops_delete_dir(full_path);
            } else {
                /* Delete file */
                rc = fops_delete_file(full_path);
            }

            if (rc == FOPS_CANCEL) {
                result = FOPS_CANCEL;
                break;
            }
            /* Keep deleting the rest, but remember the failure - the final
               rmdir below is skipped, since the directory is not empty. */
            if (rc != FOPS_OK) {
                failed = TRUE;
            }

            /* Check for user cancel (ESC) */
            if (kbd_check()) {
                KeyEvent key = kbd_get();
                if (key.type == KEY_ASCII && key.code == KEY_ESC) {
                    result = FOPS_CANCEL;
                    break;
                }
            }

        } while (dos_find_next() == 0);
    }

    /* Restore DTA */
    dos_set_dta(old_dta);
    g_dir_depth--;

    if (result == FOPS_OK && failed) {
        result = FOPS_ERROR;
    }

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
 * fops_copy_serial - Copy/move files between a local panel and a serial panel
 *
 * Handles one DOS panel and one serial panel (serial-to-serial is refused).
 * Files transfer directly; directories copy recursively (single or nested) via
 * the serialfs tree helpers. Selected items, or the cursor item if none are
 * selected.
 *---------------------------------------------------------------------------*/
static int fops_copy_serial(Panel *src, Panel *dst, int move)
{
    FileEntry __far *f;
    char local[MAX_FULL_PATH];
    char remote[MAX_FULL_PATH];
    uint16_t i;
    uint16_t selected;
    int result = FOPS_OK;

    if (src->backend == PANEL_SERIAL && dst->backend == PANEL_SERIAL) {
        ui_error("Both panels are on the serial server");
        kbd_wait();
        return FOPS_ERROR;
    }

    selected = count_selected_files(src);
    g_file_count = (selected > 0) ? selected : 1;
    g_file_current = 0;
    g_overwrite_all = 0;

    for (i = 0; i < src->files.count; i++) {
        int rc;
        f = panel_get_file(src, i);
        if (f == (FileEntry __far *)0) continue;
        if (selected > 0) {
            if (!f->selected) continue;
        } else {
            if (i != src->cursor) continue;
        }
        if (file_is_parent(f) || (f->name[0] == '.' && f->name[1] == '\0')) {
            if (selected == 0) break;
            continue;
        }
        g_file_current++;
        ui_show_progress(move ? "Moving" : "Copying", f->name,
                         g_file_current, g_file_count);

        if (src->backend == PANEL_SERIAL) {
            /* Serial -> local (download) */
            serialfs_build_rel(src, f->name, remote);
            path_build(local, dst->drive, dst->path, f->name);
            if (file_is_dir(f)) {
                rc = serialfs_get_tree(remote, local);
                if (rc == 0 && move) serialfs_delete_tree(remote);
            } else {
                /* Ask before clobbering an existing local file, matching the
                   local copy path. (Uploads and tree merges still overwrite
                   silently - the server side can't be probed cheaply.) */
                if (dos_exists(local) && !g_overwrite_all) {
                    int ow = dlg_overwrite(path_basename(local));
                    if (ow == DLG_NO) {
                        if (selected == 0) break;
                        continue;
                    } else if (ow == 'A') {
                        g_overwrite_all = 1;
                    } else if (ow != DLG_YES) {
                        result = FOPS_CANCEL;
                        break;
                    }
                }
                rc = serialfs_get_file(remote, local);
                if (rc == 0 && move) serialfs_delete(remote);
            }
        } else {
            /* Local -> serial (upload) */
            path_build(local, src->drive, src->path, f->name);
            serialfs_build_rel(dst, f->name, remote);
            if (file_is_dir(f)) {
                rc = serialfs_put_tree(local, remote);
                if (rc == 0 && move) fops_delete_dir(local);
            } else {
                rc = serialfs_put_file(local, remote);
                if (rc == 0 && move) fops_delete_file(local);
            }
        }
        if (rc != 0) {
            ui_error("Serial transfer failed");
            kbd_wait();
            result = FOPS_ERROR;
        }

        if (selected == 0) break;
    }

    ui_hide_progress();
    panel_read_dir(src);
    panel_read_dir(dst);
    return result;
}

/*---------------------------------------------------------------------------
 * fops_delete_serial - Delete files on a serial panel
 *---------------------------------------------------------------------------*/
static int fops_delete_serial(Panel *panel)
{
    FileEntry __far *f;
    char remote[MAX_FULL_PATH];
    uint16_t i;
    uint16_t selected;
    int result = FOPS_OK;

    selected = count_selected_files(panel);

    if (selected == 0) {
        f = panel_get_cursor_file(panel);
        if (f == (FileEntry __far *)0) return FOPS_CANCEL;
        if (file_is_parent(f) || (f->name[0] == '.' && f->name[1] == '\0')) {
            return FOPS_CANCEL;
        }
        if (dlg_delete_confirm(f->name, file_is_dir(f)) != DLG_YES) {
            return FOPS_CANCEL;
        }
        serialfs_build_rel(panel, f->name, remote);
        ui_show_progress("Deleting", f->name, 0, 1);
        if ((file_is_dir(f) ? serialfs_delete_tree(remote)
                            : serialfs_delete(remote)) != 0) {
            ui_error("Cannot delete on server");
            kbd_wait();
            result = FOPS_ERROR;
        }
    } else {
        char msg[40];
        str_copy(msg, "Delete ");
        num_format(msg + str_len(msg), selected);
        str_copy(msg + str_len(msg), " files?");
        if (dlg_confirm("Confirm Delete", msg) != DLG_YES) {
            return FOPS_CANCEL;
        }
        g_file_count = selected;
        g_file_current = 0;
        for (i = 0; i < panel->files.count; i++) {
            f = panel_get_file(panel, i);
            if (f == (FileEntry __far *)0 || !f->selected) continue;
            serialfs_build_rel(panel, f->name, remote);
            g_file_current++;
            ui_show_progress("Deleting", f->name, g_file_current, g_file_count);
            if ((file_is_dir(f) ? serialfs_delete_tree(remote)
                                : serialfs_delete(remote)) != 0) {
                result = FOPS_ERROR;
            }
        }
    }

    ui_hide_progress();
    panel_read_dir(panel);
    return result;
}

/*---------------------------------------------------------------------------
 * fops_copy_or_move - Ask Copy/Move once, then perform it
 *
 * Shows a single dialog: the filename when a single item is involved, or
 * "<N> files" when more than one file is selected (the highlighted name is
 * not shown). The chosen operation then runs with no further confirmation.
 *---------------------------------------------------------------------------*/
int fops_copy_or_move(void)
{
    Panel *src = panel_get_active();
    FileEntry __far *single = (FileEntry __far *)0;
    uint16_t selected = count_selected_files(src);
    char label[40];
    int op;

    if (selected > 1) {
        /* Multiple files: show the count, not a filename */
        num_format(label, selected);
        str_copy(label + str_len(label), " files");
    } else {
        /* Single item: the one selected file, or the cursor if none selected */
        if (selected == 1) {
            uint16_t i;
            for (i = 0; i < src->files.count; i++) {
                FileEntry __far *f = panel_get_file(src, i);
                if (f != (FileEntry __far *)0 && f->selected) {
                    single = f;
                    break;
                }
            }
        } else {
            single = panel_get_cursor_file(src);
        }

        /* Nothing actionable (e.g. cursor on . or ..) */
        if (single == (FileEntry __far *)0 ||
            file_is_parent(single) ||
            (single->name[0] == '.' && single->name[1] == '\0')) {
            return FOPS_CANCEL;
        }

        str_copy_n(label, single->name, sizeof(label) - 1);
    }

    op = dlg_copy_or_move(label);
    if (op == 'C') {
        return fops_copy();
    } else if (op == 'M') {
        return fops_move();
    }

    return FOPS_CANCEL;
}

/*---------------------------------------------------------------------------
 * fops_copy - Copy selected files to other panel
 *---------------------------------------------------------------------------*/
int fops_copy(void)
{
    Panel *src_panel = panel_get_active();
    Panel *dst_panel = panel_get_other();
    FileEntry __far *f;
    char src_path[MAX_FULL_PATH];
    char dst_path[MAX_FULL_PATH];
    uint16_t i;
    uint16_t selected;
    int result = FOPS_OK;

    /* Either panel on the serial server takes the serial transfer path. */
    if (src_panel->backend == PANEL_SERIAL || dst_panel->backend == PANEL_SERIAL) {
        return fops_copy_serial(src_panel, dst_panel, 0);
    }

    if (same_location(src_panel, dst_panel)) {
        ui_error("Source and destination are the same directory");
        kbd_wait();
        return FOPS_ERROR;
    }

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
        /* Copy all selected files (already confirmed by fops_copy_or_move) */
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

    /* Release the copy buffer now the batch is done. */
    copybuf_release();

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
    char src_path[MAX_FULL_PATH];
    char dst_path[MAX_FULL_PATH];
    uint16_t i;
    uint16_t selected;
    int result = FOPS_OK;

    /* Either panel on the serial server takes the serial transfer path. */
    if (src_panel->backend == PANEL_SERIAL || dst_panel->backend == PANEL_SERIAL) {
        return fops_copy_serial(src_panel, dst_panel, 1);
    }

    if (same_location(src_panel, dst_panel)) {
        ui_error("Source and destination are the same directory");
        kbd_wait();
        return FOPS_ERROR;
    }

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
        /* Move all selected files (already confirmed by fops_copy_or_move) */
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

    /* Release the copy buffer now the batch is done (rename-only moves may
     * never have allocated it; copybuf_release is a no-op then). */
    copybuf_release();

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
    char path[MAX_FULL_PATH];
    uint16_t i;
    uint16_t selected;
    int result = FOPS_OK;

    /* A serial panel deletes over the wire. */
    if (panel->backend == PANEL_SERIAL) {
        return fops_delete_serial(panel);
    }

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
    char path[MAX_FULL_PATH];

    name[0] = '\0';

    if (dlg_input("Make Directory", "Name:", name, 13) != DLG_OK) {
        return FOPS_CANCEL;
    }

    if (name[0] == '\0') {
        return FOPS_CANCEL;
    }

    /* Serial panel: create the directory on the server. */
    if (panel->backend == PANEL_SERIAL) {
        char remote[MAX_FULL_PATH];
        serialfs_build_rel(panel, name, remote);
        if (serialfs_mkdir(remote) != 0) {
            ui_error("Cannot create directory");
            kbd_wait();
            return FOPS_ERROR;
        }
        panel_read_dir(panel);
        return FOPS_OK;
    }

    /* Build full path */
    path_build(path, panel->drive, panel->path, name);

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
    char old_path[MAX_FULL_PATH];
    char new_path[MAX_FULL_PATH];

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

    /* Serial panel: rename on the server (same directory, new 8.3 name). */
    if (panel->backend == PANEL_SERIAL) {
        char old_rel[MAX_FULL_PATH];
        serialfs_build_rel(panel, old_name, old_rel);
        if (serialfs_rename(old_rel, new_name) != 0) {
            ui_error("Cannot rename file");
            kbd_wait();
            return FOPS_ERROR;
        }
        panel_read_dir(panel);
        return FOPS_OK;
    }

    /* Build full paths */
    path_build(old_path, panel->drive, panel->path, old_name);
    path_build(new_path, panel->drive, panel->path, new_name);

    if (dos_rename(old_path, new_path) != 0) {
        ui_error("Cannot rename file");
        kbd_wait();
        return FOPS_ERROR;
    }

    /* Refresh panel */
    panel_read_dir(panel);

    return FOPS_OK;
}
