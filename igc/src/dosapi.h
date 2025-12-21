/*
 * dosapi.h - IGC DOS Interface
 * DOS INT 21h wrappers
 * (Named dosapi to avoid conflict with system <dos.h>)
 */

#ifndef DOSAPI_H
#define DOSAPI_H

#include "igc.h"

/*---------------------------------------------------------------------------
 * DOS Version
 *---------------------------------------------------------------------------*/

/* Get DOS version - returns major version in low byte, minor in high byte */
/* Example: DOS 2.11 returns 0x0B02 (major=2, minor=11) */
uint16_t dos_get_version(void);

/*---------------------------------------------------------------------------
 * Program Control
 *---------------------------------------------------------------------------*/

/* Exit to DOS with return code */
void dos_exit(uint8_t code);

/*---------------------------------------------------------------------------
 * Drive Operations
 *---------------------------------------------------------------------------*/

/* Get current drive (0=A, 1=B, etc.) */
uint8_t dos_get_drive(void);

/* Set current drive */
void dos_set_drive(uint8_t drive);

/* Check if drive is valid (exists in system) */
bool_t dos_is_drive_valid(uint8_t drive);

/* Check if drive is ready (has media) */
bool_t dos_is_drive_ready(uint8_t drive);

/* Get number of valid drives (returns bitmask) */
uint32_t dos_get_valid_drives(void);

/*---------------------------------------------------------------------------
 * Critical Error Handler
 *---------------------------------------------------------------------------*/

/* Install critical error handler to suppress "Abort, Retry, Fail?" prompts */
void dos_install_crit_handler(void);

/* Restore original critical error handler */
void dos_restore_crit_handler(void);

/*---------------------------------------------------------------------------
 * DTA (Disk Transfer Area) Structure
 *---------------------------------------------------------------------------*/
typedef struct {
    uint8_t  reserved[21];      /* Reserved for DOS */
    uint8_t  attr;              /* File attribute */
    uint16_t time;              /* File time */
    uint16_t date;              /* File date */
    uint32_t size;              /* File size */
    char     name[13];          /* Filename (8.3 + null) */
} DTA;

/*---------------------------------------------------------------------------
 * Directory Operations
 *---------------------------------------------------------------------------*/

/* Get current DTA */
DTA __far *dos_get_dta(void);

/* Set DTA */
void dos_set_dta(DTA *dta);

/* Find first file (returns 0 on success) */
int dos_find_first(const char *pattern, uint8_t attr);

/* Find next file (returns 0 on success) */
int dos_find_next(void);

/* Get current directory for a drive (0=current, 1=A, etc.) */
/* Returns 0 on success, -1 on error */
int dos_get_curdir(uint8_t drive, char *buf);

/* Change directory */
int dos_chdir(const char *path);

/* Create directory */
int dos_mkdir(const char *path);

/* Remove directory */
int dos_rmdir(const char *path);

/*---------------------------------------------------------------------------
 * File Operations
 *---------------------------------------------------------------------------*/

/* File handle type */
typedef int16_t dos_handle_t;

/* Open file, returns handle or -1 on error */
dos_handle_t dos_open(const char *path, uint8_t mode);

/* Create file, returns handle or -1 on error */
dos_handle_t dos_create(const char *path, uint8_t attr);

/* Close file handle */
int dos_close(dos_handle_t handle);

/* Read from file, returns bytes read or -1 on error */
int16_t dos_read(dos_handle_t handle, void __far *buf, uint16_t count);

/* Write to file, returns bytes written or -1 on error */
int16_t dos_write(dos_handle_t handle, const void __far *buf, uint16_t count);

/* Get file size (via seek to end and back) */
uint32_t dos_file_size(dos_handle_t handle);

/* Delete file */
int dos_delete(const char *path);

/* Rename/move file */
int dos_rename(const char *oldpath, const char *newpath);

/* Check if file/dir exists */
bool_t dos_exists(const char *path);

/* Get file attributes (-1 on error) */
int16_t dos_get_attr(const char *path);

/* Set file attributes */
int dos_set_attr(const char *path, uint8_t attr);

/*---------------------------------------------------------------------------
 * Disk Space
 *---------------------------------------------------------------------------*/

/* Get free space on drive (in KB) */
/* Returns 0 on error */
uint32_t dos_get_free_space(uint8_t drive);

/*---------------------------------------------------------------------------
 * Console Control
 *---------------------------------------------------------------------------*/

/* Hide cursor via DOS */
void dos_cursor_off(void);

/* Show cursor via DOS */
void dos_cursor_on(void);

#endif /* DOSAPI_H */
