/*
 * dialog.h - IGC Modal Dialog System
 * Windows, confirmations, input dialogs
 */

#ifndef DIALOG_H
#define DIALOG_H

#include "igc.h"

/*---------------------------------------------------------------------------
 * Dialog Result Codes
 *---------------------------------------------------------------------------*/
#define DLG_OK          1
#define DLG_CANCEL      0
#define DLG_YES         1
#define DLG_NO          0
#define DLG_ABORT       (-1)    /* ESC/F10 - cancel the whole operation */

/*---------------------------------------------------------------------------
 * Window Structure (for save/restore)
 *---------------------------------------------------------------------------*/
typedef struct {
    uint8_t  x;             /* Left column */
    uint8_t  y;             /* Top row */
    uint8_t  w;             /* Width */
    uint8_t  h;             /* Height */
    uint16_t __far *save;   /* Background save buffer */
} DialogWindow;

/*---------------------------------------------------------------------------
 * Basic Window Operations
 *---------------------------------------------------------------------------*/

/* Open a dialog window (saves background) */
bool_t dlg_open(DialogWindow *win, uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                const char *title);

/* Close dialog window (restores background) */
void dlg_close(DialogWindow *win);

/* Draw text inside dialog */
void dlg_print(DialogWindow *win, uint8_t x, uint8_t y, const char *text);

/* Draw text centered */
void dlg_print_center(DialogWindow *win, uint8_t y, const char *text);

/*---------------------------------------------------------------------------
 * Standard Dialogs
 *---------------------------------------------------------------------------*/

/* Show alert message with OK button */
void dlg_alert(const char *title, const char *message);

/* Show confirmation with Yes/No buttons */
/* Returns DLG_YES or DLG_NO */
int dlg_confirm(const char *title, const char *message);

/* Show input dialog */
/* Returns DLG_OK or DLG_CANCEL, fills buf with input */
int dlg_input(const char *title, const char *prompt, char *buf, uint16_t maxlen);

/*---------------------------------------------------------------------------
 * Specialized Dialogs
 *---------------------------------------------------------------------------*/

/* Drive selection dialog */
/* Returns selected drive (0=A, 1=B, etc.), DLG_DRIVE_SERIAL_A / _B for the
 * serial file server on that port, or -1 if cancelled. The port the mouse
 * driver is loaded on (mouse_port()) is listed but cannot be chosen. */
#define DLG_DRIVE_SERIAL_A  100
#define DLG_DRIVE_SERIAL_B  101
int dlg_drive_select(uint8_t current_drive);

/* Generic list picker: items drawn one per row, initial = preselected row.
 * Returns the chosen index, or -1 on Esc/F7/F10. */
int dlg_menu(const char *title, const char * const *items, uint8_t count,
             uint8_t initial);

/* Floppy picker (A: / B: only, whichever exist). Refuses a drive that is not
 * ready. Returns 0 (A:), 1 (B:) or -1 if cancelled. */
int dlg_floppy_select(const char *title);

/* F5 file operation dialog */
/* Returns 'C' copy, 'M' move, 'D' delete, 'R' rename, 0 for cancel */
int dlg_file_op(const char *filename);

/* Delete confirmation dialog */
/* Returns DLG_YES or DLG_NO */
int dlg_delete_confirm(const char *filename, bool_t is_dir);

/* Overwrite confirmation dialog */
/* Returns DLG_YES, DLG_NO, 'A' for All, or DLG_ABORT (ESC/F10) */
int dlg_overwrite(const char *filename);

/* Exit confirmation dialog */
/* Returns DLG_YES to exit, DLG_NO to stay */
int dlg_exit_confirm(void);

#endif /* DIALOG_H */
