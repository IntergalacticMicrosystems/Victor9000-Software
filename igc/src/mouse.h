/*
 * mouse.h - IGC Mouse Input
 * Victor 9000 serial mouse through the MOUSE.DEV / MOUSE.COM driver (INT 33h)
 *
 * The driver reports positions in pixels on an 800x400 grid; in text mode
 * they are multiples of the 10x16 character cell, so x/10, y/16 is the cell.
 * Clicks arrive as KeyEvents of type KEY_MOUSE from kbd_wait(); the cell the
 * button went down in is in g_mouse_x / g_mouse_y.
 *
 * The pointer (a reverse-video cell) is only visible while kbd_wait() is
 * waiting. All drawing happens between waits, so the driver's save/restore
 * of the cell under the pointer never clashes with IGC's own VRAM writes.
 */

#ifndef MOUSE_H
#define MOUSE_H

#include "igc.h"

/* KeyEvent.code values for KEY_MOUSE events */
#define MOUSE_LEFT      1
#define MOUSE_RIGHT     2
#define MOUSE_LEFT_DBL  3       /* second left click on the same cell within MOUSE_DBL_TIME */

#define MOUSE_DBL_TIME  50      /* double-click window, hundredths of a second */

/* Cell the last reported click landed in */
extern uint8_t g_mouse_x;
extern uint8_t g_mouse_y;

/* Detect the driver and set it up for text mode. Returns TRUE if present.
 * Call after scr_init(): the driver re-reads the CRTC start address. */
bool_t mouse_init(void);

/* Hide the pointer before exit */
void mouse_shutdown(void);

/* TRUE if a driver was found at startup */
bool_t mouse_present(void);

/* Pointer visibility (idempotent: the driver's own show/hide counter is
 * kept balanced here) */
void mouse_show(void);
void mouse_hide(void);

/* Non-blocking: TRUE and a filled event (code = MOUSE_*) if a button was
 * pressed since the last poll. Position in g_mouse_x / g_mouse_y. */
bool_t mouse_poll(KeyEvent *e);

/* Discard pending clicks (the mouse half of kbd_flush) */
void mouse_flush(void);

/* Tell the mouse layer the screen changed (a dialog closed, the editor
 * opened or closed). Presses queued before the next wait for input are then
 * dropped instead of landing on whatever is now drawn under them: typically
 * the second press of a double-click whose first click did the job. */
void mouse_new_context(void);

#endif /* MOUSE_H */
