/*
 * panel.c - IGC Panel Implementation
 * File list management with dynamic allocation
 */

#include <dos.h>
#include <i86.h>
#include "panel.h"
#include "mem.h"
#include "util.h"
#include "dosapi.h"
#include "ui.h"

/*---------------------------------------------------------------------------
 * Global Panel State
 *---------------------------------------------------------------------------*/
Panel g_left_panel;
Panel g_right_panel;
uint8_t g_active_panel = 0;

/* Static DTA for find operations */
static DTA g_dta;

/*---------------------------------------------------------------------------
 * Set DTA address for find operations
 *---------------------------------------------------------------------------*/
static void set_dta(DTA *dta)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);
    regs.h.ah = 0x1A;
    regs.x.dx = FP_OFF(dta);
    sregs.ds = FP_SEG(dta);
    int86x(0x21, &regs, &regs, &sregs);
}

/*---------------------------------------------------------------------------
 * Find first file matching pattern
 *---------------------------------------------------------------------------*/
static int find_first(const char *pattern, uint8_t attr)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);
    regs.h.ah = 0x4E;
    regs.x.cx = attr;
    regs.x.dx = FP_OFF(pattern);
    sregs.ds = FP_SEG(pattern);
    int86x(0x21, &regs, &regs, &sregs);

    return regs.x.cflag ? -1 : 0;
}

/*---------------------------------------------------------------------------
 * Find next file
 *---------------------------------------------------------------------------*/
static int find_next(void)
{
    union REGS regs;

    regs.h.ah = 0x4F;
    int86(0x21, &regs, &regs);

    return regs.x.cflag ? -1 : 0;
}

/*---------------------------------------------------------------------------
 * Compare file entries for sorting (directories first, then alphabetical)
 *---------------------------------------------------------------------------*/
static int file_compare(FileEntry __far *a, FileEntry __far *b)
{
    int a_dir = (a->attr & DOS_ATTR_DIRECTORY) ? 1 : 0;
    int b_dir = (b->attr & DOS_ATTR_DIRECTORY) ? 1 : 0;

    /* ".." always comes first */
    if (a->name[0] == '.' && a->name[1] == '.' && a->name[2] == '\0') return -1;
    if (b->name[0] == '.' && b->name[1] == '.' && b->name[2] == '\0') return 1;

    /* Directories before files */
    if (a_dir != b_dir) {
        return b_dir - a_dir;
    }

    /* Alphabetical (case-insensitive) */
    return str_cmp_i(a->name, b->name);
}

/*---------------------------------------------------------------------------
 * Sort file list (simple bubble sort - adequate for small lists)
 *---------------------------------------------------------------------------*/
static void sort_files(FileList *fl)
{
    uint16_t i, j;
    FileEntry temp;
    FileEntry __far *a;
    FileEntry __far *b;

    if (fl->count < 2) return;

    for (i = 0; i < fl->count - 1; i++) {
        for (j = 0; j < fl->count - i - 1; j++) {
            a = &fl->entries[j];
            b = &fl->entries[j + 1];

            if (file_compare(a, b) > 0) {
                /* Swap */
                mem_copy_far(&temp, a, sizeof(FileEntry));
                mem_copy_far(a, b, sizeof(FileEntry));
                mem_copy_far(b, &temp, sizeof(FileEntry));
            }
        }
    }
}

/*---------------------------------------------------------------------------
 * panel_init - Initialize a panel with given capacity
 *---------------------------------------------------------------------------*/
bool_t panel_init(Panel *p, uint16_t capacity)
{
    uint32_t bytes;

    p->drive = 0;
    p->path[0] = '\0';
    p->top = 0;
    p->cursor = 0;
    p->sel_count = 0;

    bytes = (uint32_t)capacity * sizeof(FileEntry);
    p->files.entries = (FileEntry __far *)mem_alloc(bytes);

    if (p->files.entries == (FileEntry __far *)0) {
        p->files.capacity = 0;
        p->files.count = 0;
        p->files.truncated = FALSE;
        return FALSE;
    }

    p->files.capacity = capacity;
    p->files.count = 0;
    p->files.truncated = FALSE;

    return TRUE;
}

/*---------------------------------------------------------------------------
 * panel_free - Free panel resources
 *---------------------------------------------------------------------------*/
void panel_free(Panel *p)
{
    if (p->files.entries != (FileEntry __far *)0) {
        mem_free(p->files.entries);
        p->files.entries = (FileEntry __far *)0;
    }
    p->files.capacity = 0;
    p->files.count = 0;
}

/*---------------------------------------------------------------------------
 * panels_init - Initialize both panels
 *---------------------------------------------------------------------------*/
bool_t panels_init(void)
{
    uint16_t capacity = mem_get_files_per_panel();

    if (!panel_init(&g_left_panel, capacity)) {
        return FALSE;
    }

    if (!panel_init(&g_right_panel, capacity)) {
        panel_free(&g_left_panel);
        return FALSE;
    }

    /* Set initial drive and path */
    g_left_panel.drive = dos_get_drive();
    g_right_panel.drive = g_left_panel.drive;

    dos_get_curdir(g_left_panel.drive + 1, g_left_panel.path);
    str_copy(g_right_panel.path, g_left_panel.path);

    g_active_panel = 0;

    return TRUE;
}

/*---------------------------------------------------------------------------
 * panels_free - Free both panels
 *---------------------------------------------------------------------------*/
void panels_free(void)
{
    panel_free(&g_left_panel);
    panel_free(&g_right_panel);
}

/*---------------------------------------------------------------------------
 * panel_get_active - Get active panel pointer
 *---------------------------------------------------------------------------*/
Panel *panel_get_active(void)
{
    return (g_active_panel == 0) ? &g_left_panel : &g_right_panel;
}

/*---------------------------------------------------------------------------
 * panel_get_other - Get other (inactive) panel pointer
 *---------------------------------------------------------------------------*/
Panel *panel_get_other(void)
{
    return (g_active_panel == 0) ? &g_right_panel : &g_left_panel;
}

/*---------------------------------------------------------------------------
 * panel_get_inactive - Get inactive panel pointer (alias)
 *---------------------------------------------------------------------------*/
Panel *panel_get_inactive(void)
{
    return panel_get_other();
}

/*---------------------------------------------------------------------------
 * panel_switch - Switch active panel
 *---------------------------------------------------------------------------*/
void panel_switch(void)
{
    g_active_panel = (g_active_panel == 0) ? 1 : 0;
}

/*---------------------------------------------------------------------------
 * panel_read_dir - Read directory into panel
 *---------------------------------------------------------------------------*/
int panel_read_dir(Panel *p)
{
    char pattern[80];
    FileEntry __far *entry;
    uint16_t count = 0;
    int result;

    /* Show loading indicator */
    ui_status("Reading directory...");

    /* Build search pattern */
    pattern[0] = 'A' + p->drive;
    pattern[1] = ':';
    pattern[2] = '\\';

    if (p->path[0] == '\\') {
        str_copy(&pattern[3], &p->path[1]);
    } else if (p->path[0] != '\0') {
        str_copy(&pattern[3], p->path);
    } else {
        pattern[3] = '\0';
    }

    path_append(pattern, "*.*");

    /* Set DTA */
    set_dta(&g_dta);

    /* Clear file list */
    p->files.count = 0;
    p->files.truncated = FALSE;

    /* Add ".." entry if not at root */
    if (!path_is_root(p->path)) {
        entry = &p->files.entries[count];
        entry->attr = DOS_ATTR_DIRECTORY;
        entry->time = 0;
        entry->date = 0;
        entry->size = 0;
        entry->name[0] = '.';
        entry->name[1] = '.';
        entry->name[2] = '\0';
        entry->selected = 0;
        count++;
    }

    /* Find first file */
    result = find_first(pattern, DOS_ATTR_DIRECTORY | DOS_ATTR_HIDDEN | DOS_ATTR_SYSTEM);

    while (result == 0 && count < p->files.capacity) {
        /* Skip "." entry */
        if (g_dta.name[0] == '.' && g_dta.name[1] == '\0') {
            result = find_next();
            continue;
        }

        /* Skip ".." - we add it manually */
        if (g_dta.name[0] == '.' && g_dta.name[1] == '.' && g_dta.name[2] == '\0') {
            result = find_next();
            continue;
        }

        /* Copy entry */
        entry = &p->files.entries[count];
        entry->attr = g_dta.attr;
        entry->time = g_dta.time;
        entry->date = g_dta.date;
        entry->size = g_dta.size;
        str_copy_n(entry->name, g_dta.name, 13);
        entry->selected = 0;

        count++;
        result = find_next();
    }

    /* Check if truncated */
    if (result == 0) {
        p->files.truncated = TRUE;
    }

    p->files.count = count;

    /* Sort files */
    sort_files(&p->files);

    /* Reset cursor if beyond end */
    if (p->cursor >= count) {
        p->cursor = (count > 0) ? count - 1 : 0;
    }
    if (p->top > p->cursor) {
        p->top = p->cursor;
    }

    /* Clear loading indicator */
    ui_clear_status();

    return 0;
}

/*---------------------------------------------------------------------------
 * panel_refresh - Re-read current directory
 *---------------------------------------------------------------------------*/
int panel_refresh(Panel *p)
{
    uint16_t old_cursor = p->cursor;
    int result = panel_read_dir(p);

    /* Try to restore cursor position */
    if (old_cursor < p->files.count) {
        p->cursor = old_cursor;
    }

    return result;
}

/*---------------------------------------------------------------------------
 * panel_change_dir - Change to directory
 *---------------------------------------------------------------------------*/
int panel_change_dir(Panel *p, const char *dirname)
{
    char newpath[80];

    /* Build new path */
    if (p->path[0] == '\0' || (p->path[0] == '\\' && p->path[1] == '\0')) {
        /* At root */
        newpath[0] = '\\';
        str_copy(&newpath[1], dirname);
    } else {
        str_copy(newpath, p->path);
        path_append(newpath, dirname);
    }

    /* Save old path in case of failure */
    str_copy(p->path, newpath);

    /* Reset cursor */
    p->cursor = 0;
    p->top = 0;

    return panel_read_dir(p);
}

/*---------------------------------------------------------------------------
 * panel_go_parent - Go to parent directory
 *---------------------------------------------------------------------------*/
int panel_go_parent(Panel *p)
{
    if (path_is_root(p->path)) {
        return 0;  /* Already at root */
    }

    path_get_parent(p->path);
    p->cursor = 0;
    p->top = 0;

    return panel_read_dir(p);
}

/*---------------------------------------------------------------------------
 * panel_set_drive - Set drive and read root
 *---------------------------------------------------------------------------*/
int panel_set_drive(Panel *p, uint8_t drive)
{
    p->drive = drive;
    p->path[0] = '\0';
    p->cursor = 0;
    p->top = 0;

    return panel_read_dir(p);
}

/*---------------------------------------------------------------------------
 * panel_get_cursor_file - Get file at cursor
 *---------------------------------------------------------------------------*/
FileEntry __far *panel_get_cursor_file(Panel *p)
{
    if (p->cursor >= p->files.count) {
        return (FileEntry __far *)0;
    }
    return &p->files.entries[p->cursor];
}

/*---------------------------------------------------------------------------
 * panel_get_file - Get file by index
 *---------------------------------------------------------------------------*/
FileEntry __far *panel_get_file(Panel *p, uint16_t index)
{
    if (index >= p->files.count) {
        return (FileEntry __far *)0;
    }
    return &p->files.entries[index];
}

/*---------------------------------------------------------------------------
 * panel_toggle_selection - Toggle selection of file at cursor
 *---------------------------------------------------------------------------*/
void panel_toggle_selection(Panel *p)
{
    FileEntry __far *f = panel_get_cursor_file(p);

    if (f == (FileEntry __far *)0) return;

    /* Don't allow selecting ".." */
    if (file_is_parent(f)) return;

    if (f->selected) {
        f->selected = 0;
        if (p->sel_count > 0) p->sel_count--;
    } else {
        f->selected = 1;
        p->sel_count++;
    }
}

/*---------------------------------------------------------------------------
 * panel_clear_selection - Clear all selections
 *---------------------------------------------------------------------------*/
void panel_clear_selection(Panel *p)
{
    uint16_t i;

    for (i = 0; i < p->files.count; i++) {
        p->files.entries[i].selected = 0;
    }
    p->sel_count = 0;
}

/*---------------------------------------------------------------------------
 * panel_get_sel_count - Get count of selected files
 *---------------------------------------------------------------------------*/
uint16_t panel_get_sel_count(Panel *p)
{
    return p->sel_count;
}

/*---------------------------------------------------------------------------
 * Navigation functions
 *---------------------------------------------------------------------------*/

void panel_cursor_up(Panel *p)
{
    if (p->cursor > 0) {
        p->cursor--;
        if (p->cursor < p->top) {
            p->top = p->cursor;
        }
    }
}

void panel_cursor_down(Panel *p)
{
    if (p->cursor < p->files.count - 1) {
        p->cursor++;
        if (p->cursor >= p->top + PANEL_HEIGHT) {
            p->top = p->cursor - PANEL_HEIGHT + 1;
        }
    }
}

void panel_cursor_home(Panel *p)
{
    p->cursor = 0;
    p->top = 0;
}

void panel_cursor_end(Panel *p)
{
    if (p->files.count > 0) {
        p->cursor = p->files.count - 1;
        if (p->cursor >= PANEL_HEIGHT) {
            p->top = p->cursor - PANEL_HEIGHT + 1;
        } else {
            p->top = 0;
        }
    }
}

void panel_page_up(Panel *p)
{
    if (p->cursor >= PANEL_HEIGHT) {
        p->cursor -= PANEL_HEIGHT;
    } else {
        p->cursor = 0;
    }

    if (p->top >= PANEL_HEIGHT) {
        p->top -= PANEL_HEIGHT;
    } else {
        p->top = 0;
    }
}

void panel_page_down(Panel *p)
{
    uint16_t max_cursor = (p->files.count > 0) ? p->files.count - 1 : 0;

    p->cursor += PANEL_HEIGHT;
    if (p->cursor > max_cursor) {
        p->cursor = max_cursor;
    }

    p->top += PANEL_HEIGHT;
    if (p->top + PANEL_HEIGHT > p->files.count) {
        if (p->files.count >= PANEL_HEIGHT) {
            p->top = p->files.count - PANEL_HEIGHT;
        } else {
            p->top = 0;
        }
    }
}

/*---------------------------------------------------------------------------
 * Utility functions
 *---------------------------------------------------------------------------*/

bool_t file_is_dir(FileEntry __far *f)
{
    return (f->attr & DOS_ATTR_DIRECTORY) ? TRUE : FALSE;
}

bool_t file_is_parent(FileEntry __far *f)
{
    return (f->name[0] == '.' && f->name[1] == '.' && f->name[2] == '\0') ? TRUE : FALSE;
}

void file_format_size(FileEntry __far *f, char *buf)
{
    if (f->attr & DOS_ATTR_DIRECTORY) {
        str_copy(buf, "<DIR>");
    } else {
        size_format(buf, f->size);
    }
}

void file_format_date(FileEntry __far *f, char *buf)
{
    uint16_t year, month, day;

    if (f->date == 0) {
        str_copy(buf, "        ");
        return;
    }

    /* DOS date format: bits 0-4=day, 5-8=month, 9-15=year-1980 */
    day = f->date & 0x1F;
    month = (f->date >> 5) & 0x0F;
    year = ((f->date >> 9) & 0x7F) + 80;  /* 2-digit year */

    buf[0] = '0' + (month / 10);
    buf[1] = '0' + (month % 10);
    buf[2] = '-';
    buf[3] = '0' + (day / 10);
    buf[4] = '0' + (day % 10);
    buf[5] = '-';
    buf[6] = '0' + (year / 10) % 10;
    buf[7] = '0' + (year % 10);
    buf[8] = '\0';
}
