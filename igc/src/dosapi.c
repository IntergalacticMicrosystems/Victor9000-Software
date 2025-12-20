/*
 * dosapi.c - IGC DOS Interface Implementation
 * DOS INT 21h wrappers
 */

#include <dos.h>
#include <i86.h>
#include <string.h>
#include "dosapi.h"

/*---------------------------------------------------------------------------
 * Critical Error Handler (INT 24h)
 * Used to suppress "Abort, Retry, Fail?" prompts when accessing drives
 * without media (floppy drives with no disk, etc.)
 *---------------------------------------------------------------------------*/
static void (__interrupt __far *old_int24)(void) = (void (__interrupt __far *)(void))0;
static uint8_t crit_handler_installed = 0;

/* Critical error handler - set AL=3 (fail) and return
 * Using simple __interrupt handler that modifies AX on stack.
 * Stack layout from disassembly: AX is saved at [BP+22] (0x16)
 */
static void __interrupt __far crit_error_handler(void)
{
    /* Set AL=3 to tell DOS to fail the call */
    _asm {
        mov word ptr [bp+22], 3  ; Modify saved AX on stack (AL=3)
    }
}

/*---------------------------------------------------------------------------
 * dos_install_crit_handler - Install our critical error handler
 *---------------------------------------------------------------------------*/
void dos_install_crit_handler(void)
{
    if (!crit_handler_installed) {
        old_int24 = _dos_getvect(0x24);
        _dos_setvect(0x24, (void (__interrupt __far *)(void))crit_error_handler);
        crit_handler_installed = 1;
    }
}

/*---------------------------------------------------------------------------
 * dos_restore_crit_handler - Restore original critical error handler
 *---------------------------------------------------------------------------*/
void dos_restore_crit_handler(void)
{
    if (crit_handler_installed && old_int24 != (void (__interrupt __far *)(void))0) {
        _dos_setvect(0x24, old_int24);
        old_int24 = (void (__interrupt __far *)(void))0;
        crit_handler_installed = 0;
    }
}

/*---------------------------------------------------------------------------
 * dos_exit - Exit to DOS with return code
 *---------------------------------------------------------------------------*/
void dos_exit(uint8_t code)
{
    union REGS regs;

    regs.h.ah = 0x4C;
    regs.h.al = code;
    int86(0x21, &regs, &regs);
}

/*---------------------------------------------------------------------------
 * dos_get_drive - Get current drive
 *---------------------------------------------------------------------------*/
uint8_t dos_get_drive(void)
{
    union REGS regs;

    regs.h.ah = 0x19;
    int86(0x21, &regs, &regs);

    return regs.h.al;
}

/*---------------------------------------------------------------------------
 * dos_set_drive - Set current drive
 *---------------------------------------------------------------------------*/
void dos_set_drive(uint8_t drive)
{
    union REGS regs;

    regs.h.ah = 0x0E;
    regs.h.dl = drive;
    int86(0x21, &regs, &regs);
}

/*---------------------------------------------------------------------------
 * dos_is_drive_valid - Check if drive is valid (exists in system)
 * Uses critical error handler to suppress "Abort, Retry, Fail?" prompts
 *---------------------------------------------------------------------------*/
bool_t dos_is_drive_valid(uint8_t drive)
{
    union REGS regs;
    uint8_t old_drive;
    uint8_t new_drive;

    /* Install critical error handler to suppress prompts */
    dos_install_crit_handler();

    /* Save current drive */
    old_drive = dos_get_drive();

    /* Try to select the drive */
    regs.h.ah = 0x0E;
    regs.h.dl = drive;
    int86(0x21, &regs, &regs);

    /* Check what drive we're on now */
    new_drive = dos_get_drive();

    /* Restore original drive */
    regs.h.ah = 0x0E;
    regs.h.dl = old_drive;
    int86(0x21, &regs, &regs);

    /* Restore original critical error handler */
    dos_restore_crit_handler();

    /* Drive is valid if it was selected successfully */
    return (new_drive == drive) ? TRUE : FALSE;
}

/*---------------------------------------------------------------------------
 * dos_is_drive_ready - Check if drive has media (is ready)
 * Uses IOCTL to check drive status without full disk access.
 * Uses critical error handler to suppress "Abort, Retry, Fail?" prompts.
 *---------------------------------------------------------------------------*/
bool_t dos_is_drive_ready(uint8_t drive)
{
    union REGS regs;
    struct SREGS sregs;

    /* Install critical error handler to suppress prompts */
    dos_install_crit_handler();

    /* First try: IOCTL - Check if block device is remote (INT 21h AX=4409h)
     * This is a quick check that doesn't require disk access */
    regs.x.ax = 0x4409;
    regs.h.bl = drive + 1;  /* 1=A, 2=B, etc. */
    int86(0x21, &regs, &regs);

    if (regs.x.cflag) {
        /* IOCTL failed - drive not accessible */
        dos_restore_crit_handler();
        return FALSE;
    }

    /* Second check: Try to get current directory for the drive
     * This requires the drive to have media but is faster than disk free space */
    {
        char buf[68];
        segread(&sregs);
        regs.h.ah = 0x47;           /* Get current directory */
        regs.h.dl = drive + 1;      /* 1=A, 2=B, etc. */
        regs.x.si = FP_OFF(buf);
        sregs.ds = FP_SEG(buf);
        int86x(0x21, &regs, &regs, &sregs);
    }

    /* Restore original critical error handler */
    dos_restore_crit_handler();

    /* CF set means error (drive not ready) */
    return regs.x.cflag ? FALSE : TRUE;
}

/*---------------------------------------------------------------------------
 * dos_get_valid_drives - Get bitmask of valid drives
 *---------------------------------------------------------------------------*/
uint32_t dos_get_valid_drives(void)
{
    uint32_t mask = 0;
    uint8_t i;

    for (i = 0; i < 26; i++) {
        if (dos_is_drive_valid(i)) {
            mask |= (1UL << i);
        }
    }

    return mask;
}

/*---------------------------------------------------------------------------
 * dos_get_dta - Get current DTA
 *---------------------------------------------------------------------------*/
DTA __far *dos_get_dta(void)
{
    union REGS regs;
    struct SREGS sregs;

    regs.h.ah = 0x2F;
    int86x(0x21, &regs, &regs, &sregs);

    return (DTA __far *)MK_FP(sregs.es, regs.x.bx);
}

/*---------------------------------------------------------------------------
 * dos_set_dta - Set DTA
 *---------------------------------------------------------------------------*/
void dos_set_dta(DTA *dta)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x1A;
    regs.x.dx = FP_OFF(dta);
    sregs.ds = FP_SEG(dta);

    int86x(0x21, &regs, &regs, &sregs);
}

/*---------------------------------------------------------------------------
 * dos_find_first - Find first file
 *---------------------------------------------------------------------------*/
int dos_find_first(const char *pattern, uint8_t attr)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x4E;
    regs.x.cx = attr;
    regs.x.dx = FP_OFF(pattern);
    sregs.ds = FP_SEG(pattern);

    int86x(0x21, &regs, &regs, &sregs);

    return regs.x.cflag ? -1 : 0;
}

/*---------------------------------------------------------------------------
 * dos_find_next - Find next file
 *---------------------------------------------------------------------------*/
int dos_find_next(void)
{
    union REGS regs;

    regs.h.ah = 0x4F;
    int86(0x21, &regs, &regs);

    return regs.x.cflag ? -1 : 0;
}

/*---------------------------------------------------------------------------
 * dos_get_curdir - Get current directory for a drive
 *---------------------------------------------------------------------------*/
int dos_get_curdir(uint8_t drive, char *buf)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x47;
    regs.h.dl = drive;  /* 0=current, 1=A, 2=B, etc. */
    regs.x.si = FP_OFF(buf);
    sregs.ds = FP_SEG(buf);

    int86x(0x21, &regs, &regs, &sregs);

    if (regs.x.cflag) {
        buf[0] = '\0';
        return -1;
    }

    return 0;
}

/*---------------------------------------------------------------------------
 * dos_chdir - Change directory
 *---------------------------------------------------------------------------*/
int dos_chdir(const char *path)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x3B;
    regs.x.dx = FP_OFF(path);
    sregs.ds = FP_SEG(path);

    int86x(0x21, &regs, &regs, &sregs);

    return regs.x.cflag ? -1 : 0;
}

/*---------------------------------------------------------------------------
 * dos_mkdir - Create directory
 *---------------------------------------------------------------------------*/
int dos_mkdir(const char *path)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x39;
    regs.x.dx = FP_OFF(path);
    sregs.ds = FP_SEG(path);

    int86x(0x21, &regs, &regs, &sregs);

    return regs.x.cflag ? -1 : 0;
}

/*---------------------------------------------------------------------------
 * dos_rmdir - Remove directory
 *---------------------------------------------------------------------------*/
int dos_rmdir(const char *path)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x3A;
    regs.x.dx = FP_OFF(path);
    sregs.ds = FP_SEG(path);

    int86x(0x21, &regs, &regs, &sregs);

    return regs.x.cflag ? -1 : 0;
}

/*---------------------------------------------------------------------------
 * dos_open - Open file
 *---------------------------------------------------------------------------*/
dos_handle_t dos_open(const char *path, uint8_t mode)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x3D;
    regs.h.al = mode;
    regs.x.dx = FP_OFF(path);
    sregs.ds = FP_SEG(path);

    int86x(0x21, &regs, &regs, &sregs);

    if (regs.x.cflag) {
        return -1;
    }
    return (dos_handle_t)regs.x.ax;
}

/*---------------------------------------------------------------------------
 * dos_create - Create file
 *---------------------------------------------------------------------------*/
dos_handle_t dos_create(const char *path, uint8_t attr)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x3C;
    regs.x.cx = attr;
    regs.x.dx = FP_OFF(path);
    sregs.ds = FP_SEG(path);

    int86x(0x21, &regs, &regs, &sregs);

    if (regs.x.cflag) {
        return -1;
    }
    return (dos_handle_t)regs.x.ax;
}

/*---------------------------------------------------------------------------
 * dos_close - Close file handle
 *---------------------------------------------------------------------------*/
int dos_close(dos_handle_t handle)
{
    union REGS regs;

    regs.h.ah = 0x3E;
    regs.x.bx = handle;
    int86(0x21, &regs, &regs);

    return regs.x.cflag ? -1 : 0;
}

/*---------------------------------------------------------------------------
 * dos_read - Read from file
 *---------------------------------------------------------------------------*/
int16_t dos_read(dos_handle_t handle, void __far *buf, uint16_t count)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x3F;
    regs.x.bx = handle;
    regs.x.cx = count;
    regs.x.dx = FP_OFF(buf);
    sregs.ds = FP_SEG(buf);

    int86x(0x21, &regs, &regs, &sregs);

    if (regs.x.cflag) {
        return -1;
    }
    return (int16_t)regs.x.ax;
}

/*---------------------------------------------------------------------------
 * dos_write - Write to file
 *---------------------------------------------------------------------------*/
int16_t dos_write(dos_handle_t handle, const void __far *buf, uint16_t count)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x40;
    regs.x.bx = handle;
    regs.x.cx = count;
    regs.x.dx = FP_OFF(buf);
    sregs.ds = FP_SEG(buf);

    int86x(0x21, &regs, &regs, &sregs);

    if (regs.x.cflag) {
        return -1;
    }
    return (int16_t)regs.x.ax;
}

/*---------------------------------------------------------------------------
 * dos_file_size - Get file size
 *---------------------------------------------------------------------------*/
uint32_t dos_file_size(dos_handle_t handle)
{
    union REGS regs;
    uint32_t cur_pos;
    uint32_t size;

    /* Get current position */
    regs.h.ah = 0x42;
    regs.h.al = 1;  /* Seek from current position */
    regs.x.bx = handle;
    regs.x.cx = 0;
    regs.x.dx = 0;
    int86(0x21, &regs, &regs);
    if (regs.x.cflag) return 0;
    cur_pos = ((uint32_t)regs.x.dx << 16) | regs.x.ax;

    /* Seek to end */
    regs.h.ah = 0x42;
    regs.h.al = 2;  /* Seek from end */
    regs.x.bx = handle;
    regs.x.cx = 0;
    regs.x.dx = 0;
    int86(0x21, &regs, &regs);
    if (regs.x.cflag) return 0;
    size = ((uint32_t)regs.x.dx << 16) | regs.x.ax;

    /* Seek back to original position */
    regs.h.ah = 0x42;
    regs.h.al = 0;  /* Seek from beginning */
    regs.x.bx = handle;
    regs.x.cx = (uint16_t)(cur_pos >> 16);
    regs.x.dx = (uint16_t)(cur_pos & 0xFFFF);
    int86(0x21, &regs, &regs);

    return size;
}

/*---------------------------------------------------------------------------
 * dos_delete - Delete file
 *---------------------------------------------------------------------------*/
int dos_delete(const char *path)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x41;
    regs.x.dx = FP_OFF(path);
    sregs.ds = FP_SEG(path);

    int86x(0x21, &regs, &regs, &sregs);

    return regs.x.cflag ? -1 : 0;
}

/*---------------------------------------------------------------------------
 * dos_rename - Rename/move file
 *---------------------------------------------------------------------------*/
int dos_rename(const char *oldpath, const char *newpath)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x56;
    regs.x.dx = FP_OFF(oldpath);
    sregs.ds = FP_SEG(oldpath);
    regs.x.di = FP_OFF(newpath);
    sregs.es = FP_SEG(newpath);

    int86x(0x21, &regs, &regs, &sregs);

    return regs.x.cflag ? -1 : 0;
}

/*---------------------------------------------------------------------------
 * dos_exists - Check if file/dir exists
 *---------------------------------------------------------------------------*/
bool_t dos_exists(const char *path)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    /* Use get file attributes - if it works, file exists */
    regs.h.ah = 0x43;
    regs.h.al = 0;  /* Get attributes */
    regs.x.dx = FP_OFF(path);
    sregs.ds = FP_SEG(path);

    int86x(0x21, &regs, &regs, &sregs);

    return regs.x.cflag ? FALSE : TRUE;
}

/*---------------------------------------------------------------------------
 * dos_get_attr - Get file attributes
 *---------------------------------------------------------------------------*/
int16_t dos_get_attr(const char *path)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x43;
    regs.h.al = 0;  /* Get attributes */
    regs.x.dx = FP_OFF(path);
    sregs.ds = FP_SEG(path);

    int86x(0x21, &regs, &regs, &sregs);

    if (regs.x.cflag) {
        return -1;
    }
    return (int16_t)regs.x.cx;
}

/*---------------------------------------------------------------------------
 * dos_set_attr - Set file attributes
 *---------------------------------------------------------------------------*/
int dos_set_attr(const char *path, uint8_t attr)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x43;
    regs.h.al = 1;  /* Set attributes */
    regs.x.cx = attr;
    regs.x.dx = FP_OFF(path);
    sregs.ds = FP_SEG(path);

    int86x(0x21, &regs, &regs, &sregs);

    return regs.x.cflag ? -1 : 0;
}

/*---------------------------------------------------------------------------
 * dos_get_free_space - Get free space on drive (in KB)
 *---------------------------------------------------------------------------*/
uint32_t dos_get_free_space(uint8_t drive)
{
    union REGS regs;
    uint32_t clusters;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_sector;
    uint32_t free_bytes;

    /* INT 21h AH=36h: Get disk free space */
    regs.h.ah = 0x36;
    regs.h.dl = drive + 1;  /* 1=A, 2=B, etc. */
    int86(0x21, &regs, &regs);

    /* Check for error */
    if (regs.x.ax == 0xFFFF) {
        return 0;
    }

    /* AX = sectors per cluster */
    /* BX = number of free clusters */
    /* CX = bytes per sector */
    /* DX = total clusters on drive */

    sectors_per_cluster = regs.x.ax;
    clusters = regs.x.bx;
    bytes_per_sector = regs.x.cx;

    /* Calculate free bytes, avoiding overflow */
    free_bytes = clusters * sectors_per_cluster * bytes_per_sector;

    /* Return in KB */
    return free_bytes / 1024L;
}

/*---------------------------------------------------------------------------
 * dos_cursor_off - Hide cursor
 * Note: Victor 9000 doesn't support ANSI sequences - cursor control is
 * done via CRTC registers in screen.c (scr_cursor_off/on)
 *---------------------------------------------------------------------------*/
void dos_cursor_off(void)
{
    /* Not implemented for V9K - use scr_cursor_off() instead */
}

/*---------------------------------------------------------------------------
 * dos_cursor_on - Show cursor
 *---------------------------------------------------------------------------*/
void dos_cursor_on(void)
{
    /* Not implemented for V9K - use scr_cursor_on() instead */
}
