/*
 * mouse.c - IGC Mouse Input Implementation
 *
 * Talks to the Victor serial mouse driver (MOUSE.DEV / MOUSE.COM, a
 * Microsoft-style INT 33h driver). Functions used:
 *   0  reset / detect  (BX must be non-zero: BX=0 makes the driver
 *                       re-initialise the serial port and power-cycle the
 *                       mouse, which takes a while and is not needed)
 *   1  show pointer, 2 hide pointer
 *   5  button press info: BX = presses since the last call (then cleared),
 *      CX/DX = pixel position of the last press
 *  10  text pointer: BX=0 software pointer, CX = AND mask, DX = XOR mask
 *      applied to the screen word under the pointer
 */

#include <dos.h>
#include "mouse.h"

uint8_t g_mouse_x = 0;
uint8_t g_mouse_y = 0;

static bool_t g_present = FALSE;
static bool_t g_shown = FALSE;
static uint8_t g_port = MOUSE_PORT_NONE;

/* The driver's port_index word (0 = A, 1 = B) sits this far after its INT 33h
 * entry: 0B26h - 008Ah in MOUSE.DEV, and the same distance in MOUSE.COM, whose
 * resident image is the same bytes shifted by 100h. Verified against the
 * reverse-engineered source in ../mouse (MOUSE.asm, MOUSE_DRIVER.md). */
#define MOUSE_PORT_INDEX_OFS 0x0A9C

/* Last left press, for double-click detection */
static uint32_t g_last_time = 0;
static uint8_t  g_last_x = 0xFF;
static uint8_t  g_last_y = 0xFF;

/* Screen generation: bumped by mouse_new_context / mouse_flush. When the
 * pointer is next shown (= the next wait for input) in a new generation,
 * presses queued since the last wait are drained: they were made against a
 * screen that is gone, typically the second press of a double-click on a
 * button or label whose first click already did its job. */
static uint16_t g_context = 0;
static uint16_t g_shown_context = 0;

/*---------------------------------------------------------------------------
 * dos_hundredths - DOS clock as hundredths of a second within the hour
 *---------------------------------------------------------------------------*/
static uint32_t dos_hundredths(void)
{
    union REGS regs;

    regs.h.ah = 0x2C;
    int86(0x21, &regs, &regs);
    /* CH = hours, CL = minutes, DH = seconds, DL = hundredths */
    return (((uint32_t)regs.h.ch * 60L + regs.h.cl) * 60L + regs.h.dh) * 100L
           + regs.h.dl;
}

/*---------------------------------------------------------------------------
 * mouse_init - Detect the driver and configure it for the text screen
 *---------------------------------------------------------------------------*/
bool_t mouse_init(void)
{
    union REGS regs;
    struct SREGS sregs;

    g_present = FALSE;
    g_shown = FALSE;
    g_port = MOUSE_PORT_NONE;

    /* Victor DOS leaves INT 33h as the boot ROM set it (null, or some stray
     * handler), so do not call it blindly. The Victor driver puts the text
     * "MOUSE" in the 5 bytes before its INT 33h entry; PICASSO checks the
     * same thing. */
    regs.h.ah = 0x35;
    regs.h.al = 0x33;
    segread(&sregs);
    int86x(0x21, &regs, &regs, &sregs);
    if ((sregs.es == 0 && regs.x.bx == 0) || regs.x.bx < 5) {
        return FALSE;
    }
    {
        const char __far *sig = (const char __far *)MK_FP(sregs.es, regs.x.bx - 5);
        if (sig[0] != 'M' || sig[1] != 'O' || sig[2] != 'U' ||
            sig[3] != 'S' || sig[4] != 'E') {
            return FALSE;
        }
    }
    {
        /* Which serial port the mouse is on, so the serial file server is
         * not offered on it (see dlg_drive_select). Anything but 0 or 1 is
         * not this driver's layout: report unknown rather than guess. */
        uint16_t idx = *(const uint16_t __far *)
                        MK_FP(sregs.es, regs.x.bx + MOUSE_PORT_INDEX_OFS);
        if (idx <= 1) {
            g_port = (uint8_t)idx;
        }
    }

    /* Function 0: reset. AX = FFFFh means a driver answered. BX = 1 keeps
     * the serial hardware as it is (see the header comment). */
    regs.x.ax = 0;
    regs.x.bx = 1;
    int86(0x33, &regs, &regs);
    if (regs.x.ax != 0xFFFF) {
        return FALSE;
    }

    g_context = 0;
    g_shown_context = 0;

    /* Function 10: software text pointer. The driver's default is the 6845
     * hardware cursor, which IGC and the editor use themselves. AND FFFFh
     * keeps the cell, XOR 8000h flips the reverse-video attribute bit. */
    regs.x.ax = 10;
    regs.x.bx = 0;
    regs.x.cx = 0xFFFF;
    regs.x.dx = 0x8000;
    int86(0x33, &regs, &regs);

    g_present = TRUE;
    mouse_flush();
    return TRUE;
}

/*---------------------------------------------------------------------------
 * mouse_shutdown - Hide the pointer before leaving
 *---------------------------------------------------------------------------*/
void mouse_shutdown(void)
{
    union REGS regs;

    if (!g_present) return;
    mouse_hide();
    regs.x.ax = 0;              /* back to driver defaults (hardware text */
    regs.x.bx = 1;              /* cursor, hidden) for whatever runs next;  */
    int86(0x33, &regs, &regs);  /* BX = 1: no serial re-initialisation      */
    g_present = FALSE;
}

/*---------------------------------------------------------------------------
 * mouse_present
 *---------------------------------------------------------------------------*/
bool_t mouse_present(void)
{
    return g_present;
}

/*---------------------------------------------------------------------------
 * mouse_port - Serial port the driver uses (MOUSE_PORT_NONE without one)
 *---------------------------------------------------------------------------*/
uint8_t mouse_port(void)
{
    return g_present ? g_port : MOUSE_PORT_NONE;
}

/*---------------------------------------------------------------------------
 * mouse_show / mouse_hide - Balanced calls to driver functions 1 and 2
 *---------------------------------------------------------------------------*/
static uint16_t press_count(uint16_t button, uint8_t *cx, uint8_t *cy);

void mouse_show(void)
{
    union REGS regs;
    uint8_t cx, cy;

    if (!g_present || g_shown) return;
    if (g_shown_context != g_context) {
        /* First wait on a new screen: drop presses made against the old one */
        press_count(0, &cx, &cy);
        press_count(1, &cx, &cy);
        g_last_x = 0xFF;
        g_last_y = 0xFF;
        g_shown_context = g_context;
    }
    regs.x.ax = 1;
    int86(0x33, &regs, &regs);
    g_shown = TRUE;
}

void mouse_hide(void)
{
    union REGS regs;

    if (!g_present || !g_shown) return;
    regs.x.ax = 2;
    int86(0x33, &regs, &regs);
    g_shown = FALSE;
}

/*---------------------------------------------------------------------------
 * press_count - Function 5: presses of a button since the last call
 * button: 0 = left, 1 = right. Fills the cell of the last press.
 *---------------------------------------------------------------------------*/
static uint16_t press_count(uint16_t button, uint8_t *cx, uint8_t *cy)
{
    union REGS regs;

    regs.x.ax = 5;
    regs.x.bx = button;
    int86(0x33, &regs, &regs);
    *cx = (uint8_t)(regs.x.cx / 10);
    *cy = (uint8_t)(regs.x.dx / 16);
    return regs.x.bx;
}

/*---------------------------------------------------------------------------
 * mouse_poll - Report one click if a button was pressed since the last poll
 *
 * Left clicks are checked first. A second left press on the same cell within
 * MOUSE_DBL_TIME of the previous one is reported as MOUSE_LEFT_DBL. Times
 * are poll times: the poll loop is tight while waiting, and presses queued
 * while IGC was busy are either stale (dropped by mouse_show on a screen
 * change) or belong to the same screen, where a late double-click is at
 * worst reported as two single clicks. Several presses between two polls
 * count as one click.
 *---------------------------------------------------------------------------*/
bool_t mouse_poll(KeyEvent *e)
{
    uint8_t cx, cy;
    uint16_t n;
    uint32_t now;

    if (!g_present) return FALSE;

    n = press_count(0, &cx, &cy);
    if (n != 0) {
        now = dos_hundredths();
        if (now < g_last_time) {
            now += 8640000L;            /* DOS clock wrapped at midnight */
        }
        g_mouse_x = cx;
        g_mouse_y = cy;
        e->type = KEY_MOUSE;
        if (cx == g_last_x && cy == g_last_y &&
            now - g_last_time <= MOUSE_DBL_TIME) {
            e->code = MOUSE_LEFT_DBL;
            g_last_x = 0xFF;            /* a third click starts over */
            g_last_y = 0xFF;
        } else {
            e->code = MOUSE_LEFT;
            g_last_x = cx;
            g_last_y = cy;
            g_last_time = now;
        }
        return TRUE;
    }

    n = press_count(1, &cx, &cy);
    if (n != 0) {
        g_mouse_x = cx;
        g_mouse_y = cy;
        e->type = KEY_MOUSE;
        e->code = MOUSE_RIGHT;
        g_last_x = 0xFF;
        g_last_y = 0xFF;
        return TRUE;
    }

    return FALSE;
}

/*---------------------------------------------------------------------------
 * mouse_flush - Drop clicks that arrived while nobody was listening
 *---------------------------------------------------------------------------*/
void mouse_flush(void)
{
    uint8_t cx, cy;

    if (!g_present) return;
    press_count(0, &cx, &cy);
    press_count(1, &cx, &cy);
    g_last_x = 0xFF;
    g_last_y = 0xFF;
    g_context++;
}

/*---------------------------------------------------------------------------
 * mouse_new_context - The screen under the pointer changed
 *---------------------------------------------------------------------------*/
void mouse_new_context(void)
{
    g_context++;
}
