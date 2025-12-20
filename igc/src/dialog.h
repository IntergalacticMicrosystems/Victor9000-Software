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
/* Returns selected drive (0=A, 1=B, etc.) or -1 if cancelled */
int dlg_drive_select(uint8_t current_drive);

/* Copy/Move confirmation dialog */
/* Returns 'C' for copy, 'M' for move, 0 for cancel */
int dlg_copy_or_move(const char *filename);

/* Delete confirmation dialog */
/* Returns DLG_YES or DLG_NO */
int dlg_delete_confirm(const char *filename, bool_t is_dir);

/* Overwrite confirmation dialog */
/* Returns DLG_YES, DLG_NO, or 'A' for All */
int dlg_overwrite(const char *filename);

/* Exit confirmation dialog */
/* Returns DLG_YES to exit, DLG_NO to stay */
int dlg_exit_confirm(void);

#endif /* DIALOG_H */
