/*
 * fileops.h - IGC File Operations
 * Copy, move, delete, mkdir, rename
 */

#ifndef FILEOPS_H
#define FILEOPS_H

#include "igc.h"
#include "panel.h"

/*---------------------------------------------------------------------------
 * Operation Result Codes
 *---------------------------------------------------------------------------*/
#define FOPS_OK           0
#define FOPS_CANCEL      -1
#define FOPS_ERROR       -2
#define FOPS_SKIP        -3

/*---------------------------------------------------------------------------
 * Copy Buffer Size (based on memory tier)
 *---------------------------------------------------------------------------*/
#define COPY_BUF_LOW     512     /* Low memory: 512 bytes */
#define COPY_BUF_MEDIUM  2048    /* Medium memory: 2KB */
#define COPY_BUF_HIGH    8192    /* High memory: 8KB */

/*---------------------------------------------------------------------------
 * File Operations
 *---------------------------------------------------------------------------*/

/* Initialize file operations module */
bool_t fops_init(void);

/* Shutdown file operations module */
void fops_shutdown(void);

/* F5: Copy selected files to other panel */
/* Returns FOPS_OK, FOPS_CANCEL, or FOPS_ERROR */
int fops_copy(void);

/* F6: Move selected files to other panel */
/* Returns FOPS_OK, FOPS_CANCEL, or FOPS_ERROR */
int fops_move(void);

/* F7: Delete selected files */
/* Returns FOPS_OK, FOPS_CANCEL, or FOPS_ERROR */
int fops_delete(void);

/* F2: Create directory */
/* Returns FOPS_OK, FOPS_CANCEL, or FOPS_ERROR */
int fops_mkdir(void);

/* F8: Rename file/directory */
/* Returns FOPS_OK, FOPS_CANCEL, or FOPS_ERROR */
int fops_rename(void);

/*---------------------------------------------------------------------------
 * Internal Helpers (exposed for progress display)
 *---------------------------------------------------------------------------*/

/* Copy a single file */
int fops_copy_file(const char *src, const char *dst);

/* Copy a directory recursively */
int fops_copy_dir(const char *src, const char *dst);

/* Delete a single file */
int fops_delete_file(const char *path);

/* Delete a directory recursively */
int fops_delete_dir(const char *path);

#endif /* FILEOPS_H */
