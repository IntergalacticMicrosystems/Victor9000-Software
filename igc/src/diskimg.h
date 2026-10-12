/*
 * diskimg.h - Victor floppy <-> disk image file (.IMG raw, .RLE compressed)
 *
 * Whole-track INT 25h/26h access to drives A:/B:, with the image living on a
 * local DOS drive or on the serial file server (igcfs). F6 "More" menu.
 */

#ifndef DISKIMG_H
#define DISKIMG_H

#include "igc.h"

/* Drive IGC was started from (0 = A). Writing an image onto it is refused. */
void diskimg_set_exe_drive(uint8_t drive);

/* Read a floppy into an image file in the active panel's directory. */
void diskimg_read_to_file(void);

/* Write the image file under the active panel's cursor onto a floppy. */
void diskimg_write_from_file(void);

#endif /* DISKIMG_H */
