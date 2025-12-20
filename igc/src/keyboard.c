/*
 * keyboard.c - IGC Keyboard Input Implementation
 * DOS keyboard with Victor 9000 scan code translation
 */

#include <dos.h>
#include "keyboard.h"

/*---------------------------------------------------------------------------
 * kbd_init - Initialize keyboard system
 *---------------------------------------------------------------------------*/
void kbd_init(void)
{
    /* Clear any pending input */
    kbd_flush();
}

/*---------------------------------------------------------------------------
 * kbd_check - Check if key is available (non-blocking)
 *---------------------------------------------------------------------------*/
bool_t kbd_check(void)
{
    union REGS regs;

    /* DOS function 0Bh: Check STDIN status */
    regs.h.ah = 0x0B;
    int86(0x21, &regs, &regs);

    return (regs.h.al == 0xFF) ? TRUE : FALSE;
}

/*---------------------------------------------------------------------------
 * get_raw_char - Get raw character from DOS (non-blocking)
 * Returns character or -1 if none available
 *---------------------------------------------------------------------------*/
static int get_raw_char(void)
{
    union REGS regs;

    /* DOS function 06h: Direct console I/O */
    regs.h.ah = 0x06;
    regs.h.dl = 0xFF;   /* Input mode */
    int86(0x21, &regs, &regs);

    /* ZF set means no character available */
    /* Check if AL is 0 (which could mean no key or null char) */
    /* We need to check the zero flag via flags register */
    if (regs.x.cflag == 0 && regs.h.al != 0) {
        return (int)(unsigned char)regs.h.al;
    }

    /* Try checking if anything was read */
    if (kbd_check()) {
        regs.h.ah = 0x06;
        regs.h.dl = 0xFF;
        int86(0x21, &regs, &regs);
        if (regs.h.al != 0) {
            return (int)(unsigned char)regs.h.al;
        }
    }

    return -1;
}

/*---------------------------------------------------------------------------
 * get_raw_char_wait - Get raw character from DOS (blocking)
 *---------------------------------------------------------------------------*/
static int get_raw_char_wait(void)
{
    union REGS regs;

    /* DOS function 07h: Direct char input without echo */
    regs.h.ah = 0x07;
    int86(0x21, &regs, &regs);

    return (int)(unsigned char)regs.h.al;
}

/*---------------------------------------------------------------------------
 * translate_v9k_key - Translate Victor 9000 key codes to IBM AT
 *---------------------------------------------------------------------------*/
static KeyEvent translate_v9k_key(int raw)
{
    KeyEvent e;

    /* Victor 9000 F-keys: 0xF1-0xFA -> IBM AT: 0x3B-0x44 */
    if (raw >= V9K_F1 && raw <= V9K_F10) {
        e.type = KEY_EXTENDED;
        e.code = KEY_F1 + (raw - V9K_F1);
        return e;
    }

    /* Victor 9000 Page Up/Down */
    if (raw == V9K_PGUP) {
        e.type = KEY_EXTENDED;
        e.code = KEY_PGUP;
        return e;
    }
    if (raw == V9K_PGDN) {
        e.type = KEY_EXTENDED;
        e.code = KEY_PGDN;
        return e;
    }

    /* Regular ASCII */
    e.type = KEY_ASCII;
    e.code = (uint8_t)raw;
    return e;
}

/*---------------------------------------------------------------------------
 * handle_esc_sequence - Handle ESC + letter arrow key sequences
 * Victor 9000 sends ESC A/B/C/D for arrow keys
 *---------------------------------------------------------------------------*/
static KeyEvent handle_esc_sequence(void)
{
    KeyEvent e;
    int follow;

    /* Check if another character is available */
    if (!kbd_check()) {
        /* Just ESC by itself */
        e.type = KEY_ASCII;
        e.code = KEY_ESC;
        return e;
    }

    /* Get the follow-up character */
    follow = get_raw_char();
    if (follow < 0) {
        e.type = KEY_ASCII;
        e.code = KEY_ESC;
        return e;
    }

    /* Translate ESC sequences to arrow keys */
    switch (follow) {
        case 'A':
            e.type = KEY_EXTENDED;
            e.code = KEY_UP;
            break;
        case 'B':
            e.type = KEY_EXTENDED;
            e.code = KEY_DOWN;
            break;
        case 'C':
            e.type = KEY_EXTENDED;
            e.code = KEY_RIGHT;
            break;
        case 'D':
            e.type = KEY_EXTENDED;
            e.code = KEY_LEFT;
            break;
        case 'H':
            e.type = KEY_EXTENDED;
            e.code = KEY_HOME;
            break;
        case 'K':
            e.type = KEY_EXTENDED;
            e.code = KEY_END;
            break;
        default:
            /* Unknown sequence - return the follow character */
            e.type = KEY_ASCII;
            e.code = (uint8_t)follow;
            break;
    }

    return e;
}

/*---------------------------------------------------------------------------
 * kbd_get - Get key (non-blocking)
 *---------------------------------------------------------------------------*/
KeyEvent kbd_get(void)
{
    KeyEvent e;
    int raw;

    e.type = KEY_NONE;
    e.code = 0;

    /* Check if key available */
    if (!kbd_check()) {
        return e;
    }

    /* Get raw key */
    raw = get_raw_char();
    if (raw < 0) {
        return e;
    }

    /* Check for ESC - might be arrow key sequence */
    if (raw == KEY_ESC) {
        return handle_esc_sequence();
    }

    /* Check for IBM PC extended key prefix (00h or E0h) */
    if (raw == 0x00 || raw == 0xE0) {
        /* Get the scan code */
        raw = get_raw_char_wait();
        e.type = KEY_EXTENDED;
        e.code = (uint8_t)raw;
        return e;
    }

    /* Translate Victor 9000 specific codes */
    return translate_v9k_key(raw);
}

/*---------------------------------------------------------------------------
 * kbd_wait - Wait for key (blocking)
 *---------------------------------------------------------------------------*/
KeyEvent kbd_wait(void)
{
    KeyEvent e;
    int raw;

    /* Wait for key */
    raw = get_raw_char_wait();

    /* Check for ESC - might be arrow key sequence */
    if (raw == KEY_ESC) {
        return handle_esc_sequence();
    }

    /* Check for IBM PC extended key prefix */
    if (raw == 0x00 || raw == 0xE0) {
        raw = get_raw_char_wait();
        e.type = KEY_EXTENDED;
        e.code = (uint8_t)raw;
        return e;
    }

    /* Translate Victor 9000 specific codes */
    return translate_v9k_key(raw);
}

/*---------------------------------------------------------------------------
 * kbd_flush - Flush keyboard buffer
 *---------------------------------------------------------------------------*/
void kbd_flush(void)
{
    union REGS regs;

    /* DOS function 0Ch: Flush buffer and read */
    regs.h.ah = 0x0C;
    regs.h.al = 0x00;   /* Just flush, don't read */
    int86(0x21, &regs, &regs);
}

/*---------------------------------------------------------------------------
 * kbd_is_fkey - Check if key event is a function key
 *---------------------------------------------------------------------------*/
bool_t kbd_is_fkey(KeyEvent *e)
{
    if (e->type != KEY_EXTENDED) {
        return FALSE;
    }
    return (e->code >= KEY_F1 && e->code <= KEY_F10) ? TRUE : FALSE;
}

/*---------------------------------------------------------------------------
 * kbd_is_arrow - Check if key event is an arrow key
 *---------------------------------------------------------------------------*/
bool_t kbd_is_arrow(KeyEvent *e)
{
    if (e->type != KEY_EXTENDED) {
        return FALSE;
    }
    switch (e->code) {
        case KEY_UP:
        case KEY_DOWN:
        case KEY_LEFT:
        case KEY_RIGHT:
            return TRUE;
        default:
            return FALSE;
    }
}

/*---------------------------------------------------------------------------
 * kbd_is_nav - Check if key event is a navigation key
 *---------------------------------------------------------------------------*/
bool_t kbd_is_nav(KeyEvent *e)
{
    if (e->type != KEY_EXTENDED) {
        return FALSE;
    }
    switch (e->code) {
        case KEY_UP:
        case KEY_DOWN:
        case KEY_LEFT:
        case KEY_RIGHT:
        case KEY_HOME:
        case KEY_END:
        case KEY_PGUP:
        case KEY_PGDN:
            return TRUE;
        default:
            return FALSE;
    }
}

/*---------------------------------------------------------------------------
 * kbd_get_fkey_num - Get F-key number (1-10) from key event
 *---------------------------------------------------------------------------*/
uint8_t kbd_get_fkey_num(KeyEvent *e)
{
    if (e->type != KEY_EXTENDED) {
        return 0;
    }
    if (e->code >= KEY_F1 && e->code <= KEY_F10) {
        return (e->code - KEY_F1) + 1;
    }
    return 0;
}
