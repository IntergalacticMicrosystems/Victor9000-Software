/*
 * panel.h - IGC Panel Data Structures
 * File list management with dynamic allocation
 */

#ifndef PANEL_H
#define PANEL_H

#include "igc.h"

/*---------------------------------------------------------------------------
 * File Entry Structure (24 bytes)
 *---------------------------------------------------------------------------*/
typedef struct {
    uint8_t  attr;          /* DOS file attribute */
    uint16_t time;          /* DOS packed time */
    uint16_t date;          /* DOS packed date */
    uint32_t size;          /* File size in bytes */
    char     name[13];      /* Filename (8.3 + null) */
    uint8_t  selected;      /* Selection flag */
    uint8_t  padding[2];    /* Align to 24 bytes */
} FileEntry;

#define FILE_ENTRY_SIZE 24

/*---------------------------------------------------------------------------
 * File List Structure (dynamic array)
 *---------------------------------------------------------------------------*/
typedef struct {
    FileEntry __far *entries;   /* Far pointer to file array */
    uint16_t capacity;          /* Allocated slots */
    uint16_t count;             /* Actual files */
    bool_t   truncated;         /* TRUE if more files exist than capacity */
} FileList;

/*---------------------------------------------------------------------------
 * Panel Structure
 *---------------------------------------------------------------------------*/
typedef struct {
    uint8_t  drive;             /* Drive number (0=A, 1=B, etc.) */
    char     path[MAX_PATH_LEN]; /* Current directory path */
    uint16_t top;               /* Index of top visible file */
    uint16_t cursor;            /* Current cursor position */
    uint16_t sel_count;         /* Number of selected files */
    FileList files;             /* Dynamic file list */
} Panel;

/*---------------------------------------------------------------------------
 * Global Panel State
 *---------------------------------------------------------------------------*/
extern Panel g_left_panel;
extern Panel g_right_panel;
extern uint8_t g_active_panel;  /* 0=left, 1=right */

/*---------------------------------------------------------------------------
 * Panel Initialization
 *---------------------------------------------------------------------------*/

/* Initialize a panel with given capacity */
bool_t panel_init(Panel *p, uint16_t capacity);

/* Free panel resources */
void panel_free(Panel *p);

/* Initialize both panels */
bool_t panels_init(void);

/* Free both panels */
void panels_free(void);

/*---------------------------------------------------------------------------
 * Panel Access
 *---------------------------------------------------------------------------*/

/* Get active panel pointer */
Panel *panel_get_active(void);

/* Get other (inactive) panel pointer */
Panel *panel_get_other(void);

/* Get inactive panel pointer (alias for panel_get_other) */
Panel *panel_get_inactive(void);

/* Switch active panel */
void panel_switch(void);

/*---------------------------------------------------------------------------
 * Directory Operations
 *---------------------------------------------------------------------------*/

/* Read directory into panel */
int panel_read_dir(Panel *p);

/* Refresh panel (re-read current directory) */
int panel_refresh(Panel *p);

/* Change to directory (updates path and reads) */
int panel_change_dir(Panel *p, const char *dirname);

/* Go to parent directory */
int panel_go_parent(Panel *p);

/* Set drive and read root */
int panel_set_drive(Panel *p, uint8_t drive);

/*---------------------------------------------------------------------------
 * Cursor/Selection Operations
 *---------------------------------------------------------------------------*/

/* Get file at cursor */
FileEntry __far *panel_get_cursor_file(Panel *p);

/* Get file by index */
FileEntry __far *panel_get_file(Panel *p, uint16_t index);

/* Toggle selection of file at cursor */
void panel_toggle_selection(Panel *p);

/* Clear all selections */
void panel_clear_selection(Panel *p);

/* Get count of selected files */
uint16_t panel_get_sel_count(Panel *p);

/*---------------------------------------------------------------------------
 * Navigation
 *---------------------------------------------------------------------------*/

/* Move cursor up */
void panel_cursor_up(Panel *p);

/* Move cursor down */
void panel_cursor_down(Panel *p);

/* Move cursor to first file */
void panel_cursor_home(Panel *p);

/* Move cursor to last file */
void panel_cursor_end(Panel *p);

/* Page up */
void panel_page_up(Panel *p);

/* Page down */
void panel_page_down(Panel *p);

/*---------------------------------------------------------------------------
 * Utility Functions
 *---------------------------------------------------------------------------*/

/* Check if file is a directory */
bool_t file_is_dir(FileEntry __far *f);

/* Check if file is ".." entry */
bool_t file_is_parent(FileEntry __far *f);

/* Format file size for display */
void file_format_size(FileEntry __far *f, char *buf);

/* Format file date for display */
void file_format_date(FileEntry __far *f, char *buf);

#endif /* PANEL_H */
