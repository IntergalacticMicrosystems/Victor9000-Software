/*
 * keyboard.h - IGC Keyboard Input
 * DOS keyboard with Victor 9000 scan code translation
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "igc.h"

/*---------------------------------------------------------------------------
 * Keyboard Functions
 *---------------------------------------------------------------------------*/

/* Initialize keyboard system */
void kbd_init(void);

/* Check if key is available (non-blocking) */
bool_t kbd_check(void);

/* Get key (non-blocking) - returns KEY_NONE if no key available */
KeyEvent kbd_get(void);

/* Wait for key (blocking) */
KeyEvent kbd_wait(void);

/* Flush keyboard buffer */
void kbd_flush(void);

/*---------------------------------------------------------------------------
 * Key Event Helpers
 *---------------------------------------------------------------------------*/

/* Check if key event is a function key (F1-F10) */
bool_t kbd_is_fkey(KeyEvent *e);

/* Check if key event is an arrow key */
bool_t kbd_is_arrow(KeyEvent *e);

/* Check if key event is a navigation key (arrows, Home, End, PgUp, PgDn) */
bool_t kbd_is_nav(KeyEvent *e);

/* Get F-key number (1-10) from key event, or 0 if not an F-key */
uint8_t kbd_get_fkey_num(KeyEvent *e);

#endif /* KEYBOARD_H */
