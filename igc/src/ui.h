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

/* Display form of a panel's location ("B:\DIR", "SERA:\DIR"), <= 23 chars */
void ui_panel_path(const Panel *p, char *buf);

/*---------------------------------------------------------------------------
 * Activity box (centred, grows 3..6 rows) and row-23 messages
 *
 * Bracket every ui_status / ui_show_progress with ui_clear_status /
 * ui_hide_progress, and nothing may repaint the panels in between. Open and
 * close dialogs and the box in LIFO order.
 *---------------------------------------------------------------------------*/

/* Open/refresh the box with line 0 = msg (no title, no bar); blanks row 23 */
void ui_status(const char *msg);

/* Extra line n = 1..2 (hints like "ESC cancels copy"); NULL/"" removes it.
 * Set by the code that polls ESC, cleared by it when that operation ends. */
void ui_status_line(uint8_t n, const char *text);

/* Close + reset the box (title, bar, all lines), always blank row 23 */
void ui_clear_status(void);

/* Open/refresh the box: title, file name, bar at current/total; blanks row 23 */
void ui_show_progress(const char *title, const char *filename,
                      uint16_t current, uint16_t total);

/* Cheap in-place progress update - repaints only the changed bar cells and the
 * percentage (at most 24 video writes). No-op unless ui_show_progress opened
 * the bar. Safe to call between serial packets. */
void ui_progress_tick(uint16_t current, uint16_t total);

/* Same as ui_clear_status */
void ui_hide_progress(void);

/* One-off message on row 23 (not the box), e.g. "File saved" */
void ui_notice(const char *msg);

/* Error message on row 23 in reverse video */
void ui_error(const char *msg);

#endif /* UI_H */
