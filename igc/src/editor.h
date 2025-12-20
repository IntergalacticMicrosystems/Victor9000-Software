/*
 * editor.h - IGC Text Editor/Viewer
 * F3 View and F4 Edit functionality
 */

#ifndef EDITOR_H
#define EDITOR_H

#include "igc.h"

/*---------------------------------------------------------------------------
 * Editor Buffer Sizes (based on memory tier)
 *---------------------------------------------------------------------------*/
#define EDIT_BUF_LOW      16384     /* Low memory: 16KB */
#define EDIT_BUF_MEDIUM   32768     /* Medium memory: 32KB */
#define EDIT_BUF_HIGH     65535     /* High memory: 64KB (max for 16-bit) */

#define EDIT_LINES_LOW    512       /* Low memory: 512 lines max */
#define EDIT_LINES_MEDIUM 1024      /* Medium memory: 1024 lines */
#define EDIT_LINES_HIGH   2048      /* High memory: 2048 lines */

/*---------------------------------------------------------------------------
 * Editor Display Constants
 *---------------------------------------------------------------------------*/
#define EDIT_ROWS         22        /* Rows for editing (2-23) */
#define EDIT_COLS         78        /* Columns for editing (1-78) */
#define EDIT_TOP_ROW      1         /* First row of edit area */
#define EDIT_LEFT_COL     1         /* First column of edit area */

/*---------------------------------------------------------------------------
 * Editor Structure
 *---------------------------------------------------------------------------*/
typedef struct {
    char __far *buffer;             /* Text buffer (far heap) */
    uint16_t __far *line_offs;      /* Line offset table (far heap) */
    uint32_t buf_size;              /* Buffer capacity */
    uint32_t buf_used;              /* Bytes used in buffer */
    uint16_t max_lines;             /* Maximum lines capacity */
    uint16_t total_lines;           /* Actual line count */
    uint16_t top_line;              /* First visible line (0-based) */
    uint16_t cursor_line;           /* Cursor line (0-based) */
    uint16_t cursor_col;            /* Cursor column (0-based) */
    uint16_t left_col;              /* Left column for horizontal scroll */
    bool_t   modified;              /* TRUE if buffer modified */
    bool_t   readonly;              /* TRUE for view mode */
    char     filename[80];          /* Current filename */
} Editor;

/*---------------------------------------------------------------------------
 * Editor Initialization
 *---------------------------------------------------------------------------*/

/* Initialize editor module */
bool_t editor_init(void);

/* Shutdown editor module */
void editor_shutdown(void);

/*---------------------------------------------------------------------------
 * Editor Operations
 *---------------------------------------------------------------------------*/

/* View file (F3) - read-only mode */
void editor_view(const char *filename);

/* Edit file (F4) - full edit mode */
void editor_edit(const char *filename);

#endif /* EDITOR_H */
