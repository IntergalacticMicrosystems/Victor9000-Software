/*
 * ui.h - IGC User Interface Drawing
 * Frame, panels, headers, F-key bar
 */

#ifndef UI_H
#define UI_H

#include "igc.h"
#include "panel.h"

/*---------------------------------------------------------------------------
 * UI Initialization
 *---------------------------------------------------------------------------*/

/* Draw complete UI frame */
void ui_draw_frame(void);

/* Draw title bar with panel paths */
void ui_draw_title_bar(void);

/* Draw column headers */
void ui_draw_headers(void);

/* Draw F-key bar */
void ui_draw_fkey_bar(void);

/*---------------------------------------------------------------------------
 * Panel Drawing
 *---------------------------------------------------------------------------*/

/* Draw both panels */
void ui_draw_panels(void);

/* Draw a single panel */
void ui_draw_panel(Panel *p, uint8_t x_offset, bool_t active);

/* Draw a single row in a panel (for efficient cursor updates) */
void ui_draw_panel_row(Panel *p, uint8_t x_offset, bool_t active, uint16_t file_idx);

/* Update cursor display efficiently (redraws only old and new cursor rows) */
void ui_update_cursor(uint16_t old_cursor, uint16_t old_top);

/* Draw panel path in title bar */
void ui_draw_panel_path(Panel *p, uint8_t x_offset, bool_t active);

/*---------------------------------------------------------------------------
 * Status Display
 *---------------------------------------------------------------------------*/

/* Show status message on bottom line */
void ui_status(const char *msg);

/* Show error message */
void ui_error(const char *msg);

/* Clear status line */
void ui_clear_status(void);

/*---------------------------------------------------------------------------
 * Progress Display
 *---------------------------------------------------------------------------*/

/* Show simple progress (for file operations) */
void ui_show_progress(const char *title, const char *filename,
                      uint16_t current, uint16_t total);

/* Cheap in-place progress update - repaints only the changed bar cells and the
 * percentage. Requires a prior ui_show_progress to lay down the frame. Safe to
 * call between serial packets (a handful of video writes). */
void ui_progress_tick(uint16_t current, uint16_t total);

/* Hide progress display */
void ui_hide_progress(void);

#endif /* UI_H */
