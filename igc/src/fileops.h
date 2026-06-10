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
 * File Operations
 * (Copy buffer sizes COPY_BUF_* come from igc.h)
 *---------------------------------------------------------------------------*/

/* Initialize file operations module */
bool_t fops_init(void);

/* Shutdown file operations module */
void fops_shutdown(void);

/* F5: Ask Copy/Move once (filename for a single item, "N files" for many),
 * then perform the chosen operation with no further confirmation. */
/* Returns FOPS_OK, FOPS_CANCEL, or FOPS_ERROR */
int fops_copy_or_move(void);

/* Copy selected files to other panel */
/* Returns FOPS_OK, FOPS_CANCEL, or FOPS_ERROR */
int fops_copy(void);

/* Move selected files to other panel */
/* Returns FOPS_OK, FOPS_CANCEL, or FOPS_ERROR */
int fops_move(void);

/* F6: Delete selected files */
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
