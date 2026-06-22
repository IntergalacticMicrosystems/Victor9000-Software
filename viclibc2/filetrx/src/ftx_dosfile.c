/**
 * @file ftx_dosfile.c
 * @brief DOS File I/O Implementation
 *
 * DOS INT 21h wrappers for file operations.
 */

#include <dos.h>
#include <i86.h>
#include "ftx_dosfile.h"

/* Saved DTA pointer for directory operations */
static ftx_dta_t *current_dta = (ftx_dta_t *)0;

/*---------------------------------------------------------------------------
 * File Operations
 *---------------------------------------------------------------------------*/

ftx_file_t ftx_file_open(const char *path, uint8_t mode)
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
        return FTX_FILE_INVALID;
    }
    return (ftx_file_t)regs.x.ax;
}

ftx_file_t ftx_file_create(const char *path, uint8_t attr)
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
        return FTX_FILE_INVALID;
    }
    return (ftx_file_t)regs.x.ax;
}

int ftx_file_close(ftx_file_t handle)
{
    union REGS regs;

    if (handle == FTX_FILE_INVALID) {
        return -1;
    }

    regs.h.ah = 0x3E;
    regs.x.bx = (uint16_t)handle;
    int86(0x21, &regs, &regs);

    return regs.x.cflag ? -1 : 0;
}

int16_t ftx_file_read(ftx_file_t handle, uint8_t *buf, uint16_t count)
{
    union REGS regs;
    struct SREGS sregs;

    if (handle == FTX_FILE_INVALID) {
        return -1;
    }

    segread(&sregs);

    regs.h.ah = 0x3F;
    regs.x.bx = (uint16_t)handle;
    regs.x.cx = count;
    regs.x.dx = FP_OFF(buf);
    sregs.ds = FP_SEG(buf);

    int86x(0x21, &regs, &regs, &sregs);

    if (regs.x.cflag) {
        return -1;
    }
    return (int16_t)regs.x.ax;
}

int16_t ftx_file_write(ftx_file_t handle, const uint8_t *buf, uint16_t count)
{
    union REGS regs;
    struct SREGS sregs;

    if (handle == FTX_FILE_INVALID) {
        return -1;
    }

    segread(&sregs);

    regs.h.ah = 0x40;
    regs.x.bx = (uint16_t)handle;
    regs.x.cx = count;
    regs.x.dx = FP_OFF(buf);
    sregs.ds = FP_SEG(buf);

    int86x(0x21, &regs, &regs, &sregs);

    if (regs.x.cflag) {
        return -1;
    }
    return (int16_t)regs.x.ax;
}

int32_t ftx_file_seek(ftx_file_t handle, int32_t offset, uint8_t whence)
{
    union REGS regs;

    if (handle == FTX_FILE_INVALID) {
        return -1;
    }

    regs.h.ah = 0x42;
    regs.h.al = whence;
    regs.x.bx = (uint16_t)handle;
    regs.x.cx = (uint16_t)(offset >> 16);
    regs.x.dx = (uint16_t)(offset & 0xFFFF);

    int86(0x21, &regs, &regs);

    if (regs.x.cflag) {
        return -1;
    }
    return ((int32_t)regs.x.dx << 16) | regs.x.ax;
}

uint32_t ftx_file_size(ftx_file_t handle)
{
    int32_t cur_pos;
    int32_t size;

    if (handle == FTX_FILE_INVALID) {
        return 0;
    }

    /* Save current position */
    cur_pos = ftx_file_seek(handle, 0, FTX_SEEK_CUR);
    if (cur_pos < 0) {
        return 0;
    }

    /* Seek to end to get size */
    size = ftx_file_seek(handle, 0, FTX_SEEK_END);
    if (size < 0) {
        return 0;
    }

    /* Restore original position */
    ftx_file_seek(handle, cur_pos, FTX_SEEK_SET);

    return (uint32_t)size;
}

/*---------------------------------------------------------------------------
 * File Information
 *---------------------------------------------------------------------------*/

int ftx_file_exists(const char *path)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x43;
    regs.h.al = 0;  /* Get attributes */
    regs.x.dx = FP_OFF(path);
    sregs.ds = FP_SEG(path);

    int86x(0x21, &regs, &regs, &sregs);

    return regs.x.cflag ? 0 : 1;
}

int ftx_file_delete(const char *path)
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

int16_t ftx_file_get_attr(const char *path)
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

int ftx_file_get_datetime(ftx_file_t handle, uint16_t *date, uint16_t *time)
{
    union REGS regs;

    if (handle == FTX_FILE_INVALID) {
        return -1;
    }

    regs.h.ah = 0x57;
    regs.h.al = 0;  /* Get date/time */
    regs.x.bx = (uint16_t)handle;

    int86(0x21, &regs, &regs);

    if (regs.x.cflag) {
        return -1;
    }

    if (date) *date = regs.x.dx;
    if (time) *time = regs.x.cx;

    return 0;
}

int ftx_file_set_datetime(ftx_file_t handle, uint16_t date, uint16_t time)
{
    union REGS regs;

    if (handle == FTX_FILE_INVALID) {
        return -1;
    }

    regs.h.ah = 0x57;
    regs.h.al = 1;  /* Set date/time */
    regs.x.bx = (uint16_t)handle;
    regs.x.cx = time;
    regs.x.dx = date;

    int86(0x21, &regs, &regs);

    return regs.x.cflag ? -1 : 0;
}

/*---------------------------------------------------------------------------
 * Directory Operations
 *---------------------------------------------------------------------------*/

/* Set DTA for directory operations */
static void set_dta(ftx_dta_t *dta)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);

    regs.h.ah = 0x1A;
    regs.x.dx = FP_OFF(dta);
    sregs.ds = FP_SEG(dta);

    int86x(0x21, &regs, &regs, &sregs);

    current_dta = dta;
}

int ftx_dir_find_first(const char *pattern, uint8_t attr, ftx_dta_t *dta)
{
    union REGS regs;
    struct SREGS sregs;

    /* Set DTA to our structure */
    set_dta(dta);

    segread(&sregs);

    regs.h.ah = 0x4E;
    regs.x.cx = attr;
    regs.x.dx = FP_OFF(pattern);
    sregs.ds = FP_SEG(pattern);

    int86x(0x21, &regs, &regs, &sregs);

    return regs.x.cflag ? -1 : 0;
}

int ftx_dir_find_next(ftx_dta_t *dta)
{
    union REGS regs;

    /* Ensure DTA is set correctly */
    if (current_dta != dta) {
        set_dta(dta);
    }

    regs.h.ah = 0x4F;
    int86(0x21, &regs, &regs);

    return regs.x.cflag ? -1 : 0;
}

int ftx_dir_create(const char *path)
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
 * Disk Space
 *---------------------------------------------------------------------------*/

uint32_t ftx_disk_free(uint8_t drive)
{
    union REGS regs;
    uint32_t clusters;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_sector;

    regs.h.ah = 0x36;
    regs.h.dl = drive;  /* 0=current, 1=A, 2=B, etc. */
    int86(0x21, &regs, &regs);

    /* Check for error */
    if (regs.x.ax == 0xFFFF) {
        return 0;
    }

    sectors_per_cluster = regs.x.ax;
    clusters = regs.x.bx;
    bytes_per_sector = regs.x.cx;

    return clusters * sectors_per_cluster * bytes_per_sector;
}
