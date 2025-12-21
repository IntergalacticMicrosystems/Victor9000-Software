/*
 * keytest.c - Victor 9000 Keyboard Code Tester
 * Displays raw keycodes for debugging keyboard input
 * Uses DOS INT 21h only (no BIOS calls) for Victor compatibility
 *
 * Build: wcc -0 -mc -s -os keytest.c && wlink sys dos file keytest.obj
 */

#include <dos.h>
#include <i86.h>

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;

/*---------------------------------------------------------------------------
 * Print a character to stdout (DOS INT 21h AH=02)
 *---------------------------------------------------------------------------*/
static void putch(char c)
{
    union REGS regs;
    regs.h.ah = 0x02;
    regs.h.dl = c;
    int86(0x21, &regs, &regs);
}

/*---------------------------------------------------------------------------
 * Print a string to stdout
 *---------------------------------------------------------------------------*/
static void puts_raw(const char *s)
{
    while (*s) {
        putch(*s++);
    }
}

/*---------------------------------------------------------------------------
 * Print a hex byte
 *---------------------------------------------------------------------------*/
static void put_hex(uint8_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    putch(hex[(val >> 4) & 0x0F]);
    putch(hex[val & 0x0F]);
}

/*---------------------------------------------------------------------------
 * Print a decimal number
 *---------------------------------------------------------------------------*/
static void put_dec(uint16_t val)
{
    char buf[6];
    int i = 5;
    buf[5] = '\0';

    if (val == 0) {
        putch('0');
        return;
    }

    while (val > 0 && i > 0) {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    }
    puts_raw(&buf[i]);
}

/*---------------------------------------------------------------------------
 * Read one character from stdin (DOS INT 21h AH=08 - no echo)
 *---------------------------------------------------------------------------*/
static uint8_t dos_getchar(void)
{
    union REGS regs;
    regs.h.ah = 0x08;  /* Read char without echo */
    int86(0x21, &regs, &regs);
    return regs.h.al;
}

/*---------------------------------------------------------------------------
 * Main
 *---------------------------------------------------------------------------*/
int main(void)
{
    uint8_t c;
    uint8_t c2;
    uint16_t count = 0;

    puts_raw("Victor 9000 Keyboard Code Tester\r\n");
    puts_raw("================================\r\n");
    puts_raw("Press keys to see their codes.\r\n");
    puts_raw("Press ESC three times to exit.\r\n\r\n");
    puts_raw("Hex    Dec  Char\r\n");
    puts_raw("---    ---  ----\r\n");

    while (1) {
        c = dos_getchar();

        /* Display hex code */
        puts_raw("0x");
        put_hex(c);
        puts_raw("   ");

        /* Display decimal */
        put_dec(c);
        if (c < 10) puts_raw("  ");
        else if (c < 100) puts_raw(" ");
        puts_raw("  ");

        /* Display printable or description */
        if (c == 0x00) {
            puts_raw("[NUL - extended key follows]");
        } else if (c == 0x1B) {
            puts_raw("[ESC]");
            count++;
            if (count >= 3) {
                puts_raw("\r\n\r\nExiting.\r\n");
                break;
            }
        } else if (c == 0x0D) {
            puts_raw("[ENTER]");
        } else if (c == 0x0A) {
            puts_raw("[LF]");
        } else if (c == 0x09) {
            puts_raw("[TAB]");
        } else if (c == 0x08) {
            puts_raw("[BKSP]");
        } else if (c == 0x20) {
            puts_raw("[SPACE]");
        } else if (c == 0x7F) {
            puts_raw("[DEL]");
        } else if (c >= 0x01 && c <= 0x1A) {
            puts_raw("[Ctrl-");
            putch('A' + c - 1);
            putch(']');
        } else if (c >= 0x20 && c < 0x7F) {
            putch('\'');
            putch(c);
            putch('\'');
        } else if (c >= 0x80) {
            puts_raw("[High: ");
            put_dec(c);
            putch(']');
        } else {
            puts_raw("[???]");
        }

        /* Reset ESC counter if not ESC */
        if (c != 0x1B) {
            count = 0;
        }

        puts_raw("\r\n");

        /* If we got NUL, read the next byte (extended key code) */
        if (c == 0x00) {
            c2 = dos_getchar();
            puts_raw("0x");
            put_hex(c2);
            puts_raw("   ");
            put_dec(c2);
            if (c2 < 10) puts_raw("  ");
            else if (c2 < 100) puts_raw(" ");
            puts_raw("  [Extended scan code]\r\n");
        }
    }

    return 0;
}
