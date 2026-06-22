/**
 * @file ftx_dosfile.h
 * @brief DOS File I/O Wrapper for File Transfer
 *
 * Simplified DOS file operations for the file transfer library.
 * Based on INT 21h services.
 */

#ifndef FTX_DOSFILE_H
#define FTX_DOSFILE_H

#include <stdint.h>

/*===========================================================================
 * Types
 *===========================================================================*/

/* File handle type */
typedef int16_t ftx_file_t;

/* Invalid file handle */
#define FTX_FILE_INVALID    (-1)

/* File open modes */
#define FTX_OPEN_READ       0x00
#define FTX_OPEN_WRITE      0x01
#define FTX_OPEN_READWRITE  0x02

/* File attributes */
#define FTX_ATTR_NORMAL     0x00
#define FTX_ATTR_READONLY   0x01
#define FTX_ATTR_HIDDEN     0x02
#define FTX_ATTR_SYSTEM     0x04
#define FTX_ATTR_DIRECTORY  0x10
#define FTX_ATTR_ARCHIVE    0x20

/* Seek modes */
#define FTX_SEEK_SET        0   /* From beginning */
#define FTX_SEEK_CUR        1   /* From current position */
#define FTX_SEEK_END        2   /* From end */

/* DTA structure for directory enumeration */
typedef struct {
    uint8_t  reserved[21];      /* Reserved for DOS */
    uint8_t  attr;              /* File attribute */
    uint16_t time;              /* File time */
    uint16_t date;              /* File date */
    uint32_t size;              /* File size */
    char     name[13];          /* Filename (8.3 + null) */
} ftx_dta_t;

/*===========================================================================
 * File Operations
 *===========================================================================*/

/**
 * Open a file.
 * @param path File path
 * @param mode Open mode (FTX_OPEN_*)
 * @return File handle or FTX_FILE_INVALID on error
 */
ftx_file_t ftx_file_open(const char *path, uint8_t mode);

/**
 * Create a new file (overwrites if exists).
 * @param path File path
 * @param attr File attributes
 * @return File handle or FTX_FILE_INVALID on error
 */
ftx_file_t ftx_file_create(const char *path, uint8_t attr);

/**
 * Close a file.
 * @param handle File handle
 * @return 0 on success, -1 on error
 */
int ftx_file_close(ftx_file_t handle);

/**
 * Read from a file.
 * @param handle File handle
 * @param buf Buffer to read into
 * @param count Bytes to read
 * @return Bytes read, or -1 on error
 */
int16_t ftx_file_read(ftx_file_t handle, uint8_t *buf, uint16_t count);

/**
 * Write to a file.
 * @param handle File handle
 * @param buf Buffer to write from
 * @param count Bytes to write
 * @return Bytes written, or -1 on error
 */
int16_t ftx_file_write(ftx_file_t handle, const uint8_t *buf, uint16_t count);

/**
 * Seek within a file.
 * @param handle File handle
 * @param offset Offset to seek to
 * @param whence Seek mode (FTX_SEEK_*)
 * @return New position, or -1 on error
 */
int32_t ftx_file_seek(ftx_file_t handle, int32_t offset, uint8_t whence);

/**
 * Get file size.
 * @param handle Open file handle
 * @return File size in bytes
 */
uint32_t ftx_file_size(ftx_file_t handle);

/*===========================================================================
 * File Information
 *===========================================================================*/

/**
 * Check if file exists.
 * @param path File path
 * @return 1 if exists, 0 if not
 */
int ftx_file_exists(const char *path);

/**
 * Delete a file.
 * @param path File path
 * @return 0 on success, -1 on error
 */
int ftx_file_delete(const char *path);

/**
 * Get file attributes.
 * @param path File path
 * @return Attributes, or -1 on error
 */
int16_t ftx_file_get_attr(const char *path);

/**
 * Get file date/time.
 * @param handle Open file handle
 * @param date Pointer to receive DOS date
 * @param time Pointer to receive DOS time
 * @return 0 on success, -1 on error
 */
int ftx_file_get_datetime(ftx_file_t handle, uint16_t *date, uint16_t *time);

/**
 * Set file date/time.
 * @param handle Open file handle
 * @param date DOS date
 * @param time DOS time
 * @return 0 on success, -1 on error
 */
int ftx_file_set_datetime(ftx_file_t handle, uint16_t date, uint16_t time);

/*===========================================================================
 * Directory Operations
 *===========================================================================*/

/**
 * Find first matching file.
 * @param pattern Search pattern (e.g., "A:\\*.*")
 * @param attr Attribute mask
 * @param dta DTA structure to receive result
 * @return 0 if found, -1 if no match
 */
int ftx_dir_find_first(const char *pattern, uint8_t attr, ftx_dta_t *dta);

/**
 * Find next matching file.
 * @param dta DTA structure to receive result
 * @return 0 if found, -1 if no more matches
 */
int ftx_dir_find_next(ftx_dta_t *dta);

/**
 * Create directory.
 * @param path Directory path
 * @return 0 on success, -1 on error
 */
int ftx_dir_create(const char *path);

/*===========================================================================
 * Disk Space
 *===========================================================================*/

/**
 * Get free disk space.
 * @param drive Drive number (0=current, 1=A, 2=B, etc.)
 * @return Free space in bytes, or 0 on error
 */
uint32_t ftx_disk_free(uint8_t drive);

#endif /* FTX_DOSFILE_H */
